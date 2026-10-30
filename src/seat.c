#include <stdlib.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_keyboard_shortcuts_inhibit_v1.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/util/log.h>
#include "server.h"

void seat_focus_surface(struct wx_server *server, struct wlr_surface *surface) {
	struct wlr_seat *seat = server->seat;
	if (!surface) {
		wlr_seat_keyboard_notify_clear_focus(seat);
		return;
	}
	struct wlr_keyboard *kb = wlr_seat_get_keyboard(seat);
	if (kb) {
		wlr_seat_keyboard_notify_enter(seat, surface, kb->keycodes, kb->num_keycodes,
			&kb->modifiers);
	} else {
		wlr_seat_keyboard_notify_enter(seat, surface, NULL, 0, NULL);
	}
	constraint_check_focus(server);
}

static bool layer_grabs_keyboard(struct wx_server *server) {
	struct wx_layer_surface *l = server->focused_layer;
	return l && l->mapped &&
		l->layer_surface->current.keyboard_interactive ==
			ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE;
}

void seat_focus_view(struct wx_server *server, struct wx_view *view) {
	if (server->locked) {
		return;
	}
	struct wx_view *prev = server->focused;
	if (view && (!view->mapped || view->unmanaged)) {
		return;
	}
	/* update focused first so the old frame repaints as inactive */
	server->focused = view;
	if (prev && prev != view) {
		view_set_activated(prev, false);
	}
	if (!view) {
		if (!layer_grabs_keyboard(server)) {
			seat_focus_surface(server, NULL);
		}
		ipc_event_window(server, "focus", NULL);
		return;
	}
	if (view->minimized) {
		view_set_minimized(view, false);
	}
	struct wx_output *vo = view_output(view);
	if (!view->sticky && vo && view->workspace != vo->workspace) {
		workspace_switch(server, vo, view->workspace);
	}
	if (vo) {
		server->active_output = vo;
	}
	view_raise(view);
	wl_list_remove(&view->link);
	wl_list_insert(&server->views, &view->link);
	if (prev != view) {
		view_set_activated(view, true);
	}
	if (!layer_grabs_keyboard(server)) {
		server->focused_layer = NULL;
		seat_focus_surface(server, view_surface(view));
	}
	if (prev != view) {
		ipc_event_window(server, "focus", view);
	}
}

void seat_focus_layer(struct wx_server *server, struct wx_layer_surface *layer) {
	if (server->locked) {
		return;
	}
	if (!layer) {
		server->focused_layer = NULL;
		seat_refocus(server);
		return;
	}
	server->focused_layer = layer;
	seat_focus_surface(server, layer->layer_surface->surface);
}

void seat_refocus(struct wx_server *server) {
	if (server->locked) {
		return;
	}
	struct wx_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (view->mapped && view_is_visible(view)) {
			if (server->focused == view) {
				/* already focused: re-send enter in case a layer had it */
				if (!layer_grabs_keyboard(server)) {
					seat_focus_surface(server, view_surface(view));
				}
				return;
			}
			seat_focus_view(server, view);
			return;
		}
	}
	seat_focus_view(server, NULL);
}

bool seat_shortcuts_inhibited(struct wx_server *server) {
	struct wlr_surface *focused = server->seat->keyboard_state.focused_surface;
	if (!focused || !server->kb_inhibit) {
		return false;
	}
	struct wlr_keyboard_shortcuts_inhibitor_v1 *inh;
	wl_list_for_each(inh, &server->kb_inhibit->inhibitors, link) {
		if (inh->surface == focused && inh->active) {
			return true;
		}
	}
	return false;
}

static void request_cursor(struct wl_listener *listener, void *data) {
	struct wx_server *server = wl_container_of(listener, server, request_cursor);
	struct wlr_seat_pointer_request_set_cursor_event *event = data;
	if (server->seat->pointer_state.focused_client == event->seat_client &&
			server->cursor_mode == WX_CURSOR_PASSTHROUGH) {
		wlr_cursor_set_surface(server->cursor, event->surface, event->hotspot_x,
			event->hotspot_y);
	}
}

