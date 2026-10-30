#include <fnmatch.h>
#include "server.h"

static bool rule_matches(const struct wx_rule *r, struct wx_view *view) {
	if (!r->app_id && !r->title) {
		return false;
	}
	if (r->app_id && fnmatch(r->app_id, view_app_id(view), 0) != 0) {
		return false;
	}
	if (r->title && fnmatch(r->title, view_title(view), 0) != 0) {
		return false;
	}
	return true;
}

void rules_apply(struct wx_view *view) {
	struct wx_config *c = &view->server->config;
	for (size_t i = 0; i < c->rules_len; i++) {
		const struct wx_rule *r = &c->rules[i];
		if (!rule_matches(r, view)) {
			continue;
		}
		if (r->floating >= 0) {
			view->floating = r->floating;
		}
		if (r->output) {
			struct wx_output *o = output_by_name(view->server, r->output);
			if (o) {
				view->x = o->usable.x + (o->usable.width - view->w) / 2;
				view->y = o->usable.y + (o->usable.height - view->h) / 2;
				view->placed = true;
			}
		}
		if (r->w != WX_UNSET && r->h != WX_UNSET) {
			view_resize(view, r->w, r->h);
			view->w = r->w;
			view->h = r->h;
		}
		if (r->x != WX_UNSET && r->y != WX_UNSET) {
			view->x = r->x;
			view->y = r->y;
			view->placed = true;
		}
		if (r->stack >= 0) {
			view->stack = (enum wx_stack)r->stack;
		}
		if (r->workspace > 0) {
			view->workspace = r->workspace;
		}
		if (r->sticky >= 0) {
			view->sticky = r->sticky;
		}
		if (r->decorations >= 0) {
			view->ssd = r->decorations;
			view->force_csd = !r->decorations;
		}
		if (r->allow_tearing >= 0) {
			view->allow_tearing = r->allow_tearing;
		}
		if (r->maximized > 0) {
			view->maximized = true;
		}
		if (r->fullscreen > 0) {
			view->fullscreen = true;
		}
		if (r->skip_taskbar >= 0) {
			view->skip_taskbar = r->skip_taskbar;
		}
	}
}
