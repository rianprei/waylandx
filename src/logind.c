#define _GNU_SOURCE /* F_DUPFD_CLOEXEC, strchrnul */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/util/log.h>
#include "server.h"

/*
 * Desktop session plumbing that KWin gets from logind and D-Bus:
 *   - lock the screen before suspend (delay inhibitor, released once the
 *     locker shows), and on `loginctl lock-session`;
 *   - report the lock state back to logind (LockedHint);
 *   - backlight brightness without root, through logind's SetBrightness;
 *   - media keys through MPRIS on the user bus.
 * Only a real session (DRM, libseat) talks to logind; nested and headless
 * runs would otherwise act on the host desktop's session. WAYLANDX_LOGIND=1
 * forces it on (the tests point DBUS_SYSTEM_BUS_ADDRESS at a mock).
 */

#define BACKLIGHT_DIR "/sys/class/backlight"

static const char *backlight_dir(void) {
	const char *d = getenv("WAYLANDX_BACKLIGHT_DIR");
	return d && *d ? d : BACKLIGHT_DIR;
}

static int read_int_file(const char *dir, const char *name, const char *file) {
	char path[512];
	snprintf(path, sizeof(path), "%s/%s/%s", dir, name, file);
	FILE *f = fopen(path, "r");
	if (!f) {
		return -1;
	}
	int v = -1;
	if (fscanf(f, "%d", &v) != 1) {
		v = -1;
	}
	fclose(f);
	return v;
}

/* firmware > platform > raw, like KWin and systemd-backlight */
static bool pick_backlight(char *name, size_t len) {
	static const char *order[] = {"firmware", "platform", "raw"};
	const char *dir = backlight_dir();
	int best = 99;
	DIR *d = opendir(dir);
	if (!d) {
		return false;
	}
	struct dirent *e;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.') {
			continue;
		}
		char path[512], type[32] = "";
		snprintf(path, sizeof(path), "%s/%s/type", dir, e->d_name);
		FILE *f = fopen(path, "r");
		if (f) {
			if (!fgets(type, sizeof(type), f)) {
				type[0] = '\0';
			}
			fclose(f);
		}
		type[strcspn(type, "\n")] = '\0';
		int rank = 3;
		for (int i = 0; i < 3; i++) {
			if (!strcmp(type, order[i])) {
				rank = i;
			}
		}
		if (rank < best) {
			best = rank;
			snprintf(name, len, "%s", e->d_name);
		}
	}
	closedir(d);
	return best < 99;
}

/* "+5%", "-5%", "40%", "40" (percent) or "" (just read). Returns the new percent. */
static bool brightness_target(const char *arg, int cur, int max, int *out) {
	if (!arg || !*arg) {
		*out = cur;
		return true;
	}
	char *end;
	errno = 0;
	double v = strtod(arg, &end);
	if (errno || end == arg || (*end && strcmp(end, "%"))) {
		return false;
	}
	int step = (int)(v * max / 100.0 + (v >= 0 ? 0.5 : -0.5));
	int t = (arg[0] == '+' || arg[0] == '-') ? cur + step : step;
	int floor = max >= 100 ? max / 100 : 1; /* never fully dark: 1% */
	*out = t < floor ? floor : t > max ? max : t;
	return true;
}

#ifdef WX_HAVE_SDBUS
#include <systemd/sd-bus.h>

static struct wx_server *srv;
static sd_bus *sys_bus, *user_bus;
static char *session_path;
static int sleep_fd = -1;
static bool sleeping;
static struct wl_event_source *sys_src, *user_src, *sleep_timer;

static void drain(sd_bus *bus) {
	while (sd_bus_process(bus, NULL) > 0) {
	}
}

static int bus_readable(int fd, uint32_t mask, void *data) {
	drain(data);
	return 0;
}

/* sync calls may have queued signals that no longer show up as readable */
static void drain_idle(void *data) {
	drain(data);
}

