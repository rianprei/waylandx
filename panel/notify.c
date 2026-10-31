#define _GNU_SOURCE
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wayland-client.h>
#include "icons.h"
#include "notify.h"
#include "panel.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

/*
 * Desktop notifications (freedesktop Notifications spec 1.2): cards in the
 * top right with app icon, summary, body markup and action buttons. Left
 * click runs the default action, right click or x dismisses. Critical
 * notifications stay until dismissed. If another server already owns the
 * name (plasmashell, mako), we stay out of the way.
 */

#ifdef WX_HAVE_SDBUS
#include <systemd/sd-bus.h>

#define CARD_W 380
#define PAD 12
#define ICON 48
#define ICON_PX 96 /* rendered for up to 2x outputs */
#define GAP 8
#define MAX_SHOWN 5
#define MAX_CARDS 50
#define MAX_ACTIONS 6
#define DEFAULT_MS 6000
#define BTN_H 26

struct action {
	char *key, *label;
	double x0, y0, x1, y1; /* card-relative button box */
};

struct card {
	struct wl_list link; /* newest first */
	uint32_t id;
	char *app, *summary, *body; /* body is valid Pango markup */
	cairo_surface_t *icon;
	bool icon_owned;
	struct action actions[MAX_ACTIONS];
	int nactions;
	bool has_default;
	int urgency;
	long long expires; /* 0 = until dismissed */
	double h, y;       /* height and position in the popup */
};

static struct {
	sd_bus *bus;
	struct wl_list cards;
	uint32_t next_id;
	struct popup *popup;
	int count;
} N;

