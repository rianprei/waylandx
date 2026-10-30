#include <cairo.h>
#include <drm_fourcc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/interfaces/wlr_output.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/log.h>
#include "ipc.h"
#include "json.h"
#include "server.h"

/*
 * Screenshots without any portal or permission prompt (X11-style). Each
 * output involved is asked for a fresh frame; when it commits we read the
 * committed buffer back and composite it into one image in layout space.
 */

#define CAPTURE_TIMEOUT_MS 3000

struct wx_capture {
	struct wx_server *server;
	struct wx_ipc_client *client;
	char *path;
	struct wlr_box box;
	cairo_surface_t *img;
	int pending;
	bool failed;
	struct wl_list parts;
	struct wl_event_source *timer;
	struct wl_list link;
};

struct wx_capture_part {
	struct wl_list link;     /* wx_output.captures */
	struct wl_list cap_link; /* wx_capture.parts */
	struct wx_capture *cap;
	struct wx_output *output;
};

static struct wl_list captures = {&captures, &captures};

static void capture_finish(struct wx_capture *cap, const char *error) {
	struct wx_capture_part *p, *tmp;
	wl_list_for_each_safe(p, tmp, &cap->parts, cap_link) {
		wl_list_remove(&p->link);
		wl_list_remove(&p->cap_link);
		free(p);
	}
	struct wx_buf b;
	buf_init(&b);
	if (!error) {
		cairo_status_t st = cairo_surface_write_to_png(cap->img, cap->path);
		if (st != CAIRO_STATUS_SUCCESS) {
			error = cairo_status_to_string(st);
		}
	}
	if (error) {
		buf_puts(&b, "{\"ok\":false,\"error\":");
		buf_json_str(&b, error);
		buf_puts(&b, "}");
		wlr_log(WLR_ERROR, "screenshot %s failed: %s", cap->path, error);
	} else {
		buf_puts(&b, "{\"ok\":true,\"path\":");
		buf_json_str(&b, cap->path);
		buf_printf(&b, ",\"x\":%d,\"y\":%d,\"width\":%d,\"height\":%d}", cap->box.x, cap->box.y,
			cap->box.width, cap->box.height);
		wlr_log(WLR_INFO, "screenshot saved to %s", cap->path);
	}
	struct wx_ipc_client *client = cap->client;
	wl_event_source_remove(cap->timer);
	wl_list_remove(&cap->link);
	cairo_surface_destroy(cap->img);
	free(cap->path);
	free(cap);
	if (client) {
		ipc_client_reply(client, b.data);
		ipc_client_unref_pending(client);
	}
	buf_free(&b);
}

static int capture_timeout(void *data) {
	capture_finish(data, "timed out waiting for output frames");
	return 0;
}

bool capture_start(struct wx_server *server, struct wx_ipc_client *client, const char *path,
		struct wlr_box box, char **err) {
	if (wlr_box_empty(&box)) {
		*err = strdup("nothing to capture (empty area)");
		return false;
	}
	struct wx_capture *cap = calloc(1, sizeof(*cap));
	cap->server = server;
	cap->client = client;
	cap->path = strdup(path);
	cap->box = box;
	wl_list_init(&cap->parts);
	cap->img = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, box.width, box.height);
	if (cairo_surface_status(cap->img) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(cap->img);
		free(cap->path);
		free(cap);
		*err = strdup("cannot allocate image");
		return false;
	}
	struct wx_output *o;
	wl_list_for_each(o, &server->outputs, link) {
		struct wlr_box inter;
		if (!o->wlr_output->enabled || !wlr_box_intersection(&inter, &o->full, &box)) {
			continue;
		}
		struct wx_capture_part *p = calloc(1, sizeof(*p));
		p->cap = cap;
		p->output = o;
		wl_list_insert(&o->captures, &p->link);
		wl_list_insert(&cap->parts, &p->cap_link);
		cap->pending++;
		wlr_output_update_needs_frame(o->wlr_output);
		wlr_output_schedule_frame(o->wlr_output);
	}
	if (cap->pending == 0) {
		cairo_surface_destroy(cap->img);
		free(cap->path);
		free(cap);
		*err = strdup("area does not intersect any output");
		return false;
	}
	cap->timer = wl_event_loop_add_timer(server->loop, capture_timeout, cap);
	wl_event_source_timer_update(cap->timer, CAPTURE_TIMEOUT_MS);
	wl_list_insert(&captures, &cap->link);
	return true;
}

