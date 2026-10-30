#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <wayland-client.h>
#include "input-method-unstable-v2-client-protocol.h"

/* Minimal input method: when activated it commits a fixed string. */

static struct wl_seat *seat;
static struct zwp_input_method_manager_v2 *manager;
static uint32_t serial;
static bool active, pending_active, running = true;
static const char *text = "IME✓";

static void im_activate(void *d, struct zwp_input_method_v2 *im) {
	pending_active = true;
}

static void im_deactivate(void *d, struct zwp_input_method_v2 *im) {
	pending_active = false;
}

static void im_surrounding(void *d, struct zwp_input_method_v2 *im, const char *t, uint32_t c,
		uint32_t a) {
}

static void im_cause(void *d, struct zwp_input_method_v2 *im, uint32_t cause) {
}

static void im_content_type(void *d, struct zwp_input_method_v2 *im, uint32_t hint, uint32_t purpose) {
	printf("im_content_type %u %u\n", hint, purpose);
}

static void im_done(void *d, struct zwp_input_method_v2 *im) {
	serial++;
	bool was = active;
	active = pending_active;
	if (active && !was) {
		printf("im_activate\n");
		zwp_input_method_v2_set_preedit_string(im, "pre", 0, 3);
		zwp_input_method_v2_commit(im, serial);
		zwp_input_method_v2_commit_string(im, text);
		zwp_input_method_v2_set_preedit_string(im, "", 0, 0);
		zwp_input_method_v2_commit(im, serial);
	} else if (!active && was) {
		printf("im_deactivate\n");
	}
}

static void im_unavailable(void *d, struct zwp_input_method_v2 *im) {
	printf("im_unavailable\n");
	running = false;
}

static const struct zwp_input_method_v2_listener im_listener = {
	.activate = im_activate, .deactivate = im_deactivate,
	.surrounding_text = im_surrounding, .text_change_cause = im_cause,
	.content_type = im_content_type, .done = im_done, .unavailable = im_unavailable,
};

static void global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t v) {
	if (!strcmp(iface, wl_seat_interface.name) && !seat) {
		seat = wl_registry_bind(r, name, &wl_seat_interface, 1);
	} else if (!strcmp(iface, zwp_input_method_manager_v2_interface.name)) {
		manager = wl_registry_bind(r, name, &zwp_input_method_manager_v2_interface, 1);
	}
}

static void global_remove(void *d, struct wl_registry *r, uint32_t name) {
}

static const struct wl_registry_listener reg_listener = {global, global_remove};

int main(int argc, char *argv[]) {
	if (argc > 1) {
		text = argv[1];
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	struct wl_display *dpy = wl_display_connect(NULL);
	if (!dpy) {
		fprintf(stderr, "cannot connect\n");
		return 1;
	}
	struct wl_registry *reg = wl_display_get_registry(dpy);
	wl_registry_add_listener(reg, &reg_listener, NULL);
	wl_display_roundtrip(dpy);
	if (!seat || !manager) {
		fprintf(stderr, "no input-method-v2\n");
		return 1;
	}
	struct zwp_input_method_v2 *im = zwp_input_method_manager_v2_get_input_method(manager, seat);
	zwp_input_method_v2_add_listener(im, &im_listener, NULL);
	printf("im_ready\n");
	while (running && wl_display_dispatch(dpy) != -1) {
	}
	return 0;
}
