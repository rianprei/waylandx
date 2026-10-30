#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>
#include <wlr/xwayland.h>
#include "ipc.h"
#include "json.h"
#include "server.h"

#define MAX_ARGS 64

/* ---------- tokenizer ---------- */

/* Splits a command line into args; supports "double" (with \ escapes) and 'single' quotes. */
static int tokenize(const char *line, char **argv, char *storage, size_t storage_len) {
	int argc = 0;
	const char *p = line;
	char *o = storage, *end = storage + storage_len - 1;
	while (*p) {
		while (isspace((unsigned char)*p)) {
			p++;
		}
		if (!*p) {
			break;
		}
		if (argc >= MAX_ARGS) {
			return -1;
		}
		argv[argc++] = o;
		while (*p && !isspace((unsigned char)*p)) {
			if (*p == '"') {
				p++;
				while (*p && *p != '"') {
					char ch = *p++;
					if (ch == '\\' && *p) {
						ch = *p++;
						ch = ch == 'n' ? '\n' : ch == 't' ? '\t' : ch == 'r' ? '\r' : ch;
					}
					if (o >= end) {
						return -1;
					}
					*o++ = ch;
				}
				if (*p != '"') {
					return -1;
				}
				p++;
			} else if (*p == '\'') {
				p++;
				while (*p && *p != '\'') {
					if (o >= end) {
						return -1;
					}
					*o++ = *p++;
				}
				if (*p != '\'') {
					return -1;
				}
				p++;
			} else {
				if (o >= end) {
					return -1;
				}
				*o++ = *p++;
			}
		}
		*o++ = '\0';
	}
	return argc;
}

/* ---------- replies ---------- */

static bool reply_ok(struct wx_buf *out) {
	buf_puts(out, "{\"ok\":true}");
	return true;
}

static bool reply_err(struct wx_buf *out, const char *fmt, ...) {
	char msg[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	buf_puts(out, "{\"ok\":false,\"error\":");
	buf_json_str(out, msg);
	buf_puts(out, "}");
	return true;
}

static const char *stack_name(enum wx_stack s) {
	return s == WX_STACK_ABOVE ? "above" : s == WX_STACK_BELOW ? "below" : "normal";
}

void ipc_view_json(struct wx_buf *b, struct wx_view *v) {
	struct wx_output *o = view_output(v);
	buf_printf(b, "{\"id\":%u,\"app_id\":", v->id);
	buf_json_str(b, view_app_id(v));
	buf_puts(b, ",\"title\":");
	buf_json_str(b, view_title(v));
	buf_printf(b, ",\"pid\":%d,\"xwayland\":%s", view_pid(v),
		v->type == WX_VIEW_XWAYLAND ? "true" : "false");
	if (v->type == WX_VIEW_XWAYLAND) {
		buf_printf(b, ",\"x11_window\":%u", v->xsurface->window_id);
	} else {
		buf_puts(b, ",\"x11_window\":null");
	}
	buf_printf(b, ",\"x\":%d,\"y\":%d,\"width\":%d,\"height\":%d,\"workspace\":%d",
		v->x, v->y, v->w, v->h, v->workspace);
	buf_printf(b, ",\"focused\":%s,\"floating\":%s,\"tiled\":%s,\"maximized\":%s,"
		"\"fullscreen\":%s,\"minimized\":%s,\"sticky\":%s,\"visible\":%s,\"layer\":\"%s\","
		"\"decorations\":\"%s\",\"skip_taskbar\":%s,\"output\":",
		v->server->focused == v ? "true" : "false", v->floating ? "true" : "false",
		view_is_tiled(v) ? "true" : "false", v->maximized ? "true" : "false",
		v->fullscreen ? "true" : "false", v->minimized ? "true" : "false",
		v->sticky ? "true" : "false", view_is_visible(v) ? "true" : "false",
		stack_name(v->stack), v->ssd ? "server" : "client",
		v->skip_taskbar ? "true" : "false");
	buf_json_str(b, o ? o->wlr_output->name : NULL);
	buf_puts(b, "}");
}

static void view_plain(struct wx_buf *b, struct wx_view *v) {
	buf_printf(b, "%u\t%d\t%d\t", v->id, v->workspace, view_pid(v));
	buf_plain_str(b, view_app_id(v));
	buf_printf(b, "\t%d\t%d\t%d\t%d\t", v->x, v->y, v->w, v->h);
	buf_plain_str(b, view_title(v));
	buf_puts(b, "\n");
}

/* ---------- argument helpers ---------- */

struct cmd {
	struct wx_server *server;
	struct wx_ipc_client *client;
	struct wx_buf *out;
	int argc;
	char **argv;
	bool plain;
	struct wx_view *target;   /* -w ID, default focused */
	bool target_given;
	const char *output_name;  /* -o NAME */
	bool has_region;          /* -r X,Y,W,H */
	struct wlr_box region;
};

static bool parse_int_arg(const char *s, int *out) {
	char *end;
	errno = 0;
	long v = strtol(s, &end, 10);
	if (errno || end == s || *end || v < -1000000 || v > 1000000) {
		return false;
	}
	*out = (int)v;
	return true;
}

static bool parse_double_arg(const char *s, double *out) {
	char *end;
	errno = 0;
	double v = strtod(s, &end);
	if (errno || end == s || *end) {
		return false;
	}
	*out = v;
	return true;
}

/* Pulls -w/-o/-r/--plain out of argv; returns false with an error reply on bad input. */
static bool extract_options(struct cmd *c) {
	int n = 0;
	for (int i = 0; i < c->argc; i++) {
		char *a = c->argv[i];
		if (!strcmp(a, "--plain") || !strcmp(a, "-p")) {
			c->plain = true;
		} else if (!strcmp(a, "-w") && i + 1 < c->argc) {
			const char *v = c->argv[++i];
			c->target_given = true;
			if (!strcmp(v, "focused")) {
				c->target = c->server->focused;
			} else {
				int id;
				if (!parse_int_arg(v, &id) || id <= 0) {
					reply_err(c->out, "invalid window id '%s'", v);
					return false;
				}
				c->target = view_by_id(c->server, (uint32_t)id);
			}
			if (!c->target) {
				reply_err(c->out, "no such window '%s'", v);
				return false;
			}
		} else if (!strcmp(a, "-o") && i + 1 < c->argc) {
			c->output_name = c->argv[++i];
		} else if (!strcmp(a, "-r") && i + 1 < c->argc) {
			struct wlr_box *r = &c->region;
			if (sscanf(c->argv[++i], "%d,%d,%d,%d", &r->x, &r->y, &r->width, &r->height) != 4 ||
					r->width <= 0 || r->height <= 0 || r->width > 32768 || r->height > 32768) {
				reply_err(c->out, "invalid region, expected X,Y,W,H");
				return false;
			}
			c->has_region = true;
		} else {
			c->argv[n++] = a;
		}
	}
	c->argc = n;
	if (!c->target_given) {
		c->target = c->server->focused;
	}
	return true;
}

static bool need_target(struct cmd *c) {
	if (!c->target) {
		reply_err(c->out, "no window (none focused; pass -w ID)");
		return false;
	}
	return true;
}

/* on|off|toggle (default toggle) against the current value */
static bool parse_switch(struct cmd *c, int idx, bool current, bool *out) {
	if (c->argc <= idx || !strcmp(c->argv[idx], "toggle")) {
		*out = !current;
		return true;
	}
	bool v;
	if (!config_parse_bool(c->argv[idx], &v)) {
		reply_err(c->out, "expected on|off|toggle, got '%s'", c->argv[idx]);
		return false;
	}
	*out = v;
	return true;
}

/* ---------- key combos ---------- */

static bool parse_mod(struct wx_server *server, const char *name, uint32_t *mods) {
	static const struct { const char *name; uint32_t mask; } table[] = {
		{"shift", WLR_MODIFIER_SHIFT}, {"ctrl", WLR_MODIFIER_CTRL},
		{"control", WLR_MODIFIER_CTRL}, {"alt", WLR_MODIFIER_ALT},
		{"mod1", WLR_MODIFIER_ALT}, {"super", WLR_MODIFIER_LOGO},
		{"logo", WLR_MODIFIER_LOGO}, {"win", WLR_MODIFIER_LOGO},
		{"meta", WLR_MODIFIER_LOGO}, {"mod4", WLR_MODIFIER_LOGO},
		{"altgr", WLR_MODIFIER_MOD5}, {"mod5", WLR_MODIFIER_MOD5},
	};
	if (!strcasecmp(name, "mod")) {
		*mods |= server->mod_mask;
		return true;
	}
	for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
		if (!strcasecmp(name, table[i].name)) {
			*mods |= table[i].mask;
			return true;
		}
	}
	return false;
}

