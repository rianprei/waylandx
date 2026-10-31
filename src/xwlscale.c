#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/util/log.h>
#include <wlr/xwayland.h>
#include <wlr/xwayland/server.h>
#include <xcb/xcb.h>
#include "server.h"
#include "xdg-output-unstable-v1-protocol.h"

/*
 * Sharp X11 apps on HiDPI, the way KWin's "apply scaling themselves" mode
 * works. Xwayland sees one X screen in device pixels of the largest output
 * scale S: X coordinates are layout coordinates times S. X11 surfaces are
 * shown at 1/S, so on an output of scale S every X pixel is one physical
 * pixel, and on smaller scales the window is downsampled (still sharp).
 * Apps size their UI from Xft.dpi = 96*S, which we publish.
 *
 * Xwayland learns the screen layout from xdg-output, so it gets its own
 * xdg-output global with scaled numbers and never sees the wlroots one.
 */

double xwayland_scale(struct wx_server *server) {
	return server->xwl_scale > 0 ? server->xwl_scale : 1.0;
}

int xwl_to_x11(struct wx_server *server, int v) {
	return (int)lround(v * xwayland_scale(server));
}

int xwl_from_x11(struct wx_server *server, int v) {
	return (int)lround(v / xwayland_scale(server));
}

double input_surface_scale(struct wx_server *server, struct wlr_surface *surface) {
	if (surface && wlr_xwayland_surface_try_from_wlr_surface(surface)) {
		return xwayland_scale(server);
	}
	return 1.0;
}

static bool is_xwayland_client(struct wx_server *server, const struct wl_client *client) {
	return server->xwayland && server->xwayland->server &&
		server->xwayland->server->client == client;
}

/* ---------- xdg-output for Xwayland only ---------- */

#define XWL_OUTPUT_VERSION 3

static struct wl_list xwl_outputs; /* wl_resource links */

