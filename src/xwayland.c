#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/util/log.h>
#include <wlr/xcursor.h>
#include <wlr/xwayland.h>
#include <xcb/xcb_icccm.h>
#include <xcb/xproto.h>
#include "server.h"

static bool wants_ssd(struct wlr_xwayland_surface *xs) {
	if (xs->decorations != WLR_XWAYLAND_SURFACE_DECORATIONS_ALL) {
		return false;
	}
	return !wlr_xwayland_surface_has_window_type(xs, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_SPLASH) &&
		!wlr_xwayland_surface_has_window_type(xs, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_DOCK) &&
		!wlr_xwayland_surface_has_window_type(xs, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_DESKTOP);
}

static bool client_chose_position(struct wlr_xwayland_surface *xs) {
	xcb_size_hints_t *sh = xs->size_hints;
	if (sh && (sh->flags & (XCB_ICCCM_SIZE_HINT_US_POSITION | XCB_ICCCM_SIZE_HINT_P_POSITION))) {
		return true;
	}
	return xs->x != 0 || xs->y != 0;
}

static void unmanaged_map(struct wx_view *view) {
	struct wx_server *server = view->server;
	struct wlr_xwayland_surface *xs = view->xsurface;
	view->mapped = true;
	view->x = xs->x;
	view->y = xs->y;
	view->w = xs->width;
	view->h = xs->height;
	wlr_scene_node_reparent(&view->tree->node, server->unmanaged);
	wlr_scene_node_set_position(&view->tree->node, view->x, view->y);
	wlr_scene_node_set_enabled(&view->tree->node, true);
	wlr_scene_node_raise_to_top(&view->tree->node);
	wl_list_insert(&server->unmanaged_views, &view->link);
	if (wlr_xwayland_surface_override_redirect_wants_focus(xs)) {
		seat_focus_surface(server, xs->surface);
	}
}

static void xsurface_map(struct wl_listener *listener, void *data) {
	struct wx_view *view = wl_container_of(listener, view, map);
	struct wlr_xwayland_surface *xs = view->xsurface;
	view->content = wlr_scene_subsurface_tree_create(view->tree, xs->surface);
	if (view->unmanaged) {
		unmanaged_map(view);
		return;
	}
	view->w = xs->width;
	view->h = xs->height;
	view->ssd = wants_ssd(xs);
	if (client_chose_position(xs)) {
		/* X11 freedom: the window lands exactly where the app asked */
		view->has_req_pos = true;
		view->req_x = xs->x;
		view->req_y = xs->y;
	}
	view_map(view);
}

static void xsurface_unmap(struct wl_listener *listener, void *data) {
	struct wx_view *view = wl_container_of(listener, view, unmap);
	view_unmap(view);
	if (view->content) {
		wlr_scene_node_destroy(&view->content->node);
		view->content = NULL;
	}
}

static void xsurface_associate(struct wl_listener *listener, void *data) {
	struct wx_view *view = wl_container_of(listener, view, associate);
	struct wlr_surface *s = view->xsurface->surface;
	view->map.notify = xsurface_map;
	wl_signal_add(&s->events.map, &view->map);
	view->unmap.notify = xsurface_unmap;
	wl_signal_add(&s->events.unmap, &view->unmap);
}

static void xsurface_dissociate(struct wl_listener *listener, void *data) {
	struct wx_view *view = wl_container_of(listener, view, dissociate);
	wl_list_remove(&view->map.link);
	wl_list_remove(&view->unmap.link);
	view->map.link.next = view->unmap.link.next = NULL;
}

static void xsurface_destroy(struct wl_listener *listener, void *data) {
	struct wx_view *view = wl_container_of(listener, view, destroy);
	view->xsurface->data = NULL;
	view_destroy(view);
}

