#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wayland-server-core.h>
#include <wlr/render/color.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_gamma_control_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>
#include "server.h"

/*
 * Night light (KDE's Night Color, GNOME's Night Light) in the compositor:
 * warmer colors on a schedule with a 30 minute fade. Real monitors get it in
 * their hardware gamma ramps, like KWin does; outputs without ramps get it
 * from the renderer (Vulkan). A client that sets gamma itself (gammastep,
 * wlsunset) wins on that output, and ours comes back when it leaves.
 */

#define DAY_K 6500
#define FADE_MIN 30

static struct wl_event_source *timer;
static int forced = -1; /* -1 schedule, 0 off, 1 on (IPC) */
static int applied_k;

/* Kelvin -> RGB multipliers (Tanner Helland's fit of the Planckian locus). */
static void kelvin_rgb(int k, double rgb[3]) {
	double t = k / 100.0, r, g, b;
	if (t <= 66) {
		r = 255;
		g = 99.4708025861 * log(t) - 161.1195681661;
		b = t <= 19 ? 0 : 138.5177312231 * log(t - 10) - 305.0447927307;
	} else {
		r = 329.698727446 * pow(t - 60, -0.1332047592);
		g = 288.1221695283 * pow(t - 60, -0.0755148492);
		b = 255;
	}
	double v[3] = {r, g, b};
	for (int i = 0; i < 3; i++) {
		rgb[i] = v[i] < 0 ? 0 : v[i] > 255 ? 1 : v[i] / 255.0;
	}
}

/* "HH:MM-HH:MM" -> minutes of the day; false when not a schedule */
static bool parse_range(const char *s, int *from, int *to) {
	int h1, m1, h2, m2;
	if (!s || sscanf(s, "%d:%d-%d:%d", &h1, &m1, &h2, &m2) != 4 || h1 < 0 || h1 > 23 || h2 < 0 ||
			h2 > 23 || m1 < 0 || m1 > 59 || m2 < 0 || m2 > 59) {
		return false;
	}
	*from = h1 * 60 + m1;
	*to = h2 * 60 + m2;
	return true;
}

bool nightlight_valid_schedule(const char *s) {
	int a, b;
	return !strcmp(s, "off") || !strcmp(s, "on") || parse_range(s, &a, &b);
}

/* Minutes from a to b going forward around the clock. */
static int ahead(int a, int b) {
	return ((b - a) % 1440 + 1440) % 1440;
}

/* Target temperature for this minute of the day. */
int nightlight_target(const char *schedule, int night_k, int minute) {
	if (forced == 0 || !schedule || !strcmp(schedule, "off")) {
		return forced == 1 ? night_k : DAY_K;
	}
	if (forced == 1 || !strcmp(schedule, "on")) {
		return night_k;
	}
	int from, to;
	if (!parse_range(schedule, &from, &to)) {
		return DAY_K;
	}
	int night_len = ahead(from, to);
	int into = ahead(from, minute);
	if (into >= night_len) {
		return DAY_K; /* daytime */
	}
	double f = 1.0;
	if (into < FADE_MIN) {
		f = (double)into / FADE_MIN; /* evening fade in */
	} else if (night_len - into < FADE_MIN) {
		f = (double)(night_len - into) / FADE_MIN; /* morning fade out */
	}
	return (int)lround(DAY_K + (night_k - DAY_K) * f);
}

static struct wlr_color_transform *lut;    /* hardware gamma ramps, NULL by day */
static struct wlr_color_transform *render;  /* renderer fallback, NULL by day */
static struct wl_listener set_gamma;
static struct wl_event_source *reapply_timer;

/*
 * For outputs without hardware gamma (nested, some drivers) the scene renders
 * the shift itself when the renderer can (Vulkan). It replaces the scene's
 * linear -> sRGB step, so it includes that step.
 */
struct wlr_color_transform *nightlight_transform(struct wlr_output *output) {
	if (!render || wlr_output_get_gamma_size(output) > 0 ||
			!output->renderer || !output->renderer->features.output_color_transform) {
		return NULL;
	}
	return render;
}

static void set_transforms(int k) {
	wlr_color_transform_unref(lut);
	wlr_color_transform_unref(render);
	lut = render = NULL;
	if (k >= DAY_K) {
		return;
	}
	double rgb[3];
	kelvin_rgb(k, rgb);
	enum { N = 256 };
	uint16_t ramp[3][N];
	for (int c = 0; c < 3; c++) {
		for (int i = 0; i < N; i++) {
			ramp[c][i] = (uint16_t)lround(i / (N - 1.0) * 65535.0 * rgb[c]);
		}
	}
	lut = wlr_color_transform_init_lut_3x1d(N, ramp[0], ramp[1], ramp[2]);
	/* ramps act on encoded values; in linear light that is the factor ^2.2 */
	float m[9] = {(float)pow(rgb[0], 2.2), 0, 0, 0, (float)pow(rgb[1], 2.2), 0, 0, 0,
		(float)pow(rgb[2], 2.2)};
	struct wlr_color_transform *parts[2] = {
		wlr_color_transform_init_matrix(m),
		wlr_color_transform_init_linear_to_inverse_eotf(WLR_COLOR_TRANSFER_FUNCTION_GAMMA22),
	};
	if (parts[0] && parts[1]) {
		render = wlr_color_transform_init_pipeline(parts, 2);
	}
	wlr_color_transform_unref(parts[0]);
	wlr_color_transform_unref(parts[1]);
}

