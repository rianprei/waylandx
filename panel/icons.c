#define _GNU_SOURCE
#include <cairo.h>
#include <ctype.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef WX_HAVE_RSVG
#include <librsvg/rsvg.h>
#endif
#include "icons.h"

/*
 * Freedesktop icon themes without GTK: the user's theme (env, KDE or GTK
 * settings), what it inherits, hicolor and /usr/share/pixmaps. Lookups follow
 * the spec: each theme's index.theme lists its directories with sizes, and
 * a name is only searched when it is first asked for. Results are cached.
 */

struct theme_dir {
	char *path; /* absolute */
	int size;
	bool scalable;
};

struct theme {
	char *name;
	struct theme_dir *dirs;
	int ndirs;
};

static struct theme themes[16];
static int nthemes;
static bool ready;
static GHashTable *cache; /* "name@px" -> cairo_surface_t (or a NULL marker) */

static char *ini_value(const char *path, const char *section, const char *key) {
	FILE *f = fopen(path, "r");
	if (!f) {
		return NULL;
	}
	char *line = NULL, *val = NULL;
	size_t cap = 0;
	bool in = section == NULL;
	size_t kl = strlen(key);
	while (getline(&line, &cap, f) > 0) { /* hicolor's Directories= is over 10 kB */
		line[strcspn(line, "\r\n")] = '\0';
		if (line[0] == '[') {
			in = section && !strcmp(line, section);
			continue;
		}
		char *s = line;
		while (isspace((unsigned char)*s)) {
			s++;
		}
		if (in && !strncmp(s, key, kl)) {
			char *eq = s + kl;
			while (*eq == ' ') {
				eq++;
			}
			if (*eq == '=') {
				eq++;
				while (*eq == ' ') {
					eq++;
				}
				val = g_strdup(eq);
				break;
			}
		}
	}
	free(line);
	fclose(f);
	return val;
}

static GPtrArray *base_dirs(void) {
	GPtrArray *a = g_ptr_array_new_with_free_func(g_free);
	g_ptr_array_add(a, g_build_filename(g_get_home_dir(), ".icons", NULL));
	g_ptr_array_add(a, g_build_filename(g_get_user_data_dir(), "icons", NULL));
	const char *const *sys = g_get_system_data_dirs();
	for (int i = 0; sys[i]; i++) {
		g_ptr_array_add(a, g_build_filename(sys[i], "icons", NULL));
	}
	return a;
}

/* Reads the theme's directories (with sizes) from its index.theme; each one
 * is searched in every base dir that has the theme (hicolor lives in
 * /usr/share/icons, ~/.local/share/icons, flatpak exports...). */
static void load_dirs(struct theme *t, GPtrArray *bases, const char *index_path) {
	char *list = ini_value(index_path, "[Icon Theme]", "Directories");
	char *scaled = ini_value(index_path, "[Icon Theme]", "ScaledDirectories");
	char *all = g_strconcat(list ? list : "", ",", scaled ? scaled : "", NULL);
	g_free(list);
	g_free(scaled);
	char **names = g_strsplit(all, ",", -1);
	g_free(all);
	int n = (int)g_strv_length(names);
	/* size and type per directory name */
	GHashTable *size = g_hash_table_new(g_str_hash, g_str_equal);
	GHashTable *scal = g_hash_table_new(g_str_hash, g_str_equal);
	FILE *f = fopen(index_path, "r");
	char *line = NULL, *cur = NULL;
	size_t cap = 0;
	while (f && getline(&line, &cap, f) > 0) {
		line[strcspn(line, "\r\n")] = '\0';
		if (line[0] == '[') {
			g_free(cur);
			cur = g_strndup(line + 1, strcspn(line + 1, "]"));
		} else if (cur && !strncmp(line, "Size=", 5)) {
			g_hash_table_replace(size, g_strdup(cur), GINT_TO_POINTER(atoi(line + 5)));
		} else if (cur && !strcmp(line, "Type=Scalable")) {
			g_hash_table_replace(scal, g_strdup(cur), GINT_TO_POINTER(1));
		}
	}
	g_free(cur);
	free(line);
	if (f) {
		fclose(f);
	}
	GPtrArray *roots = g_ptr_array_new_with_free_func(g_free);
	for (guint b = 0; b < bases->len; b++) {
		char *root = g_build_filename(bases->pdata[b], t->name, NULL);
		if (g_file_test(root, G_FILE_TEST_IS_DIR)) {
			g_ptr_array_add(roots, root);
		} else {
			g_free(root);
		}
	}
	t->dirs = g_new0(struct theme_dir, (n > 0 ? n : 1) * (roots->len > 0 ? roots->len : 1));
	for (int i = 0; i < n; i++) {
		char *d = g_strstrip(names[i]);
		if (!*d) {
			continue;
		}
		for (guint r = 0; r < roots->len; r++) {
			char *path = g_build_filename(roots->pdata[r], d, NULL);
			if (!g_file_test(path, G_FILE_TEST_IS_DIR)) {
				g_free(path); /* skip what is not installed: fewer lookups later */
				continue;
			}
			struct theme_dir *td = &t->dirs[t->ndirs++];
			td->path = path;
			gpointer sz = g_hash_table_lookup(size, d);
			td->size = sz ? GPOINTER_TO_INT(sz) : 32;
			td->scalable = g_hash_table_contains(scal, d);
		}
	}
	g_ptr_array_free(roots, TRUE);
	g_hash_table_destroy(size);
	g_hash_table_destroy(scal);
	g_strfreev(names);
}

