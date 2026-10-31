#define _GNU_SOURCE
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include "icons.h"
#include "panel.h"
#include "tray.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

/*
 * System tray: StatusNotifierItem (KDE/freedesktop) with DBusMenu menus,
 * what Discord, Steam, KDE Connect, nm-applet and friends use. We are the
 * StatusNotifierWatcher unless someone else already is (then we register as
 * a host with theirs). Left click activates, middle click is the secondary
 * action, right click opens the item's menu, scroll goes to the item.
 */

#ifdef WX_HAVE_SDBUS
#include <systemd/sd-bus.h>

#define ICON_SIZE 18
#define ICON_PX 36
#define ROW_H 26
#define SEP_H 9
#define MENU_PAD 6

static const char *WATCHER = "org.kde.StatusNotifierWatcher";
static const char *WATCHER_PATH = "/StatusNotifierWatcher";

struct item {
	struct wl_list link;
	char *key;       /* "service/path", as the watcher lists it */
	char *service, *path;
	char *iface;     /* org.kde.StatusNotifierItem or org.freedesktop.StatusNotifierItem */
	char *icon_name, *attention_name, *theme_path, *title, *status, *menu;
	bool item_is_menu;
	cairo_surface_t *pixmap, *attention_pixmap;
	sd_bus_slot *signals;
	double x0, x1;
};

struct mnode {
	int32_t id;
	char *label;
	bool enabled, visible, separator, submenu;
	int toggle; /* 0 none, 1 checkmark, 2 radio */
	int state;
	struct mnode *children;
	int nchildren;
};

static struct {
	sd_bus *bus;
	bool own_watcher;
	struct wl_list items;
	sd_bus_slot *vtable_slot;
	/* open menu */
	struct item *menu_item;
	struct mnode menu_root;
	int32_t menu_parent;  /* id whose children are shown; 0 = top */
	struct popup *menu, *catcher;
	int hover;
	int menu_right;       /* margin from the right edge of the output */
	int bar_width;
} T;

