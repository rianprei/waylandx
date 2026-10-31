#include <libinput.h>
#include <math.h>
#include <linux/input-event-codes.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wlr/backend/libinput.h>
#include <wlr/backend/session.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_pointer_gestures_v1.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_switch.h>
#include <wlr/types/wlr_tablet_pad.h>
#include <fnmatch.h>
#include <wlr/types/wlr_tablet_tool.h>
#include <wlr/types/wlr_tablet_v2.h>
#include <wlr/types/wlr_touch.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/edges.h>
#include <wlr/util/log.h>
#include <wlr/xwayland.h>
#include "server.h"

#define MIN_W 60
#define MIN_H 40

uint32_t input_now(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void notify_activity(struct wx_server *server) {
	idle_activity(server);
}

/* ---------- keyboard ---------- */

static struct xkb_keymap *config_keymap(struct wx_server *server) {
	struct wx_config *c = &server->config;
	struct xkb_rule_names names = {
		.rules = c->kb_rules, .model = c->kb_model, .layout = c->kb_layout,
		.variant = c->kb_variant, .options = c->kb_options,
	};
	struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_keymap *keymap = xkb_keymap_new_from_names(ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (!keymap) {
		wlr_log(WLR_ERROR, "invalid xkb settings (layout=%s variant=%s options=%s), using defaults",
			c->kb_layout ? c->kb_layout : "", c->kb_variant ? c->kb_variant : "",
			c->kb_options ? c->kb_options : "");
		keymap = xkb_keymap_new_from_names(ctx, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
	}
	xkb_context_unref(ctx);
	return keymap;
}

static void keyboard_configure(struct wx_server *server, struct wx_keyboard *kb) {
	if (kb->is_virtual) {
		return; /* virtual keyboards bring their own keymap */
	}
	struct xkb_keymap *keymap = config_keymap(server);
	if (keymap) {
		wlr_keyboard_set_keymap(kb->wlr_keyboard, keymap);
		xkb_keymap_unref(keymap);
	}
	wlr_keyboard_set_repeat_info(kb->wlr_keyboard, server->config.repeat_rate,
		server->config.repeat_delay);
}

static void keyboard_modifiers(struct wl_listener *listener, void *data) {
	struct wx_keyboard *kb = wl_container_of(listener, kb, modifiers);
	switcher_modifiers(kb->server, wlr_keyboard_get_modifiers(kb->wlr_keyboard));
	if (ime_handle_modifiers(kb->server, kb)) {
		return;
	}
	wlr_seat_set_keyboard(kb->server->seat, kb->wlr_keyboard);
	wlr_seat_keyboard_notify_modifiers(kb->server->seat, &kb->wlr_keyboard->modifiers);
}

/* Keys whose press was eaten by a binding; their release is eaten too. */
static uint32_t consumed_keys[32];

static bool consumed_take(uint32_t keycode) {
	for (size_t i = 0; i < 32; i++) {
		if (consumed_keys[i] == keycode + 1) {
			consumed_keys[i] = 0;
			return true;
		}
	}
	return false;
}

static void consumed_add(uint32_t keycode) {
	for (size_t i = 0; i < 32; i++) {
		if (consumed_keys[i] == 0) {
			consumed_keys[i] = keycode + 1;
			return;
		}
	}
}

static bool handle_vt_switch(struct wx_server *server, xkb_keysym_t sym) {
	if (sym >= XKB_KEY_XF86Switch_VT_1 && sym <= XKB_KEY_XF86Switch_VT_12) {
		if (server->session) {
			wlr_session_change_vt(server->session, sym - XKB_KEY_XF86Switch_VT_1 + 1);
		}
		return true;
	}
	return false;
}

/* XF86 volume/brightness/player keys: allowed on the lock screen, like KWin */
static bool is_media_key(xkb_keysym_t sym) {
	switch (sym) {
	case XKB_KEY_XF86MonBrightnessUp: case XKB_KEY_XF86MonBrightnessDown:
	case XKB_KEY_XF86AudioRaiseVolume: case XKB_KEY_XF86AudioLowerVolume:
	case XKB_KEY_XF86AudioMute: case XKB_KEY_XF86AudioMicMute:
	case XKB_KEY_XF86AudioPlay: case XKB_KEY_XF86AudioPause: case XKB_KEY_XF86AudioStop:
	case XKB_KEY_XF86AudioNext: case XKB_KEY_XF86AudioPrev:
		return true;
	default:
		return false;
	}
}

static void keyboard_key(struct wl_listener *listener, void *data) {
	struct wx_keyboard *kb = wl_container_of(listener, kb, key);
	struct wx_server *server = kb->server;
	struct wlr_keyboard_key_event *event = data;
	notify_activity(server);
	if (!kb->is_virtual) {
		server->last_keyboard = kb;
	}

	uint32_t keycode = event->keycode + 8;
	bool handled = false;
	if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		struct xkb_state *state = kb->wlr_keyboard->xkb_state;
		const xkb_keysym_t *syms;
		int n = xkb_state_key_get_syms(state, keycode, &syms);
		for (int i = 0; i < n && !handled; i++) {
			handled = handle_vt_switch(server, syms[i]) || switcher_key(server, syms[i]);
		}
		uint32_t mods = wlr_keyboard_get_modifiers(kb->wlr_keyboard);
		if (server->locked && !server->lock) {
			idle_lock_now(server); /* the locker crashed: bring it back */
		}
		if (!handled && !seat_shortcuts_inhibited(server)) {
			/* Raw (level 0) keysyms first so Mod+Shift+1 matches "1". */
			const xkb_keysym_t *raw;
			xkb_layout_index_t layout = xkb_state_key_get_layout(state, keycode);
			int nr = xkb_keymap_key_get_syms_by_level(kb->wlr_keyboard->keymap, keycode,
				layout, 0, &raw);
			for (int i = 0; i < nr && !handled; i++) {
				if (!server->locked || is_media_key(raw[i])) {
					handled = binds_handle(server, mods, raw[i]);
				}
			}
			for (int i = 0; i < n && !handled; i++) {
				if (!server->locked || is_media_key(syms[i])) {
					handled = binds_handle(server, mods, syms[i]);
				}
			}
		}
		if (handled) {
			consumed_add(keycode);
		}
	} else if (consumed_take(keycode)) {
		handled = true;
	}

	if (!handled && !server->locked) {
		/* an active input method (fcitx5, ibus...) sees the keys first */
		handled = ime_handle_key(server, kb, event->time_msec, event->keycode, event->state);
	}
	if (!handled) {
		wlr_seat_set_keyboard(server->seat, kb->wlr_keyboard);
		wlr_seat_keyboard_notify_key(server->seat, event->time_msec, event->keycode,
			event->state);
	}
}

static void update_caps(struct wx_server *server) {
	uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
	if (!wl_list_empty(&server->keyboards) || server->inject_kb) {
		caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	}
	if (server->touch_devices > 0) {
		caps |= WL_SEAT_CAPABILITY_TOUCH;
	}
	wlr_seat_set_capabilities(server->seat, caps);
}

static void keyboard_destroy(struct wl_listener *listener, void *data) {
	struct wx_keyboard *kb = wl_container_of(listener, kb, destroy);
	struct wx_server *server = kb->server;
	if (server->last_keyboard == kb) {
		server->last_keyboard = NULL;
	}
	wl_list_remove(&kb->modifiers.link);
	wl_list_remove(&kb->key.link);
	wl_list_remove(&kb->destroy.link);
	wl_list_remove(&kb->link);
	free(kb);
	update_caps(server);
}

void input_add_keyboard(struct wx_server *server, struct wlr_keyboard *wlr_kb, bool is_virtual) {
	struct wx_keyboard *kb = calloc(1, sizeof(*kb));
	kb->server = server;
	kb->wlr_keyboard = wlr_kb;
	kb->is_virtual = is_virtual;
	keyboard_configure(server, kb);
	kb->modifiers.notify = keyboard_modifiers;
	wl_signal_add(&wlr_kb->events.modifiers, &kb->modifiers);
	kb->key.notify = keyboard_key;
	wl_signal_add(&wlr_kb->events.key, &kb->key);
	kb->destroy.notify = keyboard_destroy;
	wl_signal_add(&wlr_kb->base.events.destroy, &kb->destroy);
	wl_list_insert(&server->keyboards, &kb->link);
	if (!is_virtual) {
		server->last_keyboard = kb;
	}
	wlr_seat_set_keyboard(server->seat, wlr_kb);
	update_caps(server);
}

/* ---------- pointer ---------- */

static void pointer_configure(struct wx_server *server, struct wlr_input_device *device) {
	if (!wlr_input_device_is_libinput(device)) {
		return;
	}
	struct libinput_device *li = wlr_libinput_get_device_handle(device);
	struct wx_config *c = &server->config;
	if (c->tap >= 0 && libinput_device_config_tap_get_finger_count(li) > 0) {
		libinput_device_config_tap_set_enabled(li,
			c->tap ? LIBINPUT_CONFIG_TAP_ENABLED : LIBINPUT_CONFIG_TAP_DISABLED);
	}
	if (c->natural_scroll >= 0 && libinput_device_config_scroll_has_natural_scroll(li)) {
		libinput_device_config_scroll_set_natural_scroll_enabled(li, c->natural_scroll);
	}
	if (c->left_handed >= 0 && libinput_device_config_left_handed_is_available(li)) {
		libinput_device_config_left_handed_set(li, c->left_handed);
	}
	if (c->accel >= -1 && libinput_device_config_accel_is_available(li)) {
		libinput_device_config_accel_set_speed(li, c->accel);
	}
}

void input_add_pointer(struct wx_server *server, struct wlr_input_device *device) {
	pointer_configure(server, device);
	wlr_cursor_attach_input_device(server->cursor, device);
}

static void new_input(struct wl_listener *listener, void *data) {
	struct wx_server *server = wl_container_of(listener, server, new_input);
	struct wlr_input_device *device = data;
	switch (device->type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		input_add_keyboard(server, wlr_keyboard_from_input_device(device), false);
		break;
	case WLR_INPUT_DEVICE_POINTER:
		input_add_pointer(server, device);
		break;
	case WLR_INPUT_DEVICE_TOUCH:
		input_add_touch(server, device);
		break;
	case WLR_INPUT_DEVICE_TABLET:
		input_add_tablet(server, device);
		break;
	case WLR_INPUT_DEVICE_TABLET_PAD:
		input_add_pad(server, device);
		break;
	case WLR_INPUT_DEVICE_SWITCH:
		input_add_switch(server, device);
		break;
	default:
		wlr_log(WLR_INFO, "input device '%s' (type %d) not handled", device->name, device->type);
		break;
	}
	update_caps(server);
}

void input_apply_config(struct wx_server *server) {
	struct wx_keyboard *kb;
	wl_list_for_each(kb, &server->keyboards, link) {
		keyboard_configure(server, kb);
	}
	/* pointers are configured through libinput handles attached to the cursor */
}

/* ---------- hit testing ---------- */

struct wx_node_tag *input_node_at(struct wx_server *server, double lx, double ly,
		struct wlr_surface **surface, double *sx, double *sy) {
	*surface = NULL;
	struct wlr_scene_node *node = wlr_scene_node_at(&server->scene->tree.node, lx, ly, sx, sy);
	if (!node) {
		return NULL;
	}
	if (node->type == WLR_SCENE_NODE_BUFFER) {
		struct wlr_scene_surface *ss =
			wlr_scene_surface_try_from_buffer(wlr_scene_buffer_from_node(node));
		if (ss) {
			*surface = ss->surface;
		}
	}
	for (struct wlr_scene_node *n = node; n; n = n->parent ? &n->parent->node : NULL) {
		struct wx_node_tag *tag = n->data;
		if (tag && tag->magic == WX_TAG_MAGIC) {
			return tag;
		}
	}
	return NULL;
}

static const char *edge_cursor(uint32_t edges) {
	switch (edges) {
	case WLR_EDGE_TOP: return "n-resize";
	case WLR_EDGE_BOTTOM: return "s-resize";
	case WLR_EDGE_LEFT: return "w-resize";
	case WLR_EDGE_RIGHT: return "e-resize";
	case WLR_EDGE_TOP | WLR_EDGE_LEFT: return "nw-resize";
	case WLR_EDGE_TOP | WLR_EDGE_RIGHT: return "ne-resize";
	case WLR_EDGE_BOTTOM | WLR_EDGE_LEFT: return "sw-resize";
	case WLR_EDGE_BOTTOM | WLR_EDGE_RIGHT: return "se-resize";
	default: return "default";
	}
}

/* ---------- interactive move / resize ---------- */

void input_begin_interactive(struct wx_view *view, enum wx_cursor_mode mode, uint32_t edges) {
	struct wx_server *server = view->server;
	if (view->fullscreen || server->locked) {
		return;
	}
	if (view->maximized || view_is_tiled(view)) {
		if (mode == WX_CURSOR_RESIZE && view_is_tiled(view)) {
			return;
		}
		/* X11-style: grabbing a maximized/tiled window detaches it under the cursor */
		struct wlr_box s = view->saved;
		double rel = view->w > 0 ? (server->cursor->x - view->x) / view->w : 0.5;
		if (view->maximized) {
			view_set_maximized(view, false);
		} else {
			view_set_floating(view, true);
		}
		if (s.width > 0) {
			view_move(view, (int)(server->cursor->x - rel * s.width), view->y);
		}
	}
	server->grab_view = view;
	server->cursor_mode = mode;
	server->snap = 0;
	if (mode == WX_CURSOR_MOVE) {
		server->grab_x = server->cursor->x - view->x;
		server->grab_y = server->cursor->y - view->y;
		wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "grabbing");
	} else {
		server->grab_box = (struct wlr_box){view->x, view->y, view->w, view->h};
		double bx = view->x + ((edges & WLR_EDGE_RIGHT) ? view->w : 0);
		double by = view->y + ((edges & WLR_EDGE_BOTTOM) ? view->h : 0);
		server->grab_x = server->cursor->x - bx;
		server->grab_y = server->cursor->y - by;
		server->resize_edges = edges;
		wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, edge_cursor(edges));
		if (view->type == WX_VIEW_XDG) {
			wlr_xdg_toplevel_set_resizing(view->xdg, true);
		}
	}
	wlr_seat_pointer_notify_clear_focus(server->seat);
}