/* Adds a theme and, recursively, the themes it inherits. */
static void add_theme(GPtrArray *bases, const char *name, int depth) {
	if (!name || !*name || depth > 5 || nthemes >= 16) {
		return;
	}
	for (int i = 0; i < nthemes; i++) {
		if (!strcmp(themes[i].name, name)) {
			return;
		}
	}
	for (guint i = 0; i < bases->len; i++) {
		char *idx = g_build_filename(bases->pdata[i], name, "index.theme", NULL);
		if (access(idx, R_OK) == 0) {
			struct theme *t = &themes[nthemes++];
			t->name = g_strdup(name);
			load_dirs(t, bases, idx);
			char *inh = ini_value(idx, "[Icon Theme]", "Inherits");
			g_free(idx);
			if (inh) {
				char **parts = g_strsplit(inh, ",", -1);
				for (int j = 0; parts[j]; j++) {
					add_theme(bases, g_strstrip(parts[j]), depth + 1);
				}
				g_strfreev(parts);
				g_free(inh);
			}
			return;
		}
		g_free(idx);
	}
}

static void cache_free(gpointer p) {
	if (p != GINT_TO_POINTER(1)) {
		cairo_surface_destroy(p);
	}
}

void icons_init(void) {
	if (ready) {
		return;
	}
	ready = true;
	cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, cache_free);
	GPtrArray *bases = base_dirs();
	char *t = g_strdup(getenv("WAYLANDX_ICON_THEME"));
	if (!t) {
		char *p = g_build_filename(g_get_user_config_dir(), "kdeglobals", NULL);
		t = ini_value(p, "[Icons]", "Theme");
		g_free(p);
	}
	if (!t) {
		char *p = g_build_filename(g_get_user_config_dir(), "gtk-3.0", "settings.ini", NULL);
		t = ini_value(p, "[Settings]", "gtk-icon-theme-name");
		g_free(p);
	}
	/* the bar is dark: breeze-dark over breeze when the user runs breeze */
	if (!t || !strcmp(t, "breeze")) {
		add_theme(bases, "breeze-dark", 0);
	}
	add_theme(bases, t, 0);
	add_theme(bases, "breeze", 0);
	add_theme(bases, "Adwaita", 0);
	add_theme(bases, "hicolor", 0);
	g_free(t);
	g_ptr_array_free(bases, TRUE);
}

/* Best file for name at px: the first theme that has it wins; inside it,
 * scalable, then the smallest size >= px, then the largest. */
static char *find_icon(const char *name, int px) {
	static const char *ext[] = {".svg", ".png"};
	for (int i = 0; i < nthemes; i++) {
		struct theme *t = &themes[i];
		char *best = NULL;
		int best_score = -1;
		for (int d = 0; d < t->ndirs; d++) {
			struct theme_dir *td = &t->dirs[d];
			for (int e = 0; e < 2; e++) {
#ifndef WX_HAVE_RSVG
				if (e == 0) {
					continue;
				}
#endif
				char *p = g_strconcat(td->path, "/", name, ext[e], NULL);
				if (access(p, R_OK) != 0) {
					g_free(p);
					continue;
				}
				/* higher is better: SVG, then the closest bitmap at or above px */
				int score = e == 0 ? 100000 :
					td->size >= px ? 50000 - (td->size - px) : 40000 - (px - td->size);
				if (score > best_score) {
					g_free(best);
					best = p;
					best_score = score;
				} else {
					g_free(p);
				}
			}
		}
		if (best) {
			return best;
		}
	}
	static const char *pix[] = {".png", ".svg", NULL};
	for (int e = 0; pix[e]; e++) {
		char *p = g_strconcat("/usr/share/pixmaps/", name, pix[e], NULL);
		if (access(p, R_OK) == 0) {
			return p;
		}
		g_free(p);
	}
	return NULL;
}

