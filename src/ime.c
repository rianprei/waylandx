#include <stdlib.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_input_method_v2.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_text_input_v3.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include "server.h"

/*
 * Input method relay: text-input-v3 (apps) <-> input-method-v2 (fcitx5,
 * ibus, ...). Same model as sway: one input method per seat, text inputs
 * follow keyboard focus, IME keyboard grab receives the physical keys.
 */

struct wx_text_input {
	struct wl_list link; /* wx_server.text_inputs */
	struct wx_server *server;
	struct wlr_text_input_v3 *ti;
	struct wl_listener enable;
	struct wl_listener commit;
	struct wl_listener disable;
	struct wl_listener destroy;
};

struct wx_ime_popup {
	struct wl_list link; /* wx_server.ime_popups */
	struct wx_server *server;
	struct wlr_input_popup_surface_v2 *popup;
	struct wlr_scene_tree *tree;
	struct wl_listener commit;
	struct wl_listener destroy;
};

static struct wx_text_input *focused_text_input(struct wx_server *server) {
	struct wx_text_input *t;
	wl_list_for_each(t, &server->text_inputs, link) {
		if (t->ti->focused_surface && t->ti->current_enabled) {
			return t;
		}
	}
	return NULL;
}

static void send_im_state(struct wx_server *server, struct wlr_text_input_v3 *ti) {
	struct wlr_input_method_v2 *im = server->input_method;
	if (ti->active_features & WLR_TEXT_INPUT_V3_FEATURE_SURROUNDING_TEXT) {
		wlr_input_method_v2_send_surrounding_text(im, ti->current.surrounding.text,
			ti->current.surrounding.cursor, ti->current.surrounding.anchor);
	}
	wlr_input_method_v2_send_text_change_cause(im, ti->current.text_change_cause);
	if (ti->active_features & WLR_TEXT_INPUT_V3_FEATURE_CONTENT_TYPE) {
		wlr_input_method_v2_send_content_type(im, ti->current.content_type.hint,
			ti->current.content_type.purpose);
	}
	wlr_input_method_v2_send_done(im);
}

/* Places the IME candidate popup under the text cursor. */
static void popup_place(struct wx_ime_popup *p) {
	struct wx_server *server = p->server;
	struct wx_text_input *t = focused_text_input(server);
	if (!t) {
		wlr_scene_node_set_enabled(&p->tree->node, false);
		return;
	}
	struct wx_view *view = view_from_surface(server, t->ti->focused_surface);
	int ox = 0, oy = 0;
	if (view) {
		ox = view->x;
		oy = view->y;
		if (view->type == WX_VIEW_XDG) {
			ox -= view->xdg->base->geometry.x;
			oy -= view->xdg->base->geometry.y;
		}
	}
	struct wlr_box rect = t->ti->current.cursor_rectangle;
	wlr_input_popup_surface_v2_send_text_input_rectangle(p->popup, &rect);
	wlr_scene_node_set_position(&p->tree->node, ox + rect.x, oy + rect.y + rect.height);
	wlr_scene_node_set_enabled(&p->tree->node, true);
}

static void popups_place(struct wx_server *server) {
	struct wx_ime_popup *p;
	wl_list_for_each(p, &server->ime_popups, link) {
		popup_place(p);
	}
}

static void popup_commit(struct wl_listener *listener, void *data) {
	struct wx_ime_popup *p = wl_container_of(listener, p, commit);
	popup_place(p);
}

static void popup_destroy(struct wl_listener *listener, void *data) {
	struct wx_ime_popup *p = wl_container_of(listener, p, destroy);
	wl_list_remove(&p->commit.link);
	wl_list_remove(&p->destroy.link);
	wl_list_remove(&p->link);
	wlr_scene_node_destroy(&p->tree->node);
	free(p);
}

static void im_new_popup(struct wl_listener *listener, void *data) {
	struct wx_server *server = wl_container_of(listener, server, im_new_popup);
	struct wlr_input_popup_surface_v2 *popup = data;
	struct wx_ime_popup *p = calloc(1, sizeof(*p));
	p->server = server;
	p->popup = popup;
	p->tree = wlr_scene_subsurface_tree_create(server->layer_overlay, popup->surface);
	p->commit.notify = popup_commit;
	wl_signal_add(&popup->surface->events.commit, &p->commit);
	p->destroy.notify = popup_destroy;
	wl_signal_add(&popup->events.destroy, &p->destroy);
	wl_list_insert(&server->ime_popups, &p->link);
	popup_place(p);
}

/* ---------- input method side ---------- */

