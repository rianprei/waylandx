#ifndef WX_ICONS_H
#define WX_ICONS_H

#include <cairo.h>
#include <stdbool.h>

void icons_init(void);
/* Theme icon name, absolute path or file:// URI rendered at px x px. Cached;
 * the caller must not destroy it. NULL when not found. */
cairo_surface_t *icon_get(const char *name, int px);
/* New surface (caller destroys) from raw RGB(A) rows. */
cairo_surface_t *icon_from_rgba(const unsigned char *data, int w, int h, int stride, bool alpha,
	int px);

#endif