static void xsurface_request_configure(struct wl_listener *listener, void *data) {
	struct wx_view *view = wl_container_of(listener, view, request_configure);
	struct wlr_xwayland_surface_configure_event *ev = data;
	struct wlr_xwayland_surface *xs = view->xsurface;
	if (!view->mapped) {
		wlr_xwayland_surface_configure(xs, ev->x, ev->y, ev->width, ev->height);
		return;
	}
	if (view->unmanaged) {
		wlr_xwayland_surface_configure(xs, ev->x, ev->y, ev->width, ev->height);
		return;
	}
	if (view->fullscreen || view->maximized || view_is_tiled(view)) {
		/* managed geometry wins; tell the client where it really is */
		wlr_xwayland_surface_configure(xs, (int16_t)view->x, (int16_t)view->y,
			(uint16_t)view->w, (uint16_t)view->h);
		return;
	}
	int x = (ev->mask & XCB_CONFIG_WINDOW_X) ? ev->x : view->x;
	int y = (ev->mask & XCB_CONFIG_WINDOW_Y) ? ev->y : view->y;
	int w = (ev->mask & XCB_CONFIG_WINDOW_WIDTH) ? ev->width : view->w;
	int h = (ev->mask & XCB_CONFIG_WINDOW_HEIGHT) ? ev->height : view->h;
	view_set_box(view, x, y, w, h);
	ipc_event_window(view->server, "geometry", view);
}

static void xsurface_set_geometry(struct wl_listener *listener, void *data) {
	struct wx_view *view = wl_container_of(listener, view, set_geometry);
	struct wlr_xwayland_surface *xs = view->xsurface;
	if (!view->mapped || !view->unmanaged) {
		return;
	}
	view->x = xs->x;
	view->y = xs->y;
	view->w = xs->width;
	view->h = xs->height;
	wlr_scene_node_set_position(&view->tree->node, view->x, view->y);
}

static void xsurface_request_move(struct wl_listener *listener, void *data) {
	struct wx_view *view = wl_container_of(listener, view, request_move);
	if (view->mapped && !view->unmanaged) {
		input_begin_interactive(view, WX_CURSOR_MOVE, 0);
	}
}

static void xsurface_request_resize(struct wl_listener *listener, void *data) {
	struct wx_view *view = wl_container_of(listener, view, request_resize);
	struct wlr_xwayland_resize_event *ev = data;
	if (view->mapped && !view->unmanaged) {
		input_begin_interactive(view, WX_CURSOR_RESIZE, ev->edges);
	}
}

static void xsurface_request_maximize(struct wl_listener *listener, void *data) {
	struct wx_view *view = wl_container_of(listener, view, request_maximize);
	struct wlr_xwayland_surface *xs = view->xsurface;
	bool on = xs->maximized_horz && xs->maximized_vert;
	if (view->mapped) {
		view_set_maximized(view, on);
	} else {
		view->maximized = on;
	}
}

static void xsurface_request_fullscreen(struct wl_listener *listener, void *data) {
	struct wx_view *view = wl_container_of(listener, view, request_fullscreen);
	if (view->mapped) {
		view_set_fullscreen(view, view->xsurface->fullscreen);
	} else {
		view->fullscreen = view->xsurface->fullscreen;
	}
}

static void xsurface_request_minimize(struct wl_listener *listener, void *data) {
	struct wx_view *view = wl_container_of(listener, view, request_minimize);
	struct wlr_xwayland_minimize_event *ev = data;
	if (view->mapped) {
		view_set_minimized(view, ev->minimize);
	}
}

static void xsurface_request_activate(struct wl_listener *listener, void *data) {
	struct wx_view *view = wl_container_of(listener, view, request_activate);
	if (view->mapped && !view->unmanaged) {
		seat_focus_view(view->server, view);
	}
}

static void xsurface_request_close(struct wl_listener *listener, void *data) {
	struct wx_view *view = wl_container_of(listener, view, request_close);
	wlr_xwayland_surface_close(view->xsurface);
}