static void snap_apply(struct wx_view *view, int snap) {
	struct wx_output *o = output_focused(view->server);
	if (!o || snap == 0) {
		return;
	}
	if (snap == 3) {
		view_set_maximized(view, true);
		return;
	}
	int l, r, t, b;
	view_frame_extents(view, &l, &r, &t, &b);
	struct wlr_box u = o->usable;
	int gap = view->server->config.gap;
	int half = u.width / 2;
	int x = snap == 1 ? u.x : u.x + half;
	view->saved = (struct wlr_box){view->x, view->y, view->w, view->h};
	view_set_box(view, x + gap + l, u.y + gap + t, half - 2 * gap - l - r,
		u.height - 2 * gap - t - b);
}

void input_end_interactive(struct wx_server *server) {
	struct wx_view *view = server->grab_view;
	if (view && server->cursor_mode == WX_CURSOR_RESIZE && view->type == WX_VIEW_XDG) {
		wlr_xdg_toplevel_set_resizing(view->xdg, false);
	}
	if (view && server->cursor_mode == WX_CURSOR_MOVE && server->snap) {
		snap_apply(view, server->snap);
	}
	if (view) {
		ipc_event_window(server, "geometry", view);
	}
	server->cursor_mode = WX_CURSOR_PASSTHROUGH;
	server->grab_view = NULL;
	server->snap = 0;
	wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
	input_process_motion(server, input_now());
}

