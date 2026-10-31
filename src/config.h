#ifndef WX_CONFIG_H
#define WX_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WX_UNSET (-0x7fffffff)

struct wx_output_conf {
	char *name;          /* connector name or "*" */
	int enabled;         /* -1 unset, 0, 1 */
	int width, height;   /* 0 = preferred */
	double refresh;      /* Hz, 0 = any */
	double scale;        /* 0 = unset */
	int x, y;            /* WX_UNSET = auto */
	int transform;       /* -1 unset, 0..7 wl_output_transform */
	int adaptive_sync;   /* -1 unset */
	int allow_tearing;   /* -1 unset */
};

struct wx_bind_conf {
	char *combo;
	char *action;        /* "none" removes a default */
};

struct wx_rule {
	char *name;
	char *app_id;        /* fnmatch glob, matches xdg app_id or X11 class */
	char *title;         /* fnmatch glob */
	int floating;        /* -1 unset */
	int x, y, w, h;      /* WX_UNSET */
	int stack;           /* -1 unset, 0 below, 1 normal, 2 above */
	int workspace;       /* 0 unset */
	int sticky;
	int decorations;     /* -1 unset, 0 none (client side), 1 server */
	int allow_tearing;
	int maximized;
	int fullscreen;
	int skip_taskbar;
	char *output;
};

struct wx_config {
	/* [general] */
	char *mod;           /* auto|super|alt|ctrl */
	char *terminal;
	char *layout;        /* floating|tiling: default per workspace */
	int gap;
	int border;
	int title_height;
	char *font;
	uint32_t color_active, color_inactive, color_text, color_text_inactive;
	uint32_t color_background;
	bool focus_follows_mouse;
	bool xwayland;
	double xwayland_scale; /* 0 = auto (largest output scale) */
	bool snap;
	int snap_threshold;
	bool lid_disable_internal;  /* closing the lid turns the built-in panel off */
	char *internal_output;      /* fnmatch glob; NULL = eDP*, LVDS*, DSI* */
	char *cursor_theme;
	int cursor_size;
	char *screenshot_dir;
	int idle_lock;              /* seconds idle before locking, 0 = never */
	int idle_dpms;              /* seconds idle before screens off, 0 = never */
	char *lock_command;
	bool lock_on_sleep;         /* lock before suspend (logind) */
	char *night_light;          /* off | on | HH:MM-HH:MM */
	int night_temp;             /* Kelvin at night */

	/* [input] */
	char *kb_layout, *kb_variant, *kb_options, *kb_model, *kb_rules;
	int repeat_rate, repeat_delay;
	int natural_scroll, tap, left_handed; /* -1 unset */
	double accel;        /* -2 unset, else -1..1 */

	struct wx_output_conf *outputs;
	size_t outputs_len;
	struct wx_bind_conf *binds;
	size_t binds_len;
	struct wx_rule *rules;
	size_t rules_len;
	char **autostart;
	size_t autostart_len;
};

void config_defaults(struct wx_config *c);
void config_free(struct wx_config *c);
/* On failure *c is left untouched and *err holds "line N: message". */
bool config_parse_string(struct wx_config *c, const char *text, char **err);
bool config_load_file(struct wx_config *c, const char *path, char **err);
char *config_default_path(void);
bool config_parse_color(const char *s, uint32_t *out);
bool config_parse_bool(const char *s, bool *out);

#endif