static void xsurface_request_above(struct wl_listener *listener, void *data) {
	struct wx_view *view = wl_container_of(listener, view, request_above);
	if (view->mapped) {
		view_set_stack(view, view->xsurface->above ? WX_STACK_ABOVE : WX_STACK_NORMAL);
	}
}

static void xsurface_request_below(struct wl_listener *listener, void *data) {
	struct wx_view *view = wl_container_of(listener, view, request_below);
	if (view->mapped) {
		view_set_stack(view, view->xsurface->below ? WX_STACK_BELOW : WX_STACK_NORMAL);
	}
}

static void xsurface_request_sticky(struct wl_listener *listener, void *data) {
	struct wx_view *view = wl_container_of(listener, view, request_sticky);
	if (view->mapped) {
		view_set_sticky(view, view->xsurface->sticky);
	}
}

static void xsurface_set_title(struct wl_listener *listener, void *data) {
	struct wx_view *view = wl_container_of(listener, view, set_title);
	view_title_changed(view);
}

static void xsurface_set_class(struct wl_listener *listener, void *data) {
	struct wx_view *view = wl_container_of(listener, view, set_app_id);
	view_title_changed(view);
}

static void xsurface_set_decorations(struct wl_listener *listener, void *data) {
	struct wx_view *view = wl_container_of(listener, view, set_decorations);
	if (view->unmanaged || view->force_csd) {
		return;
	}
	view->ssd = wants_ssd(view->xsurface);
	if (view->mapped) {
		deco_update(view);
	}
}

/*
 * wlroots rewrites _NET_WM_STATE from its own (still empty) state on MapNotify,
 * which can happen before it read the client's property. Read it ourselves
 * at MapRequest so "start above / sticky / fullscreen" survives, as on X11.
 */
static void xsurface_map_request(struct wl_listener *listener, void *data) {
	struct wx_view *view = wl_container_of(listener, view, map_request);
	struct wx_server *server = view->server;
	xcb_connection_t *conn = wlr_xwayland_get_xwm_connection(server->xwayland);
	if (!conn) {
		return;
	}
	static const char *names[] = {"_NET_WM_STATE", "_NET_WM_STATE_ABOVE", "_NET_WM_STATE_BELOW",
		"_NET_WM_STATE_STICKY", "_NET_WM_STATE_FULLSCREEN", "_NET_WM_STATE_MAXIMIZED_VERT"};
	static xcb_atom_t atoms[6];
	if (!atoms[0]) {
		xcb_intern_atom_cookie_t ck[6];
		for (int i = 0; i < 6; i++) {
			ck[i] = xcb_intern_atom(conn, 0, (uint16_t)strlen(names[i]), names[i]);
		}
		for (int i = 0; i < 6; i++) {
			xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(conn, ck[i], NULL);
			atoms[i] = r ? r->atom : XCB_ATOM_NONE;
			free(r);
		}
	}
	xcb_get_property_reply_t *reply = xcb_get_property_reply(conn,
		xcb_get_property(conn, 0, view->xsurface->window_id, atoms[0], XCB_ATOM_ATOM, 0, 64), NULL);
	if (!reply) {
		return;
	}
	const xcb_atom_t *st = xcb_get_property_value(reply);
	int n = xcb_get_property_value_length(reply) / (int)sizeof(xcb_atom_t);
	for (int i = 0; i < n; i++) {
		if (st[i] == atoms[1]) {
			view->stack = WX_STACK_ABOVE;
		} else if (st[i] == atoms[2]) {
			view->stack = WX_STACK_BELOW;
		} else if (st[i] == atoms[3]) {
			view->sticky = true;
		} else if (st[i] == atoms[4]) {
			view->fullscreen = true;
		} else if (st[i] == atoms[5]) {
			view->maximized = true;
		}
	}
	free(reply);
}

