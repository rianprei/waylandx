#define _GNU_SOURCE
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "icons.h"
#include "launcher.h"
#include "panel.h"
#include "winswitch.h"

/* Overlay for the compositor's Alt+Tab switcher: icons and the picked title. */

#define TILE 112
#define ICON 56
#define PAD 16
#define PER_ROW 7
#define MAX_WIN 64

static struct {
	struct popup *popup;
	char *title[MAX_WIN], *app_id[MAX_WIN];
	int n, index;
	const struct app_list *apps;
} W;

/* Reads a JSON string at *p (just past the opening quote); returns a copy. */
static char *json_string(const char **p) {
	GString *s = g_string_new(NULL);
	const char *c = *p;
	while (*c && *c != '"') {
		if (*c == '\\' && c[1]) {
			c++;
			switch (*c) {
			case 'n': g_string_append_c(s, '\n'); break;
			case 't': g_string_append_c(s, '\t'); break;
			case 'u': {
				unsigned cp = 0;
				if (sscanf(c + 1, "%4x", &cp) == 1) {
					g_string_append_unichar(s, cp);
					c += 4;
				}
				break;
			}
			default: g_string_append_c(s, *c); break; /* \" \\ \/ */
			}
			c++;
			continue;
		}
		g_string_append_c(s, *c++);
	}
	*p = *c ? c + 1 : c;
	return g_string_free(s, FALSE);
}

static void clear(void) {
	for (int i = 0; i < W.n; i++) {
		g_free(W.title[i]);
		g_free(W.app_id[i]);
	}
	W.n = 0;
}

static cairo_surface_t *icon_for(int i) {
	const struct app_entry *e = W.apps ? apps_for_app_id(W.apps, W.app_id[i]) : NULL;
	cairo_surface_t *s = e && e->icon ? icon_get(e->icon, ICON * 2) : NULL;
	if (!s && W.app_id[i] && *W.app_id[i]) {
		s = icon_get(W.app_id[i], ICON * 2);
		if (!s) {
			char *lower = g_ascii_strdown(W.app_id[i], -1);
			s = icon_get(lower, ICON * 2);
			g_free(lower);
		}
	}
	return s;
}

static void draw(struct popup *p, cairo_t *cr, PangoLayout *l) {
	panel_round_rect(cr, 0.5, 0.5, p->width - 1, p->height - 1, 14);
	panel_set_rgb(cr, 0x16181d, 0.95);
	cairo_fill_preserve(cr);
	panel_set_rgb(cr, 0x3a3d45, 1);
	cairo_set_line_width(cr, 1);
	cairo_stroke(cr);
	int cols = W.n < PER_ROW ? W.n : PER_ROW;
	for (int i = 0; i < W.n; i++) {
		double x = PAD + (i % cols) * TILE, y = PAD + (i / cols) * TILE;
		if (i == W.index) {
			panel_round_rect(cr, x + 4, y + 4, TILE - 8, TILE - 8, 10);
			panel_set_rgb(cr, 0x3d7edd, 0.35);
			cairo_fill_preserve(cr);
			panel_set_rgb(cr, 0x3d7edd, 1);
			cairo_set_line_width(cr, 2);
			cairo_stroke(cr);
		}
		cairo_surface_t *ic = icon_for(i);
		double ix = x + (TILE - ICON) / 2.0, iy = y + 14;
		if (ic) {
			panel_paint_icon(cr, ic, ix, iy, ICON, 1);
		} else {
			panel_round_rect(cr, ix, iy, ICON, ICON, 12);
			panel_set_rgb(cr, 0x2e3440, 1);
			cairo_fill(cr);
			char initial[8] = "?";
			const char *src = W.app_id[i] && *W.app_id[i] ? W.app_id[i] : W.title[i];
			if (src && *src) {
				gunichar u = g_unichar_toupper(g_utf8_get_char(src));
				initial[g_unichar_to_utf8(u, initial)] = '\0';
			}
			pango_layout_set_text(l, initial, -1);
			pango_layout_set_width(l, -1);
			int w, h;
			pango_layout_get_pixel_size(l, &w, &h);
			panel_set_rgb(cr, 0xffffff, 1);
			cairo_move_to(cr, ix + (ICON - w) / 2.0, iy + (ICON - h) / 2.0);
			pango_cairo_show_layout(cr, l);
		}
		/* short name under each icon */
		const char *label = W.app_id[i] && *W.app_id[i] ? W.app_id[i] : W.title[i];
		const char *dot = label ? strrchr(label, '.') : NULL;
		pango_layout_set_text(l, dot && dot[1] ? dot + 1 : (label ? label : ""), -1);
		pango_layout_set_width(l, (TILE - 16) * PANGO_SCALE);
		pango_layout_set_alignment(l, PANGO_ALIGN_CENTER);
		int w, h;
		pango_layout_get_pixel_size(l, &w, &h);
		panel_set_rgb(cr, i == W.index ? 0xffffff : 0xb8bcc6, 1);
		cairo_move_to(cr, x + 8, y + TILE - h - 12);
		pango_cairo_show_layout(cr, l);
	}
	/* full title of the picked window */
	if (W.index >= 0 && W.index < W.n) {
		pango_layout_set_alignment(l, PANGO_ALIGN_CENTER);
		pango_layout_set_width(l, (p->width - 2 * PAD) * PANGO_SCALE);
		pango_layout_set_text(l, W.title[W.index] ? W.title[W.index] : "", -1);
		int w, h;
		pango_layout_get_pixel_size(l, &w, &h);
		panel_set_rgb(cr, 0xffffff, 1);
		cairo_move_to(cr, PAD, p->height - PAD - h);
		pango_cairo_show_layout(cr, l);
	}
	pango_layout_set_alignment(l, PANGO_ALIGN_LEFT);
}

void winswitch_event(const char *line, const struct app_list *apps) {
	W.apps = apps;
	if (strstr(line, "\"done\":true")) {
		if (W.popup) {
			popup_destroy(W.popup);
			W.popup = NULL;
		}
		clear();
		printf("switcher closed\n");
		return;
	}
	clear();
	const char *idx = strstr(line, "\"index\":");
	W.index = idx ? atoi(idx + 8) : 0;
	const char *p = strstr(line, "\"windows\":[");
	while (p && W.n < MAX_WIN && (p = strstr(p, "{\"title\":\""))) {
		p += 10;
		W.title[W.n] = json_string(&p);
		const char *a = strstr(p, "\"app_id\":\"");
		if (a) {
			p = a + 10;
			W.app_id[W.n] = json_string(&p);
		} else {
			W.app_id[W.n] = g_strdup("");
		}
		W.n++;
	}
	if (W.n == 0) {
		return;
	}
	int cols = W.n < PER_ROW ? W.n : PER_ROW, rows = (W.n + cols - 1) / cols;
	int w = cols * TILE + 2 * PAD, h = rows * TILE + 2 * PAD + 28;
	if (w < 360) {
		w = 360;
	}
	if (!W.popup) {
		int none[4] = {0, 0, 0, 0};
		W.popup = popup_create(POPUP_OVERLAY, 0, none, w, h, "waylandx-switcher", draw, NULL, NULL);
	} else {
		popup_resize(W.popup, w, h);
	}
	printf("switcher: %d windows, picked %d (%s)\n", W.n, W.index,
		W.title[W.index] ? W.title[W.index] : "");
}