static long long now_ms(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* ---------- items ---------- */

static struct item *item_by_key(const char *key) {
	struct item *it;
	wl_list_for_each(it, &T.items, link) {
		if (!strcmp(it->key, key)) {
			return it;
		}
	}
	return NULL;
}

static void free_strs(struct item *it) {
	free(it->icon_name);
	free(it->attention_name);
	free(it->theme_path);
	free(it->title);
	free(it->status);
	free(it->menu);
	it->icon_name = it->attention_name = it->theme_path = it->title = it->status = it->menu = NULL;
	if (it->pixmap) {
		cairo_surface_destroy(it->pixmap);
	}
	if (it->attention_pixmap) {
		cairo_surface_destroy(it->attention_pixmap);
	}
	it->pixmap = it->attention_pixmap = NULL;
}

static void menu_close(void);

static void item_free(struct item *it) {
	if (T.menu_item == it) {
		menu_close();
	}
	sd_bus_slot_unref(it->signals);
	free_strs(it);
	free(it->key);
	free(it->service);
	free(it->path);
	free(it->iface);
	wl_list_remove(&it->link);
	free(it);
}

/* a(iiay), ARGB32 in network byte order: keep the one closest above ICON_PX */
static cairo_surface_t *read_pixmap(sd_bus_message *m) {
	cairo_surface_t *best = NULL;
	int best_w = 0;
	if (sd_bus_message_enter_container(m, 'a', "(iiay)") <= 0) {
		return NULL;
	}
	while (sd_bus_message_enter_container(m, 'r', "iiay") > 0) {
		int32_t w = 0, h = 0;
		const uint8_t *px = NULL;
		size_t len = 0;
		sd_bus_message_read(m, "ii", &w, &h);
		sd_bus_message_read_array(m, 'y', (const void **)&px, &len);
		sd_bus_message_exit_container(m);
		if (w <= 0 || h <= 0 || w > 1024 || h > 1024 || len < (size_t)w * (size_t)h * 4) {
			continue;
		}
		bool better = !best || (best_w < ICON_PX ? w > best_w : (w >= ICON_PX && w < best_w));
		if (!better) {
			continue;
		}
		/* to RGBA rows for icon_from_rgba */
		unsigned char *rgba = malloc((size_t)w * h * 4);
		for (size_t i = 0; i < (size_t)w * h; i++) {
			rgba[i * 4 + 0] = px[i * 4 + 1];
			rgba[i * 4 + 1] = px[i * 4 + 2];
			rgba[i * 4 + 2] = px[i * 4 + 3];
			rgba[i * 4 + 3] = px[i * 4 + 0];
		}
		cairo_surface_t *s = icon_from_rgba(rgba, w, h, w * 4, true, ICON_PX);
		free(rgba);
		if (s) {
			if (best) {
				cairo_surface_destroy(best);
			}
			best = s;
			best_w = w;
		}
	}
	sd_bus_message_exit_container(m);
	return best;
}

static char *read_str_variant(sd_bus_message *m) {
	const char *s = NULL;
	if (sd_bus_message_read(m, "v", "s", &s) < 0 || !s) {
		return NULL;
	}
	return strdup(s);
}

static void item_refresh(struct item *it);

static int on_props(sd_bus_message *m, void *data, sd_bus_error *e) {
	struct item *it = data;
	if (sd_bus_message_is_method_error(m, NULL)) {
		/* older items only speak the freedesktop-named interface */
		if (!strcmp(it->iface, "org.kde.StatusNotifierItem")) {
			free(it->iface);
			it->iface = strdup("org.freedesktop.StatusNotifierItem");
			item_refresh(it);
		}
		return 0;
	}
	free_strs(it);
	if (sd_bus_message_enter_container(m, 'a', "{sv}") <= 0) {
		return 0;
	}
	while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
		const char *key = NULL, *contents = NULL;
		char type;
		sd_bus_message_read(m, "s", &key);
		sd_bus_message_peek_type(m, &type, &contents);
		bool str = contents && !strcmp(contents, "s");
		if (!key || !contents) {
			sd_bus_message_skip(m, "v");
		} else if (!strcmp(key, "IconName") && str) {
			it->icon_name = read_str_variant(m);
		} else if (!strcmp(key, "AttentionIconName") && str) {
			it->attention_name = read_str_variant(m);
		} else if (!strcmp(key, "IconThemePath") && str) {
			it->theme_path = read_str_variant(m);
		} else if (!strcmp(key, "Title") && str) {
			it->title = read_str_variant(m);
		} else if (!strcmp(key, "Status") && str) {
			it->status = read_str_variant(m);
		} else if (!strcmp(key, "Menu") && !strcmp(contents, "o")) {
			const char *o = NULL;
			sd_bus_message_read(m, "v", "o", &o);
			it->menu = o && strcmp(o, "/") ? strdup(o) : NULL;
		} else if (!strcmp(key, "ItemIsMenu") && !strcmp(contents, "b")) {
			int b = 0;
			sd_bus_message_read(m, "v", "b", &b);
			it->item_is_menu = b;
		} else if ((!strcmp(key, "IconPixmap") || !strcmp(key, "AttentionIconPixmap")) &&
				!strcmp(contents, "a(iiay)")) {
			sd_bus_message_enter_container(m, 'v', "a(iiay)");
			cairo_surface_t *s = read_pixmap(m);
			sd_bus_message_exit_container(m);
			if (!strcmp(key, "IconPixmap")) {
				it->pixmap = s;
			} else {
				it->attention_pixmap = s;
			}
		} else {
			sd_bus_message_skip(m, "v");
		}
		sd_bus_message_exit_container(m);
	}
	sd_bus_message_exit_container(m);
	printf("tray: %s icon=%s status=%s menu=%s\n", it->key, it->icon_name ? it->icon_name : "-",
		it->status ? it->status : "-", it->menu ? it->menu : "-");
	panel_redraw_bars();
	return 0;
}

static void item_refresh(struct item *it) {
	sd_bus_call_method_async(T.bus, NULL, it->service, it->path, "org.freedesktop.DBus.Properties",
		"GetAll", on_props, it, "s", it->iface);
}

static int on_item_signal(sd_bus_message *m, void *data, sd_bus_error *e) {
	const char *member = sd_bus_message_get_member(m);
	if (member && !strncmp(member, "New", 3)) { /* NewIcon, NewStatus, NewTitle... */
		item_refresh(data);
	}
	return 0;
}

static void emit_watcher(const char *signal, const char *arg) {
	if (T.own_watcher) {
		if (arg) {
			sd_bus_emit_signal(T.bus, WATCHER_PATH, WATCHER, signal, "s", arg);
		} else {
			sd_bus_emit_signal(T.bus, WATCHER_PATH, WATCHER, signal, "");
		}
		sd_bus_emit_properties_changed(T.bus, WATCHER_PATH, WATCHER,
			"RegisteredStatusNotifierItems", NULL);
	}
}