static void process_move(struct wx_server *server) {
	struct wx_view *view = server->grab_view;
	view_move(view, (int)(server->cursor->x - server->grab_x),
		(int)(server->cursor->y - server->grab_y));
	server->snap = 0;
	if (!server->config.snap) {
		return;
	}
	struct wx_output *o = output_at(server, server->cursor->x, server->cursor->y);
	if (!o) {
		return;
	}
	int thr = server->config.snap_threshold;
	if (server->cursor->y <= o->full.y + thr) {
		server->snap = 3;
	} else if (server->cursor->x <= o->full.x + thr) {
		server->snap = 1;
	} else if (server->cursor->x >= o->full.x + o->full.width - 1 - thr) {
		server->snap = 2;
	}
}

static void process_resize(struct wx_server *server) {
	struct wx_view *view = server->grab_view;
	double bx = server->cursor->x - server->grab_x;
	double by = server->cursor->y - server->grab_y;
	struct wlr_box g = server->grab_box;
	int left = g.x, right = g.x + g.width, top = g.y, bottom = g.y + g.height;
	uint32_t e = server->resize_edges;
	if (e & WLR_EDGE_TOP) {
		top = (int)by;
		if (top > bottom - MIN_H) {
			top = bottom - MIN_H;
		}
	} else if (e & WLR_EDGE_BOTTOM) {
		bottom = (int)by;
		if (bottom < top + MIN_H) {
			bottom = top + MIN_H;
		}
	}
	if (e & WLR_EDGE_LEFT) {
		left = (int)bx;
		if (left > right - MIN_W) {
			left = right - MIN_W;
		}
	} else if (e & WLR_EDGE_RIGHT) {
		right = (int)bx;
		if (right < left + MIN_W) {
			right = left + MIN_W;
		}
	}
	view_set_box(view, left, top, right - left, bottom - top);
}

void input_process_motion(struct wx_server *server, uint32_t time) {
	wlr_scene_node_set_position(&server->drag_icons->node,
		(int)server->cursor->x, (int)server->cursor->y);
	struct wx_output *under = output_at(server, server->cursor->x, server->cursor->y);
	if (under) {
		server->active_output = under;
	}
	if (server->cursor_mode == WX_CURSOR_MOVE && server->grab_view) {
		process_move(server);
		return;
	}
	if (server->cursor_mode == WX_CURSOR_RESIZE && server->grab_view) {
		process_resize(server);
		return;
	}

	double sx, sy;
	struct wlr_surface *surface = NULL;
	struct wx_node_tag *tag = input_node_at(server, server->cursor->x, server->cursor->y,
		&surface, &sx, &sy);
	struct wx_view *view = tag && tag->kind == WX_NODE_VIEW ? tag->ptr : NULL;

	if (!surface) {
		const char *name = "default";
		if (view && !view->unmanaged) {
			uint32_t edges = 0;
			int hit = deco_hit(view, server->cursor->x - view->x, server->cursor->y - view->y,
				&edges);
			if (hit == WX_DECO_BORDER) {
				name = edge_cursor(edges);
			}
		}
		wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, name);
		wlr_seat_pointer_clear_focus(server->seat);
	} else {
		double f = input_surface_scale(server, surface); /* X11 surfaces: device pixels */
		wlr_seat_pointer_notify_enter(server->seat, surface, sx * f, sy * f);
		wlr_seat_pointer_notify_motion(server->seat, time, sx * f, sy * f);
	}
	if (server->config.focus_follows_mouse && view && !view->unmanaged &&
			view != server->focused && !server->pressed_buttons) {
		seat_focus_view(server, view);
	}
}