static void xo_destroy(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct zxdg_output_v1_interface xo_impl = {
	.destroy = xo_destroy,
};

static void xo_resource_destroy(struct wl_resource *resource) {
	wl_list_remove(wl_resource_get_link(resource));
}

static void xo_send(struct wx_server *server, struct wl_resource *resource) {
	struct wlr_output *output = wl_resource_get_user_data(resource);
	if (!output) {
		return;
	}
	struct wlr_box box;
	wlr_output_layout_get_box(server->output_layout, output, &box);
	if (wlr_box_empty(&box)) {
		return;
	}
	double s = xwayland_scale(server);
	int x = (int)lround(box.x * s), y = (int)lround(box.y * s);
	zxdg_output_v1_send_logical_position(resource, x, y);
	zxdg_output_v1_send_logical_size(resource, (int)lround((box.x + box.width) * s) - x,
		(int)lround((box.y + box.height) * s) - y);
	if (wl_resource_get_version(resource) < 3) {
		zxdg_output_v1_send_done(resource);
	}
}

static void xo_manager_get(struct wl_client *client, struct wl_resource *manager,
		uint32_t id, struct wl_resource *output_resource) {
	struct wx_server *server = wl_resource_get_user_data(manager);
	struct wl_resource *resource = wl_resource_create(client, &zxdg_output_v1_interface,
		wl_resource_get_version(manager), id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	struct wlr_output *output = wlr_output_from_resource(output_resource);
	wl_resource_set_implementation(resource, &xo_impl, output, xo_resource_destroy);
	wl_list_insert(&xwl_outputs, wl_resource_get_link(resource));
	if (!output) {
		return; /* inert wl_output */
	}
	if (wl_resource_get_version(resource) >= 2) {
		zxdg_output_v1_send_name(resource, output->name);
		if (output->description) {
			zxdg_output_v1_send_description(resource, output->description);
		}
	}
	xo_send(server, resource);
	if (wl_resource_get_version(resource) >= 3) {
		wl_output_send_done(output_resource);
	}
}

static void xo_manager_destroy(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct zxdg_output_manager_v1_interface xo_manager_impl = {
	.destroy = xo_manager_destroy,
	.get_xdg_output = xo_manager_get,
};

static void xo_manager_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
	struct wl_resource *resource = wl_resource_create(client, &zxdg_output_manager_v1_interface,
		(int)version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &xo_manager_impl, data, NULL);
}

static bool global_filter(const struct wl_client *client, const struct wl_global *global,
		void *data) {
	struct wx_server *server = data;
	if (global == server->xwl_output_global) {
		return is_xwayland_client(server, client);
	}
	if (global == server->xdg_output_mgr->global) {
		return !is_xwayland_client(server, client);
	}
	return true;
}

/* Re-sends the scaled layout to Xwayland; call when outputs or the scale change. */
void xwl_outputs_update(struct wx_server *server) {
	struct wl_resource *resource;
	wl_resource_for_each(resource, &xwl_outputs) {
		xo_send(server, resource);
		struct wlr_output *output = wl_resource_get_user_data(resource);
		if (output && wl_resource_get_version(resource) >= 3) {
			wlr_output_schedule_done(output);
		}
	}
}

/* Disabled outputs leave their xdg_output resources inert. */
void xwl_output_destroyed(struct wlr_output *output) {
	struct wl_resource *resource;
	wl_resource_for_each(resource, &xwl_outputs) {
		if (wl_resource_get_user_data(resource) == output) {
			wl_resource_set_user_data(resource, NULL);
		}
	}
}

void xwlscale_init(struct wx_server *server) {
	wl_list_init(&xwl_outputs);
	server->xdg_output_mgr = wlr_xdg_output_manager_v1_create(server->display,
		server->output_layout);
	server->xwl_output_global = wl_global_create(server->display,
		&zxdg_output_manager_v1_interface, XWL_OUTPUT_VERSION, server, xo_manager_bind);
	wl_display_set_global_filter(server->display, global_filter, server);
}

/* ---------- scale of the X screen ---------- */

static void scale_buffer(struct wlr_scene_buffer *buffer, int sx, int sy, void *data) {
	struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(buffer);
	if (!ss) {
		return;
	}
	double s = *(double *)data;
	struct wlr_surface *surface = ss->surface;
	int w = surface->current.width, h = surface->current.height;
	if (w > 0 && h > 0) {
		wlr_scene_buffer_set_dest_size(buffer, (int)lround(w / s), (int)lround(h / s));
	}
}

/* Shows an X11 surface tree at 1/scale; wlroots resets the size on every commit. */
void xwayland_scale_content(struct wx_view *view) {
	if (view->type != WX_VIEW_XWAYLAND || !view->content) {
		return;
	}
	double s = xwayland_scale(view->server);
	wlr_scene_node_for_each_buffer(&view->content->node, scale_buffer, &s);
}

/* Publishes Xft.dpi in RESOURCE_MANAGER, keeping whatever else is there. */
static void set_xft_dpi(struct wx_server *server) {
	xcb_connection_t *c = server->xwayland ? wlr_xwayland_get_xwm_connection(server->xwayland) : NULL;
	if (!c) {
		return;
	}
	xcb_window_t root = xcb_setup_roots_iterator(xcb_get_setup(c)).data->root;
	xcb_get_property_reply_t *r = xcb_get_property_reply(c,
		xcb_get_property(c, 0, root, XCB_ATOM_RESOURCE_MANAGER, XCB_ATOM_STRING, 0, 1 << 20), NULL);
	const char *old = r ? xcb_get_property_value(r) : "";
	size_t old_len = r ? (size_t)xcb_get_property_value_length(r) : 0;
	char *out = malloc(old_len + 64);
	if (!out) {
		free(r);
		return;
	}
	size_t n = 0;
	/* copy every line except a previous Xft.dpi */
	for (size_t i = 0; i < old_len;) {
		const char *nl = memchr(old + i, '\n', old_len - i);
		size_t len = nl ? (size_t)(nl - (old + i)) + 1 : old_len - i;
		if (strncmp(old + i, "Xft.dpi:", 8)) {
			memcpy(out + n, old + i, len);
			n += len;
			if (!nl) {
				out[n++] = '\n';
			}
		}
		i += len;
	}
	n += (size_t)snprintf(out + n, 64, "Xft.dpi:\t%d\n", (int)lround(96 * xwayland_scale(server)));
	xcb_change_property(c, XCB_PROP_MODE_REPLACE, root, XCB_ATOM_RESOURCE_MANAGER, XCB_ATOM_STRING,
		8, (uint32_t)n, out);
	xcb_flush(c);
	free(out);
	free(r);
}

static double wanted_scale(struct wx_server *server) {
	if (server->config.xwayland_scale > 0) {
		return server->config.xwayland_scale;
	}
	double s = 1.0;
	struct wx_output *o;
	wl_list_for_each(o, &server->outputs, link) {
		if (o->wlr_output->enabled && o->wlr_output->scale > s) {
			s = o->wlr_output->scale;
		}
	}
	return s;
}

/* Recomputes the X screen scale; moves every X11 window into the new space. */
void xwayland_update_scale(struct wx_server *server) {
	double s = wanted_scale(server);
	bool changed = fabs(s - xwayland_scale(server)) > 1e-3;
	server->xwl_scale = s;
	xwl_outputs_update(server);
	if (!changed) {
		return;
	}
	wlr_log(WLR_INFO, "X11 apps now render at scale %.2f (Xft.dpi %d)", s, (int)lround(96 * s));
	set_xft_dpi(server);
	struct wx_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (view->type == WX_VIEW_XWAYLAND && view->mapped) {
			xwayland_scale_content(view);
			view_move(view, view->x, view->y); /* re-sends the configure in the new space */
		}
	}
	wl_list_for_each(view, &server->unmanaged_views, link) {
		xwayland_scale_content(view);
	}
	layers_update_xwayland_workareas(server);
}

void xwayland_scale_ready(struct wx_server *server) {
	server->xwl_scale = wanted_scale(server);
	set_xft_dpi(server);
}