static void take_sleep_inhibitor(void) {
	if (sleep_fd >= 0 || !sys_bus || !srv->config.lock_on_sleep) {
		return;
	}
	sd_bus_error err = SD_BUS_ERROR_NULL;
	sd_bus_message *reply = NULL;
	int r = sd_bus_call_method(sys_bus, "org.freedesktop.login1", "/org/freedesktop/login1",
		"org.freedesktop.login1.Manager", "Inhibit", &err, &reply, "ssss", "sleep", "WaylandX",
		"Lock the screen before sleeping", "delay");
	int fd;
	if (r >= 0 && sd_bus_message_read(reply, "h", &fd) >= 0) {
		sleep_fd = fcntl(fd, F_DUPFD_CLOEXEC, 3);
	} else {
		wlr_log(WLR_ERROR, "logind: no sleep inhibitor: %s", err.message ? err.message : strerror(-r));
	}
	sd_bus_error_free(&err);
	sd_bus_message_unref(reply);
	wl_event_loop_add_idle(srv->loop, drain_idle, sys_bus);
}

static void release_sleep_inhibitor(void) {
	if (sleep_fd >= 0) {
		close(sleep_fd);
		sleep_fd = -1;
	}
	if (sleep_timer) {
		wl_event_source_timer_update(sleep_timer, 0);
	}
}

static int sleep_timeout(void *data) {
	wlr_log(WLR_ERROR, "logind: locker did not show up in time; letting the system sleep");
	release_sleep_inhibitor();
	return 0;
}

static int on_prepare_for_sleep(sd_bus_message *m, void *data, sd_bus_error *e) {
	int start = 0;
	if (sd_bus_message_read(m, "b", &start) < 0) {
		return 0;
	}
	sleeping = start;
	if (start) {
		wlr_log(WLR_INFO, "logind: going to sleep");
		if (!srv->config.lock_on_sleep || srv->lock_shown) {
			release_sleep_inhibitor();
		} else {
			idle_lock_now(srv);
			wl_event_source_timer_update(sleep_timer, 3000);
		}
	} else {
		wlr_log(WLR_INFO, "logind: resumed");
		idle_activity(srv); /* screens back on */
		take_sleep_inhibitor();
	}
	return 0;
}

static int on_session_lock(sd_bus_message *m, void *data, sd_bus_error *e) {
	wlr_log(WLR_INFO, "logind: lock requested (loginctl lock-session)");
	idle_lock_now(srv);
	return 0;
}

static char *find_session_path(void) {
	sd_bus_error err = SD_BUS_ERROR_NULL;
	sd_bus_message *reply = NULL;
	const char *id = getenv("XDG_SESSION_ID");
	int r;
	if (id && *id) {
		r = sd_bus_call_method(sys_bus, "org.freedesktop.login1", "/org/freedesktop/login1",
			"org.freedesktop.login1.Manager", "GetSession", &err, &reply, "s", id);
	} else {
		r = sd_bus_call_method(sys_bus, "org.freedesktop.login1", "/org/freedesktop/login1",
			"org.freedesktop.login1.Manager", "GetSessionByPID", &err, &reply, "u", (uint32_t)getpid());
	}
	char *path = NULL;
	const char *p;
	if (r >= 0 && sd_bus_message_read(reply, "o", &p) >= 0) {
		path = strdup(p);
	} else {
		wlr_log(WLR_ERROR, "logind: cannot find our session: %s",
			err.message ? err.message : strerror(-r));
	}
	sd_bus_error_free(&err);
	sd_bus_message_unref(reply);
	return path;
}

static struct wl_event_source *watch(sd_bus *bus) {
	return wl_event_loop_add_fd(srv->loop, sd_bus_get_fd(bus), WL_EVENT_READABLE, bus_readable, bus);
}