static long long now_ms(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* ---------- text ---------- */

/* Keeps <b> <i> <u>, turns <br> into newlines, drops other tags (<a>, <img>). */
static char *filter_markup(const char *in) {
	GString *s = g_string_new(NULL);
	for (const char *p = in; *p;) {
		if (*p != '<') {
			g_string_append_c(s, *p++);
			continue;
		}
		const char *end = strchr(p, '>');
		if (!end) {
			g_string_append(s, "&lt;");
			p++;
			continue;
		}
		char tag[16] = "";
		size_t n = 0;
		const char *q = p + 1;
		if (*q == '/') {
			tag[n++] = *q++;
		}
		while (q < end && n < sizeof(tag) - 1 && *q != ' ' && *q != '/') {
			tag[n++] = *q++;
		}
		tag[n] = '\0';
		if (!strcmp(tag, "b") || !strcmp(tag, "i") || !strcmp(tag, "u") || !strcmp(tag, "/b") ||
				!strcmp(tag, "/i") || !strcmp(tag, "/u")) {
			g_string_append_len(s, p, end - p + 1);
		} else if (!strcmp(tag, "br")) {
			g_string_append_c(s, '\n');
		}
		p = end + 1;
	}
	return g_string_free(s, FALSE);
}

static char *body_markup(const char *body) {
	char *f = filter_markup(body);
	if (pango_parse_markup(f, -1, 0, NULL, NULL, NULL, NULL)) {
		return f;
	}
	g_free(f);
	return g_markup_escape_text(body, -1); /* not markup after all */
}

static PangoLayout *measure_layout(void) {
	static PangoContext *ctx;
	if (!ctx) {
		ctx = pango_font_map_create_context(pango_cairo_font_map_get_default());
	}
	PangoLayout *l = pango_layout_new(ctx);
	pango_layout_set_wrap(l, PANGO_WRAP_WORD_CHAR);
	pango_layout_set_ellipsize(l, PANGO_ELLIPSIZE_END);
	return l;
}

static void set_font(PangoLayout *l, const char *font) {
	PangoFontDescription *d = pango_font_description_from_string(font);
	pango_layout_set_font_description(l, d);
	pango_font_description_free(d);
}

static double text_w(const struct card *c) {
	return CARD_W - 2 * PAD - (c->icon ? ICON + PAD : 0) - 18 /* close button */;
}

/* Sets up l for one of the card's text blocks and returns its height. */
static int block(PangoLayout *l, const struct card *c, int which) {
	pango_layout_set_attributes(l, NULL); /* set_text keeps the last markup's attributes */
	pango_layout_set_width(l, (int)(text_w(c) * PANGO_SCALE));
	if (which == 0) {
		set_font(l, "Sans 8.5");
		pango_layout_set_height(l, -1);
		pango_layout_set_text(l, c->app, -1);
	} else if (which == 1) {
		set_font(l, "Sans Bold 10");
		pango_layout_set_height(l, -2);
		pango_layout_set_text(l, c->summary, -1);
	} else {
		set_font(l, "Sans 9.5");
		pango_layout_set_height(l, -6);
		pango_layout_set_markup(l, c->body, -1);
	}
	int w, h;
	pango_layout_get_pixel_size(l, &w, &h);
	return h;
}

static void card_measure(struct card *c) {
	PangoLayout *l = measure_layout();
	double h = PAD + block(l, c, 0) + 2 + block(l, c, 1);
	if (*c->body) {
		h += 4 + block(l, c, 2);
	}
	g_object_unref(l);
	bool buttons = false;
	for (int i = 0; i < c->nactions; i++) {
		buttons |= strcmp(c->actions[i].key, "default") != 0;
	}
	if (buttons) {
		h += 10 + BTN_H;
	}
	h += PAD;
	if (c->icon && h < ICON + 2 * PAD) {
		h = ICON + 2 * PAD;
	}
	c->h = h;
}

/* ---------- popup ---------- */

static void draw_cards(struct popup *p, cairo_t *cr, PangoLayout *l);
static void click_cards(struct popup *p, double x, double y, uint32_t button);

static void relayout(void) {
	double y = 0;
	int shown = 0;
	struct card *c;
	wl_list_for_each(c, &N.cards, link) {
		if (shown == MAX_SHOWN) {
			c->y = -1;
			continue;
		}
		c->y = y;
		y += c->h + GAP;
		shown++;
	}
	if (shown == 0) {
		if (N.popup) {
			popup_destroy(N.popup);
			N.popup = NULL;
		}
		return;
	}
	int h = (int)(y - GAP + 0.5);
	if (!N.popup) {
		int margin[4] = {panel_bar_height() + GAP, GAP, 0, 0};
		N.popup = popup_create(POPUP_TOP, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
			ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT, margin, CARD_W, h, "waylandx-notifications",
			draw_cards, click_cards, NULL);
	} else {
		popup_resize(N.popup, CARD_W, h);
	}
}

static void draw_card(struct card *c, cairo_t *cr, PangoLayout *l) {
	cairo_save(cr);
	cairo_translate(cr, 0, c->y);
	panel_round_rect(cr, 0.5, 0.5, CARD_W - 1, c->h - 1, 10);
	panel_set_rgb(cr, 0x1f232b, 0.97);
	cairo_fill_preserve(cr);
	panel_set_rgb(cr, c->urgency == 2 ? 0xe0605a : 0x3a3d45, 1);
	cairo_set_line_width(cr, c->urgency == 2 ? 2 : 1);
	cairo_stroke(cr);
	double tx = PAD;
	if (c->icon) {
		cairo_save(cr);
		cairo_translate(cr, PAD, PAD);
		double s = (double)ICON / cairo_image_surface_get_width(c->icon);
		cairo_scale(cr, s, s);
		cairo_set_source_surface(cr, c->icon, 0, 0);
		cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
		cairo_paint(cr);
		cairo_restore(cr);
		tx += ICON + PAD;
	}
	/* close button */
	panel_set_rgb(cr, 0x8a8f99, 1);
	cairo_set_line_width(cr, 1.5);
	double cx = CARD_W - PAD - 6, cy = PAD + 6;
	cairo_move_to(cr, cx - 4, cy - 4);
	cairo_line_to(cr, cx + 4, cy + 4);
	cairo_move_to(cr, cx + 4, cy - 4);
	cairo_line_to(cr, cx - 4, cy + 4);
	cairo_stroke(cr);

	pango_layout_set_single_paragraph_mode(l, FALSE);
	pango_layout_set_wrap(l, PANGO_WRAP_WORD_CHAR);
	double y = PAD;
	int h = block(l, c, 0);
	panel_set_rgb(cr, 0x9aa0aa, 1);
	cairo_move_to(cr, tx, y);
	pango_cairo_show_layout(cr, l);
	y += h + 2;
	h = block(l, c, 1);
	panel_set_rgb(cr, 0xffffff, 1);
	cairo_move_to(cr, tx, y);
	pango_cairo_show_layout(cr, l);
	y += h;
	if (*c->body) {
		y += 4;
		h = block(l, c, 2);
		panel_set_rgb(cr, 0xd6d9df, 1);
		cairo_move_to(cr, tx, y);
		pango_cairo_show_layout(cr, l);
		y += h;
	}
	/* action buttons */
	double bx = tx, by = y + 10;
	set_font(l, "Sans 9");
	pango_layout_set_height(l, -1);
	pango_layout_set_width(l, -1);
	for (int i = 0; i < c->nactions; i++) {
		struct action *a = &c->actions[i];
		if (!strcmp(a->key, "default")) {
			continue;
		}
		pango_layout_set_attributes(l, NULL);
		pango_layout_set_text(l, a->label, -1);
		int w, th;
		pango_layout_get_pixel_size(l, &w, &th);
		double bw = w + 20;
		if (bx + bw > CARD_W - PAD) {
			break;
		}
		panel_round_rect(cr, bx, by, bw, BTN_H, 6);
		panel_set_rgb(cr, 0x2e3440, 1);
		cairo_fill(cr);
		panel_set_rgb(cr, 0xffffff, 1);
		cairo_move_to(cr, bx + 10, by + (BTN_H - th) / 2.0);
		pango_cairo_show_layout(cr, l);
		a->x0 = bx;
		a->y0 = by;
		a->x1 = bx + bw;
		a->y1 = by + BTN_H;
		bx += bw + 8;
	}
	pango_layout_set_single_paragraph_mode(l, TRUE);
	pango_layout_set_attributes(l, NULL);
	cairo_restore(cr);
}

static void draw_cards(struct popup *p, cairo_t *cr, PangoLayout *l) {
	struct card *c;
	wl_list_for_each(c, &N.cards, link) {
		if (c->y >= 0) {
			draw_card(c, cr, l);
		}
	}
}

/* ---------- lifecycle of cards ---------- */

static void card_free(struct card *c) {
	free(c->app);
	free(c->summary);
	g_free(c->body);
	if (c->icon_owned && c->icon) {
		cairo_surface_destroy(c->icon);
	}
	for (int i = 0; i < c->nactions; i++) {
		free(c->actions[i].key);
		free(c->actions[i].label);
	}
	free(c);
}

/* reason: 1 expired, 2 dismissed by the user, 3 closed by CloseNotification */
static void card_close(struct card *c, uint32_t reason) {
	sd_bus_emit_signal(N.bus, "/org/freedesktop/Notifications", "org.freedesktop.Notifications",
		"NotificationClosed", "uu", c->id, reason);
	wl_list_remove(&c->link);
	N.count--;
	card_free(c);
	relayout();
}

static struct card *card_by_id(uint32_t id) {
	struct card *c;
	wl_list_for_each(c, &N.cards, link) {
		if (c->id == id) {
			return c;
		}
	}
	return NULL;
}

static void invoke(struct card *c, const char *key) {
	printf("notification %u action %s\n", c->id, key);
	sd_bus_emit_signal(N.bus, "/org/freedesktop/Notifications", "org.freedesktop.Notifications",
		"ActionInvoked", "us", c->id, key);
}

static void click_cards(struct popup *p, double x, double y, uint32_t button) {
	struct card *c, *hit = NULL;
	wl_list_for_each(c, &N.cards, link) {
		if (c->y >= 0 && y >= c->y && y < c->y + c->h) {
			hit = c;
		}
	}
	if (!hit) {
		return;
	}
	double cy = y - hit->y;
	bool close_btn = x > CARD_W - PAD - 18 && cy < PAD + 18;
	if (button == 0x110 && !close_btn) { /* BTN_LEFT */
		for (int i = 0; i < hit->nactions; i++) {
			struct action *a = &hit->actions[i];
			if (strcmp(a->key, "default") && x >= a->x0 && x < a->x1 && cy >= a->y0 && cy < a->y1) {
				invoke(hit, a->key);
				card_close(hit, 2);
				sd_bus_flush(N.bus);
				return;
			}
		}
		if (hit->has_default) {
			invoke(hit, "default");
		}
	}
	card_close(hit, 2);
	sd_bus_flush(N.bus);
}

/* ---------- D-Bus methods ---------- */

static void parse_hints(sd_bus_message *m, struct card *c, const char **image_path,
		const char **desktop) {
	if (sd_bus_message_enter_container(m, 'a', "{sv}") <= 0) {
		return;
	}
	while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
		const char *key = NULL, *contents = NULL;
		char type;
		sd_bus_message_read(m, "s", &key);
		sd_bus_message_peek_type(m, &type, &contents);
		if (!key || !contents) {
			sd_bus_message_skip(m, "v");
		} else if (!strcmp(key, "urgency") && !strcmp(contents, "y")) {
			uint8_t u = 1;
			sd_bus_message_read(m, "v", "y", &u);
			c->urgency = u;
		} else if ((!strcmp(key, "image-data") || !strcmp(key, "image_data") ||
				!strcmp(key, "icon_data")) && !strcmp(contents, "(iiibiiay)")) {
			int32_t w, h, stride, alpha, bps, ch;
			const void *px = NULL;
			size_t len = 0;
			sd_bus_message_enter_container(m, 'v', "(iiibiiay)");
			sd_bus_message_enter_container(m, 'r', "iiibiiay");
			sd_bus_message_read(m, "iiibii", &w, &h, &stride, &alpha, &bps, &ch);
			sd_bus_message_read_array(m, 'y', &px, &len);
			sd_bus_message_exit_container(m);
			sd_bus_message_exit_container(m);
			bool ok = px && bps == 8 && (ch == 3 || ch == 4) && w > 0 && h > 0 && stride > 0 &&
				(size_t)stride * (size_t)(h - 1) + (size_t)w * (size_t)ch <= len;
			if (ok && !c->icon) {
				c->icon = icon_from_rgba(px, w, h, stride, ch == 4 && alpha, ICON_PX);
				c->icon_owned = c->icon != NULL;
			}
		} else if ((!strcmp(key, "image-path") || !strcmp(key, "image_path")) &&
				!strcmp(contents, "s")) {
			sd_bus_message_read(m, "v", "s", image_path);
		} else if (!strcmp(key, "desktop-entry") && !strcmp(contents, "s")) {
			sd_bus_message_read(m, "v", "s", desktop);
		} else {
			sd_bus_message_skip(m, "v");
		}
		sd_bus_message_exit_container(m);
	}
	sd_bus_message_exit_container(m);
}

