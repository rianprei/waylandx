#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "config.h"

#define ARRAY_PUSH(arr, len, item) do { \
	void *_n = realloc((arr), ((len) + 1) * sizeof(*(arr))); \
	if (!_n) abort(); \
	(arr) = _n; (arr)[(len)++] = (item); \
} while (0)

static char *xstrdup(const char *s) {
	if (!s) {
		return NULL;
	}
	char *d = strdup(s);
	if (!d) {
		abort();
	}
	return d;
}

static void set_str(char **dst, const char *v) {
	free(*dst);
	*dst = xstrdup(v);
}

void config_defaults(struct wx_config *c) {
	memset(c, 0, sizeof(*c));
	c->mod = xstrdup("auto");
	c->terminal = xstrdup("alacritty");
	c->layout = xstrdup("floating");
	c->gap = 6;
	c->border = 2;
	c->title_height = 24;
	c->font = xstrdup("Sans 10");
	c->color_active = 0x3d7eddff;
	c->color_inactive = 0x3a3d45ff;
	c->color_text = 0xffffffff;
	c->color_text_inactive = 0xb8bcc6ff;
	c->color_background = 0x1e2128ff;
	c->focus_follows_mouse = false;
	c->xwayland = true;
	c->snap = true;
	c->snap_threshold = 12;
	c->cursor_size = 24;
	c->repeat_rate = 25;
	c->repeat_delay = 600;
	c->natural_scroll = -1;
	c->tap = -1;
	c->left_handed = -1;
	c->accel = -2;
}

void config_free(struct wx_config *c) {
	free(c->mod);
	free(c->terminal);
	free(c->layout);
	free(c->font);
	free(c->cursor_theme);
	free(c->screenshot_dir);
	free(c->kb_layout);
	free(c->kb_variant);
	free(c->kb_options);
	free(c->kb_model);
	free(c->kb_rules);
	for (size_t i = 0; i < c->outputs_len; i++) {
		free(c->outputs[i].name);
	}
	free(c->outputs);
	for (size_t i = 0; i < c->binds_len; i++) {
		free(c->binds[i].combo);
		free(c->binds[i].action);
	}
	free(c->binds);
	for (size_t i = 0; i < c->rules_len; i++) {
		free(c->rules[i].name);
		free(c->rules[i].app_id);
		free(c->rules[i].title);
		free(c->rules[i].output);
	}
	free(c->rules);
	for (size_t i = 0; i < c->autostart_len; i++) {
		free(c->autostart[i]);
	}
	free(c->autostart);
	memset(c, 0, sizeof(*c));
}

static char *trim(char *s) {
	while (isspace((unsigned char)*s)) {
		s++;
	}
	char *e = s + strlen(s);
	while (e > s && isspace((unsigned char)e[-1])) {
		*--e = '\0';
	}
	return s;
}

bool config_parse_bool(const char *s, bool *out) {
	static const char *yes[] = {"1", "true", "yes", "on"};
	static const char *no[] = {"0", "false", "no", "off"};
	for (size_t i = 0; i < 4; i++) {
		if (strcasecmp(s, yes[i]) == 0) {
			*out = true;
			return true;
		}
		if (strcasecmp(s, no[i]) == 0) {
			*out = false;
			return true;
		}
	}
	return false;
}

bool config_parse_color(const char *s, uint32_t *out) {
	if (*s == '#') {
		s++;
	}
	size_t n = strlen(s);
	if (n != 6 && n != 8) {
		return false;
	}
	for (size_t i = 0; i < n; i++) {
		if (!isxdigit((unsigned char)s[i])) {
			return false;
		}
	}
	unsigned long v = strtoul(s, NULL, 16);
	*out = n == 6 ? (uint32_t)((v << 8) | 0xff) : (uint32_t)v;
	return true;
}

static bool parse_int(const char *s, int *out) {
	char *end;
	errno = 0;
	long v = strtol(s, &end, 10);
	if (errno || end == s || *trim(end) != '\0' || v < -1000000 || v > 1000000) {
		return false;
	}
	*out = (int)v;
	return true;
}

