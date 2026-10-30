#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"

static void test_defaults(void) {
	struct wx_config c;
	config_defaults(&c);
	char *err = NULL;
	assert(config_parse_string(&c, "", &err));
	assert(!strcmp(c.mod, "auto"));
	assert(c.border == 2 && c.title_height == 24);
	assert(c.xwayland);
	config_free(&c);
}

static void test_full(void) {
	const char *text =
		"# comment\n"
		"[general]\n"
		"mod = super\n"
		"terminal = foot\n"
		"gap = 10\n"
		"color_active = #ff0000\n"
		"focus_follows_mouse = yes\n"
		"\n"
		"[input]\n"
		"kb_layout = br\n"
		"kb_variant = abnt2\n"
		"repeat_rate = 40\n"
		"accel = -0.5\n"
		"[output:DP-1]\n"
		"mode = 2560x1440@144\n"
		"scale = 1.25\n"
		"position = 1920,0\n"
		"vrr = on\n"
		"[output:*]\n"
		"transform = 90\n"
		"[keybind]\n"
		"Mod+Return = exec foot\n"
		"Mod+q = none\n"
		"[rule:float-calc]\n"
		"app_id = *calculator*\n"
		"floating = true\n"
		"position = 100,200\n"
		"size = 300x400\n"
		"layer = above\n"
		"workspace = 3\n"
		"decorations = client\n"
		"[autostart]\n"
		"panel = waylandx-panel\n"
		"bg = swaybg -c #000000\n";
	struct wx_config c;
	config_defaults(&c);
	char *err = NULL;
	bool ok = config_parse_string(&c, text, &err);
	if (!ok) {
		fprintf(stderr, "unexpected error: %s\n", err);
	}
	assert(ok);
	assert(!strcmp(c.mod, "super"));
	assert(!strcmp(c.terminal, "foot"));
	assert(c.gap == 10);
	assert(c.color_active == 0xff0000ff);
	assert(c.focus_follows_mouse);
	assert(!strcmp(c.kb_layout, "br") && !strcmp(c.kb_variant, "abnt2"));
	assert(c.repeat_rate == 40 && c.accel == -0.5);
	assert(c.outputs_len == 2);
	assert(!strcmp(c.outputs[0].name, "DP-1"));
	assert(c.outputs[0].width == 2560 && c.outputs[0].height == 1440);
	assert(c.outputs[0].refresh == 144 && c.outputs[0].scale == 1.25);
	assert(c.outputs[0].x == 1920 && c.outputs[0].y == 0 && c.outputs[0].adaptive_sync == 1);
	assert(c.outputs[1].transform == 1 && c.outputs[1].x == WX_UNSET);
	assert(c.binds_len == 2 && !strcmp(c.binds[1].action, "none"));
	assert(c.rules_len == 1);
	struct wx_rule *r = &c.rules[0];
	assert(!strcmp(r->app_id, "*calculator*") && r->floating == 1);
	assert(r->x == 100 && r->y == 200 && r->w == 300 && r->h == 400);
	assert(r->stack == 2 && r->workspace == 3 && r->decorations == 0);
	assert(r->title == NULL && r->sticky == -1);
	assert(c.autostart_len == 2 && !strcmp(c.autostart[1], "swaybg -c #000000"));
	config_free(&c);
}

/* A broken file must leave the previous config untouched (live reload safety). */
static void test_error_keeps_previous(void) {
	struct wx_config c;
	config_defaults(&c);
	char *err = NULL;
	assert(config_parse_string(&c, "[general]\ngap = 3\n", &err));
	assert(c.gap == 3);

	const char *bad[] = {
		"[general]\ngap = abc\n",
		"[general]\nthis line has no equals\n",
		"gap = 1\n",
		"[general\n",
		"[rule:x]\nlayer = sideways\n",
		"[output:X]\nmode = big\n",
		"[input]\naccel = 5\n",
		"[keybind]\nMod+x =\n",
	};
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		err = NULL;
		assert(!config_parse_string(&c, bad[i], &err));
		assert(err && !strncmp(err, "line ", 5));
		free(err);
		assert(c.gap == 3);
	}
	/* line numbers count blank lines too */
	err = NULL;
	assert(!config_parse_string(&c, "[general]\n\n\ngap = x\n", &err));
	assert(!strncmp(err, "line 4:", 7));
	free(err);
	config_free(&c);
}

static void test_values(void) {
	uint32_t col;
	assert(config_parse_color("#11223344", &col) && col == 0x11223344);
	assert(config_parse_color("aabbcc", &col) && col == 0xaabbccff);
	assert(!config_parse_color("#12345", &col));
	assert(!config_parse_color("#gg0000", &col));
	bool b;
	assert(config_parse_bool("On", &b) && b);
	assert(config_parse_bool("0", &b) && !b);
	assert(!config_parse_bool("maybe", &b));
}

int main(void) {
	test_defaults();
	test_full();
	test_error_keeps_previous();
	test_values();
	puts("config: all tests passed");
	return 0;
}
