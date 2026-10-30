#define _GNU_SOURCE
#include <cairo.h>
#include <errno.h>
#include <pango/pangocairo.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include "ext-workspace-v1-client-protocol.h"
#include "launcher.h"
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

/*
 * waylandx-panel: one top bar per monitor with that monitor's workspaces and
 * windows, a clock and a keyboard launcher. Uses only standard protocols
 * (layer-shell, foreign-toplevel, ext-workspace) plus the WaylandX IPC
 * "launcher" event for Mod+D.
 */

#define BAR_H 28
#define MAX_TASKS 128
#define MAX_WS 64
#define MAX_GROUPS 16
#define MAX_OUTS 8
#define MAX_MATCH 12
#define FONT "Sans 10"

struct task {
	struct zwlr_foreign_toplevel_handle_v1 *handle;
	char *title, *app_id;
	bool activated, minimized, maximized;
	struct wl_output *outputs[MAX_OUTS];
	int nout;
};

struct ws_group {
	struct ext_workspace_group_handle_v1 *handle;
	struct wl_output *output;
};

struct workspace {
	struct ext_workspace_handle_v1 *handle;
	struct ws_group *group;
	char *name;
	uint32_t coord;
	bool active;
};

struct hit {
	int kind; /* 0 workspace, 1 task, 2 apps button */
	int index;
	double x0, x1;
};

struct bar {
	struct wl_list link;
	uint32_t global;
	struct wl_output *output;
	char *output_name;
	int scale;
	struct wl_surface *surface;
	struct zwlr_layer_surface_v1 *layer;
	int width, height;
	bool configured, dirty;
	struct hit hits[MAX_WS + MAX_TASKS + 1];
	int nhits;
};

static struct {
	struct wl_display *display;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct wl_seat *seat;
	struct zwlr_layer_shell_v1 *layer_shell;
	struct zwlr_foreign_toplevel_manager_v1 *ftl;
	struct ext_workspace_manager_v1 *ws_mgr;
	struct wl_pointer *pointer;
	struct wl_keyboard *keyboard;
	struct xkb_context *xkb;
	struct xkb_keymap *keymap;
	struct xkb_state *xkb_state;
	bool running;
	double ptr_x;
	struct bar *ptr_bar;

	struct wl_list bars;
	struct task tasks[MAX_TASKS];
	int ntasks;
	struct ws_group groups[MAX_GROUPS];
	int ngroups;
	struct workspace ws[MAX_WS];
	int nws;

	struct bar *launcher; /* bar showing the launcher, NULL when closed */
	char query[256];
	size_t sel;
	struct app_list apps;
	bool apps_loaded;

	int ipc_fd;
	char ipc_buf[8192];
	size_t ipc_len;
} P;

static void mark_all_dirty(void) {
	struct bar *b;
	wl_list_for_each(b, &P.bars, link) {
		b->dirty = true;
	}
}

/* ---------- drawing ---------- */

static void set_rgb(cairo_t *cr, uint32_t rgb, double a) {
	cairo_set_source_rgba(cr, ((rgb >> 16) & 0xff) / 255.0, ((rgb >> 8) & 0xff) / 255.0,
		(rgb & 0xff) / 255.0, a);
}

static double text(cairo_t *cr, PangoLayout *l, int bar_h, const char *s, double x, double maxw,
		uint32_t rgb) {
	pango_layout_set_text(l, s, -1);
	pango_layout_set_width(l, maxw > 0 ? (int)(maxw * PANGO_SCALE) : -1);
	int w, h;
	pango_layout_get_pixel_size(l, &w, &h);
	set_rgb(cr, rgb, 1);
	cairo_move_to(cr, x, (bar_h - h) / 2.0);
	pango_cairo_show_layout(cr, l);
	return w;
}

static void buffer_release(void *data, struct wl_buffer *buf) {
	wl_buffer_destroy(buf);
}

static const struct wl_buffer_listener buffer_listener = {.release = buffer_release};

static bool task_on_bar(struct task *t, struct bar *b) {
	if (t->nout == 0) {
		return true;
	}
	for (int i = 0; i < t->nout; i++) {
		if (t->outputs[i] == b->output) {
			return true;
		}
	}
	return false;
}

static bool ws_on_bar(struct workspace *w, struct bar *b) {
	return !w->group || !w->group->output || w->group->output == b->output;
}

static void add_hit(struct bar *b, int kind, int index, double x0, double x1) {
	if (b->nhits < (int)(sizeof(b->hits) / sizeof(b->hits[0]))) {
		b->hits[b->nhits++] = (struct hit){kind, index, x0, x1};
	}
}

