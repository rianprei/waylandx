#define _GNU_SOURCE /* pipe2 */
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>
#include <wlr/xwayland.h>
#include <xcb/xcb.h>
#include "server.h"

/*
 * X11-style clipboard for X11 apps: wlroots refuses selection requests from
 * X11 clients while no X11 window has focus (a Wayland security rule). We
 * answer those requests ourselves from the Wayland selection, so background
 * X11 clipboard managers and tools keep working like on a real X server.
 * Requests while an X11 window is focused are left to wlroots.
 */

#define MAX_CLIP (8 << 20)  /* shortcut: no INCR protocol; bigger payloads are refused */
#define CLIP_TIMEOUT_MS 5000

enum { A_CLIPBOARD, A_PRIMARY, A_TARGETS, A_TIMESTAMP, A_UTF8, A_TEXT, A_MIME_UTF8, A_MIME_TEXT, A_N };

static const char *atom_names[A_N] = {
	"CLIPBOARD", "PRIMARY", "TARGETS", "TIMESTAMP", "UTF8_STRING", "TEXT",
	"text/plain;charset=utf-8", "text/plain",
};

static xcb_atom_t atoms[A_N];

struct transfer {
	struct wx_server *server;
	xcb_selection_request_event_t req;
	int fd;
	char *data;
	size_t len;
	struct wl_event_source *source;
	struct wl_event_source *timer;
};

static xcb_connection_t *conn_of(struct wx_server *server) {
	return server->xwayland ? wlr_xwayland_get_xwm_connection(server->xwayland) : NULL;
}

static void intern_atoms(xcb_connection_t *c) {
	if (atoms[0]) {
		return;
	}
	xcb_intern_atom_cookie_t ck[A_N];
	for (int i = 0; i < A_N; i++) {
		ck[i] = xcb_intern_atom(c, 0, (uint16_t)strlen(atom_names[i]), atom_names[i]);
	}
	for (int i = 0; i < A_N; i++) {
		xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(c, ck[i], NULL);
		atoms[i] = r ? r->atom : XCB_ATOM_NONE;
		free(r);
	}
}

static xcb_atom_t intern(xcb_connection_t *c, const char *name) {
	xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(c,
		xcb_intern_atom(c, 0, (uint16_t)strlen(name), name), NULL);
	xcb_atom_t a = r ? r->atom : XCB_ATOM_NONE;
	free(r);
	return a;
}

static void notify(xcb_connection_t *c, const xcb_selection_request_event_t *req, bool ok) {
	xcb_selection_notify_event_t ev = {
		.response_type = XCB_SELECTION_NOTIFY,
		.time = req->time,
		.requestor = req->requestor,
		.selection = req->selection,
		.target = req->target,
		.property = ok ? (req->property ? req->property : req->target) : XCB_ATOM_NONE,
	};
	xcb_send_event(c, 0, req->requestor, XCB_EVENT_MASK_NO_EVENT, (const char *)&ev);
	xcb_flush(c);
}

static struct wl_array *offered_types(struct wx_server *server, bool primary) {
	struct wlr_seat *seat = server->seat;
	if (primary) {
		return seat->primary_selection_source ? &seat->primary_selection_source->mime_types : NULL;
	}
	return seat->selection_source ? &seat->selection_source->mime_types : NULL;
}

static bool offers(struct wl_array *types, const char *mime) {
	char **t;
	wl_array_for_each(t, types) {
		if (!strcmp(*t, mime)) {
			return true;
		}
	}
	return false;
}

static void send_targets(struct wx_server *server, xcb_connection_t *c,
		const xcb_selection_request_event_t *req, struct wl_array *types) {
	xcb_atom_t list[256];
	size_t n = 0;
	list[n++] = atoms[A_TARGETS];
	list[n++] = atoms[A_TIMESTAMP];
	bool text = false;
	char **t;
	wl_array_for_each(t, types) {
		if (n >= 250) {
			break;
		}
		if (!strncmp(*t, "text/plain", 10) || !strcmp(*t, "UTF8_STRING") || !strcmp(*t, "STRING")) {
			text = true;
		}
		list[n++] = intern(c, *t);
	}
	if (text) {
		list[n++] = atoms[A_UTF8];
		list[n++] = XCB_ATOM_STRING;
		list[n++] = atoms[A_TEXT];
	}
	xcb_atom_t prop = req->property ? req->property : req->target;
	xcb_change_property(c, XCB_PROP_MODE_REPLACE, req->requestor, prop, XCB_ATOM_ATOM, 32,
		(uint32_t)n, list);
	notify(c, req, true);
}

