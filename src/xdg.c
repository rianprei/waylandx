#include <stdlib.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/edges.h>
#include <wlr/util/log.h>
#include "server.h"

struct wx_popup {
	struct wlr_xdg_popup *popup;
	struct wx_server *server;
	struct wl_listener commit;
	struct wl_listener destroy;
};

struct wx_decoration {
	struct wlr_xdg_toplevel_decoration_v1 *deco;
	struct wx_server *server;
	struct wl_listener request_mode;
	struct wl_listener destroy;
};

static void toplevel_map(struct wl_listener *listener, void *data) {
	struct wx_view *view = wl_container_of(listener, view, map);
	struct wlr_box geo = view->xdg->base->geometry;
	view->w = geo.width;
	view->h = geo.height;
	wlr_scene_node_set_position(&view->content->node, -geo.x, -geo.y);
	view_map(view);
}

static void toplevel_unmap(struct wl_listener *listener, void *data) {
	struct wx_view *view = wl_container_of(listener, view, unmap);
	view_unmap(view);
}

static void toplevel_commit(struct wl_listener *listener, void *data) {
	struct wx_view *view = wl_container_of(listener, view, commit);
	struct wlr_xdg_surface *base = view->xdg->base;
	if (base->initial_commit) {
		int w = view->req_w > 0 ? view->req_w : 0, h = view->req_h > 0 ? view->req_h : 0;
		wlr_xdg_toplevel_set_size(view->xdg, w, h);
		wlr_xdg_toplevel_set_wm_capabilities(view->xdg,
			WLR_XDG_TOPLEVEL_WM_CAPABILITIES_MAXIMIZE |
			WLR_XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN |
			WLR_XDG_TOPLEVEL_WM_CAPABILITIES_MINIMIZE);
		struct wx_output *o = output_focused(view->server);
		if (o) {
			wlr_xdg_toplevel_set_bounds(view->xdg, o->usable.width, o->usable.height);
		}
		if (view->xdg_deco) {
			wlr_xdg_toplevel_decoration_v1_set_mode(view->xdg_deco, view->ssd ?
				WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE :
				WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
		}
		return;
	}
	if (!view->mapped) {
		return;
	}
	struct wlr_box geo = base->geometry;
	wlr_scene_node_set_position(&view->content->node, -geo.x, -geo.y);
	if (geo.width == view->w && geo.height == view->h) {
		return;
	}
	struct wx_server *server = view->server;
	if (server->grab_view == view && server->cursor_mode == WX_CURSOR_RESIZE) {
		/* keep the opposite edge anchored while resizing from left/top */
		int x = view->x, y = view->y;
		if (server->resize_edges & WLR_EDGE_LEFT) {
			x = view->x + view->w - geo.width;
		}
		if (server->resize_edges & WLR_EDGE_TOP) {
			y = view->y + view->h - geo.height;
		}
		view->w = geo.width;
		view->h = geo.height;
		view_move(view, x, y);
	} else {
		view->w = geo.width;
		view->h = geo.height;
	}
	deco_update(view);
}

static void toplevel_destroy(struct wl_listener *listener, void *data) {
	struct wx_view *view = wl_container_of(listener, view, destroy);
	view_destroy(view);
}

static void toplevel_request_move(struct wl_listener *listener, void *data) {
	struct wx_view *view = wl_container_of(listener, view, request_move);
	input_begin_interactive(view, WX_CURSOR_MOVE, 0);
}

static void toplevel_request_resize(struct wl_listener *listener, void *data) {
	struct wx_view *view = wl_container_of(listener, view, request_resize);
	struct wlr_xdg_toplevel_resize_event *event = data;
	input_begin_interactive(view, WX_CURSOR_RESIZE, event->edges);
}

static void toplevel_request_maximize(struct wl_listener *listener, void *data) {
	struct wx_view *view = wl_container_of(listener, view, request_maximize);
	if (!view->mapped) {
		if (view->xdg->base->initialized) {
			wlr_xdg_surface_schedule_configure(view->xdg->base);
		}
		return;
	}
	view_set_maximized(view, view->xdg->requested.maximized);
}

static void toplevel_request_fullscreen(struct wl_listener *listener, void *data) {
	struct wx_view *view = wl_container_of(listener, view, request_fullscreen);
	if (!view->mapped) {
		if (view->xdg->base->initialized) {
			wlr_xdg_surface_schedule_configure(view->xdg->base);
		}
		return;
	}
	view_set_fullscreen(view, view->xdg->requested.fullscreen);
}

static void toplevel_request_minimize(struct wl_listener *listener, void *data) {
	struct wx_view *view = wl_container_of(listener, view, request_minimize);
	if (view->mapped) {
		view_set_minimized(view, true);
	}
}

static void toplevel_set_title(struct wl_listener *listener, void *data) {
	struct wx_view *view = wl_container_of(listener, view, set_title);
	view_title_changed(view);
}

static void toplevel_set_app_id(struct wl_listener *listener, void *data) {
	struct wx_view *view = wl_container_of(listener, view, set_app_id);
	view_title_changed(view);
}

#define LISTEN(sig, field, fn) do { view->field.notify = fn; wl_signal_add(&(sig), &view->field); } while (0)

static void new_toplevel(struct wl_listener *listener, void *data) {
	struct wx_server *server = wl_container_of(listener, server, new_xdg_toplevel);
	struct wlr_xdg_toplevel *xdg = data;
	struct wx_view *view = view_create(server, WX_VIEW_XDG);
	view->xdg = xdg;
	view->content = wlr_scene_xdg_surface_create(view->tree, xdg->base);
	xdg->base->data = view->content;
	struct wlr_surface *s = xdg->base->surface;
	LISTEN(s->events.map, map, toplevel_map);
	LISTEN(s->events.unmap, unmap, toplevel_unmap);
	LISTEN(s->events.commit, commit, toplevel_commit);
	LISTEN(xdg->events.destroy, destroy, toplevel_destroy);
	LISTEN(xdg->events.request_move, request_move, toplevel_request_move);
	LISTEN(xdg->events.request_resize, request_resize, toplevel_request_resize);
	LISTEN(xdg->events.request_maximize, request_maximize, toplevel_request_maximize);
	LISTEN(xdg->events.request_fullscreen, request_fullscreen, toplevel_request_fullscreen);
	LISTEN(xdg->events.request_minimize, request_minimize, toplevel_request_minimize);
	LISTEN(xdg->events.set_title, set_title, toplevel_set_title);
	LISTEN(xdg->events.set_app_id, set_app_id, toplevel_set_app_id);
}

static void popup_commit(struct wl_listener *listener, void *data) {
	struct wx_popup *p = wl_container_of(listener, p, commit);
	if (!p->popup->base->initial_commit) {
		return;
	}
	/* keep the popup inside the output its toplevel is on */
	struct wlr_xdg_surface *parent = p->popup->parent ?
		wlr_xdg_surface_try_from_wlr_surface(p->popup->parent) : NULL;
	struct wx_view *view = NULL;
	if (parent) {
		struct wlr_surface *root = parent->surface;
		struct wlr_xdg_popup *pp;
		while ((pp = wlr_xdg_popup_try_from_wlr_surface(root)) && pp->parent) {
			root = pp->parent;
		}
		view = view_from_surface(p->server, root);
	}
	if (view) {
		struct wx_output *o = output_for_view(view);
		if (o) {
			struct wlr_box box = o->full;
			struct wlr_box geo = view->xdg->base->geometry;
			box.x -= view->x - geo.x;
			box.y -= view->y - geo.y;
			wlr_xdg_popup_unconstrain_from_box(p->popup, &box);
		}
	}
	wlr_xdg_surface_schedule_configure(p->popup->base);
}

static void popup_destroy(struct wl_listener *listener, void *data) {
	struct wx_popup *p = wl_container_of(listener, p, destroy);
	wl_list_remove(&p->commit.link);
	wl_list_remove(&p->destroy.link);
	free(p);
}

static void new_popup(struct wl_listener *listener, void *data) {
	struct wx_server *server = wl_container_of(listener, server, new_xdg_popup);
	struct wlr_xdg_popup *popup = data;
	struct wlr_xdg_surface *parent = popup->parent ?
		wlr_xdg_surface_try_from_wlr_surface(popup->parent) : NULL;
	/* layer-shell popups have no parent yet; layer.c gives them a tree */
	if (parent && parent->data) {
		popup->base->data = wlr_scene_xdg_surface_create(parent->data, popup->base);
	}
	struct wx_popup *p = calloc(1, sizeof(*p));
	p->popup = popup;
	p->server = server;
	p->commit.notify = popup_commit;
	wl_signal_add(&popup->base->surface->events.commit, &p->commit);
	p->destroy.notify = popup_destroy;
	wl_signal_add(&popup->events.destroy, &p->destroy);
}

/* ---- decorations: server-side by default, like an X11 window manager ---- */

static void deco_apply(struct wx_decoration *d) {
	struct wx_view *view = view_from_xdg_toplevel(d->deco->toplevel);
	/* Clients that speak xdg-decoration always get server-side frames, like
	 * under an X11 WM; a rule with decorations=client flips this at map. */
	bool ssd = !view || !view->force_csd;
	if (view) {
		view->xdg_deco = d->deco;
		view->ssd = ssd;
		deco_update(view);
	}
	if (d->deco->toplevel->base->initialized) {
		wlr_xdg_toplevel_decoration_v1_set_mode(d->deco, ssd ?
			WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE :
			WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
	}
}

static void deco_request_mode(struct wl_listener *listener, void *data) {
	struct wx_decoration *d = wl_container_of(listener, d, request_mode);
	deco_apply(d);
}

static void deco_destroy_notify(struct wl_listener *listener, void *data) {
	struct wx_decoration *d = wl_container_of(listener, d, destroy);
	struct wx_view *view = view_from_xdg_toplevel(d->deco->toplevel);
	if (view) {
		view->xdg_deco = NULL;
		view->ssd = false;
		deco_update(view);
	}
	wl_list_remove(&d->request_mode.link);
	wl_list_remove(&d->destroy.link);
	free(d);
}

static void new_decoration(struct wl_listener *listener, void *data) {
	struct wx_server *server = wl_container_of(listener, server, new_xdg_decoration);
	struct wlr_xdg_toplevel_decoration_v1 *deco = data;
	struct wx_decoration *d = calloc(1, sizeof(*d));
	d->deco = deco;
	d->server = server;
	d->request_mode.notify = deco_request_mode;
	wl_signal_add(&deco->events.request_mode, &d->request_mode);
	d->destroy.notify = deco_destroy_notify;
	wl_signal_add(&deco->events.destroy, &d->destroy);
	deco_apply(d);
}

void xdg_init(struct wx_server *server) {
	server->xdg_shell = wlr_xdg_shell_create(server->display, 6);
	server->new_xdg_toplevel.notify = new_toplevel;
	wl_signal_add(&server->xdg_shell->events.new_toplevel, &server->new_xdg_toplevel);
	server->new_xdg_popup.notify = new_popup;
	wl_signal_add(&server->xdg_shell->events.new_popup, &server->new_xdg_popup);

	struct wlr_xdg_decoration_manager_v1 *xdm = wlr_xdg_decoration_manager_v1_create(server->display);
	server->new_xdg_decoration.notify = new_decoration;
	wl_signal_add(&xdm->events.new_toplevel_decoration, &server->new_xdg_decoration);
	struct wlr_server_decoration_manager *sdm = wlr_server_decoration_manager_create(server->display);
	wlr_server_decoration_manager_set_default_mode(sdm, WLR_SERVER_DECORATION_MANAGER_MODE_SERVER);
}