bool keybind_parse(struct wx_server *server, const char *combo, uint32_t *mods, xkb_keysym_t *sym) {
	char buf[128];
	if (strlen(combo) >= sizeof(buf) || !*combo) {
		return false;
	}
	strcpy(buf, combo);
	*mods = 0;
	*sym = XKB_KEY_NoSymbol;
	char *save = NULL;
	char *last = NULL;
	for (char *tok = strtok_r(buf, "+", &save); tok; tok = strtok_r(NULL, "+", &save)) {
		if (last && !parse_mod(server, last, mods)) {
			return false;
		}
		last = tok;
	}
	if (!last) {
		return false;
	}
	/* a bare modifier ("super", "ctrl+shift") is a key of its own */
	uint32_t m = 0;
	if (parse_mod(server, last, &m)) {
		*mods |= m;
		*sym = m == WLR_MODIFIER_SHIFT ? XKB_KEY_Shift_L : m == WLR_MODIFIER_CTRL ? XKB_KEY_Control_L :
			m == WLR_MODIFIER_ALT ? XKB_KEY_Alt_L : m == WLR_MODIFIER_LOGO ? XKB_KEY_Super_L :
			XKB_KEY_ISO_Level3_Shift;
		return true;
	}
	*sym = xkb_keysym_from_name(last, XKB_KEYSYM_NO_FLAGS);
	if (*sym == XKB_KEY_NoSymbol) {
		*sym = xkb_keysym_from_name(last, XKB_KEYSYM_CASE_INSENSITIVE);
	}
	if (*sym == XKB_KEY_NoSymbol) {
		return false;
	}
	*sym = xkb_keysym_to_lower(*sym);
	return true;
}

#define MOD_MASK (WLR_MODIFIER_SHIFT | WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT | \
	WLR_MODIFIER_LOGO | WLR_MODIFIER_MOD5)

