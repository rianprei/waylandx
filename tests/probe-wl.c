#define _GNU_SOURCE
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include "text-input-unstable-v3-client-protocol.h"
#include "waylandx-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

/* Minimal xdg toplevel that logs every input/configure event to stdout. */

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *wm_base;
static struct wl_seat *seat;
static struct wx_manager_v1 *wx_manager;
static struct zwp_text_input_manager_v3 *ti_manager;
static struct zwp_text_input_v3 *text_input;
static bool want_text_input;
static struct wl_surface *surface;
static struct xdg_toplevel *toplevel;
static struct xkb_context *xkb_ctx;
static struct xkb_keymap *keymap;
static struct xkb_state *xkb_state;
static int width = 300, height = 200;
static uint32_t color = 0xff2a7fff;
static bool running = true, configured;

static struct wl_buffer *make_buffer(int w, int h) {
	int stride = w * 4, size = stride * h;
	int fd = memfd_create("probe", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, size) < 0) {
		perror("memfd");
		exit(1);
	}
	uint32_t *px = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	for (int i = 0; i < w * h; i++) {
		px[i] = color;
	}
	munmap(px, (size_t)size);
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
	struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	return buf;
}

static void xdg_surface_configure(void *data, struct xdg_surface *xs, uint32_t serial) {
	xdg_surface_ack_configure(xs, serial);
	struct wl_buffer *buf = make_buffer(width, height);
	wl_surface_attach(surface, buf, 0, 0);
	wl_surface_damage_buffer(surface, 0, 0, width, height);
	wl_surface_commit(surface);
	configured = true;
}

static const struct xdg_surface_listener xdg_surface_listener = {.configure = xdg_surface_configure};

static void toplevel_configure(void *data, struct xdg_toplevel *t, int32_t w, int32_t h,
		struct wl_array *states) {
	if (w > 0 && h > 0) {
		width = w;
		height = h;
	}
	int maximized = 0, fullscreen = 0, activated = 0;
	uint32_t *s;
	wl_array_for_each(s, states) {
		maximized |= *s == XDG_TOPLEVEL_STATE_MAXIMIZED;
		fullscreen |= *s == XDG_TOPLEVEL_STATE_FULLSCREEN;
		activated |= *s == XDG_TOPLEVEL_STATE_ACTIVATED;
	}
	printf("configure %d %d max=%d fs=%d active=%d\n", width, height, maximized, fullscreen, activated);
}

static void toplevel_close(void *data, struct xdg_toplevel *t) {
	printf("close\n");
	running = false;
}

static void toplevel_bounds(void *data, struct xdg_toplevel *t, int32_t w, int32_t h) {
}

static void toplevel_caps(void *data, struct xdg_toplevel *t, struct wl_array *caps) {
}

static const struct xdg_toplevel_listener toplevel_listener = {
	.configure = toplevel_configure,
	.close = toplevel_close,
	.configure_bounds = toplevel_bounds,
	.wm_capabilities = toplevel_caps,
};

static void wm_ping(void *data, struct xdg_wm_base *b, uint32_t serial) {
	xdg_wm_base_pong(b, serial);
}

static const struct xdg_wm_base_listener wm_listener = {.ping = wm_ping};

static void kb_keymap(void *data, struct wl_keyboard *kb, uint32_t format, int32_t fd, uint32_t size) {
	char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (map == MAP_FAILED) {
		return;
	}
	xkb_state_unref(xkb_state);
	xkb_keymap_unref(keymap);
	keymap = xkb_keymap_new_from_string(xkb_ctx, map, XKB_KEYMAP_FORMAT_TEXT_V1,
		XKB_KEYMAP_COMPILE_NO_FLAGS);
	munmap(map, size);
	xkb_state = keymap ? xkb_state_new(keymap) : NULL;
	printf("keymap\n");
}

static void kb_enter(void *data, struct wl_keyboard *kb, uint32_t serial, struct wl_surface *s,
		struct wl_array *keys) {
	printf("kb_enter\n");
}

static void kb_leave(void *data, struct wl_keyboard *kb, uint32_t serial, struct wl_surface *s) {
	printf("kb_leave\n");
}

