#include <cairo.h>
#include <drm_fourcc.h>
#include <math.h>
#include <pango/pangocairo.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/edges.h>
#include "server.h"

#define GRIP 8      /* invisible resize margin outside the frame */
#define CORNER 16   /* corner zone length along each border */

struct cairo_buffer {
	struct wlr_buffer base;
	cairo_surface_t *surface;
};

static void cb_destroy(struct wlr_buffer *b) {
	struct cairo_buffer *cb = wl_container_of(b, cb, base);
	wlr_buffer_finish(b);
	cairo_surface_destroy(cb->surface);
	free(cb);
}

static bool cb_begin(struct wlr_buffer *b, uint32_t flags, void **data, uint32_t *format,
		size_t *stride) {
	struct cairo_buffer *cb = wl_container_of(b, cb, base);
	if (flags & WLR_BUFFER_DATA_PTR_ACCESS_WRITE) {
		return false;
	}
	*data = cairo_image_surface_get_data(cb->surface);
	*format = DRM_FORMAT_ARGB8888;
	*stride = (size_t)cairo_image_surface_get_stride(cb->surface);
	return true;
}

static void cb_end(struct wlr_buffer *b) {
}

static const struct wlr_buffer_impl cairo_buffer_impl = {
	.destroy = cb_destroy,
	.begin_data_ptr_access = cb_begin,
	.end_data_ptr_access = cb_end,
};

static void rgba(uint32_t c, float out[4]) {
	out[0] = ((c >> 24) & 0xff) / 255.0f;
	out[1] = ((c >> 16) & 0xff) / 255.0f;
	out[2] = ((c >> 8) & 0xff) / 255.0f;
	out[3] = (c & 0xff) / 255.0f;
}

static void cairo_rgba(cairo_t *cr, uint32_t c) {
	float f[4];
	rgba(c, f);
	cairo_set_source_rgba(cr, f[0], f[1], f[2], f[3]);
}

static struct wlr_scene_rect *grip_rect(struct wx_deco *d) {
	/* the grip is the first child of the deco tree */
	struct wlr_scene_node *n = wl_container_of(d->tree->children.next, n, link);
	return wlr_scene_rect_from_node(n);
}

void deco_create(struct wx_view *view) {
	struct wx_deco *d = &view->deco;
	if (d->tree) {
		return;
	}
	d->tree = wlr_scene_tree_create(view->tree);
	wlr_scene_node_lower_to_bottom(&d->tree->node);
	float clear[4] = {0, 0, 0, 0};
	wlr_scene_rect_create(d->tree, 1, 1, clear); /* grip */
	float c[4];
	rgba(view->server->config.color_inactive, c);
	for (int i = 0; i < 4; i++) {
		d->border[i] = wlr_scene_rect_create(d->tree, 1, 1, c);
	}
	d->title = wlr_scene_buffer_create(d->tree, NULL);
}

void deco_destroy(struct wx_view *view) {
	struct wx_deco *d = &view->deco;
	if (d->tree) {
		wlr_scene_node_destroy(&d->tree->node);
	}
	free(d->title_text);
	memset(d, 0, sizeof(*d));
}

static float view_scale(struct wx_view *view) {
	struct wx_output *o = output_for_view(view);
	return o ? o->wlr_output->scale : 1.0f;
}

static void draw_button(cairo_t *cr, int kind, double x, double size, uint32_t color) {
	double c = x + size / 2, m = size / 2, r = size * 0.18;
	cairo_rgba(cr, color);
	cairo_set_line_width(cr, 1.6);
	switch (kind) {
	case WX_DECO_CLOSE:
		cairo_move_to(cr, c - r, m - r);
		cairo_line_to(cr, c + r, m + r);
		cairo_move_to(cr, c + r, m - r);
		cairo_line_to(cr, c - r, m + r);
		break;
	case WX_DECO_MAX:
		cairo_rectangle(cr, c - r, m - r, 2 * r, 2 * r);
		break;
	case WX_DECO_MIN:
		cairo_move_to(cr, c - r, m + r * 0.6);
		cairo_line_to(cr, c + r, m + r * 0.6);
		break;
	}
	cairo_stroke(cr);
}

static void render_title(struct wx_view *view, bool active, float scale) {
	struct wx_server *server = view->server;
	struct wx_deco *d = &view->deco;
	struct wx_config *cfg = &server->config;
	int w = view->w, h = cfg->title_height;
	const char *title = view_title(view);
	if (d->title_w == w && d->title_h == h && d->title_active == active &&
			d->title_text && !strcmp(d->title_text, title) &&
			d->title->buffer && d->title->buffer->width == (int)ceilf(w * scale)) {
		return;
	}
	d->title_w = w;
	d->title_h = h;
	d->title_active = active;
	free(d->title_text);
	d->title_text = strdup(title);

	int pw = (int)ceilf(w * scale), ph = (int)ceilf(h * scale);
	if (pw < 1 || ph < 1) {
		wlr_scene_buffer_set_buffer(d->title, NULL);
		return;
	}
	cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pw, ph);
	cairo_t *cr = cairo_create(surf);
	cairo_scale(cr, scale, scale);
	cairo_rgba(cr, active ? cfg->color_active : cfg->color_inactive);
	cairo_paint(cr);

	uint32_t fg = active ? cfg->color_text : cfg->color_text_inactive;
	int buttons = 3;
	double text_w = w - buttons * h - 12;
	if (text_w > 0) {
		PangoLayout *layout = pango_cairo_create_layout(cr);
		PangoFontDescription *font = pango_font_description_from_string(cfg->font);
		pango_layout_set_font_description(layout, font);
		pango_font_description_free(font);
		pango_layout_set_text(layout, title, -1);
		pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
		pango_layout_set_width(layout, (int)(text_w * PANGO_SCALE));
		pango_layout_set_single_paragraph_mode(layout, TRUE);
		int tw, th;
		pango_layout_get_pixel_size(layout, &tw, &th);
		cairo_rgba(cr, fg);
		cairo_move_to(cr, 8, (h - th) / 2.0);
		pango_cairo_show_layout(cr, layout);
		g_object_unref(layout);
	}
	draw_button(cr, WX_DECO_CLOSE, w - h, h, fg);
	draw_button(cr, WX_DECO_MAX, w - 2 * h, h, fg);
	draw_button(cr, WX_DECO_MIN, w - 3 * h, h, fg);
	cairo_destroy(cr);
	cairo_surface_flush(surf);

	struct cairo_buffer *cb = calloc(1, sizeof(*cb));
	cb->surface = surf;
	wlr_buffer_init(&cb->base, &cairo_buffer_impl, pw, ph);
	wlr_scene_buffer_set_buffer(d->title, &cb->base);
	wlr_buffer_drop(&cb->base);
	wlr_scene_buffer_set_dest_size(d->title, w, h);
}

