#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <wlr/xwayland.h>
#include <xcb/xproto.h>
#include "server.h"

struct wx_view *view_create(struct wx_server *server, enum wx_view_type type) {
	struct wx_view *view = calloc(1, sizeof(*view));
	view->server = server;
	view->type = type;
	view->id = server->next_view_id++;
	view->tag = (struct wx_node_tag){WX_TAG_MAGIC, WX_NODE_VIEW, view};
	view->stack = WX_STACK_NORMAL;
	struct wx_output *ao = workspace_active_output(server);
	view->workspace = ao ? ao->workspace : 1;
	view->tree = wlr_scene_tree_create(server->win_normal);
	view->tree->node.data = &view->tag;
	wlr_scene_node_set_enabled(&view->tree->node, false);
	wl_list_init(&view->link);
	return view;
}

/* Every listener field; removed on destroy if it was ever attached. */
#define VIEW_LISTENERS(X) X(map) X(unmap) X(commit) X(destroy) X(request_move) \
	X(request_resize) X(request_maximize) X(request_fullscreen) X(request_minimize) \
	X(set_title) X(set_app_id) X(associate) X(dissociate) X(request_configure) \
	X(request_activate) X(request_above) X(request_below) X(request_sticky) X(request_close) \
	X(set_geometry) X(set_decorations) X(set_override_redirect) X(map_request)

void view_destroy(struct wx_view *view) {
	struct wx_server *server = view->server;
#define REMOVE(f) if (view->f.link.next) { wl_list_remove(&view->f.link); }
	VIEW_LISTENERS(REMOVE)
#undef REMOVE
	if (view->mapped) {
		view_unmap(view);
	}
	if (server->grab_view == view) {
		server->grab_view = NULL;
		server->cursor_mode = WX_CURSOR_PASSTHROUGH;
	}
	wxproto_view_destroyed(view);
	wl_list_remove(&view->link);
	deco_destroy(view);
	wlr_scene_node_destroy(&view->tree->node);
	free(view);
}

const char *view_app_id(struct wx_view *view) {
	const char *s = view->type == WX_VIEW_XDG ? view->xdg->app_id : view->xsurface->class;
	return s ? s : "";
}

const char *view_title(struct wx_view *view) {
	const char *s = view->type == WX_VIEW_XDG ? view->xdg->title : view->xsurface->title;
	return s ? s : "";
}

int view_pid(struct wx_view *view) {
	if (view->type == WX_VIEW_XWAYLAND) {
		return view->xsurface->pid;
	}
	pid_t pid = 0;
	wl_client_get_credentials(wl_resource_get_client(view->xdg->resource), &pid, NULL, NULL);
	return pid;
}

struct wlr_surface *view_surface(struct wx_view *view) {
	return view->type == WX_VIEW_XDG ? view->xdg->base->surface : view->xsurface->surface;
}

struct wx_view *view_by_id(struct wx_server *server, uint32_t id) {
	struct wx_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (view->id == id) {
			return view;
		}
	}
	return NULL;
}

struct wx_view *view_at(struct wx_server *server, double lx, double ly,
		struct wlr_surface **surface, double *sx, double *sy) {
	struct wx_node_tag *tag = input_node_at(server, lx, ly, surface, sx, sy);
	return tag && tag->kind == WX_NODE_VIEW ? tag->ptr : NULL;
}

struct wx_view *view_from_xdg_toplevel(struct wlr_xdg_toplevel *t) {
	struct wlr_scene_tree *content = t->base->data;
	if (!content || !content->node.parent) {
		return NULL;
	}
	struct wx_node_tag *tag = content->node.parent->node.data;
	return tag && tag->magic == WX_TAG_MAGIC && tag->kind == WX_NODE_VIEW ? tag->ptr : NULL;
}

struct wx_view *view_from_surface(struct wx_server *server, struct wlr_surface *surface) {
	if (!surface) {
		return NULL;
	}
	surface = wlr_surface_get_root_surface(surface);
	struct wlr_xwayland_surface *xs = wlr_xwayland_surface_try_from_wlr_surface(surface);
	if (xs) {
		return xs->data;
	}
	struct wx_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (view_surface(view) == surface) {
			return view;
		}
	}
	return NULL;
}

