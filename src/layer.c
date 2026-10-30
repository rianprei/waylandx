#include <stdlib.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <wlr/xwayland.h>
#include "server.h"

static struct wlr_scene_tree *layer_tree(struct wx_server *server,
		enum zwlr_layer_shell_v1_layer layer) {
	switch (layer) {
	case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND: return server->layer_bg;
	case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM: return server->layer_bottom;
	case ZWLR_LAYER_SHELL_V1_LAYER_TOP: return server->layer_top;
	default: return server->layer_overlay;
	}
}

void layers_update_xwayland_workareas(struct wx_server *server) {
	/* lazy Xwayland: no XWM until the first X11 client connects */
	if (!server->xwayland || !server->xwayland->xwm) {
		return;
	}
	struct wlr_box areas[16];
	size_t n = 0;
	struct wx_output *o;
	wl_list_for_each(o, &server->outputs, link) {
		if (n < 16 && !wlr_box_empty(&o->usable)) {
			areas[n++] = o->usable;
		}
	}
	wlr_xwayland_set_workareas(server->xwayland, areas, n);
}

void layers_arrange(struct wx_output *output) {
	struct wx_server *server = output->server;
	struct wlr_box full = output->full;
	struct wlr_box usable = full;
	static const enum zwlr_layer_shell_v1_layer order[] = {
		ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, ZWLR_LAYER_SHELL_V1_LAYER_TOP,
		ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM, ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND,
	};
	/* exclusive zones first so the rest see the final usable area */
	for (int pass = 0; pass < 2; pass++) {
		for (int i = 0; i < 4; i++) {
			struct wx_layer_surface *l;
			wl_list_for_each(l, &server->layers, link) {
				struct wlr_layer_surface_v1 *ls = l->layer_surface;
				if (l->output != output || !ls->initialized || ls->current.layer != order[i]) {
					continue;
				}
				bool exclusive = ls->current.exclusive_zone > 0;
				if (exclusive != (pass == 0)) {
					continue;
				}
				wlr_scene_layer_surface_v1_configure(l->scene, &full, &usable);
			}
		}
	}
	bool changed = !wlr_box_equal(&usable, &output->usable);
	output->usable = usable;
	if (changed) {
		layers_update_xwayland_workareas(server);
		layout_arrange(server);
	}

	/* the topmost exclusive-keyboard surface on top/overlay gets the keyboard */
	struct wx_layer_surface *l, *grab = NULL;
	wl_list_for_each(l, &server->layers, link) {
		struct wlr_layer_surface_v1 *ls = l->layer_surface;
		if (l->mapped && ls->current.keyboard_interactive ==
				ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE &&
				ls->current.layer >= ZWLR_LAYER_SHELL_V1_LAYER_TOP) {
			if (!grab || ls->current.layer > grab->layer_surface->current.layer) {
				grab = l;
			}
		}
	}
	if (grab && server->focused_layer != grab) {
		seat_focus_layer(server, grab);
	}
}

static void layer_map(struct wl_listener *listener, void *data) {
	struct wx_layer_surface *l = wl_container_of(listener, l, map);
	l->mapped = true;
	if (l->output) {
		layers_arrange(l->output);
	}
}

static void layer_unmap(struct wl_listener *listener, void *data) {
	struct wx_layer_surface *l = wl_container_of(listener, l, unmap);
	l->mapped = false;
	if (l->server->focused_layer == l) {
		seat_focus_layer(l->server, NULL);
	}
	if (l->output) {
		layers_arrange(l->output);
	}
}

static void layer_commit(struct wl_listener *listener, void *data) {
	struct wx_layer_surface *l = wl_container_of(listener, l, commit);
	struct wlr_layer_surface_v1 *ls = l->layer_surface;
	if (!l->output) {
		return;
	}
	if (ls->current.committed & WLR_LAYER_SURFACE_V1_STATE_LAYER) {
		struct wlr_scene_tree *t = layer_tree(l->server, ls->current.layer);
		wlr_scene_node_reparent(&l->scene->tree->node, t);
	}
	if (ls->initial_commit || ls->current.committed) {
		layers_arrange(l->output);
	}
	if (l->mapped && l->server->focused_layer == l &&
			ls->current.keyboard_interactive == ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE) {
		seat_focus_layer(l->server, NULL);
	}
}

static void layer_new_popup(struct wl_listener *listener, void *data) {
	struct wx_layer_surface *l = wl_container_of(listener, l, new_popup);
	struct wlr_xdg_popup *popup = data;
	popup->base->data = wlr_scene_xdg_surface_create(l->scene->tree, popup->base);
}

static void layer_destroy(struct wl_listener *listener, void *data) {
	struct wx_layer_surface *l = wl_container_of(listener, l, destroy);
	struct wx_server *server = l->server;
	if (server->focused_layer == l) {
		server->focused_layer = NULL;
	}
	wl_list_remove(&l->map.link);
	wl_list_remove(&l->unmap.link);
	wl_list_remove(&l->commit.link);
	wl_list_remove(&l->destroy.link);
	wl_list_remove(&l->new_popup.link);
	wl_list_remove(&l->link);
	struct wx_output *o = l->output;
	free(l);
	if (o) {
		layers_arrange(o);
	}
}

static void new_layer_surface(struct wl_listener *listener, void *data) {
	struct wx_server *server = wl_container_of(listener, server, new_layer_surface);
	struct wlr_layer_surface_v1 *ls = data;
	struct wx_output *output = ls->output ? ls->output->data : output_focused(server);
	if (!output) {
		wlr_log(WLR_ERROR, "layer surface without output, closing");
		wlr_layer_surface_v1_destroy(ls);
		return;
	}
	ls->output = output->wlr_output;
	struct wx_layer_surface *l = calloc(1, sizeof(*l));
	l->server = server;
	l->layer_surface = ls;
	l->output = output;
	l->tag = (struct wx_node_tag){WX_TAG_MAGIC, WX_NODE_LAYER, l};
	l->scene = wlr_scene_layer_surface_v1_create(layer_tree(server, ls->pending.layer), ls);
	l->scene->tree->node.data = &l->tag;
	ls->data = l;

	l->map.notify = layer_map;
	wl_signal_add(&ls->surface->events.map, &l->map);
	l->unmap.notify = layer_unmap;
	wl_signal_add(&ls->surface->events.unmap, &l->unmap);
	l->commit.notify = layer_commit;
	wl_signal_add(&ls->surface->events.commit, &l->commit);
	l->destroy.notify = layer_destroy;
	wl_signal_add(&ls->events.destroy, &l->destroy);
	l->new_popup.notify = layer_new_popup;
	wl_signal_add(&ls->events.new_popup, &l->new_popup);
	wl_list_insert(&server->layers, &l->link);
}

void layer_init(struct wx_server *server) {
	server->layer_shell = wlr_layer_shell_v1_create(server->display, 5);
	server->new_layer_surface.notify = new_layer_surface;
	wl_signal_add(&server->layer_shell->events.new_surface, &server->new_layer_surface);
}