void deco_update(struct wx_view *view) {
	struct wx_deco *d = &view->deco;
	if (!d->tree) {
		return;
	}
	if (!view->ssd || view->fullscreen || view->unmanaged || view->w <= 0) {
		wlr_scene_node_set_enabled(&d->tree->node, false);
		return;
	}
	wlr_scene_node_set_enabled(&d->tree->node, true);
	struct wx_config *cfg = &view->server->config;
	bool active = view->server->focused == view;
	int B = cfg->border, T = cfg->title_height;
	int w = view->w, h = view->h;
	int fw = w + 2 * B, fh = h + T + 2 * B;
	float c[4];
	rgba(active ? cfg->color_active : cfg->color_inactive, c);

	struct wlr_scene_rect *grip = grip_rect(d);
	wlr_scene_node_set_position(&grip->node, -B - GRIP, -T - B - GRIP);
	wlr_scene_rect_set_size(grip, fw + 2 * GRIP, fh + 2 * GRIP);

	struct { int x, y, w, h; } r[4] = {
		{-B, -T - B, fw, B},   /* top */
		{-B, h, fw, B},        /* bottom */
		{-B, -T, B, h + T},    /* left */
		{w, -T, B, h + T},     /* right */
	};
	for (int i = 0; i < 4; i++) {
		wlr_scene_node_set_position(&d->border[i]->node, r[i].x, r[i].y);
		wlr_scene_rect_set_size(d->border[i], r[i].w > 0 ? r[i].w : 0, r[i].h > 0 ? r[i].h : 0);
		wlr_scene_rect_set_color(d->border[i], c);
		wlr_scene_node_set_enabled(&d->border[i]->node, B > 0);
	}
	if (T > 0) {
		wlr_scene_node_set_enabled(&d->title->node, true);
		wlr_scene_node_set_position(&d->title->node, 0, -T);
		render_title(view, active, view_scale(view));
	} else {
		wlr_scene_node_set_enabled(&d->title->node, false);
	}
}

int deco_hit(struct wx_view *view, double vx, double vy, uint32_t *edges) {
	*edges = 0;
	if (!view->ssd || view->fullscreen || view->unmanaged) {
		return WX_DECO_NONE;
	}
	struct wx_config *cfg = &view->server->config;
	int B = cfg->border, T = cfg->title_height;
	int w = view->w, h = view->h;
	if (vx >= 0 && vx < w && vy >= 0 && vy < h) {
		return WX_DECO_NONE;
	}
	if (vx < -B - GRIP || vx >= w + B + GRIP || vy < -T - B - GRIP || vy >= h + B + GRIP) {
		return WX_DECO_NONE;
	}
	if (vy >= -T && vy < 0 && vx >= 0 && vx < w) {
		if (view->maximized || vy >= -T + 3) {
			if (vx >= w - T) {
				return WX_DECO_CLOSE;
			}
			if (vx >= w - 2 * T) {
				return WX_DECO_MAX;
			}
			if (vx >= w - 3 * T) {
				return WX_DECO_MIN;
			}
			return WX_DECO_TITLE;
		}
	}
	if (view->maximized) {
		return WX_DECO_NONE;
	}
	if (vx < 0) {
		*edges |= WLR_EDGE_LEFT;
	} else if (vx >= w) {
		*edges |= WLR_EDGE_RIGHT;
	}
	if (vy < -T) {
		*edges |= WLR_EDGE_TOP;
	} else if (vy >= h) {
		*edges |= WLR_EDGE_BOTTOM;
	}
	/* corners: extend along the border so they are easy to grab */
	if ((*edges & (WLR_EDGE_LEFT | WLR_EDGE_RIGHT)) && !(*edges & (WLR_EDGE_TOP | WLR_EDGE_BOTTOM))) {
		if (vy < -T + CORNER) {
			*edges |= WLR_EDGE_TOP;
		} else if (vy >= h - CORNER) {
			*edges |= WLR_EDGE_BOTTOM;
		}
	} else if ((*edges & (WLR_EDGE_TOP | WLR_EDGE_BOTTOM)) && !(*edges & (WLR_EDGE_LEFT | WLR_EDGE_RIGHT))) {
		if (vx < CORNER) {
			*edges |= WLR_EDGE_LEFT;
		} else if (vx >= w - CORNER) {
			*edges |= WLR_EDGE_RIGHT;
		}
	}
	if (*edges == 0) {
		*edges = WLR_EDGE_TOP; /* top strip of the title bar */
	}
	return WX_DECO_BORDER;
}