void view_frame_extents(struct wx_view *view, int *left, int *right, int *top, int *bottom) {
	if (!view->ssd || view->fullscreen || view->unmanaged) {
		*left = *right = *top = *bottom = 0;
		return;
	}
	int b = view->server->config.border;
	*left = *right = *bottom = b;
	*top = b + view->server->config.title_height;
}

void view_frame_box(struct wx_view *view, struct wlr_box *box) {
	int l, r, t, b;
	view_frame_extents(view, &l, &r, &t, &b);
	*box = (struct wlr_box){view->x - l, view->y - t, view->w + l + r, view->h + t + b};
}

struct wx_output *view_output(struct wx_view *view) {
	return view->output ? view->output : output_for_view(view);
}

/* Called whenever a window moves: crossing to another monitor adopts that
 * monitor's current workspace so the window stays visible. */
void view_update_output(struct wx_view *view) {
	if (!view->mapped || view->unmanaged) {
		return;
	}
	struct wx_output *now = output_for_view(view);
	if (!now || now == view->output) {
		return;
	}
	struct wx_output *old = view->output;
	view->output = now;
	if (old || view->orphaned) {
		view->workspace = now->workspace;
		view->orphaned = false;
	}
	protocols_view_output_changed(view, old);
	view_update_visibility(view);
}

bool view_is_visible(struct wx_view *view) {
	if (!view->mapped || view->minimized) {
		return false;
	}
	if (view->sticky || view->unmanaged) {
		return true;
	}
	struct wx_output *o = view_output(view);
	return !o || view->workspace == o->workspace;
}

bool view_is_tiled(struct wx_view *view) {
	struct wx_output *o = view_output(view);
	return view->mapped && !view->unmanaged && o && o->tiling[view->workspace] &&
		!view->floating && !view->fullscreen && !view->maximized && !view->minimized &&
		view->stack == WX_STACK_NORMAL;
}

void view_update_visibility(struct wx_view *view) {
	wlr_scene_node_set_enabled(&view->tree->node, view_is_visible(view));
}

static struct wlr_scene_tree *stack_tree(struct wx_view *view) {
	struct wx_server *s = view->server;
	if (view->unmanaged) {
		return s->unmanaged;
	}
	if (view->fullscreen) {
		return s->win_fullscreen;
	}
	switch (view->stack) {
	case WX_STACK_BELOW: return s->win_below;
	case WX_STACK_ABOVE: return s->win_above;
	default: return s->win_normal;
	}
}

static void view_restack_tree(struct wx_view *view) {
	struct wlr_scene_tree *t = stack_tree(view);
	if (view->tree->node.parent != t) {
		wlr_scene_node_reparent(&view->tree->node, t);
	}
}

void view_raise(struct wx_view *view) {
	wlr_scene_node_raise_to_top(&view->tree->node);
	if (view->type == WX_VIEW_XWAYLAND) {
		wlr_xwayland_surface_restack(view->xsurface, NULL, XCB_STACK_MODE_ABOVE);
	}
}

static void xw_configure(struct wx_view *view) {
	if (view->type == WX_VIEW_XWAYLAND) {
		xwayland_configure(view);
	}
}

void view_move(struct wx_view *view, int x, int y) {
	view->x = x;
	view->y = y;
	wlr_scene_node_set_position(&view->tree->node, x, y);
	xw_configure(view);
	view_update_output(view);
	wxproto_view_moved(view);
}

void view_resize(struct wx_view *view, int w, int h) {
	if (w < 1 || h < 1) {
		return;
	}
	if (view->type == WX_VIEW_XDG) {
		wlr_xdg_toplevel_set_size(view->xdg, w, h);
	} else {
		view->w = w;
		view->h = h;
		xw_configure(view);
		deco_update(view);
	}
}

void view_set_box(struct wx_view *view, int x, int y, int w, int h) {
	view_move(view, x, y);
	view_resize(view, w, h);
}

void view_close(struct wx_view *view) {
	if (view->type == WX_VIEW_XDG) {
		wlr_xdg_toplevel_send_close(view->xdg);
	} else {
		wlr_xwayland_surface_close(view->xsurface);
	}
}

void view_kill(struct wx_view *view) {
	if (view->type == WX_VIEW_XDG) {
		/* like xkill: drop the client connection */
		wl_client_destroy(wl_resource_get_client(view->xdg->resource));
		return;
	}
	int pid = view_pid(view);
	if (pid > 0) {
		kill(pid, SIGKILL);
	} else {
		wlr_xwayland_surface_close(view->xsurface);
	}
}

