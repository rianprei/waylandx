#include <stdio.h>
#include <string.h>
#include <wlr/types/wlr_ext_workspace_v1.h>
#include <wlr/types/wlr_output.h>
#include "server.h"

/*
 * Workspaces 1..9 per output (like dwm/xmonad screens): switching on one
 * monitor never hides windows on another. Exposed as one ext-workspace group
 * per output.
 */

static void ext_ws_commit(struct wl_listener *listener, void *data) {
	struct wx_server *server = wl_container_of(listener, server, ext_ws_commit);
	struct wlr_ext_workspace_v1_commit_event *event = data;
	struct wlr_ext_workspace_v1_request *req;
	wl_list_for_each(req, event->requests, link) {
		if (req->type != WLR_EXT_WORKSPACE_V1_REQUEST_ACTIVATE || !req->activate.workspace) {
			continue;
		}
		struct wx_output *o;
		wl_list_for_each(o, &server->outputs, link) {
			for (int i = 1; i <= WX_WORKSPACES; i++) {
				if (o->ws_handles[i] == req->activate.workspace) {
					workspace_switch(server, o, i);
				}
			}
		}
	}
}

void workspace_init(struct wx_server *server) {
	server->ext_ws = wlr_ext_workspace_manager_v1_create(server->display, 1);
	server->ext_ws_commit.notify = ext_ws_commit;
	wl_signal_add(&server->ext_ws->events.commit, &server->ext_ws_commit);
}

void workspace_output_init(struct wx_output *output) {
	struct wx_server *server = output->server;
	output->workspace = 1;
	bool tiling = !strcmp(server->config.layout, "tiling");
	for (int i = 0; i <= WX_WORKSPACES; i++) {
		output->tiling[i] = tiling;
		output->mfact[i] = 0.55;
	}
	output->ws_group = wlr_ext_workspace_group_handle_v1_create(server->ext_ws, 0);
	wlr_ext_workspace_group_handle_v1_output_enter(output->ws_group, output->wlr_output);
	for (int i = 1; i <= WX_WORKSPACES; i++) {
		char id[96], name[8];
		snprintf(id, sizeof(id), "%s/%d", output->wlr_output->name, i);
		snprintf(name, sizeof(name), "%d", i);
		struct wlr_ext_workspace_handle_v1 *h = wlr_ext_workspace_handle_v1_create(server->ext_ws,
			id, EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_ACTIVATE);
		wlr_ext_workspace_handle_v1_set_group(h, output->ws_group);
		wlr_ext_workspace_handle_v1_set_name(h, name);
		uint32_t coord = (uint32_t)(i - 1);
		wlr_ext_workspace_handle_v1_set_coordinates(h, &coord, 1);
		wlr_ext_workspace_handle_v1_set_active(h, i == output->workspace);
		output->ws_handles[i] = h;
	}
}

void workspace_output_finish(struct wx_output *output) {
	for (int i = 1; i <= WX_WORKSPACES; i++) {
		if (output->ws_handles[i]) {
			wlr_ext_workspace_handle_v1_destroy(output->ws_handles[i]);
			output->ws_handles[i] = NULL;
		}
	}
	if (output->ws_group) {
		wlr_ext_workspace_group_handle_v1_destroy(output->ws_group);
		output->ws_group = NULL;
	}
}

struct wx_output *workspace_active_output(struct wx_server *server) {
	struct wx_output *o;
	wl_list_for_each(o, &server->outputs, link) {
		if (o == server->active_output) {
			return o;
		}
	}
	return output_focused(server);
}

void workspace_switch(struct wx_server *server, struct wx_output *output, int ws) {
	if (!output || ws < 1 || ws > WX_WORKSPACES || ws == output->workspace) {
		return;
	}
	int old = output->workspace;
	output->workspace = ws;
	server->active_output = output;
	struct wx_view *view;
	wl_list_for_each(view, &server->views, link) {
		view_update_visibility(view);
	}
	wlr_ext_workspace_handle_v1_set_active(output->ws_handles[old], false);
	wlr_ext_workspace_handle_v1_set_active(output->ws_handles[ws], true);
	if (server->focused && !view_is_visible(server->focused)) {
		view_set_activated(server->focused, false);
		server->focused = NULL;
	}
	/* prefer a window on the output we just switched */
	struct wx_view *target = NULL;
	wl_list_for_each(view, &server->views, link) {
		if (view_is_visible(view) && view_output(view) == output) {
			target = view;
			break;
		}
	}
	if (target) {
		seat_focus_view(server, target);
	} else {
		seat_refocus(server);
	}
	layout_arrange(server);
	char line[160];
	snprintf(line, sizeof(line), "{\"event\":\"workspace\",\"workspace\":%d,\"output\":\"%s\"}",
		ws, output->wlr_output->name);
	ipc_broadcast_event(server, line);
}
