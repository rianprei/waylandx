#include <stdlib.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_xdg_shell.h>
#include "server.h"
#include "waylandx-unstable-v1-protocol.h"

/* waylandx-unstable-v1: X11-style self-positioning for Wayland toplevels. */

struct wx_wxcontrol {
	struct wl_resource *resource;
	struct wx_view *view;
};

static struct wx_wxcontrol *control_from_resource(struct wl_resource *r);

static void control_destroy_request(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void control_set_position(struct wl_client *client, struct wl_resource *resource,
		int32_t x, int32_t y) {
	struct wx_wxcontrol *c = control_from_resource(resource);
	struct wx_view *view = c ? c->view : NULL;
	if (!view) {
		return;
	}
	if (!view->mapped) {
		view->has_req_pos = true;
		view->req_x = x;
		view->req_y = y;
		return;
	}
	if (view->fullscreen || view->maximized) {
		return;
	}
	if (view_is_tiled(view)) {
		view_set_floating(view, true);
	}
	view_move(view, x, y);
	ipc_event_window(view->server, "geometry", view);
}

static void control_set_size(struct wl_client *client, struct wl_resource *resource,
		int32_t w, int32_t h) {
	struct wx_wxcontrol *c = control_from_resource(resource);
	struct wx_view *view = c ? c->view : NULL;
	if (!view || w < 1 || h < 1) {
		return;
	}
	if (!view->mapped) {
		view->req_w = w;
		view->req_h = h;
		return;
	}
	if (!view->fullscreen && !view->maximized && !view_is_tiled(view)) {
		view_resize(view, w, h);
	}
}

static void control_set_layer(struct wl_client *client, struct wl_resource *resource,
		uint32_t layer) {
	struct wx_wxcontrol *c = control_from_resource(resource);
	if (!c || !c->view || layer > WX_TOPLEVEL_CONTROL_V1_LAYER_ABOVE) {
		return;
	}
	enum wx_stack s = layer == WX_TOPLEVEL_CONTROL_V1_LAYER_ABOVE ? WX_STACK_ABOVE :
		layer == WX_TOPLEVEL_CONTROL_V1_LAYER_BELOW ? WX_STACK_BELOW : WX_STACK_NORMAL;
	if (c->view->mapped) {
		view_set_stack(c->view, s);
	} else {
		c->view->stack = s;
	}
}

static void control_set_sticky(struct wl_client *client, struct wl_resource *resource,
		uint32_t sticky) {
	struct wx_wxcontrol *c = control_from_resource(resource);
	if (!c || !c->view) {
		return;
	}
	if (c->view->mapped) {
		view_set_sticky(c->view, sticky != 0);
	} else {
		c->view->sticky = sticky != 0;
	}
}

static void control_set_skip_taskbar(struct wl_client *client, struct wl_resource *resource,
		uint32_t skip) {
	struct wx_wxcontrol *c = control_from_resource(resource);
	if (c && c->view) {
		c->view->skip_taskbar = skip != 0;
		view_ftl_update(c->view);
	}
}

static const struct wx_toplevel_control_v1_interface control_impl = {
	.destroy = control_destroy_request,
	.set_position = control_set_position,
	.set_size = control_set_size,
	.set_layer = control_set_layer,
	.set_sticky = control_set_sticky,
	.set_skip_taskbar = control_set_skip_taskbar,
};

static struct wx_wxcontrol *control_from_resource(struct wl_resource *r) {
	return wl_resource_instance_of(r, &wx_toplevel_control_v1_interface, &control_impl) ?
		wl_resource_get_user_data(r) : NULL;
}

static void control_resource_destroy(struct wl_resource *resource) {
	struct wx_wxcontrol *c = wl_resource_get_user_data(resource);
	if (c->view) {
		c->view->control = NULL;
	}
	free(c);
}

static void manager_destroy(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void manager_get_control(struct wl_client *client, struct wl_resource *resource,
		uint32_t id, struct wl_resource *toplevel_res) {
	struct wlr_xdg_toplevel *toplevel = wlr_xdg_toplevel_from_resource(toplevel_res);
	struct wx_view *view = toplevel ? view_from_xdg_toplevel(toplevel) : NULL;
	if (view && view->control) {
		wl_resource_post_error(resource, WX_MANAGER_V1_ERROR_ALREADY_CONTROLLED,
			"toplevel already has a control object");
		return;
	}
	struct wx_wxcontrol *c = calloc(1, sizeof(*c));
	c->resource = wl_resource_create(client, &wx_toplevel_control_v1_interface,
		wl_resource_get_version(resource), id);
	if (!c->resource) {
		free(c);
		wl_client_post_no_memory(client);
		return;
	}
	c->view = view; /* NULL if the toplevel is already gone: inert object */
	wl_resource_set_implementation(c->resource, &control_impl, c, control_resource_destroy);
	if (view) {
		view->control = c;
		if (view->mapped) {
			wx_toplevel_control_v1_send_position(c->resource, view->x, view->y);
		}
	}
}

static const struct wx_manager_v1_interface manager_impl = {
	.destroy = manager_destroy,
	.get_toplevel_control = manager_get_control,
};

static void manager_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
	struct wl_resource *r = wl_resource_create(client, &wx_manager_v1_interface, (int)version, id);
	if (!r) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(r, &manager_impl, data, NULL);
}

void wxproto_init(struct wx_server *server) {
	wl_global_create(server->display, &wx_manager_v1_interface, 1, server, manager_bind);
}

void wxproto_view_mapped(struct wx_view *view) {
	if (view->control) {
		wx_toplevel_control_v1_send_position(view->control->resource, view->x, view->y);
	}
}

void wxproto_view_moved(struct wx_view *view) {
	if (view->control && view->mapped) {
		wx_toplevel_control_v1_send_position(view->control->resource, view->x, view->y);
	}
}

void wxproto_view_destroyed(struct wx_view *view) {
	if (view->control) {
		view->control->view = NULL;
		view->control = NULL;
	}
}