static void kb_key(void *data, struct wl_keyboard *kb, uint32_t serial, uint32_t time, uint32_t key,
		uint32_t state) {
	if (!xkb_state) {
		return;
	}
	xkb_keysym_t sym = xkb_state_key_get_one_sym(xkb_state, key + 8);
	char name[64], utf8[16] = "";
	xkb_keysym_get_name(sym, name, sizeof(name));
	xkb_state_key_get_utf8(xkb_state, key + 8, utf8, sizeof(utf8));
	int ctrl = xkb_state_mod_name_is_active(xkb_state, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0;
	int shift = xkb_state_mod_name_is_active(xkb_state, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE) > 0;
	int alt = xkb_state_mod_name_is_active(xkb_state, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE) > 0;
	printf("key %s %s utf8=%s ctrl=%d shift=%d alt=%d\n", name,
		state == WL_KEYBOARD_KEY_STATE_PRESSED ? "down" : "up", utf8, ctrl, shift, alt);
}

static void kb_mods(void *data, struct wl_keyboard *kb, uint32_t serial, uint32_t dep, uint32_t lat,
		uint32_t lock, uint32_t group) {
	if (xkb_state) {
		xkb_state_update_mask(xkb_state, dep, lat, lock, 0, 0, group);
	}
}

static void kb_repeat(void *data, struct wl_keyboard *kb, int32_t rate, int32_t delay) {
}

static const struct wl_keyboard_listener kb_listener = {
	.keymap = kb_keymap, .enter = kb_enter, .leave = kb_leave, .key = kb_key,
	.modifiers = kb_mods, .repeat_info = kb_repeat,
};

static void ptr_enter(void *data, struct wl_pointer *p, uint32_t serial, struct wl_surface *s,
		wl_fixed_t x, wl_fixed_t y) {
	printf("ptr_enter %d %d\n", wl_fixed_to_int(x), wl_fixed_to_int(y));
}

static void ptr_leave(void *data, struct wl_pointer *p, uint32_t serial, struct wl_surface *s) {
	printf("ptr_leave\n");
}

static void ptr_motion(void *data, struct wl_pointer *p, uint32_t time, wl_fixed_t x, wl_fixed_t y) {
	printf("motion %d %d\n", wl_fixed_to_int(x), wl_fixed_to_int(y));
}

static void ptr_button(void *data, struct wl_pointer *p, uint32_t serial, uint32_t time,
		uint32_t button, uint32_t state) {
	printf("button %u %s\n", button, state == WL_POINTER_BUTTON_STATE_PRESSED ? "down" : "up");
}

static void ptr_axis(void *data, struct wl_pointer *p, uint32_t time, uint32_t axis, wl_fixed_t v) {
	printf("axis %u %.1f\n", axis, wl_fixed_to_double(v));
}

static void ptr_frame(void *data, struct wl_pointer *p) {
}

static void ptr_axis_source(void *data, struct wl_pointer *p, uint32_t src) {
}

static void ptr_axis_stop(void *data, struct wl_pointer *p, uint32_t time, uint32_t axis) {
}

static void ptr_axis_discrete(void *data, struct wl_pointer *p, uint32_t axis, int32_t d) {
}

static void ptr_axis_value120(void *data, struct wl_pointer *p, uint32_t axis, int32_t v) {
}

static void ptr_axis_rel(void *data, struct wl_pointer *p, uint32_t axis, uint32_t dir) {
}

static const struct wl_pointer_listener ptr_listener = {
	.enter = ptr_enter, .leave = ptr_leave, .motion = ptr_motion, .button = ptr_button,
	.axis = ptr_axis, .frame = ptr_frame, .axis_source = ptr_axis_source,
	.axis_stop = ptr_axis_stop, .axis_discrete = ptr_axis_discrete,
	.axis_value120 = ptr_axis_value120, .axis_relative_direction = ptr_axis_rel,
};

static struct wl_keyboard *keyboard;
static struct wl_pointer *pointer;

static void seat_caps(void *data, struct wl_seat *s, uint32_t caps) {
	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !keyboard) {
		keyboard = wl_seat_get_keyboard(s);
		wl_keyboard_add_listener(keyboard, &kb_listener, NULL);
	}
	if ((caps & WL_SEAT_CAPABILITY_POINTER) && !pointer) {
		pointer = wl_seat_get_pointer(s);
		wl_pointer_add_listener(pointer, &ptr_listener, NULL);
	}
}

static void seat_name(void *data, struct wl_seat *s, const char *name) {
}

static const struct wl_seat_listener seat_listener = {.capabilities = seat_caps, .name = seat_name};

static void ti_enter(void *data, struct zwp_text_input_v3 *ti, struct wl_surface *s) {
	printf("ti_enter\n");
	zwp_text_input_v3_enable(ti);
	zwp_text_input_v3_set_content_type(ti, ZWP_TEXT_INPUT_V3_CONTENT_HINT_NONE,
		ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NORMAL);
	zwp_text_input_v3_set_cursor_rectangle(ti, 10, 10, 1, 20);
	zwp_text_input_v3_commit(ti);
}

static void ti_leave(void *data, struct zwp_text_input_v3 *ti, struct wl_surface *s) {
	printf("ti_leave\n");
}

static void ti_preedit(void *data, struct zwp_text_input_v3 *ti, const char *text, int32_t b,
		int32_t e) {
	printf("ti_preedit %s\n", text ? text : "(null)");
}