void view_set_activated(struct wx_view *view, bool on) {
	if (!view->mapped) {
		return;
	}
	if (view->type == WX_VIEW_XDG) {
		wlr_xdg_toplevel_set_activated(view->xdg, on);
	} else {
		wlr_xwayland_surface_activate(view->xsurface, on);
	}
	deco_update(view);
	view_ftl_update(view);
}

static void view_output_box(struct wx_view *view, struct wlr_box *full, struct wlr_box *usable) {
	struct wx_output *o = output_for_view(view);
	if (!o) {
		o = output_focused(view->server);
	}
	if (!o) {
		*full = *usable = (struct wlr_box){0, 0, 1280, 720};
		return;
	}
	*full = o->full;
	*usable = o->usable;
}

static bool save_box(struct wx_view *view) {
	if (view->maximized || view->fullscreen || view_is_tiled(view)) {
		return false;
	}
	view->saved = (struct wlr_box){view->x, view->y, view->w, view->h};
	return true;
}

static void restore_box(struct wx_view *view) {
	struct wlr_box s = view->saved;
	if (s.width > 0 && s.height > 0) {
		view_set_box(view, s.x, s.y, s.width, s.height);
	}
}

void view_set_maximized(struct wx_view *view, bool on) {
	if (view->unmanaged || view->maximized == on || view->fullscreen) {
		if (view->type == WX_VIEW_XDG && view->xdg->base->initialized) {
			wlr_xdg_surface_schedule_configure(view->xdg->base);
		}
		return;
	}
	if (on) {
		save_box(view);
		view->maximized = true;
		struct wlr_box full, u;
		view_output_box(view, &full, &u);
		int l, r, t, b;
		view_frame_extents(view, &l, &r, &t, &b);
		view_set_box(view, u.x + l, u.y + t, u.width - l - r, u.height - t - b);
	} else {
		view->maximized = false;
		restore_box(view);
	}
	if (view->type == WX_VIEW_XDG) {
		wlr_xdg_toplevel_set_maximized(view->xdg, on);
	} else {
		wlr_xwayland_surface_set_maximized(view->xsurface, on, on);
	}
	deco_update(view);
	view_ftl_update(view);
	layout_arrange(view->server);
	ipc_event_window(view->server, "state", view);
}

void view_set_fullscreen(struct wx_view *view, bool on) {
	if (view->unmanaged || view->fullscreen == on) {
		if (view->type == WX_VIEW_XDG && view->xdg->base->initialized) {
			wlr_xdg_surface_schedule_configure(view->xdg->base);
		}
		return;
	}
	if (on) {
		save_box(view);
		view->fullscreen = true;
		struct wlr_box full, u;
		view_output_box(view, &full, &u);
		view_restack_tree(view);
		view_set_box(view, full.x, full.y, full.width, full.height);
	} else {
		view->fullscreen = false;
		view_restack_tree(view);
		if (view->maximized) {
			view->maximized = false;
			view_set_maximized(view, true);
		} else {
			restore_box(view);
		}
	}
	if (view->type == WX_VIEW_XDG) {
		wlr_xdg_toplevel_set_fullscreen(view->xdg, on);
	} else {
		wlr_xwayland_surface_set_fullscreen(view->xsurface, on);
	}
	deco_update(view);
	view_ftl_update(view);
	layout_arrange(view->server);
	ipc_event_window(view->server, "state", view);
}

void view_set_minimized(struct wx_view *view, bool on) {
	if (view->unmanaged || view->minimized == on) {
		return;
	}
	view->minimized = on;
	if (view->type == WX_VIEW_XWAYLAND) {
		wlr_xwayland_surface_set_minimized(view->xsurface, on);
	}
	view_update_visibility(view);
	view_ftl_update(view);
	if (on && view->server->focused == view) {
		view->server->focused = NULL;
		view_set_activated(view, false);
		/* move to the back of the focus order so refocus picks something else */
		wl_list_remove(&view->link);
		wl_list_insert(view->server->views.prev, &view->link);
		seat_refocus(view->server);
	}
	layout_arrange(view->server);
	ipc_event_window(view->server, "state", view);
}

