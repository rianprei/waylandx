#include <stdio.h>
#include <string.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>
#include "json.h"
#include "server.h"

/*
 * Alt+Tab / Mod+Tab window switcher, like KWin's: hold the modifier, press
 * Tab to walk the windows in most-recently-used order, release to switch.
 * Escape cancels. The panel draws the list from the IPC events; without a
 * panel it still works, just blind. Without a held modifier (IPC, scripts)
 * it switches right away, like the old "focus next".
 */

#define MAX_SW 64

static struct {
	bool active;
	uint32_t mods;          /* released -> commit */
	uint32_t ids[MAX_SW];   /* ids, not pointers: windows may close meanwhile */
	int n, index;
} sw;

static bool on_current_workspace(struct wx_view *v) {
	if (v->sticky || !v->output) {
		return true;
	}
	return v->workspace == v->output->workspace;
}

static bool switchable(struct wx_view *v) {
	return v->mapped && !v->unmanaged && !v->skip_taskbar &&
		(view_is_visible(v) || (v->minimized && on_current_workspace(v)));
}

static void build_list(struct wx_server *server) {
	sw.n = 0;
	struct wx_view *v;
	wl_list_for_each(v, &server->views, link) { /* front = most recently focused */
		if (sw.n < MAX_SW && switchable(v)) {
			sw.ids[sw.n++] = v->id;
		}
	}
}

static void broadcast(struct wx_server *server) {
	struct wx_buf b;
	buf_init(&b);
	buf_printf(&b, "{\"event\":\"switcher\",\"index\":%d,\"windows\":[", sw.index);
	for (int i = 0; i < sw.n; i++) {
		struct wx_view *v = view_by_id(server, sw.ids[i]);
		buf_puts(&b, i ? ",{\"title\":" : "{\"title\":");
		buf_json_str(&b, v ? view_title(v) : "");
		buf_puts(&b, ",\"app_id\":");
		buf_json_str(&b, v ? view_app_id(v) : "");
		buf_puts(&b, "}");
	}
	buf_puts(&b, "]}");
	ipc_broadcast_event(server, b.data);
	buf_free(&b);
}

static void activate(struct wx_server *server, struct wx_view *v) {
	if (v->minimized) {
		view_set_minimized(v, false);
	}
	if (!on_current_workspace(v)) {
		workspace_switch(server, v->output, v->workspace);
	}
	seat_focus_view(server, v);
}

static void finish(struct wx_server *server, bool commit) {
	if (!sw.active) {
		return;
	}
	sw.active = false;
	ipc_broadcast_event(server, "{\"event\":\"switcher\",\"done\":true}");
	struct wx_view *v = commit && sw.n ? view_by_id(server, sw.ids[sw.index]) : NULL;
	if (v) {
		activate(server, v);
	}
}

static uint32_t held_mods(struct wx_server *server) {
	struct wlr_keyboard *kb = wlr_seat_get_keyboard(server->seat);
	uint32_t m = kb ? wlr_keyboard_get_modifiers(kb) : 0;
	return m & (WLR_MODIFIER_ALT | WLR_MODIFIER_CTRL | WLR_MODIFIER_LOGO);
}

void switcher_step(struct wx_server *server, bool forward) {
	if (!sw.active) {
		build_list(server);
		if (sw.n == 0) {
			return;
		}
		uint32_t held = held_mods(server);
		if (!held) { /* scripted: no modifier to wait for */
			struct wx_view *v = view_by_id(server, sw.ids[forward ? (sw.n > 1 ? 1 : 0) : sw.n - 1]);
			if (v) {
				activate(server, v);
			}
			return;
		}
		sw.active = true;
		sw.mods = held;
		sw.index = forward ? (sw.n > 1 ? 1 : 0) : sw.n - 1;
	} else {
		sw.index = ((sw.index + (forward ? 1 : -1)) % sw.n + sw.n) % sw.n;
	}
	broadcast(server);
}

/* Keyboard modifiers changed: letting go of the switch modifier picks. */
void switcher_modifiers(struct wx_server *server, uint32_t mods) {
	if (sw.active && !(mods & sw.mods)) {
		finish(server, true);
	}
}

/* Keys while switching; true when consumed. */
bool switcher_key(struct wx_server *server, xkb_keysym_t sym) {
	if (!sw.active) {
		return false;
	}
	switch (sym) {
	case XKB_KEY_Escape:
		finish(server, false);
		return true;
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
		finish(server, true);
		return true;
	case XKB_KEY_Right:
	case XKB_KEY_Down:
		switcher_step(server, true);
		return true;
	case XKB_KEY_Left:
	case XKB_KEY_Up:
		switcher_step(server, false);
		return true;
	default:
		return false;
	}
}

bool switcher_active(void) {
	return sw.active;
}
