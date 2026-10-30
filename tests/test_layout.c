#include <assert.h>
#include <stdio.h>
#include "server.h"

static bool overlaps(struct wlr_box a, struct wlr_box b) {
	return a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height;
}

int main(void) {
	struct wlr_box area = {100, 50, 1000, 700}, out[8];

	layout_master_stack(area, 1, 0.5, 10, out);
	assert(out[0].x == 110 && out[0].y == 60 && out[0].width == 980 && out[0].height == 680);

	for (int n = 2; n <= 8; n++) {
		layout_master_stack(area, n, 0.55, 10, out);
		/* master on the left, full height */
		assert(out[0].x == 110 && out[0].height == 680);
		int bottom = 0;
		for (int i = 0; i < n; i++) {
			assert(out[i].width > 0 && out[i].height > 0);
			assert(out[i].x >= area.x && out[i].y >= area.y);
			assert(out[i].x + out[i].width <= area.x + area.width - 10);
			assert(out[i].y + out[i].height <= area.y + area.height - 10);
			for (int j = 0; j < i; j++) {
				assert(!overlaps(out[i], out[j]));
			}
			if (i > 0 && out[i].y + out[i].height > bottom) {
				bottom = out[i].y + out[i].height;
			}
		}
		/* stack column reaches exactly the bottom gap */
		assert(bottom == area.y + area.height - 10);
	}

	/* mfact is clamped */
	layout_master_stack(area, 2, 5.0, 0, out);
	assert(out[0].width == 900);
	puts("layout: all tests passed");
	return 0;
}