static void draw_launcher(struct bar *b, cairo_t *cr, PangoLayout *l) {
	int h = b->height;
	double x = 10;
	x += text(cr, l, h, "Run:", x, 0, 0x8fb4ff) + 8;
	char shown[300];
	snprintf(shown, sizeof(shown), "%s▏", P.query);
	x += text(cr, l, h, shown, x, 300, 0xffffff) + 16;
	size_t idx[MAX_MATCH];
	size_t n = apps_match(&P.apps, P.query, idx, MAX_MATCH);
	if (P.sel >= n) {
		P.sel = 0;
	}
	for (size_t i = 0; i < n && x < b->width - 40; i++) {
		const char *name = P.apps.items[idx[i]].name;
		pango_layout_set_text(l, name, -1);
		pango_layout_set_width(l, -1);
		int w, th;
		pango_layout_get_pixel_size(l, &w, &th);
		if (w > 220) {
			w = 220;
		}
		if (i == P.sel) {
			set_rgb(cr, 0x3d7edd, 1);
			cairo_rectangle(cr, x - 6, 3, w + 12, h - 6);
			cairo_fill(cr);
		}
		text(cr, l, h, name, x, 220, 0xeeeeee);
		x += w + 18;
	}
	if (n == 0 && *P.query) {
		text(cr, l, h, "(Enter runs it as a shell command)", x, 0, 0x888888);
	}
}

static void draw_bar(struct bar *b, cairo_t *cr, PangoLayout *l) {
	int w = b->width, h = b->height;
	b->nhits = 0;
	double x = 6;
	for (int i = 0; i < P.nws; i++) {
		struct workspace *ws = &P.ws[i];
		if (!ws_on_bar(ws, b)) {
			continue;
		}
		const char *name = ws->name ? ws->name : "?";
		pango_layout_set_text(l, name, -1);
		pango_layout_set_width(l, -1);
		int tw, th;
		pango_layout_get_pixel_size(l, &tw, &th);
		double bw = tw + 14;
		if (ws->active) {
			set_rgb(cr, 0x3d7edd, 1);
			cairo_rectangle(cr, x, 3, bw, h - 6);
			cairo_fill(cr);
		}
		text(cr, l, h, name, x + 7, 0, ws->active ? 0xffffff : 0x9aa0aa);
		add_hit(b, 0, i, x, x + bw);
		x += bw + 2;
	}
	x += 8;
	double ax = x;
	x += text(cr, l, h, "▶ Apps", x, 0, 0x8fb4ff);
	add_hit(b, 2, 0, ax, x);
	x += 14;

	char clk[64];
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	strftime(clk, sizeof(clk), "%a %d %b  %H:%M", &tm);
	pango_layout_set_text(l, clk, -1);
	pango_layout_set_width(l, -1);
	int cw, ch;
	pango_layout_get_pixel_size(l, &cw, &ch);
	text(cr, l, h, clk, w - cw - 10, 0, 0xdddddd);

	int n = 0;
	for (int i = 0; i < P.ntasks; i++) {
		n += task_on_bar(&P.tasks[i], b);
	}
	double avail = w - cw - 24 - x;
	double tw = n > 0 ? avail / n : 0;
	if (tw > 220) {
		tw = 220;
	}
	for (int i = 0; i < P.ntasks && tw > 24; i++) {
		struct task *t = &P.tasks[i];
		if (!task_on_bar(t, b)) {
			continue;
		}
		if (t->activated) {
			set_rgb(cr, 0x2b3240, 1);
			cairo_rectangle(cr, x, 3, tw - 4, h - 6);
			cairo_fill(cr);
			set_rgb(cr, 0x3d7edd, 1);
			cairo_rectangle(cr, x, h - 5, tw - 4, 2);
			cairo_fill(cr);
		}
		const char *label = t->title && *t->title ? t->title : (t->app_id ? t->app_id : "?");
		text(cr, l, h, label, x + 6, tw - 14, t->minimized ? 0x6c717c : 0xe6e6e6);
		add_hit(b, 1, i, x, x + tw - 4);
		x += tw;
	}
}

static void draw(struct bar *b) {
	if (!b->configured || b->width <= 0) {
		return;
	}
	int s = b->scale > 0 ? b->scale : 1;
	int w = b->width * s, h = b->height * s, stride = w * 4, size = stride * h;
	int fd = memfd_create("panel", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, size) < 0) {
		if (fd >= 0) {
			close(fd);
		}
		return;
	}
	void *data = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		close(fd);
		return;
	}
	cairo_surface_t *cs = cairo_image_surface_create_for_data(data, CAIRO_FORMAT_ARGB32, w, h, stride);
	cairo_t *cr = cairo_create(cs);
	cairo_scale(cr, s, s);
	set_rgb(cr, 0x16181d, 0.96);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_paint(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	PangoLayout *l = pango_cairo_create_layout(cr);
	PangoFontDescription *font = pango_font_description_from_string(FONT);
	pango_layout_set_font_description(l, font);
	pango_font_description_free(font);
	pango_layout_set_ellipsize(l, PANGO_ELLIPSIZE_END);
	pango_layout_set_single_paragraph_mode(l, TRUE);
	if (P.launcher == b) {
		draw_launcher(b, cr, l);
	} else {
		draw_bar(b, cr, l);
	}
	g_object_unref(l);
	cairo_destroy(cr);
	cairo_surface_destroy(cs);
	munmap(data, (size_t)size);
	struct wl_shm_pool *pool = wl_shm_create_pool(P.shm, fd, size);
	struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
	wl_buffer_add_listener(buf, &buffer_listener, NULL);
	wl_shm_pool_destroy(pool);
	close(fd);
	wl_surface_set_buffer_scale(b->surface, s);
	wl_surface_attach(b->surface, buf, 0, 0);
	wl_surface_damage_buffer(b->surface, 0, 0, w, h);
	wl_surface_commit(b->surface);
	b->dirty = false;
}

