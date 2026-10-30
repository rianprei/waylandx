#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/xcb.h>
#include <xcb/xcb_icccm.h>

/* Minimal X11 window that asks for an exact position and logs events. */

static xcb_atom_t atom(xcb_connection_t *c, const char *name) {
	xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(c,
		xcb_intern_atom(c, 0, (uint16_t)strlen(name), name), NULL);
	xcb_atom_t a = r ? r->atom : XCB_ATOM_NONE;
	free(r);
	return a;
}

int main(int argc, char *argv[]) {
	int x = 50, y = 50, w = 320, h = 200;
	bool override = false, above = false;
	const char *title = "probe-x11";
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--pos") && i + 2 < argc) {
			x = atoi(argv[++i]);
			y = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--size") && i + 2 < argc) {
			w = atoi(argv[++i]);
			h = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--override")) {
			override = true;
		} else if (!strcmp(argv[i], "--above")) {
			above = true;
		} else if (!strcmp(argv[i], "--title") && i + 1 < argc) {
			title = argv[++i];
		}
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	xcb_connection_t *c = xcb_connect(NULL, NULL);
	if (xcb_connection_has_error(c)) {
		fprintf(stderr, "cannot connect to X display\n");
		return 1;
	}
	xcb_screen_t *screen = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
	xcb_window_t win = xcb_generate_id(c);
	uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK;
	uint32_t values[] = {
		0x00e05020, override,
		XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_STRUCTURE_NOTIFY |
			XCB_EVENT_MASK_FOCUS_CHANGE | XCB_EVENT_MASK_EXPOSURE,
	};
	xcb_create_window(c, XCB_COPY_FROM_PARENT, win, screen->root, (int16_t)x, (int16_t)y,
		(uint16_t)w, (uint16_t)h, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, mask, values);
	xcb_icccm_set_wm_name(c, win, XCB_ATOM_STRING, 8, (uint32_t)strlen(title), title);
	xcb_icccm_set_wm_class(c, win, sizeof("probe\0Probe-X11"), "probe\0Probe-X11");
	xcb_size_hints_t hints = {0};
	xcb_icccm_size_hints_set_position(&hints, 1, x, y);
	xcb_icccm_size_hints_set_size(&hints, 1, w, h);
	xcb_icccm_set_wm_normal_hints(c, win, &hints);
	if (above) {
		xcb_atom_t state = atom(c, "_NET_WM_STATE"), a = atom(c, "_NET_WM_STATE_ABOVE");
		xcb_change_property(c, XCB_PROP_MODE_REPLACE, win, state, XCB_ATOM_ATOM, 32, 1, &a);
	}
	xcb_map_window(c, win);
	xcb_flush(c);
	printf("window 0x%x\n", win);
	xcb_generic_event_t *ev;
	while ((ev = xcb_wait_for_event(c))) {
		switch (ev->response_type & 0x7f) {
		case XCB_CONFIGURE_NOTIFY: {
			xcb_configure_notify_event_t *e = (void *)ev;
			printf("configure %d %d %d %d\n", e->x, e->y, e->width, e->height);
			break;
		}
		case XCB_KEY_PRESS: {
			xcb_key_press_event_t *e = (void *)ev;
			printf("key %u state=%u\n", e->detail, e->state);
			break;
		}
		case XCB_BUTTON_PRESS: {
			xcb_button_press_event_t *e = (void *)ev;
			printf("button %u at %d %d\n", e->detail, e->event_x, e->event_y);
			break;
		}
		case XCB_FOCUS_IN:
			printf("focus_in\n");
			break;
		case XCB_MAP_NOTIFY:
			printf("mapped\n");
			break;
		default:
			break;
		}
		free(ev);
	}
	return 0;
}