bool binds_handle(struct wx_server *server, uint32_t mods, xkb_keysym_t sym) {
	sym = xkb_keysym_to_lower(sym);
	mods &= MOD_MASK;
	struct wx_bind *b;
	wl_list_for_each(b, &server->binds, link) {
		if (b->sym != sym || b->mods != mods) {
			continue;
		}
		if (b->owner) {
			return ipc_binding_triggered(server, b);
		}
		struct wx_buf out;
		buf_init(&out);
		ipc_cmd_run(server, NULL, b->action, &out);
		if (out.data && strstr(out.data, "\"ok\":false")) {
			wlr_log(WLR_ERROR, "binding %s -> '%s' failed: %s", b->combo, b->action, out.data);
		}
		buf_free(&out);
		return true;
	}
	return false;
}

static void bind_add(struct wx_server *server, const char *combo, const char *action,
		struct wx_ipc_client *owner) {
	uint32_t mods;
	xkb_keysym_t sym;
	if (!keybind_parse(server, combo, &mods, &sym)) {
		wlr_log(WLR_ERROR, "invalid key combo '%s'", combo);
		return;
	}
	struct wx_bind *b, *tmp;
	wl_list_for_each_safe(b, tmp, &server->binds, link) {
		if (b->mods == mods && b->sym == sym && !b->owner) {
			wl_list_remove(&b->link);
			free(b->action);
			free(b->combo);
			free(b);
		}
	}
	if (action && !strcmp(action, "none")) {
		return;
	}
	b = calloc(1, sizeof(*b));
	b->mods = mods;
	b->sym = sym;
	b->combo = strdup(combo);
	b->action = action ? strdup(action) : NULL;
	b->owner = owner;
	/* IPC-owned bindings go first so scripts can shadow config ones */
	if (owner) {
		wl_list_insert(&server->binds, &b->link);
	} else {
		wl_list_insert(server->binds.prev, &b->link);
	}
}

static const char *default_binds[][2] = {
	{"Mod+Return", "terminal"},
	{"Mod+d", "launcher"},
	{"Mod+q", "close"},
	{"Mod+Shift+q", "kill"},
	{"Mod+f", "fullscreen toggle"},
	{"Mod+m", "maximize toggle"},
	{"Mod+n", "minimize on"},
	{"Mod+space", "floating toggle"},
	{"Mod+t", "tiling toggle"},
	{"Mod+a", "above toggle"},
	{"Mod+s", "sticky toggle"},
	{"Mod+Tab", "focus next"},
	{"Mod+Shift+Tab", "focus prev"},
	{"Mod+Left", "snap left"},
	{"Mod+Right", "snap right"},
	{"Mod+Up", "maximize on"},
	{"Mod+Down", "maximize off"},
	{"Mod+h", "mfact -0.05"},
	{"Mod+l", "mfact +0.05"},
	{"Mod+Shift+r", "reload"},
	{"Mod+Shift+e", "exit"},
	{"Print", "screenshot"},
	{"Mod+Shift+s", "screenshot -w focused"},
};

void binds_reload(struct wx_server *server) {
	struct wx_bind *b, *tmp;
	wl_list_for_each_safe(b, tmp, &server->binds, link) {
		if (!b->owner) {
			wl_list_remove(&b->link);
			free(b->action);
			free(b->combo);
			free(b);
		}
	}
	for (size_t i = 0; i < sizeof(default_binds) / sizeof(default_binds[0]); i++) {
		bind_add(server, default_binds[i][0], default_binds[i][1], NULL);
	}
	for (int i = 1; i <= WX_WORKSPACES; i++) {
		char combo[32], action[48];
		snprintf(combo, sizeof(combo), "Mod+%d", i);
		snprintf(action, sizeof(action), "workspace %d", i);
		bind_add(server, combo, action, NULL);
		snprintf(combo, sizeof(combo), "Mod+Shift+%d", i);
		snprintf(action, sizeof(action), "move_to_workspace %d", i);
		bind_add(server, combo, action, NULL);
	}
	for (size_t i = 0; i < server->config.binds_len; i++) {
		bind_add(server, server->config.binds[i].combo, server->config.binds[i].action, NULL);
	}
}

/* ---------- commands ---------- */

static bool cmd_list(struct cmd *c) {
	struct wx_view *v;
	if (c->plain) {
		wl_list_for_each_reverse(v, &c->server->views, link) {
			view_plain(c->out, v);
		}
		return true;
	}
	buf_puts(c->out, "{\"ok\":true,\"windows\":[");
	bool first = true;
	wl_list_for_each_reverse(v, &c->server->views, link) {
		if (!first) {
			buf_puts(c->out, ",");
		}
		first = false;
		ipc_view_json(c->out, v);
	}
	buf_puts(c->out, "]}");
	return true;
}

static bool cmd_info(struct cmd *c) {
	if (c->argc > 1) {
		int id;
		if (!parse_int_arg(c->argv[1], &id) || !(c->target = view_by_id(c->server, (uint32_t)id))) {
			return reply_err(c->out, "no such window '%s'", c->argv[1]);
		}
	}
	if (!need_target(c)) {
		return true;
	}
	if (c->plain) {
		view_plain(c->out, c->target);
		return true;
	}
	buf_puts(c->out, "{\"ok\":true,\"window\":");
	ipc_view_json(c->out, c->target);
	buf_puts(c->out, "}");
	return true;
}