static int m_notify(sd_bus_message *m, void *data, sd_bus_error *err) {
	const char *app = "", *icon = "", *summary = "", *body = "";
	uint32_t replaces = 0;
	int r = sd_bus_message_read(m, "susss", &app, &replaces, &icon, &summary, &body);
	if (r < 0) {
		return r;
	}
	struct card *c = calloc(1, sizeof(*c));
	c->urgency = 1;
	c->app = strdup(*app ? app : "Notification");
	c->summary = strdup(summary);
	c->body = body_markup(body);
	if (sd_bus_message_enter_container(m, 'a', "s") > 0) {
		const char *key, *label;
		while (sd_bus_message_read(m, "s", &key) > 0) {
			if (sd_bus_message_read(m, "s", &label) <= 0) {
				break;
			}
			if (!strcmp(key, "default")) {
				c->has_default = true;
			}
			if (c->nactions < MAX_ACTIONS) {
				c->actions[c->nactions].key = strdup(key);
				c->actions[c->nactions].label = strdup(label);
				c->nactions++;
			}
		}
		sd_bus_message_exit_container(m);
	}
	const char *image_path = NULL, *desktop = NULL;
	parse_hints(m, c, &image_path, &desktop);
	int32_t timeout = -1;
	sd_bus_message_read(m, "i", &timeout);

	/* spec order: image-data, image-path, app_icon, then the app's own icon */
	if (!c->icon && image_path && *image_path) {
		c->icon = icon_get(image_path, ICON_PX);
	}
	if (!c->icon && *icon) {
		c->icon = icon_get(icon, ICON_PX);
	}
	if (!c->icon && desktop && *desktop) {
		c->icon = icon_get(desktop, ICON_PX);
	}
	long long now = now_ms();
	c->expires = timeout == 0 || (timeout < 0 && c->urgency == 2) ? 0 :
		now + (timeout < 0 ? DEFAULT_MS : timeout);
	card_measure(c);

	struct card *old = replaces ? card_by_id(replaces) : NULL;
	if (old) {
		c->id = old->id;
		wl_list_insert(old->link.prev, &c->link);
		wl_list_remove(&old->link);
		card_free(old);
	} else {
		c->id = ++N.next_id ? N.next_id : ++N.next_id;
		wl_list_insert(&N.cards, &c->link);
		if (++N.count > MAX_CARDS) {
			card_close(wl_container_of(N.cards.prev, c, link), 2); /* oldest */
		}
	}
	printf("notification %u from %s: %s\n", c->id, c->app, c->summary);
	relayout();
	if (N.popup) {
		popup_damage(N.popup);
	}
	return sd_bus_reply_method_return(m, "u", c->id);
}