static void item_add(const char *key) {
	if (item_by_key(key)) {
		return;
	}
	const char *slash = strchr(key, '/');
	struct item *it = calloc(1, sizeof(*it));
	it->key = strdup(key);
	it->service = slash ? strndup(key, (size_t)(slash - key)) : strdup(key);
	it->path = strdup(slash ? slash : "/StatusNotifierItem");
	/* Ayatana/old items use the freedesktop name; try KDE's first, fall back on error */
	it->iface = strdup("org.kde.StatusNotifierItem");
	wl_list_insert(T.items.prev, &it->link);
	char match[512];
	snprintf(match, sizeof(match), "type='signal',sender='%s',path='%s'", it->service, it->path);
	sd_bus_add_match(T.bus, &it->signals, match, on_item_signal, it);
	item_refresh(it);
	emit_watcher("StatusNotifierItemRegistered", key);
}

static void item_remove(const char *key) {
	struct item *it = item_by_key(key);
	if (it) {
		printf("tray: %s removed\n", key);
		emit_watcher("StatusNotifierItemUnregistered", key);
		item_free(it);
		panel_redraw_bars();
	}
}

/* ---------- watcher (when nobody else is) ---------- */

static int w_register_item(sd_bus_message *m, void *data, sd_bus_error *e) {
	const char *arg = NULL;
	int r = sd_bus_message_read(m, "s", &arg);
	if (r < 0) {
		return r;
	}
	const char *sender = sd_bus_message_get_sender(m);
	char key[512];
	if (arg[0] == '/') { /* an object path on the caller's connection */
		snprintf(key, sizeof(key), "%s%s", sender, arg);
	} else {
		snprintf(key, sizeof(key), "%s/StatusNotifierItem", arg);
	}
	item_add(key);
	return sd_bus_reply_method_return(m, "");
}

static int w_register_host(sd_bus_message *m, void *data, sd_bus_error *e) {
	return sd_bus_reply_method_return(m, "");
}

static int w_items(sd_bus *bus, const char *path, const char *iface, const char *prop,
		sd_bus_message *reply, void *data, sd_bus_error *e) {
	int r = sd_bus_message_open_container(reply, 'a', "s");
	struct item *it;
	wl_list_for_each(it, &T.items, link) {
		sd_bus_message_append(reply, "s", it->key);
	}
	return r < 0 ? r : sd_bus_message_close_container(reply);
}

static int w_true(sd_bus *bus, const char *path, const char *iface, const char *prop,
		sd_bus_message *reply, void *data, sd_bus_error *e) {
	return sd_bus_message_append(reply, "b", 1);
}

static int w_version(sd_bus *bus, const char *path, const char *iface, const char *prop,
		sd_bus_message *reply, void *data, sd_bus_error *e) {
	return sd_bus_message_append(reply, "i", 0);
}