static bool cmd_focused(struct cmd *c) {
	struct wx_view *v = c->server->focused;
	if (c->plain) {
		if (v) {
			buf_printf(c->out, "%u\n", v->id);
		}
		return true;
	}
	if (v) {
		buf_printf(c->out, "{\"ok\":true,\"id\":%u}", v->id);
	} else {
		buf_puts(c->out, "{\"ok\":true,\"id\":null}");
	}
	return true;
}

static bool cmd_outputs(struct cmd *c) {
	struct wx_output *o;
	if (c->plain) {
		wl_list_for_each(o, &c->server->outputs, link) {
			buf_printf(c->out, "%s\t%d\t%d\t%d\t%d\t%.2f\t%d\n", o->wlr_output->name, o->full.x,
				o->full.y, o->full.width, o->full.height, o->wlr_output->scale, o->workspace);
		}
		return true;
	}
	buf_puts(c->out, "{\"ok\":true,\"outputs\":[");
	bool first = true;
	wl_list_for_each(o, &c->server->outputs, link) {
		struct wlr_output *w = o->wlr_output;
		buf_puts(c->out, first ? "{\"name\":" : ",{\"name\":");
		first = false;
		buf_json_str(c->out, w->name);
		buf_puts(c->out, ",\"description\":");
		buf_json_str(c->out, w->description);
		buf_printf(c->out, ",\"workspace\":%d,\"tiling\":%s,\"active\":%s", o->workspace,
			o->tiling[o->workspace] ? "true" : "false",
			o == workspace_active_output(c->server) ? "true" : "false");
		buf_printf(c->out, ",\"x\":%d,\"y\":%d,\"width\":%d,\"height\":%d,\"scale\":%.3f,"
			"\"refresh_mhz\":%d,\"enabled\":%s,\"adaptive_sync\":%s,"
			"\"usable\":{\"x\":%d,\"y\":%d,\"width\":%d,\"height\":%d}}",
			o->full.x, o->full.y, o->full.width, o->full.height, w->scale, w->refresh,
			w->enabled ? "true" : "false",
			w->adaptive_sync_status == WLR_OUTPUT_ADAPTIVE_SYNC_ENABLED ? "true" : "false",
			o->usable.x, o->usable.y, o->usable.width, o->usable.height);
	}
	buf_puts(c->out, "]}");
	return true;
}

/* -o NAME, else the output workspace commands currently apply to */
static struct wx_output *cmd_output(struct cmd *c) {
	if (c->output_name) {
		struct wx_output *o = output_by_name(c->server, c->output_name);
		if (!o) {
			reply_err(c->out, "no such output '%s'", c->output_name);
		}
		return o;
	}
	struct wx_output *o = workspace_active_output(c->server);
	if (!o) {
		reply_err(c->out, "no outputs");
	}
	return o;
}

static bool cmd_status(struct cmd *c) {
	struct wx_server *s = c->server;
	struct wx_output *o = cmd_output(c);
	if (!o) {
		return true;
	}
	int ws = o->workspace;
	buf_printf(c->out, "{\"ok\":true,\"version\":\"%s\",\"output\":\"%s\",\"workspace\":%d,"
		"\"tiling\":%s,\"mfact\":%.2f,\"nested\":%s,\"locked\":%s,\"mod\":\"%s\","
		"\"wayland_display\":",
		WX_VERSION, o->wlr_output->name, ws, o->tiling[ws] ? "true" : "false",
		o->mfact[ws], s->nested ? "true" : "false", s->locked ? "true" : "false",
		s->mod_mask == WLR_MODIFIER_ALT ? "alt" : s->mod_mask == WLR_MODIFIER_CTRL ? "ctrl" : "super");
	buf_json_str(c->out, s->socket);
	buf_puts(c->out, ",\"x11_display\":");
	buf_json_str(c->out, s->xwayland ? s->xwayland->display_name : NULL);
	buf_puts(c->out, "}");
	return true;
}

static bool cmd_cursor(struct cmd *c) {
	struct wlr_cursor *cur = c->server->cursor;
	double sx, sy;
	struct wlr_surface *surface;
	struct wx_view *v = view_at(c->server, cur->x, cur->y, &surface, &sx, &sy);
	if (c->plain) {
		buf_printf(c->out, "%d\t%d\t%u\n", (int)cur->x, (int)cur->y, v ? v->id : 0);
		return true;
	}
	buf_printf(c->out, "{\"ok\":true,\"x\":%d,\"y\":%d,\"window\":", (int)cur->x, (int)cur->y);
	if (v) {
		buf_printf(c->out, "%u}", v->id);
	} else {
		buf_puts(c->out, "null}");
	}
	return true;
}