static void transfer_finish(struct transfer *t, bool ok) {
	xcb_connection_t *c = conn_of(t->server);
	if (c) {
		if (ok) {
			xcb_atom_t prop = t->req.property ? t->req.property : t->req.target;
			xcb_atom_t type = t->req.target == atoms[A_TEXT] ? atoms[A_UTF8] : t->req.target;
			xcb_change_property(c, XCB_PROP_MODE_REPLACE, t->req.requestor, prop, type, 8,
				(uint32_t)t->len, t->data);
		}
		notify(c, &t->req, ok);
	}
	if (t->source) {
		wl_event_source_remove(t->source);
	}
	wl_event_source_remove(t->timer);
	close(t->fd);
	free(t->data);
	free(t);
}

static int transfer_readable(int fd, uint32_t mask, void *data) {
	struct transfer *t = data;
	char buf[65536];
	for (;;) {
		ssize_t n = read(fd, buf, sizeof(buf));
		if (n > 0) {
			if (t->len + (size_t)n > MAX_CLIP) {
				wlr_log(WLR_ERROR, "X11 clipboard request over %d bytes refused", MAX_CLIP);
				transfer_finish(t, false);
				return 0;
			}
			char *d = realloc(t->data, t->len + (size_t)n);
			if (!d) {
				transfer_finish(t, false);
				return 0;
			}
			t->data = d;
			memcpy(t->data + t->len, buf, (size_t)n);
			t->len += (size_t)n;
			continue;
		}
		if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
			return 0;
		}
		transfer_finish(t, n == 0); /* EOF: the source wrote everything */
		return 0;
	}
}

static int transfer_timeout(void *data) {
	transfer_finish(data, false);
	return 0;
}

/* Picks the Wayland mime type that answers an X11 target. */
static const char *pick_mime(xcb_connection_t *c, xcb_atom_t target, struct wl_array *types,
		char *scratch, size_t scratch_len) {
	if (target == atoms[A_UTF8] || target == atoms[A_TEXT] || target == XCB_ATOM_STRING) {
		static const char *text[] = {"text/plain;charset=utf-8", "UTF8_STRING", "text/plain",
			"STRING", "TEXT"};
		for (size_t i = 0; i < sizeof(text) / sizeof(text[0]); i++) {
			if (offers(types, text[i])) {
				return text[i];
			}
		}
		return NULL;
	}
	xcb_get_atom_name_reply_t *r = xcb_get_atom_name_reply(c, xcb_get_atom_name(c, target), NULL);
	if (!r) {
		return NULL;
	}
	int len = xcb_get_atom_name_name_length(r);
	if ((size_t)len >= scratch_len) {
		free(r);
		return NULL;
	}
	memcpy(scratch, xcb_get_atom_name_name(r), (size_t)len);
	scratch[len] = '\0';
	free(r);
	return offers(types, scratch) ? scratch : NULL;
}

static bool x11_focused(struct wx_server *server) {
	return server->focused && server->focused->type == WX_VIEW_XWAYLAND;
}

static bool handle_selection_request(struct wx_server *server, xcb_selection_request_event_t *req) {
	xcb_connection_t *c = conn_of(server);
	if (!c || x11_focused(server)) {
		return false; /* wlroots' own bridge already allows this */
	}
	intern_atoms(c);
	bool primary = req->selection == atoms[A_PRIMARY];
	if (!primary && req->selection != atoms[A_CLIPBOARD]) {
		return false;
	}
	struct wl_array *types = offered_types(server, primary);
	if (!types) {
		return false; /* nothing on the Wayland side: let wlroots answer */
	}
	if (req->target == atoms[A_TARGETS]) {
		send_targets(server, c, req, types);
		return true;
	}
	if (req->target == atoms[A_TIMESTAMP]) {
		return false;
	}
	char scratch[256];
	const char *mime = pick_mime(c, req->target, types, scratch, sizeof(scratch));
	int fds[2];
	if (!mime || pipe2(fds, O_CLOEXEC | O_NONBLOCK) < 0) {
		notify(c, req, false);
		return true;
	}
	struct transfer *t = calloc(1, sizeof(*t));
	t->server = server;
	t->req = *req;
	t->fd = fds[0];
	t->source = wl_event_loop_add_fd(server->loop, fds[0], WL_EVENT_READABLE, transfer_readable, t);
	t->timer = wl_event_loop_add_timer(server->loop, transfer_timeout, t);
	wl_event_source_timer_update(t->timer, CLIP_TIMEOUT_MS);
	if (primary) {
		wlr_primary_selection_source_send(server->seat->primary_selection_source, mime, fds[1]);
	} else {
		wlr_data_source_send(server->seat->selection_source, mime, fds[1]);
	}
	/* the source owns fds[1] now and closes it when done */
	wl_display_flush_clients(server->display);
	return true;
}

static bool x11_event(struct wlr_xwayland *xwayland, xcb_generic_event_t *event) {
	struct wx_server *server = xwayland->data;
	if ((event->response_type & 0x7f) == XCB_SELECTION_REQUEST) {
		return handle_selection_request(server, (xcb_selection_request_event_t *)event);
	}
	return false;
}

void x11clip_init(struct wx_server *server) {
	server->xwayland->data = server;
	server->xwayland->user_event_handler = x11_event;
}
