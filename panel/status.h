#ifndef WX_STATUS_H
#define WX_STATUS_H

#include <cairo.h>
#include <pango/pangocairo.h>
#include <stdint.h>

enum status_item { ST_VOLUME, ST_NETWORK, ST_BATTERY, ST_N };

void status_init(void);
int status_fd(void);        /* `pactl subscribe` pipe, or -1 */
void status_dispatch(void);
void status_tick(void);     /* periodic refresh and OSD expiry */
int status_next_ms(void);   /* -1 when nothing is pending */

/* Draws the items right-aligned ending at x_right; returns their left edge. */
double status_draw(cairo_t *cr, PangoLayout *l, int h, double x_right,
	void (*hit)(void *ctx, int item, double x0, double x1), void *ctx);
void status_click(int item, uint32_t button);
void status_scroll(int item, double delta);

void status_osd_volume(void);
void status_osd_brightness(int percent);

#endif