static bool cmd_state(struct cmd *c, const char *what) {
	if (!need_target(c)) {
		return true;
	}
	struct wx_view *v = c->target;
	bool on;
	if (!strcmp(what, "maximize")) {
		if (parse_switch(c, 1, v->maximized, &on)) {
			view_set_maximized(v, on);
		} else {
			return true;
		}
	} else if (!strcmp(what, "fullscreen")) {
		if (parse_switch(c, 1, v->fullscreen, &on)) {
			view_set_fullscreen(v, on);
		} else {
			return true;
		}
	} else if (!strcmp(what, "minimize")) {
		if (parse_switch(c, 1, v->minimized, &on)) {
			view_set_minimized(v, on);
			if (!on) {
				seat_focus_view(c->server, v);
			}
		} else {
			return true;
		}
	} else if (!strcmp(what, "above")) {
		if (parse_switch(c, 1, v->stack == WX_STACK_ABOVE, &on)) {
			view_set_stack(v, on ? WX_STACK_ABOVE : WX_STACK_NORMAL);
		} else {
			return true;
		}
	} else if (!strcmp(what, "below")) {
		if (parse_switch(c, 1, v->stack == WX_STACK_BELOW, &on)) {
			view_set_stack(v, on ? WX_STACK_BELOW : WX_STACK_NORMAL);
		} else {
			return true;
		}
	} else if (!strcmp(what, "sticky")) {
		if (parse_switch(c, 1, v->sticky, &on)) {
			view_set_sticky(v, on);
		} else {
			return true;
		}
	} else if (!strcmp(what, "floating")) {
		if (parse_switch(c, 1, v->floating, &on)) {
			view_set_floating(v, on);
		} else {
			return true;
		}
	}
	return reply_ok(c->out);
}

static bool cmd_geometry(struct cmd *c, const char *what) {
	if (!need_target(c)) {
		return true;
	}
	struct wx_view *v = c->target;
	int a[4];
	int need = !strcmp(what, "geometry") ? 4 : 2;
	if (c->argc != need + 1) {
		return reply_err(c->out, "usage: %s %s", what, need == 4 ? "X Y W H" : "A B");
	}
	for (int i = 0; i < need; i++) {
		if (!parse_int_arg(c->argv[i + 1], &a[i])) {
			return reply_err(c->out, "invalid number '%s'", c->argv[i + 1]);
		}
	}
	if (v->fullscreen || v->maximized) {
		return reply_err(c->out, "window is %s", v->fullscreen ? "fullscreen" : "maximized");
	}
	if (view_is_tiled(v)) {
		view_set_floating(v, true);
	}
	if (!strcmp(what, "move")) {
		view_move(v, a[0], a[1]);
	} else if (!strcmp(what, "resize")) {
		if (a[0] < 1 || a[1] < 1) {
			return reply_err(c->out, "size must be positive");
		}
		view_resize(v, a[0], a[1]);
	} else {
		if (a[2] < 1 || a[3] < 1) {
			return reply_err(c->out, "size must be positive");
		}
		view_set_box(v, a[0], a[1], a[2], a[3]);
	}
	ipc_event_window(c->server, "geometry", v);
	return reply_ok(c->out);
}

static bool cmd_focus(struct cmd *c) {
	if (c->argc > 1 && !strcmp(c->argv[1], "next")) {
		view_cycle_focus(c->server, true);
		return reply_ok(c->out);
	}
	if (c->argc > 1 && !strcmp(c->argv[1], "prev")) {
		view_cycle_focus(c->server, false);
		return reply_ok(c->out);
	}
	if (c->argc > 1) {
		int id;
		if (!parse_int_arg(c->argv[1], &id) || !(c->target = view_by_id(c->server, (uint32_t)id))) {
			return reply_err(c->out, "no such window '%s'", c->argv[1]);
		}
	}
	if (!need_target(c)) {
		return true;
	}
	seat_focus_view(c->server, c->target);
	return reply_ok(c->out);
}

static bool cmd_snap(struct cmd *c) {
	if (!need_target(c)) {
		return true;
	}
	struct wx_view *v = c->target;
	if (c->argc < 2) {
		return reply_err(c->out, "usage: snap left|right|max");
	}
	struct wx_output *o = output_for_view(v);
	if (!o) {
		return reply_err(c->out, "window is on no output");
	}
	if (!strcmp(c->argv[1], "max")) {
		view_set_maximized(v, true);
		return reply_ok(c->out);
	}
	bool left = !strcmp(c->argv[1], "left");
	if (!left && strcmp(c->argv[1], "right")) {
		return reply_err(c->out, "usage: snap left|right|max");
	}
	if (v->maximized) {
		view_set_maximized(v, false);
	}
	if (v->fullscreen) {
		view_set_fullscreen(v, false);
	}
	if (view_is_tiled(v)) {
		view_set_floating(v, true);
	}
	int l, r, t, b, gap = c->server->config.gap;
	view_frame_extents(v, &l, &r, &t, &b);
	struct wlr_box u = o->usable;
	int half = u.width / 2;
	v->saved = (struct wlr_box){v->x, v->y, v->w, v->h};
	view_set_box(v, (left ? u.x : u.x + half) + gap + l, u.y + gap + t,
		half - 2 * gap - l - r, u.height - 2 * gap - t - b);
	return reply_ok(c->out);
}

static bool cmd_workspace(struct cmd *c, bool move) {
	int ws;
	if (c->argc < 2 || !parse_int_arg(c->argv[1], &ws) || ws < 1 || ws > WX_WORKSPACES) {
		return reply_err(c->out, "workspace must be 1-%d", WX_WORKSPACES);
	}
	if (move) {
		if (!need_target(c)) {
			return true;
		}
		view_set_workspace(c->target, ws);
	} else {
		struct wx_output *o = cmd_output(c);
		if (!o) {
			return true;
		}
		workspace_switch(c->server, o, ws);
	}
	return reply_ok(c->out);
}

