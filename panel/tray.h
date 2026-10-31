#ifndef WX_TRAY_H
#define WX_TRAY_H

#include <cairo.h>
#include <stdint.h>

/* StatusNotifierItem tray. Hit indexes it reports are >= 100. */
void tray_init(void);
int tray_fd(void);
void tray_dispatch(void);
double tray_draw(cairo_t *cr, int h, double x_right, int bar_index,
	void (*hit)(void *ctx, int item, double x0, double x1), void *ctx);
void tray_click(int index, uint32_t button, double x0, double x1, int bar_width);
void tray_scroll(int index, double delta);

#endif