static cairo_surface_t *scale_to(cairo_surface_t *src, int px) {
	int w = cairo_image_surface_get_width(src), h = cairo_image_surface_get_height(src);
	if (w <= 0 || h <= 0) {
		return NULL;
	}
	cairo_surface_t *dst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, px, px);
	cairo_t *cr = cairo_create(dst);
	double s = (double)px / (w > h ? w : h);
	cairo_translate(cr, (px - w * s) / 2, (px - h * s) / 2);
	cairo_scale(cr, s, s);
	cairo_set_source_surface(cr, src, 0, 0);
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
	cairo_paint(cr);
	cairo_destroy(cr);
	return dst;
}

static cairo_surface_t *load_file(const char *path, int px) {
	const char *dot = strrchr(path, '.');
	if (dot && !strcmp(dot, ".png")) {
		cairo_surface_t *png = cairo_image_surface_create_from_png(path);
		if (cairo_surface_status(png) != CAIRO_STATUS_SUCCESS) {
			cairo_surface_destroy(png);
			return NULL;
		}
		cairo_surface_t *out = scale_to(png, px);
		cairo_surface_destroy(png);
		return out;
	}
#ifdef WX_HAVE_RSVG
	if (dot && !strcmp(dot, ".svg")) {
		GError *err = NULL;
		RsvgHandle *h = rsvg_handle_new_from_file(path, &err);
		if (!h) {
			g_clear_error(&err);
			return NULL;
		}
		cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, px, px);
		cairo_t *cr = cairo_create(out);
		RsvgRectangle vp = {0, 0, px, px};
		if (!rsvg_handle_render_document(h, cr, &vp, &err)) {
			g_clear_error(&err);
		}
		cairo_destroy(cr);
		g_object_unref(h);
		return out;
	}
#endif
	return NULL;
}

cairo_surface_t *icon_get(const char *name, int px) {
	if (!name || !*name || px <= 0) {
		return NULL;
	}
	icons_init();
	char *key = g_strdup_printf("%s@%d", name, px);
	gpointer hit;
	if (g_hash_table_lookup_extended(cache, key, NULL, &hit)) {
		g_free(key);
		return hit == GINT_TO_POINTER(1) ? NULL : hit;
	}
	char *found = NULL;
	const char *path = NULL;
	if (!strncmp(name, "file://", 7)) {
		path = name + 7;
	} else if (name[0] == '/') {
		path = name;
	} else {
		found = find_icon(name, px);
		/* "foo.png" given as a name */
		if (!found && strchr(name, '.')) {
			char *bare = g_strndup(name, (gsize)(strrchr(name, '.') - name));
			found = find_icon(bare, px);
			g_free(bare);
		}
		path = found;
	}
	cairo_surface_t *s = path ? load_file(path, px) : NULL;
	g_free(found);
	g_hash_table_insert(cache, key, s ? (gpointer)s : GINT_TO_POINTER(1));
	return s;
}

/* Raw RGBA rows, as in the notification "image-data" hint. */
cairo_surface_t *icon_from_rgba(const unsigned char *data, int w, int h, int stride, bool alpha,
		int px) {
	if (w <= 0 || h <= 0 || w > 4096 || h > 4096 || stride < w * (alpha ? 4 : 3)) {
		return NULL;
	}
	cairo_surface_t *src = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
	unsigned char *dst = cairo_image_surface_get_data(src);
	int ds = cairo_image_surface_get_stride(src);
	for (int y = 0; y < h; y++) {
		const unsigned char *row = data + (size_t)y * stride;
		uint32_t *out = (uint32_t *)(dst + (size_t)y * ds);
		for (int x = 0; x < w; x++) {
			const unsigned char *p = row + x * (alpha ? 4 : 3);
			uint32_t a = alpha ? p[3] : 255;
			uint32_t r = p[0] * a / 255, g = p[1] * a / 255, b = p[2] * a / 255;
			out[x] = (a << 24) | (r << 16) | (g << 8) | b;
		}
	}
	cairo_surface_mark_dirty(src);
	cairo_surface_t *out = scale_to(src, px);
	cairo_surface_destroy(src);
	return out;
}
