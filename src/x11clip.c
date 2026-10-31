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
 * The other direction (background X11 owner -> Wayland) is further down.
 */

#define MAX_CLIP (8 << 20)  /* shortcut: no INCR protocol; bigger payloads are refused */
#define CLIP_TIMEOUT_MS 5000

enum { A_CLIPBOARD, A_PRIMARY, A_TARGETS, A_TIMESTAMP, A_UTF8, A_TEXT, A_MIME_UTF8, A_MIME_TEXT,
	A_INCR, A_SNAP_PROP, A_N };

static const char *atom_names[A_N] = {
	"CLIPBOARD", "PRIMARY", "TARGETS", "TIMESTAMP", "UTF8_STRING", "TEXT",
	"text/plain;charset=utf-8", "text/plain", "INCR", "_WAYLANDX_SNAPSHOT",
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

/*
 * X11 -> Wayland for background X11 owners. wlroots only takes a selection
 * from an X11 client that has focus. When a background X11 client (xclip, a
 * clipboard manager, a script) takes CLIPBOARD or PRIMARY while a Wayland
 * window is focused, we copy every offered type into memory and publish that
 * as the Wayland selection, like a clipboard manager. The content survives the
 * X11 client exiting, which plain X11 never guaranteed.
 */

#define SNAP_MAX (64 << 20)  /* shortcut: total snapshot cap; types past it are dropped */
#define SNAP_TIMEOUT_MS 2000

struct snap_entry {
	char *mime;
	xcb_atom_t atom;
	char *data;
	size_t len;
	bool ok;
};

struct snapshot {
	struct wx_server *server;
	bool primary;
	xcb_atom_t selection;
	struct snap_entry *e;
	size_t n, cur, total;
	bool incr, overflow;
	struct wl_event_source *timer;
};

static struct snapshot *snap; /* at most one in flight; a newer owner replaces it */
static xcb_window_t snap_win;

static void entries_free(struct snap_entry *e, size_t n) {
	for (size_t i = 0; i < n; i++) {
		free(e[i].mime);
		free(e[i].data);
	}
	free(e);
}

static void snap_free(struct snapshot *s) {
	if (snap == s) {
		snap = NULL;
	}
	wl_event_source_remove(s->timer);
	entries_free(s->e, s->n);
	free(s);
}

struct writer {
	char *data;
	size_t len, off;
	int fd;
	struct wl_event_source *source;
};

static void writer_finish(struct writer *w) {
	if (w->source) {
		wl_event_source_remove(w->source);
	}
	close(w->fd);
	free(w->data);
	free(w);
}

static int writer_writable(int fd, uint32_t mask, void *data) {
	struct writer *w = data;
	while (w->off < w->len) {
		ssize_t n = write(fd, w->data + w->off, w->len - w->off);
		if (n < 0 && errno == EINTR) {
			continue;
		}
		if (n < 0 && errno == EAGAIN) {
			return 0;
		}
		if (n <= 0) {
			break; /* reader went away */
		}
		w->off += (size_t)n;
	}
	writer_finish(w);
	return 0;
}

static void send_entries(struct wx_server *server, struct snap_entry *e, size_t n,
		const char *mime, int fd) {
	for (size_t i = 0; i < n; i++) {
		if (strcmp(e[i].mime, mime)) {
			continue;
		}
		struct writer *w = calloc(1, sizeof(*w));
		w->data = malloc(e[i].len ? e[i].len : 1);
		if (!w->data) {
			free(w);
			break;
		}
		memcpy(w->data, e[i].data, e[i].len);
		w->len = e[i].len;
		w->fd = fd;
		fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
		w->source = wl_event_loop_add_fd(server->loop, fd, WL_EVENT_WRITABLE, writer_writable, w);
		return;
	}
	close(fd);
}

struct snap_clip {
	struct wlr_data_source base;
	struct wx_server *server;
	struct snap_entry *e;
	size_t n;
};

static void snap_clip_send(struct wlr_data_source *src, const char *mime, int fd) {
	struct snap_clip *c = wl_container_of(src, c, base);
	send_entries(c->server, c->e, c->n, mime, fd);
}

static void snap_clip_destroy(struct wlr_data_source *src) {
	struct snap_clip *c = wl_container_of(src, c, base);
	entries_free(c->e, c->n);
	free(c);
}

static const struct wlr_data_source_impl snap_clip_impl = {
	.send = snap_clip_send,
	.destroy = snap_clip_destroy,
};

struct snap_primary {
	struct wlr_primary_selection_source base;
	struct wx_server *server;
	struct snap_entry *e;
	size_t n;
};

static void snap_primary_send(struct wlr_primary_selection_source *src, const char *mime, int fd) {
	struct snap_primary *c = wl_container_of(src, c, base);
	send_entries(c->server, c->e, c->n, mime, fd);
}

static void snap_primary_destroy(struct wlr_primary_selection_source *src) {
	struct snap_primary *c = wl_container_of(src, c, base);
	entries_free(c->e, c->n);
	free(c);
}

static const struct wlr_primary_selection_source_impl snap_primary_impl = {
	.send = snap_primary_send,
	.destroy = snap_primary_destroy,
};

/* Moves the converted types into a Wayland source and makes it the selection. */
static void snap_publish(struct snapshot *s) {
	struct wx_server *server = s->server;
	size_t n = 0;
	for (size_t i = 0; i < s->n; i++) {
		if (s->e[i].ok) {
			s->e[n++] = s->e[i];
		} else {
			free(s->e[i].mime);
			free(s->e[i].data);
		}
	}
	struct snap_entry *e = s->e;
	s->e = NULL;
	s->n = 0;
	bool primary = s->primary;
	snap_free(s);
	if (n == 0) {
		free(e);
		return;
	}
	struct wl_array *types;
	if (primary) {
		struct snap_primary *c = calloc(1, sizeof(*c));
		wlr_primary_selection_source_init(&c->base, &snap_primary_impl);
		c->server = server;
		c->e = e;
		c->n = n;
		types = &c->base.mime_types;
		for (size_t i = 0; i < n; i++) {
			*(char **)wl_array_add(types, sizeof(char *)) = strdup(e[i].mime);
		}
		wlr_seat_set_primary_selection(server->seat, &c->base, wl_display_next_serial(server->display));
	} else {
		struct snap_clip *c = calloc(1, sizeof(*c));
		wlr_data_source_init(&c->base, &snap_clip_impl);
		c->server = server;
		c->e = e;
		c->n = n;
		types = &c->base.mime_types;
		for (size_t i = 0; i < n; i++) {
			*(char **)wl_array_add(types, sizeof(char *)) = strdup(e[i].mime);
		}
		wlr_seat_set_selection(server->seat, &c->base, wl_display_next_serial(server->display));
	}
	wlr_log(WLR_DEBUG, "published %zu types from a background X11 %s owner", n,
		primary ? "PRIMARY" : "CLIPBOARD");
}

/* Converts the next offered type, or publishes once all are done. */
static void snap_next(struct snapshot *s) {
	xcb_connection_t *c = conn_of(s->server);
	if (!c || s->cur >= s->n) {
		if (c) {
			snap_publish(s);
		} else {
			snap_free(s);
		}
		return;
	}
	s->incr = false;
	s->overflow = false;
	xcb_convert_selection(c, snap_win, s->selection, s->e[s->cur].atom, atoms[A_SNAP_PROP],
		XCB_CURRENT_TIME);
	xcb_flush(c);
	wl_event_source_timer_update(s->timer, SNAP_TIMEOUT_MS);
}

static void snap_entry_done(struct snapshot *s, bool ok) {
	struct snap_entry *e = &s->e[s->cur];
	e->ok = ok && !s->overflow;
	if (!e->ok) {
		s->total -= e->len;
		free(e->data);
		e->data = NULL;
		e->len = 0;
	}
	s->cur++;
	snap_next(s);
}

static int snap_timeout(void *data) {
	struct snapshot *s = data;
	wlr_log(WLR_DEBUG, "X11 owner did not answer for %s", s->e[s->cur].mime);
	snap_entry_done(s, false);
	return 0;
}

/* Reads and deletes our property; false if the read failed. */
static bool snap_read(struct snapshot *s, xcb_connection_t *c, bool *incr, size_t *got) {
	xcb_get_property_reply_t *r = xcb_get_property_reply(c,
		xcb_get_property(c, 1, snap_win, atoms[A_SNAP_PROP], XCB_GET_PROPERTY_TYPE_ANY, 0,
			0x1fffffff), NULL);
	if (!r) {
		return false;
	}
	*incr = r->type == atoms[A_INCR];
	int len = xcb_get_property_value_length(r);
	*got = len > 0 ? (size_t)len : 0;
	if (!*incr && len > 0 && !s->overflow) {
		struct snap_entry *e = &s->e[s->cur];
		if (s->total + (size_t)len > SNAP_MAX) {
			s->overflow = true; /* keep draining INCR chunks, store nothing */
		} else {
			char *d = realloc(e->data, e->len + (size_t)len);
			if (!d) {
				s->overflow = true;
			} else {
				e->data = d;
				memcpy(e->data + e->len, xcb_get_property_value(r), (size_t)len);
				e->len += (size_t)len;
				s->total += (size_t)len;
			}
		}
	}
	free(r);
	return true;
}

static void snap_on_notify(xcb_connection_t *c, xcb_selection_notify_event_t *ev) {
	struct snapshot *s = snap;
	if (!s || s->cur >= s->n || ev->selection != s->selection || ev->target != s->e[s->cur].atom) {
		return; /* stale answer for a snapshot we already gave up on */
	}
	if (ev->property == XCB_ATOM_NONE) {
		snap_entry_done(s, false);
		return;
	}
	bool incr;
	size_t got;
	if (!snap_read(s, c, &incr, &got)) {
		snap_entry_done(s, false);
		return;
	}
	if (incr) {
		/* deleting the INCR property asked the owner for the first chunk */
		s->incr = true;
		wl_event_source_timer_update(s->timer, SNAP_TIMEOUT_MS);
		return;
	}
	snap_entry_done(s, true);
}

static void snap_on_property(xcb_connection_t *c, xcb_property_notify_event_t *ev) {
	struct snapshot *s = snap;
	if (!s || !s->incr || ev->atom != atoms[A_SNAP_PROP] || ev->state != XCB_PROPERTY_NEW_VALUE) {
		return;
	}
	bool incr;
	size_t got;
	if (!snap_read(s, c, &incr, &got)) {
		snap_entry_done(s, false);
		return;
	}
	if (got == 0) {
		snap_entry_done(s, true); /* an empty chunk ends INCR */
		return;
	}
	wl_event_source_timer_update(s->timer, SNAP_TIMEOUT_MS);
}

static bool has_mime(struct snap_entry *e, size_t n, const char *mime) {
	for (size_t i = 0; i < n; i++) {
		if (!strcmp(e[i].mime, mime)) {
			return true;
		}
	}
	return false;
}

/* Same atom -> mime mapping as wlroots' own bridge, plus plain STRING. */
static char *mime_for_atom(xcb_connection_t *c, xcb_atom_t a) {
	if (a == atoms[A_UTF8]) {
		return strdup("text/plain;charset=utf-8");
	}
	if (a == atoms[A_TEXT] || a == XCB_ATOM_STRING) {
		return strdup("text/plain");
	}
	if (a == atoms[A_TARGETS] || a == atoms[A_TIMESTAMP]) {
		return NULL;
	}
	xcb_get_atom_name_reply_t *r = xcb_get_atom_name_reply(c, xcb_get_atom_name(c, a), NULL);
	if (!r) {
		return NULL;
	}
	int len = xcb_get_atom_name_name_length(r);
	const char *name = xcb_get_atom_name_name(r);
	char *mime = NULL;
	if (memchr(name, '/', (size_t)len)) {
		mime = strndup(name, (size_t)len);
	}
	free(r);
	return mime;
}

static xcb_window_t ensure_snap_window(xcb_connection_t *c) {
	if (!snap_win) {
		xcb_screen_t *screen = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
		snap_win = xcb_generate_id(c);
		uint32_t values[] = {1, XCB_EVENT_MASK_PROPERTY_CHANGE};
		xcb_create_window(c, XCB_COPY_FROM_PARENT, snap_win, screen->root, -1, -1, 1, 1, 0,
			XCB_WINDOW_CLASS_INPUT_ONLY, XCB_COPY_FROM_PARENT,
			XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK, values);
	}
	return snap_win;
}

/*
 * wlroots converted TARGETS from a new X11 owner and is about to refuse it
 * because no X11 window has focus. Take over: snapshot every type.
 */
static bool handle_owner_targets(struct wx_server *server, xcb_connection_t *c,
		xcb_selection_notify_event_t *ev) {
	bool primary = ev->selection == atoms[A_PRIMARY];
	xcb_get_property_reply_t *r = xcb_get_property_reply(c,
		xcb_get_property(c, 1, ev->requestor, ev->property, XCB_ATOM_ATOM, 0, 4096), NULL);
	if (!r) {
		return true;
	}
	const xcb_atom_t *list = xcb_get_property_value(r);
	size_t count = r->type == XCB_ATOM_ATOM ? (size_t)xcb_get_property_value_length(r) / sizeof(*list) : 0;
	struct snap_entry *e = calloc(count ? count : 1, sizeof(*e));
	size_t n = 0;
	for (size_t i = 0; i < count; i++) {
		char *mime = mime_for_atom(c, list[i]);
		if (!mime || has_mime(e, n, mime)) {
			free(mime);
			continue;
		}
		e[n].mime = mime;
		e[n].atom = list[i];
		n++;
	}
	free(r);
	if (n == 0) {
		free(e);
		return true;
	}
	if (snap) {
		snap_free(snap);
	}
	struct snapshot *s = calloc(1, sizeof(*s));
	s->server = server;
	s->primary = primary;
	s->selection = ev->selection;
	s->e = e;
	s->n = n;
	s->timer = wl_event_loop_add_timer(server->loop, snap_timeout, s);
	snap = s;
	ensure_snap_window(c);
	snap_next(s);
	return true;
}

static bool handle_selection_notify(struct wx_server *server, xcb_selection_notify_event_t *ev) {
	xcb_connection_t *c = conn_of(server);
	if (!c) {
		return false;
	}
	intern_atoms(c);
	if (snap_win && ev->requestor == snap_win) {
		snap_on_notify(c, ev);
		return true;
	}
	/* only wlroots' own TARGETS probe of a new owner lands here with this target */
	if (ev->target != atoms[A_TARGETS] || ev->property == XCB_ATOM_NONE || x11_focused(server)) {
		return false;
	}
	if (ev->selection != atoms[A_PRIMARY] && ev->selection != atoms[A_CLIPBOARD]) {
		return false;
	}
	return handle_owner_targets(server, c, ev);
}

static bool x11_event(struct wlr_xwayland *xwayland, xcb_generic_event_t *event) {
	struct wx_server *server = xwayland->data;
	switch (event->response_type & 0x7f) {
	case XCB_SELECTION_REQUEST:
		return handle_selection_request(server, (xcb_selection_request_event_t *)event);
	case XCB_SELECTION_NOTIFY:
		return handle_selection_notify(server, (xcb_selection_notify_event_t *)event);
	case XCB_PROPERTY_NOTIFY: {
		xcb_property_notify_event_t *ev = (xcb_property_notify_event_t *)event;
		if (snap_win && ev->window == snap_win) {
			xcb_connection_t *c = conn_of(server);
			if (c) {
				snap_on_property(c, ev);
			}
			return true;
		}
		return false;
	}
	}
	return false;
}

void x11clip_init(struct wx_server *server) {
	server->xwayland->data = server;
	server->xwayland->user_event_handler = x11_event;
}

void x11clip_finish(struct wx_server *server) {
	if (snap) {
		snap_free(snap);
	}
	snap_win = 0;
}