static bool constraint_blocks_motion(struct wx_server *server, double dx, double dy) {
	struct wlr_pointer_constraint_v1 *c = server->active_constraint;
	if (!c || server->cursor_mode != WX_CURSOR_PASSTHROUGH) {
		return false;
	}
	if (c->type == WLR_POINTER_CONSTRAINT_V1_LOCKED) {
		return true;
	}
	struct wx_view *view = view_from_surface(server, c->surface);
	if (!view) {
		return false;
	}
	int ox = view->x, oy = view->y;
	if (view->type == WX_VIEW_XDG) {
		ox -= view->xdg->base->geometry.x;
		oy -= view->xdg->base->geometry.y;
	}
	double f = input_surface_scale(server, c->surface);
	double tx = (server->cursor->x + dx - ox) * f, ty = (server->cursor->y + dy - oy) * f;
	return !pixman_region32_contains_point(&c->region, (int)tx, (int)ty, NULL);
}

static void cursor_motion(struct wl_listener *listener, void *data) {
	struct wx_server *server = wl_container_of(listener, server, cursor_motion);
	struct wlr_pointer_motion_event *e = data;
	notify_activity(server);
	double f = input_surface_scale(server, server->seat->pointer_state.focused_surface);
	wlr_relative_pointer_manager_v1_send_relative_motion(server->relative_pointer, server->seat,
		(uint64_t)e->time_msec * 1000, e->delta_x * f, e->delta_y * f, e->unaccel_dx * f,
		e->unaccel_dy * f);
	if (constraint_blocks_motion(server, e->delta_x, e->delta_y)) {
		return;
	}
	wlr_cursor_move(server->cursor, &e->pointer->base, e->delta_x, e->delta_y);
	input_process_motion(server, e->time_msec);
}

static void cursor_motion_abs(struct wl_listener *listener, void *data) {
	struct wx_server *server = wl_container_of(listener, server, cursor_motion_abs);
	struct wlr_pointer_motion_absolute_event *e = data;
	notify_activity(server);
	double lx, ly;
	wlr_cursor_absolute_to_layout_coords(server->cursor, &e->pointer->base, e->x, e->y, &lx, &ly);
	double dx = lx - server->cursor->x, dy = ly - server->cursor->y;
	double f = input_surface_scale(server, server->seat->pointer_state.focused_surface);
	wlr_relative_pointer_manager_v1_send_relative_motion(server->relative_pointer, server->seat,
		(uint64_t)e->time_msec * 1000, dx * f, dy * f, dx * f, dy * f);
	if (constraint_blocks_motion(server, dx, dy)) {
		return;
	}
	wlr_cursor_warp_absolute(server->cursor, &e->pointer->base, e->x, e->y);
	input_process_motion(server, e->time_msec);
}

static uint32_t last_title_click;
static struct wx_view *last_title_view;

void input_pointer_button(struct wx_server *server, uint32_t time, uint32_t button, bool pressed) {
	notify_activity(server);
	if (pressed) {
		server->pressed_buttons++;
	} else if (server->pressed_buttons > 0) {
		server->pressed_buttons--;
	}

	if (!pressed) {
		if (server->cursor_mode != WX_CURSOR_PASSTHROUGH) {
			input_end_interactive(server);
			return;
		}
		wlr_seat_pointer_notify_button(server->seat, time, button, WL_POINTER_BUTTON_STATE_RELEASED);
		return;
	}
	if (server->locked) {
		wlr_seat_pointer_notify_button(server->seat, time, button, WL_POINTER_BUTTON_STATE_PRESSED);
		return;
	}

	double sx, sy;
	struct wlr_surface *surface = NULL;
	struct wx_node_tag *tag = input_node_at(server, server->cursor->x, server->cursor->y,
		&surface, &sx, &sy);
	if (tag && tag->kind == WX_NODE_VIEW) {
		struct wx_view *view = tag->ptr;
		if (view->unmanaged) {
			if (view->xsurface && wlr_xwayland_surface_override_redirect_wants_focus(view->xsurface)) {
				seat_focus_surface(server, surface);
			}
		} else {
			seat_focus_view(server, view);
			struct wlr_keyboard *kb = wlr_seat_get_keyboard(server->seat);
			uint32_t mods = kb ? wlr_keyboard_get_modifiers(kb) : 0;
			if ((mods & server->mod_mask) && (button == BTN_LEFT || button == BTN_RIGHT)) {
				if (button == BTN_LEFT) {
					input_begin_interactive(view, WX_CURSOR_MOVE, 0);
				} else {
					uint32_t edges = 0;
					edges |= server->cursor->x < view->x + view->w / 2.0 ? WLR_EDGE_LEFT : WLR_EDGE_RIGHT;
					edges |= server->cursor->y < view->y + view->h / 2.0 ? WLR_EDGE_TOP : WLR_EDGE_BOTTOM;
					input_begin_interactive(view, WX_CURSOR_RESIZE, edges);
				}
				server->pressed_buttons = 0;
				return;
			}
			if (!surface) {
				uint32_t edges = 0;
				int hit = deco_hit(view, server->cursor->x - view->x, server->cursor->y - view->y,
					&edges);
				server->pressed_buttons = 0;
				switch (hit) {
				case WX_DECO_CLOSE:
					view_close(view);
					return;
				case WX_DECO_MAX:
					view_set_maximized(view, !view->maximized);
					return;
				case WX_DECO_MIN:
					view_set_minimized(view, true);
					return;
				case WX_DECO_TITLE:
					if (button == BTN_MIDDLE) {
						view_set_stack(view, WX_STACK_BELOW);
						return;
					}
					if (last_title_view == view && time - last_title_click < 400) {
						last_title_view = NULL;
						view_set_maximized(view, !view->maximized);
						return;
					}
					last_title_view = view;
					last_title_click = time;
					input_begin_interactive(view, WX_CURSOR_MOVE, 0);
					return;
				case WX_DECO_BORDER:
					input_begin_interactive(view, WX_CURSOR_RESIZE, edges);
					return;
				default:
					break;
				}
			}
		}
	} else if (tag && tag->kind == WX_NODE_LAYER) {
		struct wx_layer_surface *layer = tag->ptr;
		if (layer->layer_surface->current.keyboard_interactive !=
				ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE) {
			seat_focus_layer(server, layer);
		}
	}
	wlr_seat_pointer_notify_button(server->seat, time, button, WL_POINTER_BUTTON_STATE_PRESSED);
}

static void cursor_button(struct wl_listener *listener, void *data) {
	struct wx_server *server = wl_container_of(listener, server, cursor_button);
	struct wlr_pointer_button_event *e = data;
	input_pointer_button(server, e->time_msec, e->button,
		e->state == WL_POINTER_BUTTON_STATE_PRESSED);
}

