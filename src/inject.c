#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>
#include "json.h"
#include "server.h"

/*
 * Input injection (xdotool-like). Keys are sent straight into the seat from a
 * private wlr_keyboard whose keymap is generated on the fly, so any Unicode
 * text can be typed regardless of the user's layout (same trick as wtype).
 */

#define FIRST_KEYCODE 9
#define MAX_KEYS (255 - FIRST_KEYCODE)

static const struct wlr_keyboard_impl inject_impl = {
	.name = "waylandx-inject",
};

struct keyent {
	xkb_keysym_t sym;
	bool two_level; /* [lower, upper] so Shift works for combos */
};

static char *build_keymap(const struct keyent *keys, int n) {
	struct wx_buf b;
	buf_init(&b);
	buf_printf(&b, "xkb_keymap {\nxkb_keycodes \"wx\" {\nminimum = 8;\nmaximum = %d;\n",
		FIRST_KEYCODE + (n > 0 ? n : 1));
	for (int i = 0; i < n; i++) {
		buf_printf(&b, "<K%d> = %d;\n", i, FIRST_KEYCODE + i);
	}
	buf_puts(&b, "};\nxkb_types \"wx\" { include \"complete\" };\n"
		"xkb_compatibility \"wx\" { include \"complete\" };\nxkb_symbols \"wx\" {\n");
	for (int i = 0; i < n; i++) {
		char name[64], upper[64];
		xkb_keysym_get_name(keys[i].sym, name, sizeof(name));
		xkb_keysym_t up = xkb_keysym_to_upper(keys[i].sym);
		if (keys[i].two_level && up != keys[i].sym) {
			xkb_keysym_get_name(up, upper, sizeof(upper));
			buf_printf(&b, "key <K%d> {[ %s, %s ]};\n", i, name, upper);
		} else {
			buf_printf(&b, "key <K%d> {[ %s ]};\n", i, name);
		}
	}
	buf_puts(&b, "};\n};\n");
	return b.data;
}

static bool set_inject_keymap(struct wx_server *server, const struct keyent *keys, int n,
		char **err) {
	char *text = build_keymap(keys, n);
	if (server->inject_keymap_cache && !strcmp(server->inject_keymap_cache, text)) {
		free(text);
		return true;
	}
	struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_keymap *km = xkb_keymap_new_from_string(ctx, text, XKB_KEYMAP_FORMAT_TEXT_V1,
		XKB_KEYMAP_COMPILE_NO_FLAGS);
	xkb_context_unref(ctx);
	if (!km) {
		free(text);
		*err = strdup("failed to compile injection keymap");
		return false;
	}
	wlr_keyboard_set_keymap(server->inject_kb, km);
	xkb_keymap_unref(km);
	free(server->inject_keymap_cache);
	server->inject_keymap_cache = text;
	return true;
}

static void send_mods(struct wx_server *server, uint32_t mods) {
	/* keep the injected keyboard's own state in sync so held modifiers
	 * also drive compositor actions (e.g. keydown super + drag) */
	wlr_keyboard_notify_modifiers(server->inject_kb, mods, 0, 0, 0);
	wlr_seat_keyboard_notify_modifiers(server->seat, &server->inject_kb->modifiers);
	switcher_modifiers(server, mods); /* keyup alt ends an Alt+Tab, as on a real keyboard */
}

static void send_key(struct wx_server *server, int index, bool pressed) {
	wlr_seat_keyboard_notify_key(server->seat, input_now(),
		(uint32_t)(FIRST_KEYCODE + index - 8),
		pressed ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED);
}

static void begin(struct wx_server *server) {
	wlr_seat_set_keyboard(server->seat, server->inject_kb);
}

static void end(struct wx_server *server) {
	/* hand the seat back to the real keyboard (resends its keymap) */
	if (server->last_keyboard) {
		wlr_seat_set_keyboard(server->seat, server->last_keyboard->wlr_keyboard);
		wlr_seat_keyboard_notify_modifiers(server->seat,
			&server->last_keyboard->wlr_keyboard->modifiers);
	}
}

static xkb_keysym_t char_keysym(uint32_t cp) {
	switch (cp) {
	case '\n': return XKB_KEY_Return;
	case '\t': return XKB_KEY_Tab;
	case '\b': return XKB_KEY_BackSpace;
	case 0x1b: return XKB_KEY_Escape;
	default: return xkb_utf32_to_keysym(cp);
	}
}

/* Decodes one UTF-8 sequence; returns bytes consumed or 0 on invalid input. */
static int utf8_decode(const unsigned char *s, uint32_t *cp) {
	if (s[0] < 0x80) {
		*cp = s[0];
		return 1;
	}
	int len = (s[0] & 0xe0) == 0xc0 ? 2 : (s[0] & 0xf0) == 0xe0 ? 3 : (s[0] & 0xf8) == 0xf0 ? 4 : 0;
	if (!len) {
		return 0;
	}
	uint32_t v = s[0] & (0xff >> (len + 1));
	for (int i = 1; i < len; i++) {
		if ((s[i] & 0xc0) != 0x80) {
			return 0;
		}
		v = (v << 6) | (s[i] & 0x3f);
	}
	*cp = v;
	return len;
}