static void im_commit(struct wl_listener *listener, void *data) {
	struct wx_server *server = wl_container_of(listener, server, im_commit);
	struct wlr_input_method_v2 *im = server->input_method;
	struct wx_text_input *t = focused_text_input(server);
	if (!t) {
		return;
	}
	struct wlr_text_input_v3 *ti = t->ti;
	if (im->current.preedit.text) {
		wlr_text_input_v3_send_preedit_string(ti, im->current.preedit.text,
			im->current.preedit.cursor_begin, im->current.preedit.cursor_end);
	}
	if (im->current.commit_text) {
		wlr_text_input_v3_send_commit_string(ti, im->current.commit_text);
	}
	if (im->current.delete.before_length || im->current.delete.after_length) {
		wlr_text_input_v3_send_delete_surrounding_text(ti, im->current.delete.before_length,
			im->current.delete.after_length);
	}
	wlr_text_input_v3_send_done(ti);
}

static void grab_destroy(struct wl_listener *listener, void *data) {
	struct wx_server *server = wl_container_of(listener, server, im_grab_destroy);
	wl_list_remove(&server->im_grab_destroy.link);
	wl_list_init(&server->im_grab_destroy.link);
	struct wlr_input_method_keyboard_grab_v2 *grab = data;
	server->im_grab = NULL;
	if (grab->keyboard) {
		/* give the app the real modifier state back */
		wlr_seat_keyboard_notify_modifiers(server->seat, &grab->keyboard->modifiers);
	}
}

static void im_grab_keyboard(struct wl_listener *listener, void *data) {
	struct wx_server *server = wl_container_of(listener, server, im_grab_keyboard);
	struct wlr_input_method_keyboard_grab_v2 *grab = data;
	server->im_grab = grab;
	struct wlr_keyboard *kb = server->last_keyboard ? server->last_keyboard->wlr_keyboard :
		wlr_seat_get_keyboard(server->seat);
	wlr_input_method_keyboard_grab_v2_set_keyboard(grab, kb);
	wl_list_remove(&server->im_grab_destroy.link);
	server->im_grab_destroy.notify = grab_destroy;
	wl_signal_add(&grab->events.destroy, &server->im_grab_destroy);
}

static void im_destroy(struct wl_listener *listener, void *data) {
	struct wx_server *server = wl_container_of(listener, server, im_destroy);
	wl_list_remove(&server->im_commit.link);
	wl_list_remove(&server->im_grab_keyboard.link);
	wl_list_remove(&server->im_new_popup.link);
	wl_list_remove(&server->im_destroy.link);
	wl_list_init(&server->im_commit.link);
	wl_list_init(&server->im_grab_keyboard.link);
	wl_list_init(&server->im_new_popup.link);
	wl_list_init(&server->im_destroy.link);
	server->input_method = NULL;
	server->im_grab = NULL;
	struct wx_text_input *t = focused_text_input(server);
	if (t) {
		/* drop any half-typed preedit in the app */
		wlr_text_input_v3_send_preedit_string(t->ti, NULL, 0, 0);
		wlr_text_input_v3_send_done(t->ti);
	}
}

static void new_input_method(struct wl_listener *listener, void *data) {
	struct wx_server *server = wl_container_of(listener, server, new_input_method);
	struct wlr_input_method_v2 *im = data;
	if (server->input_method) {
		wlr_log(WLR_INFO, "second input method refused; one is already running");
		wlr_input_method_v2_send_unavailable(im);
		return;
	}
	server->input_method = im;
	server->im_commit.notify = im_commit;
	wl_signal_add(&im->events.commit, &server->im_commit);
	server->im_grab_keyboard.notify = im_grab_keyboard;
	wl_signal_add(&im->events.grab_keyboard, &server->im_grab_keyboard);
	server->im_new_popup.notify = im_new_popup;
	wl_signal_add(&im->events.new_popup_surface, &server->im_new_popup);
	server->im_destroy.notify = im_destroy;
	wl_signal_add(&im->events.destroy, &server->im_destroy);
	struct wx_text_input *t = focused_text_input(server);
	if (t) {
		wlr_input_method_v2_send_activate(im);
		send_im_state(server, t->ti);
	}
}

/* ---------- text input side ---------- */

static void ti_enable(struct wl_listener *listener, void *data) {
	struct wx_text_input *t = wl_container_of(listener, t, enable);
	struct wx_server *server = t->server;
	if (server->input_method) {
		wlr_input_method_v2_send_activate(server->input_method);
		send_im_state(server, t->ti);
	}
	popups_place(server);
}

static void ti_commit(struct wl_listener *listener, void *data) {
	struct wx_text_input *t = wl_container_of(listener, t, commit);
	struct wx_server *server = t->server;
	if (!t->ti->current_enabled) {
		return;
	}
	if (server->input_method) {
		send_im_state(server, t->ti);
	}
	popups_place(server);
}

static void ti_deactivate(struct wx_text_input *t) {
	struct wx_server *server = t->server;
	if (server->input_method) {
		wlr_input_method_v2_send_deactivate(server->input_method);
		wlr_input_method_v2_send_done(server->input_method);
	}
	popups_place(server);
}