void input_pointer_axis(struct wx_server *server, uint32_t time, bool horizontal, double delta) {
	wlr_seat_pointer_notify_axis(server->seat, time,
		horizontal ? WL_POINTER_AXIS_HORIZONTAL_SCROLL : WL_POINTER_AXIS_VERTICAL_SCROLL,
		delta, (int32_t)(delta / 15.0 * 120), WL_POINTER_AXIS_SOURCE_WHEEL,
		WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
	wlr_seat_pointer_notify_frame(server->seat);
}

static void cursor_axis(struct wl_listener *listener, void *data) {
	struct wx_server *server = wl_container_of(listener, server, cursor_axis);
	struct wlr_pointer_axis_event *e = data;
	notify_activity(server);
	wlr_seat_pointer_notify_axis(server->seat, e->time_msec, e->orientation, e->delta,
		e->delta_discrete, e->source, e->relative_direction);
}

static void cursor_frame(struct wl_listener *listener, void *data) {
	struct wx_server *server = wl_container_of(listener, server, cursor_frame);
	wlr_seat_pointer_notify_frame(server->seat);
}

#define GESTURE(name, type, call) \
	static void name(struct wl_listener *listener, void *data) { \
		struct wx_server *server = wl_container_of(listener, server, name); \
		struct type *e = data; \
		notify_activity(server); \
		call; \
	}

GESTURE(cursor_swipe_begin, wlr_pointer_swipe_begin_event,
	wlr_pointer_gestures_v1_send_swipe_begin(server->gestures, server->seat, e->time_msec, e->fingers))
GESTURE(cursor_swipe_update, wlr_pointer_swipe_update_event,
	wlr_pointer_gestures_v1_send_swipe_update(server->gestures, server->seat, e->time_msec, e->dx, e->dy))
GESTURE(cursor_swipe_end, wlr_pointer_swipe_end_event,
	wlr_pointer_gestures_v1_send_swipe_end(server->gestures, server->seat, e->time_msec, e->cancelled))
GESTURE(cursor_pinch_begin, wlr_pointer_pinch_begin_event,
	wlr_pointer_gestures_v1_send_pinch_begin(server->gestures, server->seat, e->time_msec, e->fingers))
GESTURE(cursor_pinch_update, wlr_pointer_pinch_update_event,
	wlr_pointer_gestures_v1_send_pinch_update(server->gestures, server->seat, e->time_msec, e->dx,
		e->dy, e->scale, e->rotation))
GESTURE(cursor_pinch_end, wlr_pointer_pinch_end_event,
	wlr_pointer_gestures_v1_send_pinch_end(server->gestures, server->seat, e->time_msec, e->cancelled))
GESTURE(cursor_hold_begin, wlr_pointer_hold_begin_event,
	wlr_pointer_gestures_v1_send_hold_begin(server->gestures, server->seat, e->time_msec, e->fingers))
GESTURE(cursor_hold_end, wlr_pointer_hold_end_event,
	wlr_pointer_gestures_v1_send_hold_end(server->gestures, server->seat, e->time_msec, e->cancelled))

/* ---------- touch ---------- */

#define MAX_TOUCH 16

/* Surface origin per touch point, fixed at touch-down so motion stays
 * relative to the surface that was touched. */
static struct {
	int32_t id;
	bool used;
	double ox, oy;
	double f; /* surface units per layout unit (X11 surfaces: the X screen scale) */
} touch_points[MAX_TOUCH];

/* The first finger on something without wl_touch drives the pointer instead. */
static int32_t emulating_touch = -1;

static void touch_device_gone(struct wl_listener *listener, void *data) {
	struct wx_touch_dev *t = wl_container_of(listener, t, destroy);
	t->server->touch_devices--;
	update_caps(t->server);
	wl_list_remove(&t->destroy.link);
	free(t);
}

void input_add_touch(struct wx_server *server, struct wlr_input_device *device) {
	struct wlr_touch *touch = wlr_touch_from_input_device(device);
	wlr_cursor_attach_input_device(server->cursor, device);
	if (touch->output_name) {
		struct wx_output *o = output_by_name(server, touch->output_name);
		if (o) {
			wlr_cursor_map_input_to_output(server->cursor, device, o->wlr_output);
		}
	}
	struct wx_touch_dev *t = calloc(1, sizeof(*t));
	t->server = server;
	t->destroy.notify = touch_device_gone;
	wl_signal_add(&device->events.destroy, &t->destroy);
	server->touch_devices++;
	update_caps(server);
}

static void touch_point_set(int32_t id, double ox, double oy, double f) {
	for (int i = 0; i < MAX_TOUCH; i++) {
		if (!touch_points[i].used) {
			touch_points[i].used = true;
			touch_points[i].id = id;
			touch_points[i].ox = ox;
			touch_points[i].oy = oy;
			touch_points[i].f = f;
			return;
		}
	}
}

static int touch_point_find(int32_t id) {
	for (int i = 0; i < MAX_TOUCH; i++) {
		if (touch_points[i].used && touch_points[i].id == id) {
			return i;
		}
	}
	return -1;
}

static void cursor_touch_down(struct wl_listener *listener, void *data) {
	struct wx_server *server = wl_container_of(listener, server, cursor_touch_down);
	struct wlr_touch_down_event *e = data;
	notify_activity(server);
	double lx, ly, sx, sy;
	wlr_cursor_absolute_to_layout_coords(server->cursor, &e->touch->base, e->x, e->y, &lx, &ly);
	struct wlr_surface *surface = NULL;
	struct wx_node_tag *tag = input_node_at(server, lx, ly, &surface, &sx, &sy);
	if (tag && tag->kind == WX_NODE_VIEW && !((struct wx_view *)tag->ptr)->unmanaged) {
		seat_focus_view(server, tag->ptr);
	}
	if (surface && wlr_surface_accepts_touch(surface, server->seat)) {
		double f = input_surface_scale(server, surface);
		touch_point_set(e->touch_id, lx - sx, ly - sy, f);
		wlr_seat_touch_notify_down(server->seat, surface, e->time_msec, e->touch_id, sx * f, sy * f);
		return;
	}
	if (emulating_touch < 0) {
		/* no wl_touch there (or a title bar): behave like a mouse */
		emulating_touch = e->touch_id;
		wlr_cursor_warp_closest(server->cursor, NULL, lx, ly);
		input_process_motion(server, e->time_msec);
		input_pointer_button(server, e->time_msec, BTN_LEFT, true);
		wlr_seat_pointer_notify_frame(server->seat);
	}
}

static void cursor_touch_up(struct wl_listener *listener, void *data) {
	struct wx_server *server = wl_container_of(listener, server, cursor_touch_up);
	struct wlr_touch_up_event *e = data;
	notify_activity(server);
	if (e->touch_id == emulating_touch) {
		emulating_touch = -1;
		input_pointer_button(server, e->time_msec, BTN_LEFT, false);
		wlr_seat_pointer_notify_frame(server->seat);
		return;
	}
	int i = touch_point_find(e->touch_id);
	if (i >= 0) {
		touch_points[i].used = false;
	}
	wlr_seat_touch_notify_up(server->seat, e->time_msec, e->touch_id);
}

static void cursor_touch_motion(struct wl_listener *listener, void *data) {
	struct wx_server *server = wl_container_of(listener, server, cursor_touch_motion);
	struct wlr_touch_motion_event *e = data;
	notify_activity(server);
	double lx, ly;
	wlr_cursor_absolute_to_layout_coords(server->cursor, &e->touch->base, e->x, e->y, &lx, &ly);
	if (e->touch_id == emulating_touch) {
		wlr_cursor_warp_closest(server->cursor, NULL, lx, ly);
		input_process_motion(server, e->time_msec);
		wlr_seat_pointer_notify_frame(server->seat);
		return;
	}
	int i = touch_point_find(e->touch_id);
	if (i >= 0) {
		wlr_seat_touch_notify_motion(server->seat, e->time_msec, e->touch_id,
			(lx - touch_points[i].ox) * touch_points[i].f, (ly - touch_points[i].oy) * touch_points[i].f);
	}
}

static void cursor_touch_cancel(struct wl_listener *listener, void *data) {
	struct wx_server *server = wl_container_of(listener, server, cursor_touch_cancel);
	struct wlr_touch_cancel_event *e = data;
	int i = touch_point_find(e->touch_id);
	if (i >= 0) {
		touch_points[i].used = false;
		struct wlr_touch_point *p = wlr_seat_touch_get_point(server->seat, e->touch_id);
		if (p && p->client) {
			wlr_seat_touch_notify_cancel(server->seat, p->client);
		}
	}
	if (e->touch_id == emulating_touch) {
		emulating_touch = -1;
		input_pointer_button(server, input_now(), BTN_LEFT, false);
	}
}

static void cursor_touch_frame(struct wl_listener *listener, void *data) {
	struct wx_server *server = wl_container_of(listener, server, cursor_touch_frame);
	wlr_seat_touch_notify_frame(server->seat);
}

/* ---------- drawing tablets (tablet-v2, pointer emulation as fallback) ---------- */

struct wx_tool {
	struct wlr_tablet_v2_tablet_tool *v2;
	double ox, oy;     /* origin of the surface the tool is on */
	double f;          /* surface units per layout unit */
	bool emulating;    /* client lacks tablet-v2: we drive the pointer */
	struct wl_listener destroy;
};

static void tool_destroy(struct wl_listener *listener, void *data) {
	struct wx_tool *t = wl_container_of(listener, t, destroy);
	struct wlr_tablet_tool *tool = data;
	tool->data = NULL;
	wl_list_remove(&t->destroy.link);
	free(t);
}

static struct wx_tool *tool_get(struct wx_server *server, struct wlr_tablet_tool *tool) {
	if (!tool->data) {
		struct wx_tool *t = calloc(1, sizeof(*t));
		t->f = 1.0;
		t->v2 = wlr_tablet_tool_create(server->tablet_manager, server->seat, tool);
		t->destroy.notify = tool_destroy;
		wl_signal_add(&tool->events.destroy, &t->destroy);
		tool->data = t;
	}
	return tool->data;
}

void input_add_tablet(struct wx_server *server, struct wlr_input_device *device) {
	device->data = wlr_tablet_create(server->tablet_manager, server->seat, device);
	server->last_tablet = device->data;
	wlr_cursor_attach_input_device(server->cursor, device);
}

/* ---------- tablet pads (buttons, rings, strips on the tablet itself) ---------- */

/* shortcut: a pad is paired with the most recently added tablet, which covers
 * the usual one-tablet setup; track attach_tablet for several tablets */
struct wx_pad {
	struct wl_list link;
	struct wx_server *server;
	struct wlr_tablet_v2_tablet_pad *v2;
	struct wlr_surface *focus;
	struct wl_listener button, ring, strip, destroy;
};

static void pad_focus(struct wx_pad *pad, struct wlr_surface *old, struct wlr_surface *now) {
	struct wx_server *server = pad->server;
	if (pad->focus && pad->focus == old) {
		wlr_tablet_v2_tablet_pad_notify_leave(pad->v2, old);
	}
	pad->focus = NULL;
	if (now && server->last_tablet && wlr_surface_accepts_tablet_v2(now, server->last_tablet)) {
		wlr_tablet_v2_tablet_pad_notify_enter(pad->v2, server->last_tablet, now);
		pad->focus = now;
	}
}

static void kb_focus_pads(struct wl_listener *listener, void *data) {
	struct wx_server *server = wl_container_of(listener, server, kb_focus_pads);
	struct wlr_seat_keyboard_focus_change_event *e = data;
	struct wx_pad *pad;
	wl_list_for_each(pad, &server->pads, link) {
		pad_focus(pad, e->old_surface, e->new_surface);
	}
}

static void pad_button(struct wl_listener *listener, void *data) {
	struct wx_pad *pad = wl_container_of(listener, pad, button);
	struct wlr_tablet_pad_button_event *e = data;
	notify_activity(pad->server);
	wlr_tablet_v2_tablet_pad_notify_button(pad->v2, e->button, e->time_msec,
		e->state == WLR_BUTTON_PRESSED ? ZWP_TABLET_PAD_V2_BUTTON_STATE_PRESSED :
		ZWP_TABLET_PAD_V2_BUTTON_STATE_RELEASED);
}

static void pad_ring(struct wl_listener *listener, void *data) {
	struct wx_pad *pad = wl_container_of(listener, pad, ring);
	struct wlr_tablet_pad_ring_event *e = data;
	wlr_tablet_v2_tablet_pad_notify_ring(pad->v2, e->ring, e->position,
		e->source == WLR_TABLET_PAD_RING_SOURCE_FINGER, e->time_msec);
}

static void pad_strip(struct wl_listener *listener, void *data) {
	struct wx_pad *pad = wl_container_of(listener, pad, strip);
	struct wlr_tablet_pad_strip_event *e = data;
	wlr_tablet_v2_tablet_pad_notify_strip(pad->v2, e->strip, e->position,
		e->source == WLR_TABLET_PAD_STRIP_SOURCE_FINGER, e->time_msec);
}

static void pad_destroy(struct wl_listener *listener, void *data) {
	struct wx_pad *pad = wl_container_of(listener, pad, destroy);
	wl_list_remove(&pad->button.link);
	wl_list_remove(&pad->ring.link);
	wl_list_remove(&pad->strip.link);
	wl_list_remove(&pad->destroy.link);
	wl_list_remove(&pad->link);
	free(pad);
}

void input_add_pad(struct wx_server *server, struct wlr_input_device *device) {
	struct wlr_tablet_pad *wpad = wlr_tablet_pad_from_input_device(device);
	struct wx_pad *pad = calloc(1, sizeof(*pad));
	pad->server = server;
	pad->v2 = wlr_tablet_pad_create(server->tablet_manager, server->seat, device);
	pad->button.notify = pad_button;
	wl_signal_add(&wpad->events.button, &pad->button);
	pad->ring.notify = pad_ring;
	wl_signal_add(&wpad->events.ring, &pad->ring);
	pad->strip.notify = pad_strip;
	wl_signal_add(&wpad->events.strip, &pad->strip);
	pad->destroy.notify = pad_destroy;
	wl_signal_add(&device->events.destroy, &pad->destroy);
	wl_list_insert(&server->pads, &pad->link);
	pad_focus(pad, NULL, server->seat->keyboard_state.focused_surface);
}

/* ---------- switches: laptop lid ---------- */

static bool output_is_internal(struct wx_server *server, struct wx_output *o) {
	const char *name = o->wlr_output->name;
	if (server->config.internal_output) {
		return fnmatch(server->config.internal_output, name, 0) == 0;
	}
	return !strncmp(name, "eDP", 3) || !strncmp(name, "LVDS", 4) || !strncmp(name, "DSI", 3);
}

/* Closing the lid with an external monitor attached turns the built-in panel
 * off (its windows move to the remaining outputs); opening brings it back. */
void input_lid(struct wx_server *server, bool closed) {
	char line[64];
	snprintf(line, sizeof(line), "{\"event\":\"lid\",\"closed\":%s}", closed ? "true" : "false");
	ipc_broadcast_event(server, line);
	struct wx_output *o;
	if (!closed) {
		wl_list_for_each(o, &server->outputs, link) {
			if (o->lid_disabled) {
				o->lid_disabled = false;
				output_set_enabled(o, true);
			}
		}
		return;
	}
	if (!server->config.lid_disable_internal) {
		return;
	}
	bool external = false;
	wl_list_for_each(o, &server->outputs, link) {
		if (o->wlr_output->enabled && !output_is_internal(server, o)) {
			external = true;
		}
	}
	if (!external) {
		return; /* never leave the session without a screen; logind handles suspend */
	}
	wl_list_for_each(o, &server->outputs, link) {
		if (o->wlr_output->enabled && output_is_internal(server, o)) {
			o->lid_disabled = true;
			output_set_enabled(o, false);
			wlr_log(WLR_INFO, "lid closed: %s off", o->wlr_output->name);
		}
	}
}

struct wx_switch {
	struct wx_server *server;
	struct wl_listener toggle, destroy;
};

static void switch_toggle(struct wl_listener *listener, void *data) {
	struct wx_switch *sw = wl_container_of(listener, sw, toggle);
	struct wlr_switch_toggle_event *e = data;
	if (e->switch_type == WLR_SWITCH_TYPE_LID) {
		input_lid(sw->server, e->switch_state == WLR_SWITCH_STATE_ON);
	}
}

static void switch_destroy(struct wl_listener *listener, void *data) {
	struct wx_switch *sw = wl_container_of(listener, sw, destroy);
	wl_list_remove(&sw->toggle.link);
	wl_list_remove(&sw->destroy.link);
	free(sw);
}

void input_add_switch(struct wx_server *server, struct wlr_input_device *device) {
	struct wx_switch *sw = calloc(1, sizeof(*sw));
	sw->server = server;
	sw->toggle.notify = switch_toggle;
	wl_signal_add(&wlr_switch_from_input_device(device)->events.toggle, &sw->toggle);
	sw->destroy.notify = switch_destroy;
	wl_signal_add(&device->events.destroy, &sw->destroy);
}

/* Moves the cursor to the tool and routes focus: tablet-v2 if the surface
 * speaks it, pointer events otherwise. */
static void tool_position(struct wx_server *server, struct wlr_tablet *tablet, struct wx_tool *t,
		uint32_t time) {
	struct wlr_tablet_v2_tablet *tv2 = tablet->base.data;
	if (t->v2->is_down && t->v2->focused_surface) {
		/* implicit grab: keep coordinates relative to the pressed surface */
		wlr_tablet_v2_tablet_tool_notify_motion(t->v2, (server->cursor->x - t->ox) * t->f,
			(server->cursor->y - t->oy) * t->f);
		return;
	}
	double sx, sy;
	struct wlr_surface *surface = NULL;
	input_node_at(server, server->cursor->x, server->cursor->y, &surface, &sx, &sy);
	if (surface && tv2 && wlr_surface_accepts_tablet_v2(surface, tv2)) {
		if (t->emulating) {
			wlr_seat_pointer_notify_clear_focus(server->seat);
			t->emulating = false;
		}
		if (t->v2->focused_surface != surface) {
			wlr_tablet_v2_tablet_tool_notify_proximity_in(t->v2, tv2, surface);
		}
		t->ox = server->cursor->x - sx;
		t->oy = server->cursor->y - sy;
		t->f = input_surface_scale(server, surface);
		wlr_tablet_v2_tablet_tool_notify_motion(t->v2, sx * t->f, sy * t->f);
		wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
		return;
	}
	if (t->v2->focused_surface) {
		wlr_tablet_v2_tablet_tool_notify_proximity_out(t->v2);
	}
	t->emulating = true;
	input_process_motion(server, time);
	wlr_seat_pointer_notify_frame(server->seat);
}

static void move_cursor_to_tool(struct wx_server *server, struct wlr_tablet *tablet, uint32_t axes,
		double x, double y) {
	bool hx = axes & WLR_TABLET_TOOL_AXIS_X, hy = axes & WLR_TABLET_TOOL_AXIS_Y;
	wlr_cursor_warp_absolute(server->cursor, &tablet->base, hx ? x : NAN, hy ? y : NAN);
}

static void cursor_tool_proximity(struct wl_listener *listener, void *data) {
	struct wx_server *server = wl_container_of(listener, server, cursor_tool_proximity);
	struct wlr_tablet_tool_proximity_event *e = data;
	notify_activity(server);
	struct wx_tool *t = tool_get(server, e->tool);
	if (e->state == WLR_TABLET_TOOL_PROXIMITY_OUT) {
		if (t->v2->focused_surface) {
			wlr_tablet_v2_tablet_tool_notify_proximity_out(t->v2);
		}
		return;
	}
	move_cursor_to_tool(server, e->tablet, WLR_TABLET_TOOL_AXIS_X | WLR_TABLET_TOOL_AXIS_Y, e->x, e->y);
	tool_position(server, e->tablet, t, e->time_msec);
}

static void cursor_tool_axis(struct wl_listener *listener, void *data) {
	struct wx_server *server = wl_container_of(listener, server, cursor_tool_axis);
	struct wlr_tablet_tool_axis_event *e = data;
	notify_activity(server);
	struct wx_tool *t = tool_get(server, e->tool);
	if (e->updated_axes & (WLR_TABLET_TOOL_AXIS_X | WLR_TABLET_TOOL_AXIS_Y)) {
		move_cursor_to_tool(server, e->tablet, e->updated_axes, e->x, e->y);
		tool_position(server, e->tablet, t, e->time_msec);
	}
	if (t->emulating || !t->v2->focused_surface) {
		return;
	}
	uint32_t a = e->updated_axes;
	if (a & WLR_TABLET_TOOL_AXIS_PRESSURE) {
		wlr_tablet_v2_tablet_tool_notify_pressure(t->v2, e->pressure);
	}
	if (a & WLR_TABLET_TOOL_AXIS_DISTANCE) {
		wlr_tablet_v2_tablet_tool_notify_distance(t->v2, e->distance);
	}
	if (a & (WLR_TABLET_TOOL_AXIS_TILT_X | WLR_TABLET_TOOL_AXIS_TILT_Y)) {
		wlr_tablet_v2_tablet_tool_notify_tilt(t->v2, e->tilt_x, e->tilt_y);
	}
	if (a & WLR_TABLET_TOOL_AXIS_ROTATION) {
		wlr_tablet_v2_tablet_tool_notify_rotation(t->v2, e->rotation);
	}
	if (a & WLR_TABLET_TOOL_AXIS_SLIDER) {
		wlr_tablet_v2_tablet_tool_notify_slider(t->v2, e->slider);
	}
	if (a & WLR_TABLET_TOOL_AXIS_WHEEL) {
		wlr_tablet_v2_tablet_tool_notify_wheel(t->v2, e->wheel_delta, 0);
	}
}

static void cursor_tool_tip(struct wl_listener *listener, void *data) {
	struct wx_server *server = wl_container_of(listener, server, cursor_tool_tip);
	struct wlr_tablet_tool_tip_event *e = data;
	notify_activity(server);
	struct wx_tool *t = tool_get(server, e->tool);
	bool down = e->state == WLR_TABLET_TOOL_TIP_DOWN;
	if (!t->emulating && t->v2->focused_surface) {
		if (down) {
			struct wx_view *view = view_from_surface(server, t->v2->focused_surface);
			if (view && !view->unmanaged) {
				seat_focus_view(server, view);
			}
			wlr_tablet_v2_tablet_tool_notify_down(t->v2);
			wlr_tablet_tool_v2_start_implicit_grab(t->v2);
		} else {
			wlr_tablet_v2_tablet_tool_notify_up(t->v2);
		}
		return;
	}
	input_pointer_button(server, e->time_msec, BTN_LEFT, down);
	wlr_seat_pointer_notify_frame(server->seat);
}

static void cursor_tool_button(struct wl_listener *listener, void *data) {
	struct wx_server *server = wl_container_of(listener, server, cursor_tool_button);
	struct wlr_tablet_tool_button_event *e = data;
	struct wx_tool *t = tool_get(server, e->tool);
	bool pressed = e->state == WLR_BUTTON_PRESSED;
	if (!t->emulating && t->v2->focused_surface) {
		wlr_tablet_v2_tablet_tool_notify_button(t->v2, e->button, pressed ?
			ZWP_TABLET_PAD_V2_BUTTON_STATE_PRESSED : ZWP_TABLET_PAD_V2_BUTTON_STATE_RELEASED);
		return;
	}
	/* stylus buttons act as right/middle click for apps without tablet support */
	uint32_t b = e->button == BTN_STYLUS ? BTN_RIGHT : e->button == BTN_STYLUS2 ? BTN_MIDDLE : e->button;
	input_pointer_button(server, e->time_msec, b, pressed);
	wlr_seat_pointer_notify_frame(server->seat);
}

#define LISTEN(sig, field, fn) do { server->field.notify = fn; wl_signal_add(&(sig), &server->field); } while (0)

void input_init(struct wx_server *server) {
	wl_list_init(&server->keyboards);
	server->gestures = wlr_pointer_gestures_v1_create(server->display);
	server->relative_pointer = wlr_relative_pointer_manager_v1_create(server->display);
	struct wlr_cursor *c = server->cursor;
	LISTEN(server->backend->events.new_input, new_input, new_input);
	LISTEN(c->events.motion, cursor_motion, cursor_motion);
	LISTEN(c->events.motion_absolute, cursor_motion_abs, cursor_motion_abs);
	LISTEN(c->events.button, cursor_button, cursor_button);
	LISTEN(c->events.axis, cursor_axis, cursor_axis);
	LISTEN(c->events.frame, cursor_frame, cursor_frame);
	LISTEN(c->events.swipe_begin, cursor_swipe_begin, cursor_swipe_begin);
	LISTEN(c->events.swipe_update, cursor_swipe_update, cursor_swipe_update);
	LISTEN(c->events.swipe_end, cursor_swipe_end, cursor_swipe_end);
	LISTEN(c->events.pinch_begin, cursor_pinch_begin, cursor_pinch_begin);
	LISTEN(c->events.pinch_update, cursor_pinch_update, cursor_pinch_update);
	LISTEN(c->events.pinch_end, cursor_pinch_end, cursor_pinch_end);
	LISTEN(c->events.hold_begin, cursor_hold_begin, cursor_hold_begin);
	LISTEN(c->events.hold_end, cursor_hold_end, cursor_hold_end);
	LISTEN(c->events.touch_down, cursor_touch_down, cursor_touch_down);
	LISTEN(c->events.touch_up, cursor_touch_up, cursor_touch_up);
	LISTEN(c->events.touch_motion, cursor_touch_motion, cursor_touch_motion);
	LISTEN(c->events.touch_cancel, cursor_touch_cancel, cursor_touch_cancel);
	LISTEN(c->events.touch_frame, cursor_touch_frame, cursor_touch_frame);
	server->tablet_manager = wlr_tablet_v2_create(server->display);
	wl_list_init(&server->pads);
	LISTEN(server->seat->keyboard_state.events.focus_change, kb_focus_pads, kb_focus_pads);
	LISTEN(c->events.tablet_tool_proximity, cursor_tool_proximity, cursor_tool_proximity);
	LISTEN(c->events.tablet_tool_axis, cursor_tool_axis, cursor_tool_axis);
	LISTEN(c->events.tablet_tool_tip, cursor_tool_tip, cursor_tool_tip);
	LISTEN(c->events.tablet_tool_button, cursor_tool_button, cursor_tool_button);
}