static bool parse_double(const char *s, double *out) {
	char *end;
	errno = 0;
	double v = strtod(s, &end);
	if (errno || end == s || *trim(end) != '\0') {
		return false;
	}
	*out = v;
	return true;
}

static bool parse_tristate(const char *s, int *out) {
	bool b;
	if (!config_parse_bool(s, &b)) {
		return false;
	}
	*out = b;
	return true;
}

struct parse_ctx {
	struct wx_config *c;
	enum { SEC_NONE, SEC_GENERAL, SEC_INPUT, SEC_KEYBIND, SEC_AUTOSTART,
		SEC_OUTPUT, SEC_RULE, SEC_UNKNOWN } sec;
	int line;
	char *err;
};

static bool fail(struct parse_ctx *p, const char *fmt, ...) {
	char msg[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	char buf[300];
	snprintf(buf, sizeof(buf), "line %d: %s", p->line, msg);
	free(p->err);
	p->err = xstrdup(buf);
	return false;
}

#define WANT(cond, what) do { if (!(cond)) return fail(p, "invalid %s value '%s' for '%s'", what, v, k); } while (0)

static bool parse_general(struct parse_ctx *p, const char *k, const char *v) {
	struct wx_config *c = p->c;
	if (!strcmp(k, "mod")) {
		WANT(!strcasecmp(v, "auto") || !strcasecmp(v, "super") || !strcasecmp(v, "alt") ||
			!strcasecmp(v, "ctrl"), "mod");
		set_str(&c->mod, v);
	} else if (!strcmp(k, "terminal")) {
		set_str(&c->terminal, v);
	} else if (!strcmp(k, "layout")) {
		WANT(!strcmp(v, "floating") || !strcmp(v, "tiling"), "layout");
		set_str(&c->layout, v);
	} else if (!strcmp(k, "gap")) {
		WANT(parse_int(v, &c->gap) && c->gap >= 0, "integer");
	} else if (!strcmp(k, "border")) {
		WANT(parse_int(v, &c->border) && c->border >= 0, "integer");
	} else if (!strcmp(k, "title_height")) {
		WANT(parse_int(v, &c->title_height) && c->title_height >= 0, "integer");
	} else if (!strcmp(k, "font")) {
		set_str(&c->font, v);
	} else if (!strcmp(k, "color_active")) {
		WANT(config_parse_color(v, &c->color_active), "color");
	} else if (!strcmp(k, "color_inactive")) {
		WANT(config_parse_color(v, &c->color_inactive), "color");
	} else if (!strcmp(k, "color_text")) {
		WANT(config_parse_color(v, &c->color_text), "color");
	} else if (!strcmp(k, "color_text_inactive")) {
		WANT(config_parse_color(v, &c->color_text_inactive), "color");
	} else if (!strcmp(k, "color_background")) {
		WANT(config_parse_color(v, &c->color_background), "color");
	} else if (!strcmp(k, "focus_follows_mouse")) {
		WANT(config_parse_bool(v, &c->focus_follows_mouse), "boolean");
	} else if (!strcmp(k, "xwayland")) {
		WANT(config_parse_bool(v, &c->xwayland), "boolean");
	} else if (!strcmp(k, "snap")) {
		WANT(config_parse_bool(v, &c->snap), "boolean");
	} else if (!strcmp(k, "snap_threshold")) {
		WANT(parse_int(v, &c->snap_threshold) && c->snap_threshold >= 0, "integer");
	} else if (!strcmp(k, "cursor_theme")) {
		set_str(&c->cursor_theme, v);
	} else if (!strcmp(k, "cursor_size")) {
		WANT(parse_int(v, &c->cursor_size) && c->cursor_size > 0, "integer");
	} else if (!strcmp(k, "screenshot_dir")) {
		set_str(&c->screenshot_dir, v);
	} else {
		fprintf(stderr, "waylandx: config line %d: unknown key '%s' in [general]\n", p->line, k);
	}
	return true;
}

static bool parse_input(struct parse_ctx *p, const char *k, const char *v) {
	struct wx_config *c = p->c;
	if (!strcmp(k, "kb_layout")) {
		set_str(&c->kb_layout, v);
	} else if (!strcmp(k, "kb_variant")) {
		set_str(&c->kb_variant, v);
	} else if (!strcmp(k, "kb_options")) {
		set_str(&c->kb_options, v);
	} else if (!strcmp(k, "kb_model")) {
		set_str(&c->kb_model, v);
	} else if (!strcmp(k, "kb_rules")) {
		set_str(&c->kb_rules, v);
	} else if (!strcmp(k, "repeat_rate")) {
		WANT(parse_int(v, &c->repeat_rate) && c->repeat_rate >= 0, "integer");
	} else if (!strcmp(k, "repeat_delay")) {
		WANT(parse_int(v, &c->repeat_delay) && c->repeat_delay >= 0, "integer");
	} else if (!strcmp(k, "natural_scroll")) {
		WANT(parse_tristate(v, &c->natural_scroll), "boolean");
	} else if (!strcmp(k, "tap")) {
		WANT(parse_tristate(v, &c->tap), "boolean");
	} else if (!strcmp(k, "left_handed")) {
		WANT(parse_tristate(v, &c->left_handed), "boolean");
	} else if (!strcmp(k, "accel")) {
		WANT(parse_double(v, &c->accel) && c->accel >= -1 && c->accel <= 1, "number (-1..1)");
	} else {
		fprintf(stderr, "waylandx: config line %d: unknown key '%s' in [input]\n", p->line, k);
	}
	return true;
}

static bool parse_output(struct parse_ctx *p, const char *k, const char *v) {
	struct wx_output_conf *o = &p->c->outputs[p->c->outputs_len - 1];
	if (!strcmp(k, "mode")) {
		/* WIDTHxHEIGHT[@HZ] or "preferred" */
		if (!strcmp(v, "preferred")) {
			o->width = o->height = 0;
			o->refresh = 0;
			return true;
		}
		int w, h;
		double r = 0;
		int n = sscanf(v, "%dx%d@%lf", &w, &h, &r);
		WANT(n >= 2 && w > 0 && h > 0, "mode (WxH[@Hz])");
		o->width = w;
		o->height = h;
		o->refresh = n == 3 ? r : 0;
	} else if (!strcmp(k, "scale")) {
		WANT(parse_double(v, &o->scale) && o->scale >= 0.25 && o->scale <= 8, "scale");
	} else if (!strcmp(k, "position")) {
		int x, y;
		WANT(sscanf(v, "%d,%d", &x, &y) == 2, "position (X,Y)");
		o->x = x;
		o->y = y;
	} else if (!strcmp(k, "transform")) {
		static const char *names[] = {"normal", "90", "180", "270",
			"flipped", "flipped-90", "flipped-180", "flipped-270"};
		o->transform = -1;
		for (int i = 0; i < 8; i++) {
			if (!strcmp(v, names[i])) {
				o->transform = i;
			}
		}
		WANT(o->transform >= 0, "transform");
	} else if (!strcmp(k, "adaptive_sync") || !strcmp(k, "vrr")) {
		WANT(parse_tristate(v, &o->adaptive_sync), "boolean");
	} else if (!strcmp(k, "enabled")) {
		WANT(parse_tristate(v, &o->enabled), "boolean");
	} else if (!strcmp(k, "allow_tearing")) {
		WANT(parse_tristate(v, &o->allow_tearing), "boolean");
	} else {
		fprintf(stderr, "waylandx: config line %d: unknown key '%s' in [output]\n", p->line, k);
	}
	return true;
}

static bool parse_rule(struct parse_ctx *p, const char *k, const char *v) {
	struct wx_rule *r = &p->c->rules[p->c->rules_len - 1];
	if (!strcmp(k, "app_id") || !strcmp(k, "class")) {
		set_str(&r->app_id, v);
	} else if (!strcmp(k, "title")) {
		set_str(&r->title, v);
	} else if (!strcmp(k, "floating")) {
		WANT(parse_tristate(v, &r->floating), "boolean");
	} else if (!strcmp(k, "position")) {
		WANT(sscanf(v, "%d,%d", &r->x, &r->y) == 2, "position (X,Y)");
	} else if (!strcmp(k, "size")) {
		WANT(sscanf(v, "%dx%d", &r->w, &r->h) == 2 && r->w > 0 && r->h > 0, "size (WxH)");
	} else if (!strcmp(k, "layer")) {
		if (!strcmp(v, "below")) {
			r->stack = 0;
		} else if (!strcmp(v, "normal")) {
			r->stack = 1;
		} else if (!strcmp(v, "above")) {
			r->stack = 2;
		} else {
			WANT(false, "layer (below|normal|above)");
		}
	} else if (!strcmp(k, "workspace")) {
		WANT(parse_int(v, &r->workspace) && r->workspace >= 1 && r->workspace <= 9,
			"workspace (1-9)");
	} else if (!strcmp(k, "sticky")) {
		WANT(parse_tristate(v, &r->sticky), "boolean");
	} else if (!strcmp(k, "decorations")) {
		if (!strcmp(v, "server")) {
			r->decorations = 1;
		} else if (!strcmp(v, "client") || !strcmp(v, "none")) {
			r->decorations = 0;
		} else {
			WANT(false, "decorations (server|client)");
		}
	} else if (!strcmp(k, "allow_tearing")) {
		WANT(parse_tristate(v, &r->allow_tearing), "boolean");
	} else if (!strcmp(k, "maximized")) {
		WANT(parse_tristate(v, &r->maximized), "boolean");
	} else if (!strcmp(k, "fullscreen")) {
		WANT(parse_tristate(v, &r->fullscreen), "boolean");
	} else if (!strcmp(k, "skip_taskbar")) {
		WANT(parse_tristate(v, &r->skip_taskbar), "boolean");
	} else if (!strcmp(k, "output")) {
		set_str(&r->output, v);
	} else {
		fprintf(stderr, "waylandx: config line %d: unknown key '%s' in [rule]\n", p->line, k);
	}
	return true;
}

static bool parse_section(struct parse_ctx *p, char *name) {
	struct wx_config *c = p->c;
	if (!strcmp(name, "general")) {
		p->sec = SEC_GENERAL;
	} else if (!strcmp(name, "input")) {
		p->sec = SEC_INPUT;
	} else if (!strcmp(name, "keybind") || !strcmp(name, "keybinds")) {
		p->sec = SEC_KEYBIND;
	} else if (!strcmp(name, "autostart")) {
		p->sec = SEC_AUTOSTART;
	} else if (!strncmp(name, "output:", 7) && name[7]) {
		struct wx_output_conf o = {
			.name = xstrdup(trim(name + 7)), .enabled = -1, .x = WX_UNSET, .y = WX_UNSET,
			.transform = -1, .adaptive_sync = -1, .allow_tearing = -1,
		};
		ARRAY_PUSH(c->outputs, c->outputs_len, o);
		p->sec = SEC_OUTPUT;
	} else if (!strncmp(name, "rule:", 5) && name[5]) {
		struct wx_rule r = {
			.name = xstrdup(trim(name + 5)), .floating = -1, .x = WX_UNSET, .y = WX_UNSET,
			.w = WX_UNSET, .h = WX_UNSET, .stack = -1, .sticky = -1, .decorations = -1,
			.allow_tearing = -1, .maximized = -1, .fullscreen = -1, .skip_taskbar = -1,
		};
		ARRAY_PUSH(c->rules, c->rules_len, r);
		p->sec = SEC_RULE;
	} else {
		fprintf(stderr, "waylandx: config line %d: unknown section [%s]\n", p->line, name);
		p->sec = SEC_UNKNOWN;
	}
	return true;
}

static bool parse_line(struct parse_ctx *p, char *line) {
	char *s = trim(line);
	if (*s == '\0' || *s == '#' || *s == ';') {
		return true;
	}
	if (*s == '[') {
		char *end = strchr(s, ']');
		if (!end || *trim(end + 1) != '\0') {
			return fail(p, "malformed section header");
		}
		*end = '\0';
		return parse_section(p, trim(s + 1));
	}
	char *eq = strchr(s, '=');
	if (!eq) {
		return fail(p, "expected 'key = value'");
	}
	*eq = '\0';
	char *k = trim(s);
	char *v = trim(eq + 1);
	if (*k == '\0') {
		return fail(p, "empty key");
	}
	switch (p->sec) {
	case SEC_NONE:
		return fail(p, "key '%s' outside of any section", k);
	case SEC_GENERAL:
		return parse_general(p, k, v);
	case SEC_INPUT:
		return parse_input(p, k, v);
	case SEC_KEYBIND: {
		if (*v == '\0') {
			return fail(p, "empty action for '%s'", k);
		}
		struct wx_bind_conf b = {.combo = xstrdup(k), .action = xstrdup(v)};
		ARRAY_PUSH(p->c->binds, p->c->binds_len, b);
		return true;
	}
	case SEC_AUTOSTART:
		if (*v) {
			ARRAY_PUSH(p->c->autostart, p->c->autostart_len, xstrdup(v));
		}
		return true;
	case SEC_OUTPUT:
		return parse_output(p, k, v);
	case SEC_RULE:
		return parse_rule(p, k, v);
	case SEC_UNKNOWN:
		return true;
	}
	return true;
}

bool config_parse_string(struct wx_config *out, const char *text, char **err) {
	struct wx_config c;
	config_defaults(&c);
	struct parse_ctx p = {.c = &c, .sec = SEC_NONE};
	char *copy = xstrdup(text);
	bool ok = true;
	char *cur = copy;
	while (cur) {
		char *nl = strchr(cur, '\n');
		if (nl) {
			*nl = '\0';
		}
		p.line++;
		if (!parse_line(&p, cur)) {
			ok = false;
			break;
		}
		cur = nl ? nl + 1 : NULL;
	}
	free(copy);
	if (!ok) {
		config_free(&c);
		if (err) {
			*err = p.err;
		} else {
			free(p.err);
		}
		return false;
	}
	free(p.err);
	config_free(out);
	*out = c;
	return true;
}

bool config_load_file(struct wx_config *c, const char *path, char **err) {
	FILE *f = fopen(path, "r");
	if (!f) {
		if (errno == ENOENT) {
			/* No config file: built-in defaults plus our panel. */
			return config_parse_string(c, "[autostart]\npanel = waylandx-panel\n", err);
		}
		char buf[512];
		snprintf(buf, sizeof(buf), "cannot open %s: %s", path, strerror(errno));
		if (err) {
			*err = xstrdup(buf);
		}
		return false;
	}
	size_t cap = 4096, len = 0;
	char *text = malloc(cap);
	if (!text) {
		abort();
	}
	size_t n;
	while ((n = fread(text + len, 1, cap - len - 1, f)) > 0) {
		len += n;
		if (len + 1 >= cap) {
			if (cap > (1u << 22)) {
				fclose(f);
				free(text);
				if (err) {
					*err = xstrdup("config file too large (>4MiB)");
				}
				return false;
			}
			cap *= 2;
			char *nt = realloc(text, cap);
			if (!nt) {
				abort();
			}
			text = nt;
		}
	}
	text[len] = '\0';
	fclose(f);
	bool ok = config_parse_string(c, text, err);
	free(text);
	return ok;
}

char *config_default_path(void) {
	const char *xdg = getenv("XDG_CONFIG_HOME");
	const char *home = getenv("HOME");
	char buf[4096];
	if (xdg && *xdg) {
		snprintf(buf, sizeof(buf), "%s/waylandx/config.ini", xdg);
	} else {
		snprintf(buf, sizeof(buf), "%s/.config/waylandx/config.ini", home ? home : "");
	}
	return xstrdup(buf);
}