static bool cmd_tiling(struct cmd *c) {
	struct wx_server *s = c->server;
	struct wx_output *o = cmd_output(c);
	if (!o) {
		return true;
	}
	int ws = o->workspace;
	bool on;
	if (!parse_switch(c, 1, o->tiling[ws], &on)) {
		return true;
	}
	o->tiling[ws] = on;
	if (!on) {
		/* give floating windows back their previous geometry */
		struct wx_view *v;
		wl_list_for_each(v, &s->views, link) {
			if (view_output(v) == o && v->workspace == ws && v->saved.width > 0 && !v->floating &&
					!v->maximized && !v->fullscreen) {
				view_set_box(v, v->saved.x, v->saved.y, v->saved.width, v->saved.height);
			}
		}
	} else {
		struct wx_view *v;
		wl_list_for_each(v, &s->views, link) {
			if (view_output(v) == o && v->workspace == ws && !v->floating && !v->maximized &&
					!v->fullscreen) {
				v->saved = (struct wlr_box){v->x, v->y, v->w, v->h};
			}
		}
	}
	layout_arrange(s);
	return reply_ok(c->out);
}

static bool cmd_mfact(struct cmd *c) {
	struct wx_server *s = c->server;
	double v;
	if (c->argc < 2 || !parse_double_arg(c->argv[1], &v)) {
		return reply_err(c->out, "usage: mfact 0.6|+0.05|-0.05");
	}
	struct wx_output *o = cmd_output(c);
	if (!o) {
		return true;
	}
	double *m = &o->mfact[o->workspace];
	*m = (c->argv[1][0] == '+' || c->argv[1][0] == '-') ? *m + v : v;
	if (*m < 0.1) {
		*m = 0.1;
	} else if (*m > 0.9) {
		*m = 0.9;
	}
	layout_arrange(s);
	return reply_ok(c->out);
}

static bool cmd_exec(struct cmd *c, const char *line) {
	const char *p = line;
	while (isspace((unsigned char)*p)) {
		p++;
	}
	p += 4; /* "exec" */
	while (isspace((unsigned char)*p)) {
		p++;
	}
	if (!*p) {
		return reply_err(c->out, "usage: exec COMMAND");
	}
	server_spawn(c->server, p);
	return reply_ok(c->out);
}

static int x11_button(int b) {
	switch (b) {
	case 1: return BTN_LEFT;
	case 2: return BTN_MIDDLE;
	case 3: return BTN_RIGHT;
	case 8: return BTN_SIDE;
	case 9: return BTN_EXTRA;
	default: return 0;
	}
}

static void scroll_button(struct wx_server *s, int b) {
	uint32_t t = input_now();
	switch (b) {
	case 4: input_pointer_axis(s, t, false, -15); break;
	case 5: input_pointer_axis(s, t, false, 15); break;
	case 6: input_pointer_axis(s, t, true, -15); break;
	case 7: input_pointer_axis(s, t, true, 15); break;
	}
}

static bool cmd_click(struct cmd *c, int mode) {
	/* mode: 0 click, 1 down, 2 up */
	int button = 1, count = 1;
	if (c->argc > 1 && (!parse_int_arg(c->argv[1], &button) || button < 1 || button > 9)) {
		return reply_err(c->out, "button must be 1-9 (X11 numbering)");
	}
	if (mode == 0 && c->argc > 2 && (!parse_int_arg(c->argv[2], &count) || count < 1 || count > 50)) {
		return reply_err(c->out, "count must be 1-50");
	}
	struct wx_server *s = c->server;
	if (button >= 4 && button <= 7) {
		if (mode != 2) {
			for (int i = 0; i < count; i++) {
				scroll_button(s, button);
			}
		}
		return reply_ok(c->out);
	}
	int code = x11_button(button);
	for (int i = 0; i < count; i++) {
		if (mode != 2) {
			input_pointer_button(s, input_now(), code, true);
			wlr_seat_pointer_notify_frame(s->seat);
		}
		if (mode != 1) {
			input_pointer_button(s, input_now(), code, false);
			wlr_seat_pointer_notify_frame(s->seat);
		}
	}
	return reply_ok(c->out);
}

static bool cmd_mousemove(struct cmd *c, bool relative) {
	int x, y;
	if (c->argc != 3 || !parse_int_arg(c->argv[1], &x) || !parse_int_arg(c->argv[2], &y)) {
		return reply_err(c->out, "usage: %s X Y", relative ? "mousemove_rel" : "mousemove");
	}
	inject_mouse_move(c->server, x, y, relative);
	return reply_ok(c->out);
}

static bool cmd_scroll(struct cmd *c) {
	double dx, dy;
	if (c->argc != 3 || !parse_double_arg(c->argv[1], &dx) || !parse_double_arg(c->argv[2], &dy)) {
		return reply_err(c->out, "usage: scroll DX DY");
	}
	uint32_t t = input_now();
	if (dx != 0) {
		input_pointer_axis(c->server, t, true, dx);
	}
	if (dy != 0) {
		input_pointer_axis(c->server, t, false, dy);
	}
	return reply_ok(c->out);
}

static bool cmd_type(struct cmd *c) {
	if (c->argc < 2) {
		return reply_err(c->out, "usage: type TEXT");
	}
	struct wx_buf text;
	buf_init(&text);
	for (int i = 1; i < c->argc; i++) {
		if (i > 1) {
			buf_puts(&text, " ");
		}
		buf_puts(&text, c->argv[i]);
	}
	char *err = NULL;
	bool ok = inject_type(c->server, text.data, &err);
	buf_free(&text);
	if (!ok) {
		reply_err(c->out, "%s", err ? err : "type failed");
		free(err);
		return true;
	}
	return reply_ok(c->out);
}