static void cursor_shape_request(struct wl_listener *listener, void *data) {
	struct wx_server *server = wl_container_of(listener, server, cursor_shape_request);
	struct wlr_cursor_shape_manager_v1_request_set_shape_event *event = data;
	if (server->seat->pointer_state.focused_client == event->seat_client &&
			server->cursor_mode == WX_CURSOR_PASSTHROUGH) {
		wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr,
			wlr_cursor_shape_v1_name(event->shape));
	}
}

static void request_set_selection(struct wl_listener *listener, void *data) {
	struct wx_server *server = wl_container_of(listener, server, request_set_selection);
	struct wlr_seat_request_set_selection_event *event = data;
	wlr_seat_set_selection(server->seat, event->source, event->serial);
}

static void request_set_primary_selection(struct wl_listener *listener, void *data) {
	struct wx_server *server = wl_container_of(listener, server, request_set_primary_selection);
	struct wlr_seat_request_set_primary_selection_event *event = data;
	wlr_seat_set_primary_selection(server->seat, event->source, event->serial);
}

static void request_start_drag(struct wl_listener *listener, void *data) {
	struct wx_server *server = wl_container_of(listener, server, request_start_drag);
	struct wlr_seat_request_start_drag_event *event = data;
	if (wlr_seat_validate_pointer_grab_serial(server->seat, event->origin, event->serial)) {
		wlr_seat_start_pointer_drag(server->seat, event->drag, event->serial);
		return;
	}
	wlr_data_source_destroy(event->drag->source);
}

static void drag_destroy(struct wl_listener *listener, void *data) {
	struct wx_server *server = wl_container_of(listener, server, drag_destroy);
	wl_list_remove(&server->drag_destroy.link);
	wl_list_init(&server->drag_destroy.link);
	input_process_motion(server, input_now());
}

static void start_drag(struct wl_listener *listener, void *data) {
	struct wx_server *server = wl_container_of(listener, server, start_drag);
	struct wlr_drag *drag = data;
	if (drag->icon) {
		wlr_scene_drag_icon_create(server->drag_icons, drag->icon);
		wlr_scene_node_set_position(&server->drag_icons->node,
			(int)server->cursor->x, (int)server->cursor->y);
	}
	wl_list_remove(&server->drag_destroy.link);
	server->drag_destroy.notify = drag_destroy;
	wl_signal_add(&drag->events.destroy, &server->drag_destroy);
}

void seat_init(struct wx_server *server) {
	server->seat = wlr_seat_create(server->display, "seat0");
	wlr_data_device_manager_create(server->display);
	wlr_primary_selection_v1_device_manager_create(server->display);

	server->cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(server->cursor, server->output_layout);
	const char *theme = server->config.cursor_theme;
	if (!theme) {
		theme = getenv("XCURSOR_THEME");
	}
	server->cursor_mgr = wlr_xcursor_manager_create(theme, server->config.cursor_size);
	wlr_xcursor_manager_load(server->cursor_mgr, 1);

	server->request_cursor.notify = request_cursor;
	wl_signal_add(&server->seat->events.request_set_cursor, &server->request_cursor);
	server->request_set_selection.notify = request_set_selection;
	wl_signal_add(&server->seat->events.request_set_selection, &server->request_set_selection);
	server->request_set_primary_selection.notify = request_set_primary_selection;
	wl_signal_add(&server->seat->events.request_set_primary_selection,
		&server->request_set_primary_selection);
	server->request_start_drag.notify = request_start_drag;
	wl_signal_add(&server->seat->events.request_start_drag, &server->request_start_drag);
	server->start_drag.notify = start_drag;
	wl_signal_add(&server->seat->events.start_drag, &server->start_drag);
	wl_list_init(&server->drag_destroy.link);

	server->cursor_shape = wlr_cursor_shape_manager_v1_create(server->display, 1);
	server->cursor_shape_request.notify = cursor_shape_request;
	wl_signal_add(&server->cursor_shape->events.request_set_shape, &server->cursor_shape_request);
}