static void apply_output(struct wx_server *server, struct wx_output *o) {
	struct wlr_output *out = o->wlr_output;
	if (!out->enabled || o->dpms_off) {
		return;
	}
	if (wlr_output_get_gamma_size(out) > 0) {
		if (server->gamma_mgr && wlr_gamma_control_manager_v1_get_control(server->gamma_mgr, out)) {
			return; /* gammastep & co. own this output's ramps */
		}
		struct wlr_output_state state;
		wlr_output_state_init(&state);
		wlr_output_state_set_color_transform(&state, lut);
		if (!wlr_output_commit_state(out, &state)) {
			wlr_log(WLR_ERROR, "output %s: cannot set gamma ramps", out->name);
		}
		wlr_output_state_finish(&state);
	} else if (o->scene_output) {
		if (render && !nightlight_transform(out)) {
			wlr_log(WLR_DEBUG, "output %s: no gamma ramps and the renderer cannot apply color "
				"transforms (use WLR_RENDERER=vulkan); night light is off there", out->name);
		}
		/* renderer path: the scene redraws everything with the new transform */
		pixman_region32_t all;
		pixman_region32_init_rect(&all, 0, 0, (unsigned)out->width, (unsigned)out->height);
		struct wlr_output_event_damage ev = {.output = out, .damage = &all};
		wl_signal_emit_mutable(&out->events.damage, &ev);
		pixman_region32_fini(&all);
	}
}

/* After a gamma client leaves, the scene resets the ramps: put ours back. */
static int reapply(void *data) {
	nightlight_reapply(data);
	return 0;
}

static void handle_set_gamma(struct wl_listener *listener, void *data) {
	const struct wlr_gamma_control_manager_v1_set_gamma_event *ev = data;
	if (!ev->control && reapply_timer) {
		wl_event_source_timer_update(reapply_timer, 200); /* after the scene's next frame */
	}
}

void nightlight_reapply(struct wx_server *server) {
	struct wx_output *o;
	wl_list_for_each(o, &server->outputs, link) {
		apply_output(server, o);
	}
}

static int current_minute(void) {
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	return tm.tm_hour * 60 + tm.tm_min;
}

/* Re-applies everywhere; also call after outputs change or wake up. */
void nightlight_update(struct wx_server *server) {
	int k = nightlight_target(server->config.night_light, server->config.night_temp,
		current_minute());
	if (k != applied_k) {
		wlr_log(WLR_INFO, "night light: %dK", k);
		char ev[80];
		snprintf(ev, sizeof(ev), "{\"event\":\"night_light\",\"temperature\":%d}", k);
		ipc_broadcast_event(server, ev);
	}
	if (k != applied_k) {
		applied_k = k;
		set_transforms(k);
		nightlight_reapply(server);
	}
}

int nightlight_current(void) {
	return applied_k ? applied_k : DAY_K;
}

static int tick(void *data) {
	nightlight_update(data);
	wl_event_source_timer_update(timer, 60 * 1000);
	return 0;
}

/* IPC: on|off force it, auto follows the schedule again. */
bool nightlight_set_mode(struct wx_server *server, const char *mode) {
	if (!strcmp(mode, "on")) {
		forced = 1;
	} else if (!strcmp(mode, "off")) {
		forced = 0;
	} else if (!strcmp(mode, "auto")) {
		forced = -1;
	} else {
		return false;
	}
	nightlight_update(server);
	return true;
}

void nightlight_init(struct wx_server *server) {
	applied_k = DAY_K;
	timer = wl_event_loop_add_timer(server->loop, tick, server);
	reapply_timer = wl_event_loop_add_timer(server->loop, reapply, server);
	if (server->gamma_mgr) {
		set_gamma.notify = handle_set_gamma;
		wl_signal_add(&server->gamma_mgr->events.set_gamma, &set_gamma);
	}
	wl_event_source_timer_update(timer, 1000); /* after outputs are up */
}

void nightlight_finish(struct wx_server *server) {
	set_transforms(DAY_K);
	if (set_gamma.link.next) {
		wl_list_remove(&set_gamma.link);
		set_gamma.link.next = NULL;
	}
	if (reapply_timer) {
		wl_event_source_remove(reapply_timer);
		reapply_timer = NULL;
	}
	if (timer) {
		wl_event_source_remove(timer);
		timer = NULL;
	}
}