void view_set_stack(struct wx_view *view, enum wx_stack stack) {
	if (view->unmanaged) {
		return;
	}
	view->stack = stack;
	view_restack_tree(view);
	if (view->type == WX_VIEW_XWAYLAND) {
		wlr_xwayland_surface_set_above(view->xsurface, stack == WX_STACK_ABOVE);
		wlr_xwayland_surface_set_below(view->xsurface, stack == WX_STACK_BELOW);
	}
	layout_arrange(view->server);
	ipc_event_window(view->server, "state", view);
}

void view_set_sticky(struct wx_view *view, bool on) {
	view->sticky = on;
	if (view->type == WX_VIEW_XWAYLAND) {
		wlr_xwayland_surface_set_sticky(view->xsurface, on);
	}
	view_update_visibility(view);
	ipc_event_window(view->server, "state", view);
}

void view_set_floating(struct wx_view *view, bool on) {
	if (view->floating == on) {
		return;
	}
	bool was_tiled = view_is_tiled(view);
	view->floating = on;
	if (on && was_tiled) {
		restore_box(view);
	} else if (!on) {
		save_box(view);
	}
	layout_arrange(view->server);
	ipc_event_window(view->server, "state", view);
}

void view_set_workspace(struct wx_view *view, int ws) {
	if (ws < 1 || ws > WX_WORKSPACES) {
		return;
	}
	view->workspace = ws;
	view_update_visibility(view);
	if (view->server->focused == view && !view_is_visible(view)) {
		view->server->focused = NULL;
		view_set_activated(view, false);
		seat_refocus(view->server);
	}
	layout_arrange(view->server);
	ipc_event_window(view->server, "state", view);
}

void view_title_changed(struct wx_view *view) {
	if (!view->mapped) {
		return;
	}
	deco_update(view);
	view_ftl_update(view);
	ipc_event_window(view->server, "title", view);
}

static bool spot_taken(struct wx_view *self, int x, int y) {
	struct wx_view *v;
	wl_list_for_each(v, &self->server->views, link) {
		if (v != self && v->mapped && view_is_visible(v) && v->x == x && v->y == y) {
			return true;
		}
	}
	return false;
}

static struct wx_view *view_parent(struct wx_view *view) {
	if (view->type == WX_VIEW_XDG) {
		return view->xdg->parent ? view_from_surface(view->server, view->xdg->parent->base->surface)
			: NULL;
	}
	return view->xsurface->parent ? view->xsurface->parent->data : NULL;
}

static void place_view(struct wx_view *view) {
	struct wx_server *server = view->server;
	struct wx_output *o = output_focused(server);
	struct wlr_box u = o ? o->usable : (struct wlr_box){0, 0, 1280, 720};
	int l, r, t, b;
	view_frame_extents(view, &l, &r, &t, &b);
	/* never larger than the usable area */
	if (view->w + l + r > u.width || view->h + t + b > u.height) {
		int w = view->w + l + r > u.width ? u.width - l - r : view->w;
		int h = view->h + t + b > u.height ? u.height - t - b : view->h;
		view_resize(view, w, h);
		if (view->type == WX_VIEW_XDG) {
			view->w = w;
			view->h = h;
		}
	}
	if (view->placed) {
		/* honour the requested spot, but like X11 WMs keep the title bar
		 * out from under panels so the window can still be grabbed */
		struct wx_output *ro = output_at(server, view->x, view->y);
		struct wlr_box ru = ro ? ro->usable : u;
		int x = view->x, y = view->y;
		if (y - t < ru.y) {
			y = ru.y + t;
		}
		if (x - l < ru.x) {
			x = ru.x + l;
		}
		view_move(view, x, y);
		return;
	}
	struct wx_view *parent = view_parent(view);
	int x, y;
	if (parent && parent->mapped) {
		x = parent->x + (parent->w - view->w) / 2;
		y = parent->y + (parent->h - view->h) / 2;
	} else {
		x = u.x + (u.width - view->w) / 2;
		y = u.y + (u.height - view->h) / 2 + t / 2;
		for (int i = 0; i < 20 && spot_taken(view, x, y); i++) {
			x += t ? t : 24;
			y += t ? t : 24;
		}
	}
	if (y - t < u.y) {
		y = u.y + t;
	}
	if (x - l < u.x) {
		x = u.x + l;
	}
	view->placed = true;
	view_move(view, x, y);
}

