#include <stdlib.h>
#include <time.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/log.h>
#include "server.h"

/*
 * Built-in idle handling, what swayidle does on other compositors and
 * PowerDevil does on KDE: lock the session after idle_lock seconds and turn
 * the screens off after idle_dpms seconds. Visible idle inhibitors (video
 * players, games, presentations) pause both. Any input, including input
 * injected over IPC (like XTest on X11), counts as activity and wakes the
 * screens. Nested sessions leave this to the host desktop.
 */

static struct wl_event_source *lock_timer, *dpms_timer;
static bool inhibited;

static bool active(struct wx_server *server) {
	return !server->nested && !inhibited;
}

static void arm(struct wx_server *server) {
	if (!lock_timer) {
		return;
	}
	struct wx_config *c = &server->config;
	bool on = active(server);
	wl_event_source_timer_update(lock_timer, on && c->idle_lock > 0 && !server->lock ?
		c->idle_lock * 1000 : 0);
	wl_event_source_timer_update(dpms_timer, on && c->idle_dpms > 0 ? c->idle_dpms * 1000 : 0);
}

/* Starts the locker unless one is running. Also how a crashed locker comes back. */
void idle_lock_now(struct wx_server *server) {
	const char *cmd = server->config.lock_command;
	if (server->lock || !cmd || !*cmd) {
		return;
	}
	static struct timespec last;
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	if (last.tv_sec && now.tv_sec - last.tv_sec < 3) {
		return; /* still starting */
	}
	last = now;
	wlr_log(WLR_INFO, "locking the session: %s", cmd);
	server_spawn(server, cmd);
}

static void output_power(struct wx_output *o, bool on) {
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, on);
	if (!wlr_output_commit_state(o->wlr_output, &state)) {
		wlr_log(WLR_ERROR, "output %s: cannot turn %s", o->wlr_output->name, on ? "on" : "off");
	}
	wlr_output_state_finish(&state);
}

void idle_set_screens(struct wx_server *server, bool on) {
	struct wx_output *o;
	wl_list_for_each(o, &server->outputs, link) {
		if (o->lid_disabled) {
			continue;
		}
		if (!on && o->wlr_output->enabled && !o->dpms_off) {
			output_power(o, false);
			o->dpms_off = true;
		} else if (on && o->dpms_off) {
			o->dpms_off = false;
			output_power(o, true);
		}
	}
	server->screens_off = !on;
	if (on) {
		nightlight_reapply(server); /* a powered-off output may come back with a fresh ramp */
	}
	ipc_broadcast_event(server, on ? "{\"event\":\"screens\",\"on\":true}" :
		"{\"event\":\"screens\",\"on\":false}");
}

static int on_lock_timer(void *data) {
	idle_lock_now(data);
	return 0;
}

static int on_dpms_timer(void *data) {
	struct wx_server *server = data;
	wlr_log(WLR_INFO, "idle: turning screens off");
	idle_set_screens(server, false);
	return 0;
}

void idle_activity(struct wx_server *server) {
	if (server->idle_notifier) {
		wlr_idle_notifier_v1_notify_activity(server->idle_notifier, server->seat);
	}
	if (server->screens_off) {
		idle_set_screens(server, true);
	}
	arm(server);
}

void idle_set_inhibited(struct wx_server *server, bool on) {
	if (inhibited != on) {
		inhibited = on;
		wlr_log(WLR_DEBUG, "idle %s", on ? "inhibited" : "allowed");
		arm(server);
	}
}

/* Config reload and lock state changes re-arm the timers. */
void idle_rearm(struct wx_server *server) {
	arm(server);
}

void idle_init(struct wx_server *server) {
	lock_timer = wl_event_loop_add_timer(server->loop, on_lock_timer, server);
	dpms_timer = wl_event_loop_add_timer(server->loop, on_dpms_timer, server);
	arm(server);
}

void idle_finish(struct wx_server *server) {
	if (lock_timer) {
		wl_event_source_remove(lock_timer);
		wl_event_source_remove(dpms_timer);
		lock_timer = dpms_timer = NULL;
	}
}