static void ti_commit_string(void *data, struct zwp_text_input_v3 *ti, const char *text) {
	printf("ti_commit %s\n", text ? text : "(null)");
}

static void ti_delete(void *data, struct zwp_text_input_v3 *ti, uint32_t before, uint32_t after) {
}

static void ti_done(void *data, struct zwp_text_input_v3 *ti, uint32_t serial) {
	printf("ti_done\n");
}

static const struct zwp_text_input_v3_listener ti_listener = {
	.enter = ti_enter, .leave = ti_leave, .preedit_string = ti_preedit,
	.commit_string = ti_commit_string, .delete_surrounding_text = ti_delete, .done = ti_done,
};

static void control_position(void *data, struct wx_toplevel_control_v1 *c, int32_t x, int32_t y) {
	printf("wx_position %d %d\n", x, y);
}

static const struct wx_toplevel_control_v1_listener control_listener = {
	.position = control_position,
};

static void reg_global(void *data, struct wl_registry *r, uint32_t name, const char *iface,
		uint32_t version) {
	if (!strcmp(iface, wl_compositor_interface.name)) {
		compositor = wl_registry_bind(r, name, &wl_compositor_interface, 4);
	} else if (!strcmp(iface, wl_shm_interface.name)) {
		shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
	} else if (!strcmp(iface, xdg_wm_base_interface.name)) {
		wm_base = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
		xdg_wm_base_add_listener(wm_base, &wm_listener, NULL);
	} else if (!strcmp(iface, wl_seat_interface.name)) {
		seat = wl_registry_bind(r, name, &wl_seat_interface, version < 9 ? version : 9);
		wl_seat_add_listener(seat, &seat_listener, NULL);
	} else if (!strcmp(iface, zwp_text_input_manager_v3_interface.name)) {
		ti_manager = wl_registry_bind(r, name, &zwp_text_input_manager_v3_interface, 1);
	} else if (!strcmp(iface, wx_manager_v1_interface.name)) {
		wx_manager = wl_registry_bind(r, name, &wx_manager_v1_interface, 1);
	}
}

static void reg_remove(void *data, struct wl_registry *r, uint32_t name) {
}

static const struct wl_registry_listener reg_listener = {.global = reg_global,
	.global_remove = reg_remove};

int main(int argc, char *argv[]) {
	const char *title = "probe-wl";
	bool want_pos = false, want_above = false;
	int px = 0, py = 0;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--title") && i + 1 < argc) {
			title = argv[++i];
		} else if (!strcmp(argv[i], "--size") && i + 2 < argc) {
			width = atoi(argv[++i]);
			height = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--pos") && i + 2 < argc) {
			want_pos = true;
			px = atoi(argv[++i]);
			py = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--text-input")) {
			want_text_input = true;
		} else if (!strcmp(argv[i], "--above")) {
			want_above = true;
		} else if (!strcmp(argv[i], "--color") && i + 1 < argc) {
			color = (uint32_t)strtoul(argv[++i], NULL, 16);
		}
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct wl_display *display = wl_display_connect(NULL);
	if (!display) {
		fprintf(stderr, "cannot connect to wayland display\n");
		return 1;
	}
	struct wl_registry *reg = wl_display_get_registry(display);
	wl_registry_add_listener(reg, &reg_listener, NULL);
	wl_display_roundtrip(display);
	if (!compositor || !shm || !wm_base) {
		fprintf(stderr, "missing globals\n");
		return 1;
	}
	printf("wx_manager %s\n", wx_manager ? "yes" : "no");
	if (want_text_input && ti_manager && seat) {
		text_input = zwp_text_input_manager_v3_get_text_input(ti_manager, seat);
		zwp_text_input_v3_add_listener(text_input, &ti_listener, NULL);
	}
	surface = wl_compositor_create_surface(compositor);
	struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(wm_base, surface);
	xdg_surface_add_listener(xs, &xdg_surface_listener, NULL);
	toplevel = xdg_surface_get_toplevel(xs);
	xdg_toplevel_add_listener(toplevel, &toplevel_listener, NULL);
	xdg_toplevel_set_title(toplevel, title);
	xdg_toplevel_set_app_id(toplevel, "probe");
	if (wx_manager && (want_pos || want_above)) {
		struct wx_toplevel_control_v1 *ctl = wx_manager_v1_get_toplevel_control(wx_manager, toplevel);
		wx_toplevel_control_v1_add_listener(ctl, &control_listener, NULL);
		if (want_pos) {
			wx_toplevel_control_v1_set_position(ctl, px, py);
		}
		if (want_above) {
			wx_toplevel_control_v1_set_layer(ctl, WX_TOPLEVEL_CONTROL_V1_LAYER_ABOVE);
		}
	}
	wl_surface_commit(surface);
	while (running && wl_display_dispatch(display) != -1) {
	}
	return 0;
}
