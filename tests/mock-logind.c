/*
 * Stand-in for logind and an MPRIS player on a private bus, so the tests can
 * exercise sleep locking, lock-session, brightness and media keys without
 * touching the real session. Commands on stdin: sleep | resume | lock.
 * Everything the compositor does is printed on stdout.
 *   mock-logind BACKLIGHT_DIR
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <systemd/sd-bus.h>

#define SESSION "/org/freedesktop/login1/session/test"

static sd_bus *bus;
static const char *backlight;
static int inhibit_rd = -1;

static int m_get_session(sd_bus_message *m, void *data, sd_bus_error *e) {
	return sd_bus_reply_method_return(m, "o", SESSION);
}

static int m_inhibit(sd_bus_message *m, void *data, sd_bus_error *e) {
	const char *what, *who, *why, *mode;
	sd_bus_message_read(m, "ssss", &what, &who, &why, &mode);
	int p[2];
	if (pipe2(p, O_CLOEXEC) < 0) {
		return -errno;
	}
	if (inhibit_rd >= 0) {
		close(inhibit_rd);
	}
	inhibit_rd = p[0];
	printf("inhibit %s %s %s\n", what, who, mode);
	int r = sd_bus_reply_method_return(m, "h", p[1]);
	close(p[1]);
	return r;
}

static int m_locked_hint(sd_bus_message *m, void *data, sd_bus_error *e) {
	int b;
	sd_bus_message_read(m, "b", &b);
	printf("lockedhint %d\n", b);
	return sd_bus_reply_method_return(m, "");
}

static int m_brightness(sd_bus_message *m, void *data, sd_bus_error *e) {
	const char *sub, *name;
	uint32_t v;
	sd_bus_message_read(m, "ssu", &sub, &name, &v);
	printf("brightness %s %s %u\n", sub, name, v);
	char path[512];
	snprintf(path, sizeof(path), "%s/%s/brightness", backlight, name);
	FILE *f = fopen(path, "w");
	if (f) {
		fprintf(f, "%u\n", v);
		fclose(f);
	}
	return sd_bus_reply_method_return(m, "");
}

static int m_player(sd_bus_message *m, void *data, sd_bus_error *e) {
	printf("mpris %s\n", sd_bus_message_get_member(m));
	return sd_bus_reply_method_return(m, "");
}

static int p_status(sd_bus *b, const char *path, const char *iface, const char *prop,
		sd_bus_message *reply, void *data, sd_bus_error *e) {
	return sd_bus_message_append(reply, "s", "Playing");
}

static const sd_bus_vtable manager_vt[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("GetSession", "s", "o", m_get_session, 0),
	SD_BUS_METHOD("GetSessionByPID", "u", "o", m_get_session, 0),
	SD_BUS_METHOD("Inhibit", "ssss", "h", m_inhibit, 0),
	SD_BUS_SIGNAL("PrepareForSleep", "b", 0),
	SD_BUS_VTABLE_END,
};

static const sd_bus_vtable session_vt[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("SetLockedHint", "b", "", m_locked_hint, 0),
	SD_BUS_METHOD("SetBrightness", "ssu", "", m_brightness, 0),
	SD_BUS_SIGNAL("Lock", "", 0),
	SD_BUS_VTABLE_END,
};

static const sd_bus_vtable player_vt[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("PlayPause", "", "", m_player, 0),
	SD_BUS_METHOD("Play", "", "", m_player, 0),
	SD_BUS_METHOD("Pause", "", "", m_player, 0),
	SD_BUS_METHOD("Stop", "", "", m_player, 0),
	SD_BUS_METHOD("Next", "", "", m_player, 0),
	SD_BUS_METHOD("Previous", "", "", m_player, 0),
	SD_BUS_PROPERTY("PlaybackStatus", "s", p_status, 0, 0),
	SD_BUS_VTABLE_END,
};

int main(int argc, char *argv[]) {
	if (argc < 2) {
		fprintf(stderr, "usage: mock-logind BACKLIGHT_DIR\n");
		return 1;
	}
	backlight = argv[1];
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (sd_bus_open_system(&bus) < 0) {
		fprintf(stderr, "no bus\n");
		return 1;
	}
	sd_bus_add_object_vtable(bus, NULL, "/org/freedesktop/login1", "org.freedesktop.login1.Manager",
		manager_vt, NULL);
	sd_bus_add_object_vtable(bus, NULL, SESSION, "org.freedesktop.login1.Session", session_vt, NULL);
	sd_bus_add_object_vtable(bus, NULL, "/org/mpris/MediaPlayer2", "org.mpris.MediaPlayer2.Player",
		player_vt, NULL);
	if (sd_bus_request_name(bus, "org.freedesktop.login1", 0) < 0 ||
			sd_bus_request_name(bus, "org.mpris.MediaPlayer2.mock", 0) < 0) {
		fprintf(stderr, "cannot own names\n");
		return 1;
	}
	printf("ready\n");
	for (;;) {
		while (sd_bus_process(bus, NULL) > 0) {
		}
		struct pollfd fds[3] = {
			{.fd = sd_bus_get_fd(bus), .events = (short)sd_bus_get_events(bus)},
			{.fd = 0, .events = POLLIN},
			{.fd = inhibit_rd, .events = POLLIN},
		};
		if (poll(fds, 3, -1) < 0 && errno != EINTR) {
			return 1;
		}
		if (fds[2].revents) {
			printf("inhibitor released\n");
			close(inhibit_rd);
			inhibit_rd = -1;
		}
		if (fds[1].revents & POLLIN) {
			char line[64];
			if (!fgets(line, sizeof(line), stdin)) {
				return 0;
			}
			if (!strncmp(line, "sleep", 5) || !strncmp(line, "resume", 6)) {
				sd_bus_emit_signal(bus, "/org/freedesktop/login1", "org.freedesktop.login1.Manager",
					"PrepareForSleep", "b", line[0] == 's');
			} else if (!strncmp(line, "lock", 4)) {
				sd_bus_emit_signal(bus, SESSION, "org.freedesktop.login1.Session", "Lock", "");
			}
			sd_bus_flush(bus);
		} else if (fds[1].revents & POLLHUP) {
			return 0;
		}
	}
}