static bool view_is_dialog(struct wx_view *view) {
	if (view_parent(view)) {
		return true;
	}
	if (view->type == WX_VIEW_XWAYLAND) {
		struct wlr_xwayland_surface *xs = view->xsurface;
		if (wlr_xwayland_surface_has_window_type(xs, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_DIALOG) ||
				wlr_xwayland_surface_has_window_type(xs, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_UTILITY) ||
				wlr_xwayland_surface_has_window_type(xs, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_SPLASH) ||
				wlr_xwayland_surface_has_window_type(xs, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_TOOLBAR)) {
			return true;
		}
		xcb_size_hints_t *sh = xs->size_hints;
		if (sh && sh->min_width > 0 && sh->min_width == sh->max_width &&
				sh->min_height > 0 && sh->min_height == sh->max_height) {
			return true;
		}
	}
	return false;
}

void view_map(struct wx_view *view) {
	struct wx_server *server = view->server;
	view->mapped = true;
	view->workspace = 0; /* chosen after placement unless a rule sets it */
	if (view_is_dialog(view)) {
		view->floating = true;
	}
	if (view->has_req_pos) {
		view->x = view->req_x;
		view->y = view->req_y;
		view->placed = true;
	}
	rules_apply(view);
	if (view->type == WX_VIEW_XDG && view->xdg_deco) {
		wlr_xdg_toplevel_decoration_v1_set_mode(view->xdg_deco, view->ssd ?
			WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE :
			WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
	}

	deco_create(view);
	view_restack_tree(view);
	place_view(view);
	view_update_output(view);
	if (view->workspace == 0) {
		struct wx_output *o = view_output(view);
		view->workspace = o ? o->workspace : 1;
	}
	wl_list_insert(&server->views, &view->link);
	view_update_visibility(view);
	deco_update(view);
	protocols_view_map(view);
	wxproto_view_mapped(view);

	bool want_max = false, want_fs = false;
	if (view->type == WX_VIEW_XDG) {
		want_max = view->xdg->requested.maximized;
		want_fs = view->xdg->requested.fullscreen;
	} else {
		want_max = view->xsurface->maximized_horz && view->xsurface->maximized_vert;
		want_fs = view->xsurface->fullscreen;
		if (view->xsurface->above) {
			view->stack = WX_STACK_ABOVE;
		}
		if (view->xsurface->sticky) {
			view->sticky = true;
		}
		if (view->stack != WX_STACK_NORMAL) {
			view_set_stack(view, view->stack);
		}
		if (view->sticky) {
			view_set_sticky(view, true);
		}
	}
	if (want_fs || view->fullscreen) {
		view->fullscreen = false;
		view_set_fullscreen(view, true);
	} else if (want_max || view->maximized) {
		view->maximized = false;
		view_set_maximized(view, true);
	}
	layout_arrange(server);
	ipc_event_window(server, "new", view);
	if (view_is_visible(view)) {
		seat_focus_view(server, view);
	}
	wlr_log(WLR_DEBUG, "mapped view %u '%s' (%s)", view->id, view_title(view), view_app_id(view));
}

void view_unmap(struct wx_view *view) {
	struct wx_server *server = view->server;
	if (!view->mapped) {
		return;
	}
	view->mapped = false;
	if (server->grab_view == view) {
		server->grab_view = NULL;
		server->cursor_mode = WX_CURSOR_PASSTHROUGH;
	}
	wlr_scene_node_set_enabled(&view->tree->node, false);
	wl_list_remove(&view->link);
	wl_list_init(&view->link);
	if (view->unmanaged) {
		struct wlr_surface *s = view_surface(view);
		if (s && server->seat->keyboard_state.focused_surface == s) {
			seat_refocus(server);
		}
		return;
	}
	protocols_view_unmap(view);
	ipc_event_window(server, "close", view);
	if (server->focused == view) {
		server->focused = NULL;
		seat_refocus(server);
	}
	layout_arrange(server);
}

void view_cycle_focus(struct wx_server *server, bool forward) {
	struct wx_view *target = NULL, *v;
	if (forward) {
		wl_list_for_each_reverse(v, &server->views, link) {
			if (view_is_visible(v) && v != server->focused) {
				target = v;
				break;
			}
		}
	} else if (server->focused) {
		struct wx_view *cur = server->focused;
		wl_list_for_each(v, &server->views, link) {
			if (view_is_visible(v) && v != cur) {
				target = v;
				break;
			}
		}
		if (target) {
			wl_list_remove(&cur->link);
			wl_list_insert(server->views.prev, &cur->link);
		}
	}
	if (target) {
		seat_focus_view(server, target);
	}
}