/* ---------- launcher ---------- */

static void launcher_open(struct bar *b) {
	if (P.launcher == b) {
		return;
	}
	if (P.launcher) {
		struct bar *old = P.launcher;
		P.launcher = NULL;
		zwlr_layer_surface_v1_set_keyboard_interactivity(old->layer,
			ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
		draw(old);
	}
	P.launcher = b;
	P.query[0] = '\0';
	P.sel = 0;
	if (!b) {
		return;
	}
	if (!P.apps_loaded) {
		apps_load(&P.apps);
		P.apps_loaded = true;
	}
	zwlr_layer_surface_v1_set_keyboard_interactivity(b->layer,
		ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
	draw(b);
}

static void launcher_accept(void) {
	size_t idx[MAX_MATCH];
	size_t n = apps_match(&P.apps, P.query, idx, MAX_MATCH);
	if (n > 0 && *P.query) {
		apps_launch(&P.apps.items[idx[P.sel < n ? P.sel : 0]]);
	} else if (*P.query) {
		launch_command(P.query);
	}
	launcher_open(NULL);
}

static void launcher_key(xkb_keysym_t sym, const char *utf8) {
	size_t len = strlen(P.query);
	switch (sym) {
	case XKB_KEY_Escape:
		launcher_open(NULL);
		return;
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
		launcher_accept();
		return;
	case XKB_KEY_BackSpace:
		while (len > 0) {
			/* drop a whole UTF-8 sequence */
			unsigned char c = (unsigned char)P.query[--len];
			P.query[len] = '\0';
			if ((c & 0xc0) != 0x80) {
				break;
			}
		}
		P.sel = 0;
		break;
	case XKB_KEY_Tab:
	case XKB_KEY_Right:
		P.sel++;
		break;
	case XKB_KEY_ISO_Left_Tab:
	case XKB_KEY_Left:
		if (P.sel > 0) {
			P.sel--;
		}
		break;
	default:
		if (utf8[0] && (unsigned char)utf8[0] >= 0x20 && len + strlen(utf8) < sizeof(P.query) - 1) {
			strcat(P.query, utf8);
			P.sel = 0;
		}
	}
	draw(P.launcher);
}

/* ---------- foreign toplevels ---------- */

static struct task *task_of(struct zwlr_foreign_toplevel_handle_v1 *h) {
	for (int i = 0; i < P.ntasks; i++) {
		if (P.tasks[i].handle == h) {
			return &P.tasks[i];
		}
	}
	return NULL;
}

static void t_title(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, const char *title) {
	struct task *t = task_of(h);
	if (t) {
		free(t->title);
		t->title = strdup(title);
	}
}

static void t_app_id(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, const char *id) {
	struct task *t = task_of(h);
	if (t) {
		free(t->app_id);
		t->app_id = strdup(id);
	}
}

static void t_out_enter(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_output *o) {
	struct task *t = task_of(h);
	if (t && t->nout < MAX_OUTS) {
		t->outputs[t->nout++] = o;
	}
}

static void t_out_leave(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_output *o) {
	struct task *t = task_of(h);
	if (!t) {
		return;
	}
	for (int i = 0; i < t->nout; i++) {
		if (t->outputs[i] == o) {
			t->outputs[i] = t->outputs[--t->nout];
			break;
		}
	}
}

static void t_state(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_array *st) {
	struct task *t = task_of(h);
	if (!t) {
		return;
	}
	t->activated = t->minimized = t->maximized = false;
	uint32_t *s;
	wl_array_for_each(s, st) {
		if (*s == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED) {
			t->activated = true;
		} else if (*s == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MINIMIZED) {
			t->minimized = true;
		} else if (*s == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MAXIMIZED) {
			t->maximized = true;
		}
	}
}

static void t_done(void *d, struct zwlr_foreign_toplevel_handle_v1 *h) {
	mark_all_dirty();
}

static void t_closed(void *d, struct zwlr_foreign_toplevel_handle_v1 *h) {
	struct task *t = task_of(h);
	if (!t) {
		return;
	}
	free(t->title);
	free(t->app_id);
	zwlr_foreign_toplevel_handle_v1_destroy(h);
	int i = (int)(t - P.tasks);
	memmove(&P.tasks[i], &P.tasks[i + 1], (size_t)(P.ntasks - i - 1) * sizeof(*t));
	P.ntasks--;
	mark_all_dirty();
}

static void t_parent(void *d, struct zwlr_foreign_toplevel_handle_v1 *h,
		struct zwlr_foreign_toplevel_handle_v1 *parent) {
}

static const struct zwlr_foreign_toplevel_handle_v1_listener task_listener = {
	.title = t_title, .app_id = t_app_id, .output_enter = t_out_enter,
	.output_leave = t_out_leave, .state = t_state, .done = t_done, .closed = t_closed,
	.parent = t_parent,
};

static void ftl_toplevel(void *d, struct zwlr_foreign_toplevel_manager_v1 *m,
		struct zwlr_foreign_toplevel_handle_v1 *h) {
	if (P.ntasks >= MAX_TASKS) {
		zwlr_foreign_toplevel_handle_v1_destroy(h);
		return;
	}
	P.tasks[P.ntasks++] = (struct task){.handle = h};
	zwlr_foreign_toplevel_handle_v1_add_listener(h, &task_listener, NULL);
}

static void ftl_finished(void *d, struct zwlr_foreign_toplevel_manager_v1 *m) {
}

static const struct zwlr_foreign_toplevel_manager_v1_listener ftl_listener = {
	.toplevel = ftl_toplevel, .finished = ftl_finished,
};

/* ---------- workspaces ---------- */

static struct workspace *ws_of(struct ext_workspace_handle_v1 *h) {
	for (int i = 0; i < P.nws; i++) {
		if (P.ws[i].handle == h) {
			return &P.ws[i];
		}
	}
	return NULL;
}

static struct ws_group *group_of(struct ext_workspace_group_handle_v1 *g) {
	for (int i = 0; i < P.ngroups; i++) {
		if (P.groups[i].handle == g) {
			return &P.groups[i];
		}
	}
	return NULL;
}

static void ws_id(void *d, struct ext_workspace_handle_v1 *h, const char *id) {
}

static void ws_name(void *d, struct ext_workspace_handle_v1 *h, const char *name) {
	struct workspace *w = ws_of(h);
	if (w) {
		free(w->name);
		w->name = strdup(name);
	}
}

static void ws_coords(void *d, struct ext_workspace_handle_v1 *h, struct wl_array *c) {
	struct workspace *w = ws_of(h);
	if (w && c->size >= sizeof(uint32_t)) {
		w->coord = *(uint32_t *)c->data;
	}
}

static void ws_state(void *d, struct ext_workspace_handle_v1 *h, uint32_t state) {
	struct workspace *w = ws_of(h);
	if (w) {
		w->active = state & EXT_WORKSPACE_HANDLE_V1_STATE_ACTIVE;
	}
}

static void ws_caps(void *d, struct ext_workspace_handle_v1 *h, uint32_t caps) {
}

static void ws_removed(void *d, struct ext_workspace_handle_v1 *h) {
	struct workspace *w = ws_of(h);
	if (!w) {
		return;
	}
	free(w->name);
	ext_workspace_handle_v1_destroy(h);
	int i = (int)(w - P.ws);
	memmove(&P.ws[i], &P.ws[i + 1], (size_t)(P.nws - i - 1) * sizeof(*w));
	P.nws--;
	mark_all_dirty();
}

static const struct ext_workspace_handle_v1_listener ws_listener = {
	.id = ws_id, .name = ws_name, .coordinates = ws_coords, .state = ws_state,
	.capabilities = ws_caps, .removed = ws_removed,
};

static void wsg_caps(void *d, struct ext_workspace_group_handle_v1 *g, uint32_t c) {
}

static void wsg_out_enter(void *d, struct ext_workspace_group_handle_v1 *g, struct wl_output *o) {
	struct ws_group *grp = group_of(g);
	if (grp) {
		grp->output = o;
	}
}

static void wsg_out_leave(void *d, struct ext_workspace_group_handle_v1 *g, struct wl_output *o) {
	struct ws_group *grp = group_of(g);
	if (grp && grp->output == o) {
		grp->output = NULL;
	}
}

static void wsg_ws_enter(void *d, struct ext_workspace_group_handle_v1 *g,
		struct ext_workspace_handle_v1 *h) {
	struct workspace *w = ws_of(h);
	if (w) {
		w->group = group_of(g);
	}
}

static void wsg_ws_leave(void *d, struct ext_workspace_group_handle_v1 *g,
		struct ext_workspace_handle_v1 *h) {
	struct workspace *w = ws_of(h);
	if (w && w->group == group_of(g)) {
		w->group = NULL;
	}
}

static void wsg_removed(void *d, struct ext_workspace_group_handle_v1 *g) {
	struct ws_group *grp = group_of(g);
	if (grp) {
		for (int i = 0; i < P.nws; i++) {
			if (P.ws[i].group == grp) {
				P.ws[i].group = NULL;
			}
		}
		/* the slot is reused by the next new group */
		grp->handle = NULL;
		grp->output = NULL;
	}
	ext_workspace_group_handle_v1_destroy(g);
}

static const struct ext_workspace_group_handle_v1_listener wsg_listener = {
	.capabilities = wsg_caps, .output_enter = wsg_out_enter, .output_leave = wsg_out_leave,
	.workspace_enter = wsg_ws_enter, .workspace_leave = wsg_ws_leave, .removed = wsg_removed,
};

static void wsm_group(void *d, struct ext_workspace_manager_v1 *m,
		struct ext_workspace_group_handle_v1 *g) {
	struct ws_group *slot = NULL;
	for (int i = 0; i < P.ngroups && !slot; i++) {
		if (!P.groups[i].handle) {
			slot = &P.groups[i];
		}
	}
	if (!slot && P.ngroups < MAX_GROUPS) {
		slot = &P.groups[P.ngroups++];
	}
	if (!slot) {
		ext_workspace_group_handle_v1_destroy(g);
		return;
	}
	*slot = (struct ws_group){.handle = g};
	ext_workspace_group_handle_v1_add_listener(g, &wsg_listener, NULL);
}

static void wsm_workspace(void *d, struct ext_workspace_manager_v1 *m,
		struct ext_workspace_handle_v1 *h) {
	if (P.nws >= MAX_WS) {
		ext_workspace_handle_v1_destroy(h);
		return;
	}
	P.ws[P.nws++] = (struct workspace){.handle = h};
	ext_workspace_handle_v1_add_listener(h, &ws_listener, NULL);
}

static int by_coord(const void *a, const void *b) {
	const struct workspace *x = a, *y = b;
	return x->coord < y->coord ? -1 : x->coord > y->coord;
}

static void wsm_done(void *d, struct ext_workspace_manager_v1 *m) {
	/* stable order: workspaces keep their group pointers, only array order changes */
	qsort(P.ws, (size_t)P.nws, sizeof(P.ws[0]), by_coord);
	mark_all_dirty();
}

static void wsm_finished(void *d, struct ext_workspace_manager_v1 *m) {
}

static const struct ext_workspace_manager_v1_listener wsm_listener = {
	.workspace_group = wsm_group, .workspace = wsm_workspace, .done = wsm_done,
	.finished = wsm_finished,
};

/* ---------- input ---------- */

static void click(struct bar *b, uint32_t button) {
	if (P.launcher) {
		launcher_open(NULL);
		return;
	}
	double x = P.ptr_x;
	for (int i = 0; i < b->nhits; i++) {
		struct hit *h = &b->hits[i];
		if (x < h->x0 || x >= h->x1) {
			continue;
		}
		if (h->kind == 0 && h->index < P.nws) {
			ext_workspace_handle_v1_activate(P.ws[h->index].handle);
			ext_workspace_manager_v1_commit(P.ws_mgr);
		} else if (h->kind == 2) {
			launcher_open(b);
		} else if (h->kind == 1 && h->index < P.ntasks) {
			struct task *t = &P.tasks[h->index];
			if (button == 0x112) { /* BTN_MIDDLE */
				zwlr_foreign_toplevel_handle_v1_close(t->handle);
			} else if (t->activated && !t->minimized) {
				zwlr_foreign_toplevel_handle_v1_set_minimized(t->handle);
			} else {
				if (t->minimized) {
					zwlr_foreign_toplevel_handle_v1_unset_minimized(t->handle);
				}
				zwlr_foreign_toplevel_handle_v1_activate(t->handle, P.seat);
			}
		}
		return;
	}
}

static struct bar *bar_of_surface(struct wl_surface *s) {
	struct bar *b;
	wl_list_for_each(b, &P.bars, link) {
		if (b->surface == s) {
			return b;
		}
	}
	return NULL;
}

static void p_enter(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s,
		wl_fixed_t x, wl_fixed_t y) {
	P.ptr_bar = bar_of_surface(s);
	P.ptr_x = wl_fixed_to_double(x);
}

static void p_leave(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s) {
	P.ptr_bar = NULL;
}

static void p_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y) {
	P.ptr_x = wl_fixed_to_double(x);
}

static void p_button(void *d, struct wl_pointer *p, uint32_t serial, uint32_t t, uint32_t button,
		uint32_t state) {
	if (state == WL_POINTER_BUTTON_STATE_PRESSED && P.ptr_bar) {
		click(P.ptr_bar, button);
	}
}

static void p_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t axis, wl_fixed_t v) {
	/* scroll over a bar switches that monitor's workspace, like X11 pagers */
	struct bar *b = P.ptr_bar;
	if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL || !b || P.launcher) {
		return;
	}
	int idx[MAX_WS], n = 0, cur = -1;
	for (int i = 0; i < P.nws; i++) {
		if (ws_on_bar(&P.ws[i], b)) {
			if (P.ws[i].active) {
				cur = n;
			}
			idx[n++] = i;
		}
	}
	int next = cur + (wl_fixed_to_double(v) > 0 ? 1 : -1);
	if (cur >= 0 && next >= 0 && next < n) {
		ext_workspace_handle_v1_activate(P.ws[idx[next]].handle);
		ext_workspace_manager_v1_commit(P.ws_mgr);
	}
}

