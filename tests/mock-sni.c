/*
 * A StatusNotifierItem with a DBusMenu, like Discord or nm-applet, for the
 * tray test. Prints every call it gets.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <systemd/sd-bus.h>

static int m_click(sd_bus_message *m, void *d, sd_bus_error *e) {
	int x = 0, y = 0;
	sd_bus_message_read(m, "ii", &x, &y);
	printf("item %s\n", sd_bus_message_get_member(m));
	return sd_bus_reply_method_return(m, "");
}

static int m_scroll(sd_bus_message *m, void *d, sd_bus_error *e) {
	int delta = 0;
	const char *dir = "";
	sd_bus_message_read(m, "is", &delta, &dir);
	printf("item Scroll %d %s\n", delta, dir);
	return sd_bus_reply_method_return(m, "");
}

#define PROP_S(name, value) \
	static int p_##name(sd_bus *b, const char *p, const char *i, const char *pr, sd_bus_message *r, \
		void *d, sd_bus_error *e) { return sd_bus_message_append(r, "s", value); }
PROP_S(category, "ApplicationStatus")
PROP_S(id, "mock")
PROP_S(title, "Mock app")
PROP_S(status, "Active")
PROP_S(icon, "firefox")

static int p_menu(sd_bus *b, const char *p, const char *i, const char *pr, sd_bus_message *r,
		void *d, sd_bus_error *e) {
	return sd_bus_message_append(r, "o", "/MenuBar");
}

static int p_false(sd_bus *b, const char *p, const char *i, const char *pr, sd_bus_message *r,
		void *d, sd_bus_error *e) {
	return sd_bus_message_append(r, "b", 0);
}

static const sd_bus_vtable item_vt[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("Activate", "ii", "", m_click, 0),
	SD_BUS_METHOD("SecondaryActivate", "ii", "", m_click, 0),
	SD_BUS_METHOD("ContextMenu", "ii", "", m_click, 0),
	SD_BUS_METHOD("Scroll", "is", "", m_scroll, 0),
	SD_BUS_PROPERTY("Category", "s", p_category, 0, 0),
	SD_BUS_PROPERTY("Id", "s", p_id, 0, 0),
	SD_BUS_PROPERTY("Title", "s", p_title, 0, 0),
	SD_BUS_PROPERTY("Status", "s", p_status, 0, 0),
	SD_BUS_PROPERTY("IconName", "s", p_icon, 0, 0),
	SD_BUS_PROPERTY("Menu", "o", p_menu, 0, 0),
	SD_BUS_PROPERTY("ItemIsMenu", "b", p_false, 0, 0),
	SD_BUS_VTABLE_END,
};

struct node {
	int id;
	const char *label, *type;
	int nchildren;
	const struct node *children;
};

static const struct node more[] = {{5, "Child item", NULL, 0, NULL}};
static const struct node top[] = {
	{1, "_Open window", NULL, 0, NULL},
	{2, NULL, "separator", 0, NULL},
	{4, "More", NULL, 1, more},
	{3, "_Quit", NULL, 0, NULL},
};
static const struct node root = {0, NULL, NULL, 4, top};

static void append_prop(sd_bus_message *r, const char *key, const char *value) {
	sd_bus_message_open_container(r, 'e', "sv");
	sd_bus_message_append(r, "s", key);
	sd_bus_message_append(r, "v", "s", value);
	sd_bus_message_close_container(r);
}

static void append_node(sd_bus_message *r, const struct node *n) {
	sd_bus_message_open_container(r, 'r', "ia{sv}av");
	sd_bus_message_append(r, "i", n->id);
	sd_bus_message_open_container(r, 'a', "{sv}");
	if (n->label) {
		append_prop(r, "label", n->label);
	}
	if (n->type) {
		append_prop(r, "type", n->type);
	}
	if (n->nchildren) {
		append_prop(r, "children-display", "submenu");
	}
	sd_bus_message_close_container(r);
	sd_bus_message_open_container(r, 'a', "v");
	for (int i = 0; i < n->nchildren; i++) {
		sd_bus_message_open_container(r, 'v', "(ia{sv}av)");
		append_node(r, &n->children[i]);
		sd_bus_message_close_container(r);
	}
	sd_bus_message_close_container(r);
	sd_bus_message_close_container(r);
}

static int m_layout(sd_bus_message *m, void *d, sd_bus_error *e) {
	sd_bus_message *r = NULL;
	sd_bus_message_new_method_return(m, &r);
	sd_bus_message_append(r, "u", 1);
	append_node(r, &root);
	int ret = sd_bus_send(NULL, r, NULL);
	sd_bus_message_unref(r);
	printf("menu GetLayout\n");
	return ret;
}

static int m_about(sd_bus_message *m, void *d, sd_bus_error *e) {
	return sd_bus_reply_method_return(m, "b", 0);
}

static int m_event(sd_bus_message *m, void *d, sd_bus_error *e) {
	int id = 0;
	const char *type = "";
	sd_bus_message_read(m, "is", &id, &type);
	printf("menu event %d %s\n", id, type);
	return sd_bus_reply_method_return(m, "");
}

static const sd_bus_vtable menu_vt[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("GetLayout", "iias", "u(ia{sv}av)", m_layout, 0),
	SD_BUS_METHOD("AboutToShow", "i", "b", m_about, 0),
	SD_BUS_METHOD("Event", "isvu", "", m_event, 0),
	SD_BUS_VTABLE_END,
};

int main(void) {
	setvbuf(stdout, NULL, _IOLBF, 0);
	sd_bus *bus;
	if (sd_bus_open_user(&bus) < 0) {
		return 1;
	}
	sd_bus_add_object_vtable(bus, NULL, "/StatusNotifierItem", "org.kde.StatusNotifierItem", item_vt,
		NULL);
	sd_bus_add_object_vtable(bus, NULL, "/MenuBar", "com.canonical.dbusmenu", menu_vt, NULL);
	char name[64];
	snprintf(name, sizeof(name), "org.kde.StatusNotifierItem-%d-1", (int)getpid());
	sd_bus_request_name(bus, name, 0);
	sd_bus_error err = SD_BUS_ERROR_NULL;
	if (sd_bus_call_method(bus, "org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher",
			"org.kde.StatusNotifierWatcher", "RegisterStatusNotifierItem", &err, NULL, "s", name) < 0) {
		fprintf(stderr, "register failed: %s\n", err.message);
		return 1;
	}
	printf("registered %s\n", name);
	for (;;) {
		int r = sd_bus_process(bus, NULL);
		if (r < 0) {
			return 1;
		}
		if (r == 0 && sd_bus_wait(bus, (uint64_t)-1) < 0) {
			return 1;
		}
	}
}