void logind_init(struct wx_server *server) {
	srv = server;
	if (sd_bus_open_user(&user_bus) >= 0) {
		user_src = watch(user_bus);
	} else {
		user_bus = NULL;
	}
	const char *force = getenv("WAYLANDX_LOGIND");
	if (!(server->session || (force && !strcmp(force, "1"))) || server->nested) {
		return;
	}
	if (sd_bus_open_system(&sys_bus) < 0) {
		wlr_log(WLR_ERROR, "logind: no system bus; sleep locking and brightness are off");
		sys_bus = NULL;
		return;
	}
	session_path = find_session_path();
	sd_bus_match_signal(sys_bus, NULL, "org.freedesktop.login1", "/org/freedesktop/login1",
		"org.freedesktop.login1.Manager", "PrepareForSleep", on_prepare_for_sleep, NULL);
	if (session_path) {
		sd_bus_match_signal(sys_bus, NULL, "org.freedesktop.login1", session_path,
			"org.freedesktop.login1.Session", "Lock", on_session_lock, NULL);
	}
	sleep_timer = wl_event_loop_add_timer(server->loop, sleep_timeout, NULL);
	sys_src = watch(sys_bus);
	take_sleep_inhibitor();
	drain(sys_bus);
	wlr_log(WLR_INFO, "logind: session %s", session_path ? session_path : "(unknown)");
}

void logind_finish(struct wx_server *server) {
	release_sleep_inhibitor();
	if (sleep_timer) {
		wl_event_source_remove(sleep_timer);
		sleep_timer = NULL;
	}
	if (sys_src) {
		wl_event_source_remove(sys_src);
		sys_src = NULL;
	}
	if (user_src) {
		wl_event_source_remove(user_src);
		user_src = NULL;
	}
	sys_bus = sd_bus_flush_close_unref(sys_bus);
	user_bus = sd_bus_flush_close_unref(user_bus);
	free(session_path);
	session_path = NULL;
}

/* Config reload: lock_on_sleep may have changed. */
void logind_apply_config(struct wx_server *server) {
	if (server->config.lock_on_sleep) {
		take_sleep_inhibitor();
	} else {
		release_sleep_inhibitor();
	}
}

/* The locker is on screen (or gone): tell logind, let a pending sleep go on. */
void logind_lock_changed(struct wx_server *server) {
	if (sleeping && server->lock_shown) {
		release_sleep_inhibitor();
	}
	if (!sys_bus || !session_path) {
		return;
	}
	sd_bus_error err = SD_BUS_ERROR_NULL;
	int r = sd_bus_call_method(sys_bus, "org.freedesktop.login1", session_path,
		"org.freedesktop.login1.Session", "SetLockedHint", &err, NULL, "b", (int)server->locked);
	if (r < 0) {
		wlr_log(WLR_DEBUG, "logind: SetLockedHint: %s", err.message ? err.message : strerror(-r));
	}
	sd_bus_error_free(&err);
	wl_event_loop_add_idle(server->loop, drain_idle, sys_bus);
}

static bool set_brightness_logind(const char *name, int value, char **err) {
	if (!sys_bus || !session_path) {
		return false;
	}
	sd_bus_error e = SD_BUS_ERROR_NULL;
	int r = sd_bus_call_method(sys_bus, "org.freedesktop.login1", session_path,
		"org.freedesktop.login1.Session", "SetBrightness", &e, NULL, "ssu", "backlight", name,
		(uint32_t)value);
	if (r < 0 && err) {
		*err = strdup(e.message ? e.message : strerror(-r));
	}
	sd_bus_error_free(&e);
	return r >= 0;
}

/* ---------- MPRIS ---------- */

static bool player_playing(const char *name) {
	char *status = NULL;
	if (sd_bus_get_property_string(user_bus, name, "/org/mpris/MediaPlayer2",
			"org.mpris.MediaPlayer2.Player", "PlaybackStatus", NULL, &status) < 0) {
		return false;
	}
	bool playing = !strcmp(status, "Playing");
	free(status);
	return playing;
}