static void p_frame(void *d, struct wl_pointer *p) {
}

static void p_axis_source(void *d, struct wl_pointer *p, uint32_t s) {
}

static void p_axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a) {
}

static void p_axis_discrete(void *d, struct wl_pointer *p, uint32_t a, int32_t v) {
}

static void p_axis_value120(void *d, struct wl_pointer *p, uint32_t a, int32_t v) {
}

static void p_axis_rel(void *d, struct wl_pointer *p, uint32_t a, uint32_t dir) {
}

static const struct wl_pointer_listener pointer_listener = {
	.enter = p_enter, .leave = p_leave, .motion = p_motion, .button = p_button,
	.axis = p_axis, .frame = p_frame, .axis_source = p_axis_source, .axis_stop = p_axis_stop,
	.axis_discrete = p_axis_discrete, .axis_value120 = p_axis_value120,
	.axis_relative_direction = p_axis_rel,
};

static void k_keymap(void *d, struct wl_keyboard *k, uint32_t fmt, int32_t fd, uint32_t size) {
	char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (map == MAP_FAILED) {
		return;
	}
	struct xkb_keymap *km = xkb_keymap_new_from_string(P.xkb, map, XKB_KEYMAP_FORMAT_TEXT_V1,
		XKB_KEYMAP_COMPILE_NO_FLAGS);
	munmap(map, size);
	if (!km) {
		return;
	}
	xkb_state_unref(P.xkb_state);
	xkb_keymap_unref(P.keymap);
	P.keymap = km;
	P.xkb_state = xkb_state_new(km);
}

