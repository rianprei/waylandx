#include <stdlib.h>
#include "server.h"

void layout_master_stack(struct wlr_box area, int n, double mfact, int gap,
		struct wlr_box *out) {
	if (n <= 0) {
		return;
	}
	int x = area.x + gap, y = area.y + gap;
	int w = area.width - 2 * gap, h = area.height - 2 * gap;
	if (n == 1) {
		out[0] = (struct wlr_box){x, y, w, h};
		return;
	}
	if (mfact < 0.1) {
		mfact = 0.1;
	} else if (mfact > 0.9) {
		mfact = 0.9;
	}
	int mw = (int)((w - gap) * mfact);
	out[0] = (struct wlr_box){x, y, mw, h};
	int sx = x + mw + gap, sw = w - mw - gap;
	int rows = n - 1;
	int avail = h - (rows - 1) * gap;
	int cy = y;
	for (int i = 0; i < rows; i++) {
		/* last row absorbs the rounding remainder */
		int rh = i == rows - 1 ? y + h - cy : avail / rows;
		out[i + 1] = (struct wlr_box){sx, cy, sw, rh};
		cy += rh + gap;
	}
}

#ifndef WX_LAYOUT_TEST
static int by_id(const void *a, const void *b) {
	const struct wx_view *va = *(struct wx_view *const *)a, *vb = *(struct wx_view *const *)b;
	return va->id < vb->id ? -1 : va->id > vb->id;
}

void layout_arrange(struct wx_server *server) {
	int total = wl_list_length(&server->views);
	if (total == 0) {
		return;
	}
	struct wx_view **list = calloc((size_t)total, sizeof(*list));
	struct wlr_box *boxes = calloc((size_t)total, sizeof(*boxes));
	struct wx_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		int ws = output->workspace;
		if (!output->tiling[ws]) {
			continue;
		}
		int n = 0;
		struct wx_view *view;
		wl_list_for_each(view, &server->views, link) {
			if (view_is_tiled(view) && view_is_visible(view) && view_output(view) == output) {
				list[n++] = view;
			}
		}
		if (n == 0) {
			continue;
		}
		qsort(list, (size_t)n, sizeof(*list), by_id);
		layout_master_stack(output->usable, n, output->mfact[ws], server->config.gap, boxes);
		for (int i = 0; i < n; i++) {
			int l, r, t, b;
			view_frame_extents(list[i], &l, &r, &t, &b);
			struct wlr_box bx = boxes[i];
			int nx = bx.x + l, ny = bx.y + t, nw = bx.width - l - r, nh = bx.height - t - b;
			if (nw < 1 || nh < 1) {
				continue;
			}
			if (list[i]->x != nx || list[i]->y != ny || list[i]->w != nw || list[i]->h != nh) {
				view_set_box(list[i], nx, ny, nw, nh);
			}
		}
	}
	free(list);
	free(boxes);
}
#endif
