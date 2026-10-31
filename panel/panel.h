#ifndef WX_PANEL_H
#define WX_PANEL_H

#include <cairo.h>
#include <pango/pangocairo.h>
#include <stdbool.h>
#include <stdint.h>
#include <wayland-client.h>

/* A layer-shell surface the panel draws with cairo (notifications, OSD). */
struct popup;
typedef void (*popup_draw_fn)(struct popup *p, cairo_t *cr, PangoLayout *l);
typedef void (*popup_click_fn)(struct popup *p, double x, double y, uint32_t button);
typedef void (*popup_motion_fn)(struct popup *p, double x, double y); /* -1,-1 on leave */

struct popup {
	struct wl_list link;
	struct wl_surface *surface;
	struct zwlr_layer_surface_v1 *layer;
	int width, height;
	bool configured, dirty;
	popup_draw_fn draw;
	popup_click_fn click;
	popup_motion_fn motion; /* optional, set after popup_create */
	void *data;
};

enum popup_layer { POPUP_TOP = 2, POPUP_OVERLAY = 3 };

/* anchor: zwlr_layer_surface_v1 anchor bits; margin: top, right, bottom, left */
struct popup *popup_create(enum popup_layer layer, uint32_t anchor, const int margin[4], int w,
	int h, const char *ns, popup_draw_fn draw, popup_click_fn click, void *data);
void popup_resize(struct popup *p, int w, int h);
void popup_damage(struct popup *p);
void popup_destroy(struct popup *p);

void panel_set_rgb(cairo_t *cr, uint32_t rgb, double a);
/* Paints s scaled into a size x size box at x,y. */
void panel_paint_icon(cairo_t *cr, cairo_surface_t *s, double x, double y, double size, double alpha);
/* Rounded rectangle path. */
void panel_round_rect(cairo_t *cr, double x, double y, double w, double h, double r);
void panel_redraw_bars(void);
void panel_spawn(const char *cmd);
int panel_bar_height(void);

#endif