static void k_enter(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s,
		struct wl_array *keys) {
}

static void k_leave(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s) {
}

static void k_key(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t t, uint32_t key,
		uint32_t state) {
	if (state != WL_KEYBOARD_KEY_STATE_PRESSED || !P.xkb_state || !P.launcher) {
		return;
	}
	xkb_keysym_t sym = xkb_state_key_get_one_sym(P.xkb_state, key + 8);
	char utf8[16] = "";
	xkb_state_key_get_utf8(P.xkb_state, key + 8, utf8, sizeof(utf8));
	launcher_key(sym, utf8);
}

static void k_mods(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t dep, uint32_t lat,
		uint32_t lock, uint32_t group) {
	if (P.xkb_state) {
		xkb_state_update_mask(P.xkb_state, dep, lat, lock, 0, 0, group);
	}
}

static void k_repeat(void *d, struct wl_keyboard *k, int32_t rate, int32_t delay) {
}

static const struct wl_keyboard_listener keyboard_listener = {
	.keymap = k_keymap, .enter = k_enter, .leave = k_leave, .key = k_key,
	.modifiers = k_mods, .repeat_info = k_repeat,
};

static void seat_caps(void *d, struct wl_seat *s, uint32_t caps) {
	if ((caps & WL_SEAT_CAPABILITY_POINTER) && !P.pointer) {
		P.pointer = wl_seat_get_pointer(s);
		wl_pointer_add_listener(P.pointer, &pointer_listener, NULL);
	}
	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !P.keyboard) {
		P.keyboard = wl_seat_get_keyboard(s);
		wl_keyboard_add_listener(P.keyboard, &keyboard_listener, NULL);
	}
}