static bool cmd_key(struct cmd *c, int mode) {
	/* mode: 0 press+release, 1 down, 2 up */
	if (c->argc < 2) {
		return reply_err(c->out, "usage: key COMBO [COMBO...]");
	}
	for (int i = 1; i < c->argc; i++) {
		char *err = NULL;
		if (!inject_key_combo(c->server, c->argv[i], mode, &err)) {
			reply_err(c->out, "%s", err ? err : "key failed");
			free(err);
			return true;
		}
	}
	return reply_ok(c->out);
}

static char *default_screenshot_path(struct wx_server *s) {
	const char *dir = s->config.screenshot_dir;
	const char *home = getenv("HOME");
	char d[2048];
	if (dir && dir[0] == '~' && home) {
		snprintf(d, sizeof(d), "%s%s", home, dir + 1);
	} else {
		snprintf(d, sizeof(d), "%s", dir ? dir : (home ? home : "/tmp"));
	}
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	char name[64];
	strftime(name, sizeof(name), "waylandx-%Y%m%d-%H%M%S.png", &tm);
	char *path = malloc(strlen(d) + strlen(name) + 2);
	sprintf(path, "%s/%s", d, name);
	return path;
}

static bool cmd_screenshot(struct cmd *c) {
	struct wx_server *s = c->server;
	struct wlr_box box = {0};
	if (c->has_region) {
		box = c->region;
	} else if (c->target_given) {
		view_frame_box(c->target, &box);
	} else if (c->output_name) {
		struct wx_output *o = output_by_name(s, c->output_name);
		if (!o) {
			return reply_err(c->out, "no such output '%s'", c->output_name);
		}
		box = o->full;
	} else {
		/* whole layout */
		struct wx_output *o;
		bool first = true;
		wl_list_for_each(o, &s->outputs, link) {
			if (wlr_box_empty(&o->full)) {
				continue;
			}
			if (first) {
				box = o->full;
				first = false;
			} else {
				int x1 = box.x < o->full.x ? box.x : o->full.x;
				int y1 = box.y < o->full.y ? box.y : o->full.y;
				int x2 = box.x + box.width > o->full.x + o->full.width ? box.x + box.width :
					o->full.x + o->full.width;
				int y2 = box.y + box.height > o->full.y + o->full.height ? box.y + box.height :
					o->full.y + o->full.height;
				box = (struct wlr_box){x1, y1, x2 - x1, y2 - y1};
			}
		}
	}
	char *path = c->argc > 1 ? strdup(c->argv[1]) : default_screenshot_path(s);
	if (path[0] != '/') {
		free(path);
		return reply_err(c->out, "path must be absolute");
	}
	char *err = NULL;
	bool ok = capture_start(s, c->client, path, box, &err);
	free(path);
	if (!ok) {
		reply_err(c->out, "%s", err ? err : "capture failed");
		free(err);
		return true;
	}
	/* deferred: the reply is sent from capture.c once the frame is read back */
	return c->client == NULL;
}

static bool cmd_bind(struct cmd *c, bool add) {
	if (!c->client) {
		return reply_err(c->out, "bind is only available over IPC");
	}
	if (c->argc != 2) {
		return reply_err(c->out, "usage: %s COMBO", add ? "bind" : "unbind");
	}
	uint32_t mods;
	xkb_keysym_t sym;
	if (!keybind_parse(c->server, c->argv[1], &mods, &sym)) {
		return reply_err(c->out, "invalid key combo '%s'", c->argv[1]);
	}
	struct wx_bind *b, *tmp;
	wl_list_for_each_safe(b, tmp, &c->server->binds, link) {
		if (b->owner == c->client && b->mods == mods && b->sym == sym) {
			wl_list_remove(&b->link);
			free(b->combo);
			free(b);
		}
	}
	if (add) {
		/* binding events arrive on this same connection */
		bind_add(c->server, c->argv[1], NULL, c->client);
	}
	return reply_ok(c->out);
}

static const char *help_text =
	"commands (window target: -w ID|focused, default focused):\n"
	"  version | status | list [--plain] | info [ID] [--plain] | focused [--plain]\n"
	"  outputs [--plain] | cursor [--plain]\n"
	"  focus [ID|next|prev] | close | kill | move X Y | resize W H | geometry X Y W H\n"
	"  maximize|fullscreen|minimize|above|below|sticky|floating [on|off|toggle]\n"
	"  snap left|right|max | workspace N [-o OUT] | move_to_workspace N\n"
	"  tiling [on|off|toggle] [-o OUT]\n"
	"  mfact V|+D|-D | exec CMD | terminal | launcher | reload | exit\n"
	"  type TEXT | key COMBO... | keydown COMBO | keyup COMBO\n"
	"  mousemove X Y | mousemove_rel DX DY | click [BUTTON] [COUNT] | mousedown B | mouseup B\n"
	"  scroll DX DY | screenshot [PATH] [-o OUTPUT|-w ID|-r X,Y,W,H]\n"
	"  bind COMBO | unbind COMBO | subscribe | ping";