static void xsurface_set_override_redirect(struct wl_listener *listener, void *data) {
	struct wx_view *view = wl_container_of(listener, view, set_override_redirect);
	bool was_mapped = view->mapped;
	if (was_mapped) {
		xsurface_unmap(&view->unmap, NULL);
	}
	view->unmanaged = view->xsurface->override_redirect;
	if (was_mapped) {
		xsurface_map(&view->map, NULL);
	}
}

#define LISTEN(sig, field, fn) do { view->field.notify = fn; wl_signal_add(&(sig), &view->field); } while (0)

static void new_xsurface(struct wl_listener *listener, void *data) {
	struct wx_server *server = wl_container_of(listener, server, new_xwayland_surface);
	struct wlr_xwayland_surface *xs = data;
	struct wx_view *view = view_create(server, WX_VIEW_XWAYLAND);
	view->xsurface = xs;
	view->unmanaged = xs->override_redirect;
	xs->data = view;
	LISTEN(xs->events.destroy, destroy, xsurface_destroy);
	LISTEN(xs->events.associate, associate, xsurface_associate);
	LISTEN(xs->events.dissociate, dissociate, xsurface_dissociate);
	LISTEN(xs->events.request_configure, request_configure, xsurface_request_configure);
	LISTEN(xs->events.set_geometry, set_geometry, xsurface_set_geometry);
	LISTEN(xs->events.request_move, request_move, xsurface_request_move);
	LISTEN(xs->events.request_resize, request_resize, xsurface_request_resize);
	LISTEN(xs->events.request_maximize, request_maximize, xsurface_request_maximize);
	LISTEN(xs->events.request_fullscreen, request_fullscreen, xsurface_request_fullscreen);
	LISTEN(xs->events.request_minimize, request_minimize, xsurface_request_minimize);
	LISTEN(xs->events.request_activate, request_activate, xsurface_request_activate);
	LISTEN(xs->events.request_close, request_close, xsurface_request_close);
	LISTEN(xs->events.request_above, request_above, xsurface_request_above);
	LISTEN(xs->events.request_below, request_below, xsurface_request_below);
	LISTEN(xs->events.request_sticky, request_sticky, xsurface_request_sticky);
	LISTEN(xs->events.set_title, set_title, xsurface_set_title);
	LISTEN(xs->events.set_class, set_app_id, xsurface_set_class);
	LISTEN(xs->events.set_decorations, set_decorations, xsurface_set_decorations);
	LISTEN(xs->events.set_override_redirect, set_override_redirect, xsurface_set_override_redirect);
	LISTEN(xs->events.map_request, map_request, xsurface_map_request);
}

static void xwayland_ready(struct wl_listener *listener, void *data) {
	struct wx_server *server = wl_container_of(listener, server, xwayland_ready);
	wlr_xwayland_set_seat(server->xwayland, server->seat);
	layers_update_xwayland_workareas(server);
	struct wlr_xcursor *xc = wlr_xcursor_manager_get_xcursor(server->cursor_mgr, "default", 1);
	if (xc && xc->image_count > 0) {
		struct wlr_xcursor_image *img = xc->images[0];
		wlr_xwayland_set_cursor(server->xwayland, wlr_xcursor_image_get_buffer(img),
			img->hotspot_x, img->hotspot_y);
	}
	wlr_log(WLR_INFO, "Xwayland ready on DISPLAY=%s", server->xwayland->display_name);
}

void xwayland_init(struct wx_server *server) {
	server->xwayland = wlr_xwayland_create(server->display, server->compositor, true);
	if (!server->xwayland) {
		wlr_log(WLR_ERROR, "cannot start Xwayland; X11 apps disabled");
		return;
	}
	server->new_xwayland_surface.notify = new_xsurface;
	wl_signal_add(&server->xwayland->events.new_surface, &server->new_xwayland_surface);
	x11clip_init(server);
	server->xwayland_ready.notify = xwayland_ready;
	wl_signal_add(&server->xwayland->events.ready, &server->xwayland_ready);
	setenv("DISPLAY", server->xwayland->display_name, 1);
}