static void seat_name(void *d, struct wl_seat *s, const char *n) {
}

static const struct wl_seat_listener seat_listener = {.capabilities = seat_caps, .name = seat_name};

/* ---------- bars (one per output) ---------- */

static void layer_configure(void *d, struct zwlr_layer_surface_v1 *l, uint32_t serial, uint32_t w,
		uint32_t h) {
	struct bar *b = d;
	zwlr_layer_surface_v1_ack_configure(l, serial);
	b->width = (int)w;
	b->height = h ? (int)h : BAR_H;
	b->configured = true;
	draw(b);
}

static void bar_destroy(struct bar *b);

static void layer_closed(void *d, struct zwlr_layer_surface_v1 *l) {
	bar_destroy(d);
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
	.configure = layer_configure, .closed = layer_closed,
};

static void bar_create_surface(struct bar *b) {
	if (b->surface || !P.compositor || !P.layer_shell) {
		return;
	}
	b->surface = wl_compositor_create_surface(P.compositor);
	b->layer = zwlr_layer_shell_v1_get_layer_surface(P.layer_shell, b->surface, b->output,
		ZWLR_LAYER_SHELL_V1_LAYER_TOP, "waylandx-panel");
	zwlr_layer_surface_v1_add_listener(b->layer, &layer_listener, b);
	zwlr_layer_surface_v1_set_anchor(b->layer, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
	zwlr_layer_surface_v1_set_size(b->layer, 0, BAR_H);
	zwlr_layer_surface_v1_set_exclusive_zone(b->layer, BAR_H);
	wl_surface_commit(b->surface);
}

static void bar_destroy(struct bar *b) {
	if (P.launcher == b) {
		P.launcher = NULL;
	}
	if (P.ptr_bar == b) {
		P.ptr_bar = NULL;
	}
	if (b->layer) {
		zwlr_layer_surface_v1_destroy(b->layer);
	}
	if (b->surface) {
		wl_surface_destroy(b->surface);
	}
	if (b->output) {
		if (wl_output_get_version(b->output) >= WL_OUTPUT_RELEASE_SINCE_VERSION) {
			wl_output_release(b->output);
		} else {
			wl_output_destroy(b->output);
		}
	}
	wl_list_remove(&b->link);
	free(b->output_name);
	free(b);
}

static void out_geometry(void *d, struct wl_output *o, int32_t x, int32_t y, int32_t pw, int32_t ph,
		int32_t sub, const char *make, const char *model, int32_t transform) {
}

static void out_mode(void *d, struct wl_output *o, uint32_t flags, int32_t w, int32_t h, int32_t r) {
}

static void out_done(void *d, struct wl_output *o) {
	struct bar *b = d;
	b->dirty = true;
}

static void out_scale(void *d, struct wl_output *o, int32_t factor) {
	struct bar *b = d;
	b->scale = factor;
}

static void out_name(void *d, struct wl_output *o, const char *name) {
	struct bar *b = d;
	free(b->output_name);
	b->output_name = strdup(name);
}

static void out_description(void *d, struct wl_output *o, const char *desc) {
}

static const struct wl_output_listener output_listener = {
	.geometry = out_geometry, .mode = out_mode, .done = out_done, .scale = out_scale,
	.name = out_name, .description = out_description,
};

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t v) {
	if (!strcmp(iface, wl_compositor_interface.name)) {
		P.compositor = wl_registry_bind(r, name, &wl_compositor_interface, 4);
	} else if (!strcmp(iface, wl_shm_interface.name)) {
		P.shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
	} else if (!strcmp(iface, wl_seat_interface.name) && !P.seat) {
		P.seat = wl_registry_bind(r, name, &wl_seat_interface, v < 7 ? v : 7);
		wl_seat_add_listener(P.seat, &seat_listener, NULL);
	} else if (!strcmp(iface, wl_output_interface.name)) {
		struct bar *b = calloc(1, sizeof(*b));
		b->global = name;
		b->scale = 1;
		b->output = wl_registry_bind(r, name, &wl_output_interface, v < 4 ? v : 4);
		wl_output_add_listener(b->output, &output_listener, b);
		wl_list_insert(P.bars.prev, &b->link);
	} else if (!strcmp(iface, zwlr_layer_shell_v1_interface.name)) {
		P.layer_shell = wl_registry_bind(r, name, &zwlr_layer_shell_v1_interface, v < 4 ? v : 4);
	} else if (!strcmp(iface, zwlr_foreign_toplevel_manager_v1_interface.name)) {
		P.ftl = wl_registry_bind(r, name, &zwlr_foreign_toplevel_manager_v1_interface, v < 3 ? v : 3);
		zwlr_foreign_toplevel_manager_v1_add_listener(P.ftl, &ftl_listener, NULL);
	} else if (!strcmp(iface, ext_workspace_manager_v1_interface.name)) {
		P.ws_mgr = wl_registry_bind(r, name, &ext_workspace_manager_v1_interface, 1);
		ext_workspace_manager_v1_add_listener(P.ws_mgr, &wsm_listener, NULL);
	}
}