static const sd_bus_vtable watcher_vt[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("RegisterStatusNotifierItem", "s", "", w_register_item, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("RegisterStatusNotifierHost", "s", "", w_register_host, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_PROPERTY("RegisteredStatusNotifierItems", "as", w_items, 0,
		SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	SD_BUS_PROPERTY("IsStatusNotifierHostRegistered", "b", w_true, 0, SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_PROPERTY("ProtocolVersion", "i", w_version, 0, SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_SIGNAL("StatusNotifierItemRegistered", "s", 0),
	SD_BUS_SIGNAL("StatusNotifierItemUnregistered", "s", 0),
	SD_BUS_SIGNAL("StatusNotifierHostRegistered", "", 0),
	SD_BUS_VTABLE_END,
};

static int on_name_owner(sd_bus_message *m, void *data, sd_bus_error *e) {
	const char *name, *old, *new;
	if (sd_bus_message_read(m, "sss", &name, &old, &new) < 0 || *new) {
		return 0;
	}
	struct item *it, *tmp;
	wl_list_for_each_safe(it, tmp, &T.items, link) {
		if (!strcmp(it->service, name) || !strcmp(it->service, old)) {
			item_remove(it->key);
		}
	}
	return 0;
}

/* ---------- host of someone else's watcher ---------- */

static int on_remote_registered(sd_bus_message *m, void *data, sd_bus_error *e) {
	const char *key;
	if (sd_bus_message_read(m, "s", &key) >= 0) {
		item_add(key);
	}
	return 0;
}

static int on_remote_unregistered(sd_bus_message *m, void *data, sd_bus_error *e) {
	const char *key;
	if (sd_bus_message_read(m, "s", &key) >= 0) {
		item_remove(key);
	}
	return 0;
}

static void host_of_remote_watcher(void) {
	char name[64];
	snprintf(name, sizeof(name), "org.kde.StatusNotifierHost-%d", (int)getpid());
	sd_bus_request_name(T.bus, name, 0);
	sd_bus_match_signal(T.bus, NULL, WATCHER, WATCHER_PATH, WATCHER,
		"StatusNotifierItemRegistered", on_remote_registered, NULL);
	sd_bus_match_signal(T.bus, NULL, WATCHER, WATCHER_PATH, WATCHER,
		"StatusNotifierItemUnregistered", on_remote_unregistered, NULL);
	sd_bus_call_method(T.bus, WATCHER, WATCHER_PATH, WATCHER, "RegisterStatusNotifierHost", NULL,
		NULL, "s", name);
	char **keys = NULL;
	if (sd_bus_get_property_strv(T.bus, WATCHER, WATCHER_PATH, WATCHER,
			"RegisteredStatusNotifierItems", NULL, &keys) >= 0) {
		for (char **k = keys; k && *k; k++) {
			item_add(*k);
			free(*k);
		}
		free(keys);
	}
}

/* ---------- menus (com.canonical.dbusmenu) ---------- */

static void mnode_free(struct mnode *n) {
	for (int i = 0; i < n->nchildren; i++) {
		mnode_free(&n->children[i]);
	}
	free(n->children);
	free(n->label);
	memset(n, 0, sizeof(*n));
}

/* "_File" -> "File", "__" -> "_" */
static char *strip_mnemonic(const char *s) {
	char *out = malloc(strlen(s) + 1), *o = out;
	for (const char *p = s; *p; p++) {
		if (*p == '_' && p[1] == '_') {
			*o++ = '_';
			p++;
		} else if (*p != '_') {
			*o++ = *p;
		}
	}
	*o = '\0';
	return out;
}

/* at the contents of a (ia{sv}av) struct */
static void parse_node(sd_bus_message *m, struct mnode *n, int depth) {
	n->enabled = n->visible = true;
	sd_bus_message_read(m, "i", &n->id);
	if (sd_bus_message_enter_container(m, 'a', "{sv}") > 0) {
		while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
			const char *key = NULL, *contents = NULL;
			char type;
			sd_bus_message_read(m, "s", &key);
			sd_bus_message_peek_type(m, &type, &contents);
			if (!key || !contents) {
				sd_bus_message_skip(m, "v");
			} else if (!strcmp(key, "label") && !strcmp(contents, "s")) {
				const char *s = NULL;
				sd_bus_message_read(m, "v", "s", &s);
				free(n->label);
				n->label = strip_mnemonic(s ? s : "");
			} else if ((!strcmp(key, "enabled") || !strcmp(key, "visible")) &&
					!strcmp(contents, "b")) {
				int b = 1;
				sd_bus_message_read(m, "v", "b", &b);
				if (!strcmp(key, "enabled")) {
					n->enabled = b;
				} else {
					n->visible = b;
				}
			} else if (!strcmp(key, "type") && !strcmp(contents, "s")) {
				const char *s = NULL;
				sd_bus_message_read(m, "v", "s", &s);
				n->separator = s && !strcmp(s, "separator");
			} else if (!strcmp(key, "toggle-type") && !strcmp(contents, "s")) {
				const char *s = NULL;
				sd_bus_message_read(m, "v", "s", &s);
				n->toggle = !s ? 0 : !strcmp(s, "checkmark") ? 1 : !strcmp(s, "radio") ? 2 : 0;
			} else if (!strcmp(key, "toggle-state") && !strcmp(contents, "i")) {
				sd_bus_message_read(m, "v", "i", &n->state);
			} else if (!strcmp(key, "children-display") && !strcmp(contents, "s")) {
				const char *s = NULL;
				sd_bus_message_read(m, "v", "s", &s);
				n->submenu = s && !strcmp(s, "submenu");
			} else {
				sd_bus_message_skip(m, "v");
			}
			sd_bus_message_exit_container(m);
		}
		sd_bus_message_exit_container(m);
	}
	if (sd_bus_message_enter_container(m, 'a', "v") > 0) {
		while (sd_bus_message_enter_container(m, 'v', "(ia{sv}av)") > 0) {
			if (sd_bus_message_enter_container(m, 'r', "ia{sv}av") > 0) {
				struct mnode child = {0};
				if (depth < 8) {
					parse_node(m, &child, depth + 1);
				} else {
					sd_bus_message_skip(m, "ia{sv}av");
				}
				sd_bus_message_exit_container(m);
				struct mnode *c = realloc(n->children, (size_t)(n->nchildren + 1) * sizeof(*c));
				if (c) {
					n->children = c;
					n->children[n->nchildren++] = child;
				} else {
					mnode_free(&child);
				}
			}
			sd_bus_message_exit_container(m);
		}
		sd_bus_message_exit_container(m);
	}
	if (n->nchildren > 0) {
		n->submenu = true;
	}
	if (!n->label) {
		n->label = strdup("");
	}
}

static struct mnode *find_node(struct mnode *n, int32_t id) {
	if (n->id == id) {
		return n;
	}
	for (int i = 0; i < n->nchildren; i++) {
		struct mnode *f = find_node(&n->children[i], id);
		if (f) {
			return f;
		}
	}
	return NULL;
}

static struct mnode *parent_of(struct mnode *n, struct mnode *child) {
	for (int i = 0; child && i < n->nchildren; i++) {
		if (&n->children[i] == child) {
			return n;
		}
		struct mnode *f = parent_of(&n->children[i], child);
		if (f) {
			return f;
		}
	}
	return NULL;
}

/* Rows shown: the children of menu_parent, plus a "back" row inside submenus. */
static int rows(struct mnode **out, int max) {
	struct mnode *parent = find_node(&T.menu_root, T.menu_parent);
	int n = 0;
	if (!parent) {
		return 0;
	}
	for (int i = 0; i < parent->nchildren && n < max; i++) {
		if (parent->children[i].visible) {
			out[n++] = &parent->children[i];
		}
	}
	return n;
}

static double row_y(struct mnode **r, int i) {
	double y = MENU_PAD + (T.menu_parent != T.menu_root.id ? ROW_H : 0);
	for (int k = 0; k < i; k++) {
		y += r[k]->separator ? SEP_H : ROW_H;
	}
	return y;
}

static void menu_draw(struct popup *p, cairo_t *cr, PangoLayout *l) {
	panel_round_rect(cr, 0.5, 0.5, p->width - 1, p->height - 1, 8);
	panel_set_rgb(cr, 0x1f232b, 0.98);
	cairo_fill_preserve(cr);
	panel_set_rgb(cr, 0x3a3d45, 1);
	cairo_set_line_width(cr, 1);
	cairo_stroke(cr);
	struct mnode *r[64];
	int n = rows(r, 64);
	if (T.menu_parent != T.menu_root.id) {
		pango_layout_set_text(l, "‹  Back", -1);
		pango_layout_set_width(l, -1);
		int w, h;
		pango_layout_get_pixel_size(l, &w, &h);
		if (T.hover == -2) {
			panel_round_rect(cr, 4, MENU_PAD, p->width - 8, ROW_H, 5);
			panel_set_rgb(cr, 0x3d7edd, 1);
			cairo_fill(cr);
		}
		panel_set_rgb(cr, 0x9aa0aa, 1);
		cairo_move_to(cr, 14, MENU_PAD + (ROW_H - h) / 2.0);
		pango_cairo_show_layout(cr, l);
	}
	for (int i = 0; i < n; i++) {
		double y = row_y(r, i);
		if (r[i]->separator) {
			panel_set_rgb(cr, 0x3a3d45, 1);
			cairo_rectangle(cr, 10, y + SEP_H / 2.0, p->width - 20, 1);
			cairo_fill(cr);
			continue;
		}
		if (i == T.hover && r[i]->enabled) {
			panel_round_rect(cr, 4, y, p->width - 8, ROW_H, 5);
			panel_set_rgb(cr, 0x3d7edd, 1);
			cairo_fill(cr);
		}
		uint32_t fg = r[i]->enabled ? 0xeeeeee : 0x6c717c;
		if (r[i]->toggle) {
			panel_set_rgb(cr, fg, 1);
			cairo_set_line_width(cr, 1.4);
			if (r[i]->toggle == 1) {
				cairo_rectangle(cr, 12.5, y + 8.5, 10, 10);
				cairo_stroke(cr);
				if (r[i]->state == 1) {
					cairo_move_to(cr, 14.5, y + 13.5);
					cairo_line_to(cr, 17, y + 16);
					cairo_line_to(cr, 21, y + 10.5);
					cairo_stroke(cr);
				}
			} else {
				cairo_arc(cr, 17.5, y + ROW_H / 2.0, 5, 0, 2 * G_PI);
				cairo_stroke(cr);
				if (r[i]->state == 1) {
					cairo_arc(cr, 17.5, y + ROW_H / 2.0, 2.5, 0, 2 * G_PI);
					cairo_fill(cr);
				}
			}
		}
		pango_layout_set_text(l, r[i]->label, -1);
		pango_layout_set_width(l, (p->width - 60) * PANGO_SCALE);
		int w, h;
		pango_layout_get_pixel_size(l, &w, &h);
		panel_set_rgb(cr, fg, 1);
		cairo_move_to(cr, 32, y + (ROW_H - h) / 2.0);
		pango_cairo_show_layout(cr, l);
		if (r[i]->submenu) {
			cairo_move_to(cr, p->width - 18, y + 9);
			cairo_line_to(cr, p->width - 13, y + ROW_H / 2.0);
			cairo_line_to(cr, p->width - 18, y + ROW_H - 9);
			cairo_set_line_width(cr, 1.4);
			cairo_stroke(cr);
		}
	}
}

/* -2 = back row, -1 = nothing */
static int row_at(double y) {
	if (T.menu_parent != T.menu_root.id && y >= MENU_PAD && y < MENU_PAD + ROW_H) {
		return -2;
	}
	struct mnode *r[64];
	int n = rows(r, 64);
	for (int i = 0; i < n; i++) {
		double ry = row_y(r, i), h = r[i]->separator ? SEP_H : ROW_H;
		if (y >= ry && y < ry + h) {
			return r[i]->separator ? -1 : i;
		}
	}
	return -1;
}

static void menu_motion(struct popup *p, double x, double y) {
	int h = x < 0 ? -1 : row_at(y);
	if (h != T.hover) {
		T.hover = h;
		popup_damage(p);
	}
}

static void menu_show(void);
static void menu_close(void) {
	if (T.menu || T.catcher) {
		printf("tray: menu closed\n");
	}
	if (T.menu) {
		popup_destroy(T.menu);
		T.menu = NULL;
	}
	if (T.catcher) {
		popup_destroy(T.catcher);
		T.catcher = NULL;
	}
	mnode_free(&T.menu_root);
	T.menu_item = NULL;
	T.menu_parent = 0;
	T.hover = -1;
}

static void menu_event(struct mnode *n) {
	struct item *it = T.menu_item;
	printf("tray: menu event %d clicked (%s)\n", n->id, n->label);
	sd_bus_call_method_async(T.bus, NULL, it->service, it->menu, "com.canonical.dbusmenu", "Event",
		NULL, NULL, "isvu", n->id, "clicked", "i", 0, (uint32_t)(now_ms() & 0xffffffff));
}

static void menu_click(struct popup *p, double x, double y, uint32_t button) {
	int i = row_at(y);
	if (i == -2) {
		struct mnode *up = parent_of(&T.menu_root, find_node(&T.menu_root, T.menu_parent));
		T.menu_parent = up ? up->id : T.menu_root.id;
		T.hover = -1;
		menu_show();
		return;
	}
	struct mnode *r[64];
	int n = rows(r, 64);
	if (i < 0 || i >= n || !r[i]->enabled) {
		return;
	}
	if (r[i]->submenu) {
		sd_bus_call_method_async(T.bus, NULL, T.menu_item->service, T.menu_item->menu,
			"com.canonical.dbusmenu", "AboutToShow", NULL, NULL, "i", r[i]->id);
		T.menu_parent = r[i]->id;
		T.hover = -1;
		menu_show();
		return;
	}
	menu_event(r[i]);
	menu_close();
}

static void catcher_draw(struct popup *p, cairo_t *cr, PangoLayout *l) {
}

static void catcher_click(struct popup *p, double x, double y, uint32_t button) {
	menu_close(); /* a click anywhere else closes the menu, as on X11 */
}

/* (Re)creates the menu surface for the current rows. */
static void menu_show(void) {
	struct mnode *r[64];
	int n = rows(r, 64);
	PangoContext *ctx = pango_font_map_create_context(pango_cairo_font_map_get_default());
	PangoLayout *l = pango_layout_new(ctx);
	g_object_unref(ctx); /* the layout keeps its own reference */
	PangoFontDescription *fd = pango_font_description_from_string("Sans 10");
	pango_layout_set_font_description(l, fd);
	pango_font_description_free(fd);
	int width = 180, height = 2 * MENU_PAD + (T.menu_parent != T.menu_root.id ? ROW_H : 0);
	for (int i = 0; i < n; i++) {
		height += r[i]->separator ? SEP_H : ROW_H;
		pango_layout_set_text(l, r[i]->label, -1);
		int w, h;
		pango_layout_get_pixel_size(l, &w, &h);
		if (w + 64 > width) {
			width = w + 64 > 420 ? 420 : w + 64;
		}
	}
	g_object_unref(l);
	if (n == 0) {
		height += ROW_H; /* empty menu: still show something to close */
	}
	if (T.menu) {
		popup_destroy(T.menu);
		T.menu = NULL;
	}
	if (!T.catcher) {
		int none[4] = {0, 0, 0, 0};
		T.catcher = popup_create(POPUP_TOP, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
			ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
			ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT, none, 0, 0, "waylandx-menu-catcher", catcher_draw,
			catcher_click, NULL);
	}
	int right = T.menu_right;
	if (right + width > T.bar_width - 4) {
		right = T.bar_width - 4 - width;
	}
	int margin[4] = {panel_bar_height() + 2, right < 4 ? 4 : right, 0, 0};
	T.menu = popup_create(POPUP_OVERLAY, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT, margin, width, height, "waylandx-menu", menu_draw,
		menu_click, NULL);
	if (T.menu) {
		T.menu->motion = menu_motion;
	}
	printf("tray: menu open with %d rows\n", n);
}

static int on_layout(sd_bus_message *m, void *data, sd_bus_error *e) {
	struct item *it = data;
	if (sd_bus_message_is_method_error(m, NULL) || T.menu_item != it) {
		return 0;
	}
	uint32_t revision;
	if (sd_bus_message_read(m, "u", &revision) < 0 ||
			sd_bus_message_enter_container(m, 'r', "ia{sv}av") <= 0) {
		return 0;
	}
	mnode_free(&T.menu_root);
	parse_node(m, &T.menu_root, 0);
	sd_bus_message_exit_container(m);
	T.menu_parent = T.menu_root.id;
	T.hover = -1;
	menu_show();
	return 0;
}

static void menu_open(struct item *it, double x1, int bar_width) {
	menu_close();
	T.menu_item = it;
	T.bar_width = bar_width;
	T.menu_right = (int)(bar_width - x1);
	sd_bus_call_method_async(T.bus, NULL, it->service, it->menu, "com.canonical.dbusmenu",
		"AboutToShow", NULL, NULL, "i", 0);
	sd_bus_call_method_async(T.bus, NULL, it->service, it->menu, "com.canonical.dbusmenu",
		"GetLayout", on_layout, it, "iias", 0, -1, 0);
}

/* ---------- bar integration ---------- */

static bool visible_item(struct item *it) {
	return !it->status || strcmp(it->status, "Passive");
}

static cairo_surface_t *item_icon(struct item *it) {
	bool attention = it->status && !strcmp(it->status, "NeedsAttention");
	const char *name = attention && it->attention_name ? it->attention_name : it->icon_name;
	cairo_surface_t *pix = attention && it->attention_pixmap ? it->attention_pixmap : it->pixmap;
	if (name && *name) {
		if (it->theme_path && *it->theme_path && name[0] != '/') {
			static const char *ext[] = {".png", ".svg"};
			for (int i = 0; i < 2; i++) {
				char p[1024];
				snprintf(p, sizeof(p), "%s/%s%s", it->theme_path, name, ext[i]);
				if (access(p, R_OK) == 0) {
					cairo_surface_t *s = icon_get(p, ICON_PX);
					if (s) {
						return s;
					}
				}
			}
		}
		cairo_surface_t *s = icon_get(name, ICON_PX);
		if (s) {
			return s;
		}
	}
	return pix;
}

double tray_draw(cairo_t *cr, int h, double x_right, int bar_index,
		void (*hit)(void *ctx, int item, double x0, double x1), void *ctx) {
	double x = x_right;
	struct item *it;
	int i = 0;
	if (!T.bus) {
		return x; /* bars can draw before tray_init */
	}
	/* right to left, newest on the left like KDE */
	wl_list_for_each_reverse(it, &T.items, link) {
		i++;
		if (!visible_item(it)) {
			continue;
		}
		cairo_surface_t *s = item_icon(it);
		x -= ICON_SIZE;
		if (s) {
			panel_paint_icon(cr, s, x, (h - ICON_SIZE) / 2.0, ICON_SIZE, 1);
		} else { /* no icon at all: a dot with the title's initial */
			panel_set_rgb(cr, 0x3d7edd, 1);
			cairo_arc(cr, x + ICON_SIZE / 2.0, h / 2.0, ICON_SIZE / 2.0 - 1, 0, 2 * G_PI);
			cairo_fill(cr);
		}
		if (bar_index == 0 && (it->x0 != x || it->x1 != x + ICON_SIZE)) {
			it->x0 = x;
			it->x1 = x + ICON_SIZE;
			printf("tray: %s at %.0f-%.0f\n", it->key, it->x0, it->x1);
		}
		hit(ctx, 100 + i, x - 3, x + ICON_SIZE + 3);
		x -= 8;
	}
	return x;
}

static struct item *item_by_hit(int index) {
	struct item *it;
	int i = 0;
	if (!T.bus) {
		return NULL;
	}
	wl_list_for_each_reverse(it, &T.items, link) {
		if (++i + 100 == index) {
			return it;
		}
	}
	return NULL;
}

void tray_click(int index, uint32_t button, double x0, double x1, int bar_width) {
	struct item *it = item_by_hit(index);
	if (!it) {
		return;
	}
	int cx = (int)((x0 + x1) / 2), cy = panel_bar_height();
	bool left = button == 0x110, middle = button == 0x112;
	printf("tray: click %s button %u\n", it->key, button);
	if ((left && it->item_is_menu && it->menu) || (!left && !middle && it->menu)) {
		menu_open(it, x1, bar_width);
	} else if (left) {
		sd_bus_call_method_async(T.bus, NULL, it->service, it->path, it->iface, "Activate", NULL,
			NULL, "ii", cx, cy);
	} else if (middle) {
		sd_bus_call_method_async(T.bus, NULL, it->service, it->path, it->iface,
			"SecondaryActivate", NULL, NULL, "ii", cx, cy);
	} else {
		sd_bus_call_method_async(T.bus, NULL, it->service, it->path, it->iface, "ContextMenu",
			NULL, NULL, "ii", cx, cy);
	}
	sd_bus_flush(T.bus);
}

void tray_scroll(int index, double delta) {
	struct item *it = item_by_hit(index);
	if (it) {
		sd_bus_call_method_async(T.bus, NULL, it->service, it->path, it->iface, "Scroll", NULL,
			NULL, "is", (int32_t)(-delta), "vertical");
	}
}

/* ---------- lifecycle ---------- */

void tray_init(void) {
	wl_list_init(&T.items);
	T.hover = -1;
	if (sd_bus_open_user(&T.bus) < 0) {
		T.bus = NULL;
		return;
	}
	sd_bus_match_signal(T.bus, NULL, "org.freedesktop.DBus", "/org/freedesktop/DBus",
		"org.freedesktop.DBus", "NameOwnerChanged", on_name_owner, NULL);
	sd_bus_add_object_vtable(T.bus, &T.vtable_slot, WATCHER_PATH, WATCHER, watcher_vt, NULL);
	if (sd_bus_request_name(T.bus, WATCHER, 0) >= 0) {
		T.own_watcher = true;
		emit_watcher("StatusNotifierHostRegistered", NULL);
		printf("tray: watcher ready\n");
	} else {
		T.vtable_slot = sd_bus_slot_unref(T.vtable_slot);
		host_of_remote_watcher();
		printf("tray: using the existing watcher\n");
	}
	sd_bus_flush(T.bus);
}

int tray_fd(void) {
	return T.bus ? sd_bus_get_fd(T.bus) : -1;
}

void tray_dispatch(void) {
	if (T.bus) {
		while (sd_bus_process(T.bus, NULL) > 0) {
		}
	}
}

#else

void tray_init(void) {
}

int tray_fd(void) {
	return -1;
}

void tray_dispatch(void) {
}

double tray_draw(cairo_t *cr, int h, double x_right, int bar_index,
		void (*hit)(void *ctx, int item, double x0, double x1), void *ctx) {
	return x_right;
}

void tray_click(int index, uint32_t button, double x0, double x1, int bar_width) {
}

void tray_scroll(int index, double delta) {
}

#endif