bool inject_type(struct wx_server *server, const char *utf8, char **err) {
	size_t cap = strlen(utf8) + 1;
	xkb_keysym_t *syms = malloc(cap * sizeof(*syms));
	size_t nsyms = 0;
	for (const unsigned char *p = (const unsigned char *)utf8; *p;) {
		uint32_t cp;
		int n = utf8_decode(p, &cp);
		if (n == 0) {
			free(syms);
			*err = strdup("invalid UTF-8 in text");
			return false;
		}
		p += n;
		xkb_keysym_t sym = char_keysym(cp);
		if (sym == XKB_KEY_NoSymbol) {
			continue;
		}
		syms[nsyms++] = sym;
	}

	begin(server);
	send_mods(server, 0);
	size_t i = 0;
	while (i < nsyms) {
		/* each chunk uses at most MAX_KEYS distinct keysyms */
		struct keyent keys[MAX_KEYS];
		int nkeys = 0;
		size_t j = i;
		for (; j < nsyms; j++) {
			int k;
			for (k = 0; k < nkeys && keys[k].sym != syms[j]; k++) {
			}
			if (k == nkeys) {
				if (nkeys == MAX_KEYS) {
					break;
				}
				keys[nkeys++] = (struct keyent){syms[j], false};
			}
		}
		if (!set_inject_keymap(server, keys, nkeys, err)) {
			end(server);
			free(syms);
			return false;
		}
		/* the seat only forwards a new keymap on keyboard change; force it */
		wlr_seat_set_keyboard(server->seat, NULL);
		wlr_seat_set_keyboard(server->seat, server->inject_kb);
		for (size_t t = i; t < j; t++) {
			int k;
			for (k = 0; k < nkeys && keys[k].sym != syms[t]; k++) {
			}
			send_key(server, k, true);
			send_key(server, k, false);
		}
		i = j;
	}
	end(server);
	free(syms);
	return true;
}

bool inject_key_combo(struct wx_server *server, const char *combo, int press_mode, char **err) {
	uint32_t mods;
	xkb_keysym_t sym;
	if (!keybind_parse(server, combo, &mods, &sym)) {
		char buf[160];
		snprintf(buf, sizeof(buf), "invalid key combo '%s'", combo);
		*err = strdup(buf);
		return false;
	}
	/* modifiers held by earlier keydowns stay held, like xdotool keydown */
	static uint32_t held;
	/* Like X11 XTest: injected keys can trigger compositor bindings too,
	 * including with modifiers held by an earlier keydown. */
	if (press_mode == 0 && (switcher_key(server, sym) || binds_handle(server, held | mods, sym))) {
		return true;
	}
	struct keyent key = {sym, true};
	begin(server);
	if (!set_inject_keymap(server, &key, 1, err)) {
		end(server);
		return false;
	}
	wlr_seat_set_keyboard(server->seat, NULL);
	wlr_seat_set_keyboard(server->seat, server->inject_kb);
	if (press_mode == 1) {
		held |= mods;
	}
	if (press_mode != 2) {
		send_mods(server, held | mods);
		send_key(server, 0, true);
	}
	if (press_mode != 1) {
		send_key(server, 0, false);
		if (press_mode == 2) {
			held &= ~mods;
		}
		send_mods(server, held);
	}
	/* the injected keyboard stays active while anything is held */
	if (press_mode != 1 && held == 0) {
		end(server);
	}
	return true;
}

void inject_mouse_move(struct wx_server *server, double x, double y, bool relative) {
	struct wlr_cursor *c = server->cursor;
	double dx = relative ? x : x - c->x, dy = relative ? y : y - c->y;
	wlr_relative_pointer_manager_v1_send_relative_motion(server->relative_pointer, server->seat,
		(uint64_t)input_now() * 1000, dx, dy, dx, dy);
	if (relative) {
		wlr_cursor_move(c, NULL, x, y);
	} else {
		wlr_cursor_warp_closest(c, NULL, x, y);
	}
	input_process_motion(server, input_now());
	wlr_seat_pointer_notify_frame(server->seat);
}

void inject_init(struct wx_server *server) {
	struct wlr_keyboard *kb = calloc(1, sizeof(*kb));
	wlr_keyboard_init(kb, &inject_impl, "waylandx-inject");
	server->inject_kb = kb;
	struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_rule_names names = {
		.layout = server->config.kb_layout, .variant = server->config.kb_variant,
		.options = server->config.kb_options, .model = server->config.kb_model,
	};
	struct xkb_keymap *km = xkb_keymap_new_from_names(ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (!km) {
		km = xkb_keymap_new_from_names(ctx, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
	}
	xkb_context_unref(ctx);
	if (km) {
		wlr_keyboard_set_keymap(kb, km);
		xkb_keymap_unref(km);
	}
	/* headless/remote sessions may have no physical keyboard at all */
	if (!wlr_seat_get_keyboard(server->seat)) {
		wlr_seat_set_keyboard(server->seat, kb);
	}
	wlr_seat_set_capabilities(server->seat, WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD);
}

void inject_finish(struct wx_server *server) {
	if (server->inject_kb) {
		wlr_keyboard_finish(server->inject_kb);
		free(server->inject_kb);
		server->inject_kb = NULL;
	}
	free(server->inject_keymap_cache);
}