static void reg_remove(void *d, struct wl_registry *r, uint32_t name) {
	struct bar *b, *tmp;
	wl_list_for_each_safe(b, tmp, &P.bars, link) {
		if (b->global == name) {
			bar_destroy(b);
		}
	}
}

static const struct wl_registry_listener reg_listener = {.global = reg_global,
	.global_remove = reg_remove};

/* ---------- WaylandX IPC: the "launcher" event (Mod+D) ---------- */

static int ipc_connect(void) {
	const char *path = getenv("WAYLANDX_SOCK");
	if (!path || !*path) {
		return -1;
	}
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	struct sockaddr_un addr = {.sun_family = AF_UNIX};
	if (fd < 0 || strlen(path) >= sizeof(addr.sun_path)) {
		if (fd >= 0) {
			close(fd);
		}
		return -1;
	}
	strcpy(addr.sun_path, path);
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
			write(fd, "subscribe\n", 10) != 10) {
		close(fd);
		return -1;
	}
	return fd;
}

static struct bar *bar_for_output_name(const char *name) {
	struct bar *b;
	wl_list_for_each(b, &P.bars, link) {
		if (b->output_name && name && !strcmp(b->output_name, name)) {
			return b;
		}
	}
	return wl_list_empty(&P.bars) ? NULL : wl_container_of(P.bars.next, b, link);
}