static int m_close(sd_bus_message *m, void *data, sd_bus_error *err) {
	uint32_t id;
	int r = sd_bus_message_read(m, "u", &id);
	if (r < 0) {
		return r;
	}
	struct card *c = card_by_id(id);
	if (c) {
		card_close(c, 3);
	}
	return sd_bus_reply_method_return(m, "");
}

static int m_caps(sd_bus_message *m, void *data, sd_bus_error *err) {
	sd_bus_message *reply = NULL;
	int r = sd_bus_message_new_method_return(m, &reply);
	if (r < 0) {
		return r;
	}
	sd_bus_message_append_strv(reply, (char *[]){"actions", "body", "body-markup", "icon-static",
		NULL});
	r = sd_bus_send(NULL, reply, NULL);
	sd_bus_message_unref(reply);
	return r;
}

static int m_info(sd_bus_message *m, void *data, sd_bus_error *err) {
	return sd_bus_reply_method_return(m, "ssss", "WaylandX", "WaylandX", WX_VERSION, "1.2");
}

static const sd_bus_vtable vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("Notify", "susssasa{sv}i", "u", m_notify, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("CloseNotification", "u", "", m_close, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("GetCapabilities", "", "as", m_caps, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("GetServerInformation", "", "ssss", m_info, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_SIGNAL("NotificationClosed", "uu", 0),
	SD_BUS_SIGNAL("ActionInvoked", "us", 0),
	SD_BUS_VTABLE_END,
};

void notify_init(void) {
	wl_list_init(&N.cards);
	if (sd_bus_open_user(&N.bus) < 0) {
		N.bus = NULL;
		return;
	}
	if (sd_bus_add_object_vtable(N.bus, NULL, "/org/freedesktop/Notifications",
			"org.freedesktop.Notifications", vtable, NULL) < 0 ||
			sd_bus_request_name(N.bus, "org.freedesktop.Notifications", 0) < 0) {
		fprintf(stderr, "waylandx-panel: another notification server is running; not showing "
			"notifications\n");
		N.bus = sd_bus_flush_close_unref(N.bus);
		return;
	}
	printf("notification server ready\n");
}

int notify_fd(void) {
	return N.bus ? sd_bus_get_fd(N.bus) : -1;
}

void notify_dispatch(void) {
	if (N.bus) {
		while (sd_bus_process(N.bus, NULL) > 0) {
		}
	}
}

void notify_tick(void) {
	if (!N.bus) {
		return;
	}
	notify_dispatch(); /* messages read during a reply may be queued */
	long long now = now_ms();
	struct card *c, *tmp;
	bool closed = false;
	wl_list_for_each_safe(c, tmp, &N.cards, link) {
		/* only visible cards count down, so a burst is not lost unseen */
		if (c->expires && c->y >= 0 && now >= c->expires) {
			card_close(c, 1);
			closed = true;
		}
	}
	if (closed) {
		sd_bus_flush(N.bus);
	}
	wl_list_for_each(c, &N.cards, link) {
		if (c->y < 0 && c->expires && c->expires < now + 2000) {
			c->expires = now + 2000; /* came into view late: give it time */
		}
	}
}

int notify_next_ms(void) {
	long long now = now_ms(), next = -1;
	struct card *c;
	wl_list_for_each(c, &N.cards, link) {
		if (c->expires && c->y >= 0 && (next < 0 || c->expires < next)) {
			next = c->expires;
		}
	}
	if (next < 0) {
		return -1;
	}
	return next <= now ? 0 : (int)(next - now);
}

#else /* no sd-bus */

void notify_init(void) {
}

int notify_fd(void) {
	return -1;
}

void notify_dispatch(void) {
}

void notify_tick(void) {
}

int notify_next_ms(void) {
	return -1;
}

#endif