bool logind_media(struct wx_server *server, const char *action, char **err) {
	static const char *actions[][2] = {{"play-pause", "PlayPause"}, {"play", "Play"},
		{"pause", "Pause"}, {"stop", "Stop"}, {"next", "Next"}, {"previous", "Previous"}};
	const char *method = NULL;
	for (size_t i = 0; i < sizeof(actions) / sizeof(actions[0]); i++) {
		if (!strcmp(action, actions[i][0])) {
			method = actions[i][1];
		}
	}
	if (!method) {
		*err = strdup("usage: media play-pause|play|pause|stop|next|previous");
		return false;
	}
	if (!user_bus) {
		*err = strdup("no user D-Bus session");
		return false;
	}
	char **names = NULL;
	if (sd_bus_list_names(user_bus, &names, NULL) < 0) {
		*err = strdup("cannot list D-Bus names");
		return false;
	}
	/* the player that is playing wins, else the first one */
	const char *target = NULL;
	for (char **n = names; n && *n; n++) {
		if (strncmp(*n, "org.mpris.MediaPlayer2.", 23)) {
			continue;
		}
		if (!target) {
			target = *n;
		}
		if (player_playing(*n)) {
			target = *n;
			break;
		}
	}
	bool ok = false;
	if (!target) {
		*err = strdup("no media player (MPRIS) running");
	} else {
		sd_bus_error e = SD_BUS_ERROR_NULL;
		int r = sd_bus_call_method(user_bus, target, "/org/mpris/MediaPlayer2",
			"org.mpris.MediaPlayer2.Player", method, &e, NULL, "");
		ok = r >= 0;
		if (!ok) {
			*err = strdup(e.message ? e.message : strerror(-r));
		}
		sd_bus_error_free(&e);
	}
	for (char **n = names; n && *n; n++) {
		free(*n);
	}
	free(names);
	wl_event_loop_add_idle(server->loop, drain_idle, user_bus);
	return ok;
}

#else /* no sd-bus: brightness through sysfs only, no sleep locking, no MPRIS */

void logind_init(struct wx_server *server) {
}

void logind_finish(struct wx_server *server) {
}

void logind_apply_config(struct wx_server *server) {
}

void logind_lock_changed(struct wx_server *server) {
}

static bool set_brightness_logind(const char *name, int value, char **err) {
	return false;
}

bool logind_media(struct wx_server *server, const char *action, char **err) {
	*err = strdup("built without sd-bus (libsystemd/libelogind)");
	return false;
}

#endif

/* Sets or reads the backlight; *percent gets the resulting level. */
bool logind_brightness(struct wx_server *server, const char *arg, int *percent, char **err) {
	char name[256];
	const char *dir = backlight_dir();
	if (!pick_backlight(name, sizeof(name))) {
		*err = strdup("no backlight device");
		return false;
	}
	int max = read_int_file(dir, name, "max_brightness");
	int cur = read_int_file(dir, name, "brightness");
	if (max <= 0 || cur < 0) {
		*err = strdup("cannot read the backlight level");
		return false;
	}
	int target;
	if (!brightness_target(arg, cur, max, &target)) {
		*err = strdup("usage: brightness [+N%|-N%|N%]");
		return false;
	}
	if (target != cur) {
		char *lerr = NULL;
		if (!set_brightness_logind(name, target, &lerr)) {
			/* no logind: works when a udev rule lets the user write it */
			char path[512];
			snprintf(path, sizeof(path), "%s/%s/brightness", dir, name);
			FILE *f = fopen(path, "w");
			if (!f || fprintf(f, "%d\n", target) < 0) {
				*err = lerr ? lerr : strdup("no permission to change the backlight");
				if (f) {
					fclose(f);
				}
				return false;
			}
			fclose(f);
			free(lerr);
		}
	}
	*percent = (int)((target * 100.0) / max + 0.5);
	return true;
}