static void ipc_line(const char *line) {
	if (!strstr(line, "\"event\":\"launcher\"")) {
		return;
	}
	if (P.launcher) {
		launcher_open(NULL);
		return;
	}
	char name[128] = "";
	const char *o = strstr(line, "\"output\":\"");
	if (o) {
		o += 10;
		size_t n = strcspn(o, "\"");
		if (n < sizeof(name)) {
			memcpy(name, o, n);
			name[n] = '\0';
		}
	}
	launcher_open(bar_for_output_name(name));
}

static void ipc_read(void) {
	ssize_t n = read(P.ipc_fd, P.ipc_buf + P.ipc_len, sizeof(P.ipc_buf) - 1 - P.ipc_len);
	if (n <= 0) {
		close(P.ipc_fd);
		P.ipc_fd = -1;
		return;
	}
	P.ipc_len += (size_t)n;
	P.ipc_buf[P.ipc_len] = '\0';
	char *nl;
	while ((nl = strchr(P.ipc_buf, '\n'))) {
		*nl = '\0';
		ipc_line(P.ipc_buf);
		size_t used = (size_t)(nl - P.ipc_buf) + 1;
		memmove(P.ipc_buf, nl + 1, P.ipc_len - used + 1);
		P.ipc_len -= used;
	}
	if (P.ipc_len >= sizeof(P.ipc_buf) - 1) {
		P.ipc_len = 0; /* drop an oversized line */
	}
}

int main(int argc, char *argv[]) {
	signal(SIGCHLD, SIG_IGN);
	setvbuf(stdout, NULL, _IOLBF, 0);
	wl_list_init(&P.bars);
	P.running = true;
	P.ipc_fd = -1;
	P.xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	P.display = wl_display_connect(NULL);
	if (!P.display) {
		fprintf(stderr, "waylandx-panel: cannot connect to Wayland display\n");
		return 1;
	}
	struct wl_registry *reg = wl_display_get_registry(P.display);
	wl_registry_add_listener(reg, &reg_listener, NULL);
	wl_display_roundtrip(P.display);
	if (!P.compositor || !P.shm || !P.layer_shell) {
		fprintf(stderr, "waylandx-panel: compositor lacks wl_shm or layer-shell\n");
		return 1;
	}
	struct bar *b;
	wl_list_for_each(b, &P.bars, link) {
		bar_create_surface(b);
	}
	wl_display_roundtrip(P.display);
	P.ipc_fd = ipc_connect();
	if (argc > 1 && !strcmp(argv[1], "--launcher") && !wl_list_empty(&P.bars)) {
		launcher_open(wl_container_of(P.bars.next, b, link));
	}

	int last_min = -1;
	while (P.running) {
		/* outputs plugged in after start get their bar here */
		wl_list_for_each(b, &P.bars, link) {
			bar_create_surface(b);
			if (b->dirty) {
				draw(b);
			}
		}
		while (wl_display_prepare_read(P.display) != 0) {
			wl_display_dispatch_pending(P.display);
		}
		wl_display_flush(P.display);
		struct pollfd fds[2] = {
			{.fd = wl_display_get_fd(P.display), .events = POLLIN},
			{.fd = P.ipc_fd, .events = POLLIN},
		};
		int r = poll(fds, P.ipc_fd >= 0 ? 2 : 1, 1000);
		if (r > 0 && (fds[0].revents & POLLIN)) {
			wl_display_read_events(P.display);
		} else {
			wl_display_cancel_read(P.display);
		}
		if (wl_display_dispatch_pending(P.display) < 0) {
			break;
		}
		if (P.ipc_fd >= 0 && r > 0 && (fds[1].revents & (POLLIN | POLLHUP))) {
			ipc_read();
		}
		time_t now = time(NULL);
		struct tm tm;
		localtime_r(&now, &tm);
		if (tm.tm_min != last_min) {
			last_min = tm.tm_min;
			mark_all_dirty();
		}
		if (fds[0].revents & (POLLERR | POLLHUP)) {
			break;
		}
	}
	return 0;
}