bool ipc_cmd_run(struct wx_server *server, struct wx_ipc_client *client, const char *line,
		struct wx_buf *out) {
	size_t len = strlen(line);
	char *storage = malloc(len + 2);
	char *argv[MAX_ARGS];
	int argc = tokenize(line, argv, storage, len + 2);
	bool done = true;
	if (argc < 0) {
		reply_err(out, "could not parse command (unbalanced quotes or too many args)");
		goto out;
	}
	if (argc == 0) {
		reply_err(out, "empty command");
		goto out;
	}
	struct cmd c = {.server = server, .client = client, .out = out, .argc = argc, .argv = argv};
	if (!extract_options(&c)) {
		goto out;
	}
	if (c.argc == 0) {
		reply_err(out, "empty command");
		goto out;
	}
	const char *name = c.argv[0];
	if (!strcmp(name, "ping")) {
		buf_puts(out, "{\"ok\":true,\"pong\":true}");
	} else if (!strcmp(name, "version")) {
		buf_printf(out, "{\"ok\":true,\"version\":\"%s\"}", WX_VERSION);
	} else if (!strcmp(name, "help")) {
		buf_puts(out, "{\"ok\":true,\"help\":");
		buf_json_str(out, help_text);
		buf_puts(out, "}");
	} else if (!strcmp(name, "status")) {
		cmd_status(&c);
	} else if (!strcmp(name, "list")) {
		cmd_list(&c);
	} else if (!strcmp(name, "info")) {
		cmd_info(&c);
	} else if (!strcmp(name, "focused")) {
		cmd_focused(&c);
	} else if (!strcmp(name, "outputs")) {
		cmd_outputs(&c);
	} else if (!strcmp(name, "cursor")) {
		cmd_cursor(&c);
	} else if (!strcmp(name, "focus")) {
		cmd_focus(&c);
	} else if (!strcmp(name, "close") || !strcmp(name, "kill")) {
		if (need_target(&c)) {
			if (name[0] == 'c') {
				view_close(c.target);
			} else {
				view_kill(c.target);
			}
			reply_ok(out);
		}
	} else if (!strcmp(name, "move") || !strcmp(name, "resize") || !strcmp(name, "geometry")) {
		cmd_geometry(&c, name);
	} else if (!strcmp(name, "maximize") || !strcmp(name, "fullscreen") ||
			!strcmp(name, "minimize") || !strcmp(name, "above") || !strcmp(name, "below") ||
			!strcmp(name, "sticky") || !strcmp(name, "floating")) {
		cmd_state(&c, name);
	} else if (!strcmp(name, "snap")) {
		cmd_snap(&c);
	} else if (!strcmp(name, "workspace")) {
		cmd_workspace(&c, false);
	} else if (!strcmp(name, "move_to_workspace")) {
		cmd_workspace(&c, true);
	} else if (!strcmp(name, "tiling")) {
		cmd_tiling(&c);
	} else if (!strcmp(name, "mfact")) {
		cmd_mfact(&c);
	} else if (!strcmp(name, "exec")) {
		cmd_exec(&c, line);
	} else if (!strcmp(name, "terminal")) {
		server_spawn(server, server->config.terminal);
		reply_ok(out);
	} else if (!strcmp(name, "launcher")) {
		struct wx_output *ao = workspace_active_output(server);
		char ev[160];
		snprintf(ev, sizeof(ev), "{\"event\":\"launcher\",\"output\":\"%s\"}",
			ao ? ao->wlr_output->name : "");
		ipc_broadcast_event(server, ev);
		reply_ok(out);
	} else if (!strcmp(name, "reload")) {
		char *err = NULL;
		if (server_reload(server, &err)) {
			reply_ok(out);
		} else {
			reply_err(out, "config error, kept previous: %s", err ? err : "?");
		}
		free(err);
	} else if (!strcmp(name, "exit")) {
		wl_display_terminate(server->display);
		reply_ok(out);
	} else if (!strcmp(name, "type")) {
		cmd_type(&c);
	} else if (!strcmp(name, "key")) {
		cmd_key(&c, 0);
	} else if (!strcmp(name, "keydown")) {
		cmd_key(&c, 1);
	} else if (!strcmp(name, "keyup")) {
		cmd_key(&c, 2);
	} else if (!strcmp(name, "mousemove")) {
		cmd_mousemove(&c, false);
	} else if (!strcmp(name, "mousemove_rel")) {
		cmd_mousemove(&c, true);
	} else if (!strcmp(name, "click")) {
		cmd_click(&c, 0);
	} else if (!strcmp(name, "mousedown")) {
		cmd_click(&c, 1);
	} else if (!strcmp(name, "mouseup")) {
		cmd_click(&c, 2);
	} else if (!strcmp(name, "scroll")) {
		cmd_scroll(&c);
	} else if (!strcmp(name, "screenshot")) {
		done = cmd_screenshot(&c);
	} else if (!strcmp(name, "bind")) {
		cmd_bind(&c, true);
	} else if (!strcmp(name, "unbind")) {
		cmd_bind(&c, false);
	} else if (!strcmp(name, "subscribe")) {
		if (!client) {
			reply_err(out, "subscribe is only available over IPC");
		} else {
			client->subscribed = true;
			reply_ok(out);
		}
	} else {
		reply_err(out, "unknown command '%s' (try 'help')", name);
	}
out:
	free(storage);
	return done;
}