static void ti_disable(struct wl_listener *listener, void *data) {
	struct wx_text_input *t = wl_container_of(listener, t, disable);
	ti_deactivate(t);
}

static void ti_destroy(struct wl_listener *listener, void *data) {
	struct wx_text_input *t = wl_container_of(listener, t, destroy);
	if (t->ti->current_enabled) {
		ti_deactivate(t);
	}
	wl_list_remove(&t->enable.link);
	wl_list_remove(&t->commit.link);
	wl_list_remove(&t->disable.link);
	wl_list_remove(&t->destroy.link);
	wl_list_remove(&t->link);
	free(t);
}

static bool same_client(struct wlr_text_input_v3 *ti, struct wlr_surface *surface) {
	return surface && wl_resource_get_client(ti->resource) == wl_resource_get_client(surface->resource);
}

static void new_text_input(struct wl_listener *listener, void *data) {
	struct wx_server *server = wl_container_of(listener, server, new_text_input);
	struct wlr_text_input_v3 *ti = data;
	struct wx_text_input *t = calloc(1, sizeof(*t));
	t->server = server;
	t->ti = ti;
	t->enable.notify = ti_enable;
	wl_signal_add(&ti->events.enable, &t->enable);
	t->commit.notify = ti_commit;
	wl_signal_add(&ti->events.commit, &t->commit);
	t->disable.notify = ti_disable;
	wl_signal_add(&ti->events.disable, &t->disable);
	t->destroy.notify = ti_destroy;
	wl_signal_add(&ti->events.destroy, &t->destroy);
	wl_list_insert(&server->text_inputs, &t->link);
	struct wlr_surface *focus = server->seat->keyboard_state.focused_surface;
	if (same_client(ti, focus)) {
		wlr_text_input_v3_send_enter(ti, focus);
	}
}

static void keyboard_focus_change(struct wl_listener *listener, void *data) {
	struct wx_server *server = wl_container_of(listener, server, kb_focus_change);
	struct wlr_seat_keyboard_focus_change_event *e = data;
	struct wx_text_input *t;
	wl_list_for_each(t, &server->text_inputs, link) {
		if (t->ti->focused_surface && t->ti->focused_surface != e->new_surface) {
			if (t->ti->current_enabled) {
				ti_deactivate(t);
			}
			wlr_text_input_v3_send_leave(t->ti);
		}
		if (!t->ti->focused_surface && same_client(t->ti, e->new_surface)) {
			wlr_text_input_v3_send_enter(t->ti, e->new_surface);
		}
	}
}

bool ime_handle_key(struct wx_server *server, struct wx_keyboard *kb, uint32_t time,
		uint32_t keycode, uint32_t state) {
	struct wlr_input_method_keyboard_grab_v2 *grab = server->im_grab;
	if (!grab) {
		return false;
	}
	/* keys the IME itself injects through its virtual keyboard go to the app */
	if (kb->is_virtual) {
		struct wlr_virtual_keyboard_v1 *vk =
			wlr_input_device_get_virtual_keyboard(&kb->wlr_keyboard->base);
		if (vk && wl_resource_get_client(vk->resource) ==
				wl_resource_get_client(grab->input_method->resource)) {
			return false;
		}
	}
	wlr_input_method_keyboard_grab_v2_set_keyboard(grab, kb->wlr_keyboard);
	wlr_input_method_keyboard_grab_v2_send_key(grab, time, keycode, state);
	return true;
}

bool ime_handle_modifiers(struct wx_server *server, struct wx_keyboard *kb) {
	struct wlr_input_method_keyboard_grab_v2 *grab = server->im_grab;
	if (!grab || kb->is_virtual) {
		return false;
	}
	wlr_input_method_keyboard_grab_v2_set_keyboard(grab, kb->wlr_keyboard);
	wlr_input_method_keyboard_grab_v2_send_modifiers(grab, &kb->wlr_keyboard->modifiers);
	return true;
}

void ime_init(struct wx_server *server) {
	wl_list_init(&server->text_inputs);
	wl_list_init(&server->ime_popups);
	wl_list_init(&server->im_grab_destroy.link);
	struct wlr_text_input_manager_v3 *tim = wlr_text_input_manager_v3_create(server->display);
	server->new_text_input.notify = new_text_input;
	wl_signal_add(&tim->events.new_text_input, &server->new_text_input);
	struct wlr_input_method_manager_v2 *imm = wlr_input_method_manager_v2_create(server->display);
	server->new_input_method.notify = new_input_method;
	wl_signal_add(&imm->events.new_input_method, &server->new_input_method);
	server->kb_focus_change.notify = keyboard_focus_change;
	wl_signal_add(&server->seat->keyboard_state.events.focus_change, &server->kb_focus_change);
}