static void blit_output(struct wx_capture *cap, struct wx_output *o, cairo_surface_t *src) {
	cairo_t *cr = cairo_create(cap->img);
	cairo_translate(cr, o->full.x - cap->box.x, o->full.y - cap->box.y);
	double sx = (double)o->full.width / cairo_image_surface_get_width(src);
	double sy = (double)o->full.height / cairo_image_surface_get_height(src);
	cairo_scale(cr, sx, sy);
	cairo_set_source_surface(cr, src, 0, 0);
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
	cairo_paint(cr);
	cairo_destroy(cr);
}

void capture_output_commit(struct wx_output *output, void *data) {
	struct wlr_output_event_commit *event = data;
	if (wl_list_empty(&output->captures) || !(event->state->committed & WLR_OUTPUT_STATE_BUFFER) ||
			!event->state->buffer) {
		return;
	}
	struct wx_server *server = output->server;
	struct wlr_buffer *buffer = event->state->buffer;
	struct wlr_texture *tex = wlr_texture_from_buffer(server->renderer, buffer);
	cairo_surface_t *src = NULL;
	const char *error = NULL;
	if (!tex) {
		error = "cannot import output buffer";
	} else {
		src = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, buffer->width, buffer->height);
		cairo_surface_flush(src);
		struct wlr_texture_read_pixels_options opts = {
			.data = cairo_image_surface_get_data(src),
			.format = DRM_FORMAT_ARGB8888,
			.stride = (uint32_t)cairo_image_surface_get_stride(src),
		};
		if (!wlr_texture_read_pixels(tex, &opts)) {
			error = "cannot read output pixels";
		} else {
			/* outputs are opaque; force alpha so PNG viewers show what the screen showed */
			unsigned char *px = cairo_image_surface_get_data(src);
			int stride = cairo_image_surface_get_stride(src);
			for (int y = 0; y < buffer->height; y++) {
				uint32_t *row = (uint32_t *)(px + (size_t)y * stride);
				for (int x = 0; x < buffer->width; x++) {
					row[x] |= 0xff000000u;
				}
			}
			cairo_surface_mark_dirty(src);
		}
		wlr_texture_destroy(tex);
	}
	struct wx_capture_part *p, *tmp;
	wl_list_for_each_safe(p, tmp, &output->captures, link) {
		struct wx_capture *cap = p->cap;
		if (!error) {
			blit_output(cap, output, src);
		} else {
			cap->failed = true;
		}
		wl_list_remove(&p->link);
		wl_list_remove(&p->cap_link);
		free(p);
		if (--cap->pending == 0) {
			capture_finish(cap, cap->failed ? (error ? error : "capture failed") : NULL);
		}
	}
	if (src) {
		cairo_surface_destroy(src);
	}
}

void capture_output_destroy(struct wx_output *output) {
	struct wx_capture_part *p, *tmp;
	wl_list_for_each_safe(p, tmp, &output->captures, link) {
		struct wx_capture *cap = p->cap;
		wl_list_remove(&p->link);
		wl_list_remove(&p->cap_link);
		free(p);
		cap->failed = true;
		if (--cap->pending == 0) {
			capture_finish(cap, "output disappeared during capture");
		}
	}
}

void capture_client_gone(struct wx_server *server, struct wx_ipc_client *client) {
	struct wx_capture *cap;
	wl_list_for_each(cap, &captures, link) {
		if (cap->client == client) {
			cap->client = NULL; /* still write the file, just don't reply */
		}
	}
}
