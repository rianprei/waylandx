#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/util/log.h>
#include <wlr/xwayland.h>
#include "server.h"

#ifndef WX_DATADIR
#define WX_DATADIR "/usr/local/share"
#endif

void server_spawn(struct wx_server *server, const char *cmd) {
	pid_t pid = fork();
	if (pid < 0) {
		wlr_log_errno(WLR_ERROR, "fork failed");
		return;
	}
	if (pid == 0) {
		/* Double fork so the compositor never accumulates zombies. */
		sigset_t set;
		sigemptyset(&set);
		sigprocmask(SIG_SETMASK, &set, NULL);
		setsid();
		pid_t child = fork();
		if (child == 0) {
			execl("/bin/sh", "/bin/sh", "-c", cmd, (char *)NULL);
			_exit(127);
		}
		_exit(child < 0 ? 1 : 0);
	}
	waitpid(pid, NULL, 0);
	wlr_log(WLR_INFO, "spawned: %s", cmd);
}

static void apply_mod(struct wx_server *server) {
	const char *m = server->config.mod;
	if (!strcasecmp(m, "alt")) {
		server->mod_mask = WLR_MODIFIER_ALT;
	} else if (!strcasecmp(m, "ctrl")) {
		server->mod_mask = WLR_MODIFIER_CTRL;
	} else if (!strcasecmp(m, "super")) {
		server->mod_mask = WLR_MODIFIER_LOGO;
	} else {
		/* auto: the host compositor usually owns Super when nested */
		server->mod_mask = server->nested ? WLR_MODIFIER_ALT : WLR_MODIFIER_LOGO;
	}
}

bool server_reload(struct wx_server *server, char **err) {
	if (!config_load_file(&server->config, server->config_path, err)) {
		wlr_log(WLR_ERROR, "config reload failed, keeping previous: %s", *err);
		return false;
	}
	wlr_log(WLR_INFO, "config reloaded from %s", server->config_path);
	apply_mod(server);
	binds_reload(server);
	input_apply_config(server);
	outputs_apply_config(server);
	struct wx_view *view;
	wl_list_for_each(view, &server->views, link) {
		deco_update(view);
	}
	layout_arrange(server);
	return true;
}

static int handle_sighup(int sig, void *data) {
	char *err = NULL;
	server_reload(data, &err);
	free(err);
	return 0;
}

static int handle_terminate(int sig, void *data) {
	struct wx_server *server = data;
	wl_display_terminate(server->display);
	return 0;
}

static void setup_shims_path(void) {
	const char *dir = getenv("WAYLANDX_SHIMS");
	char buf[4096];
	if (!dir || !*dir) {
		snprintf(buf, sizeof(buf), "%s/waylandx/shims", WX_DATADIR);
		dir = buf;
	}
	struct stat st;
	if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
		return;
	}
	const char *path = getenv("PATH");
	if (path && strstr(path, dir)) {
		return;
	}
	size_t n = strlen(dir) + (path ? strlen(path) : 0) + 2;
	char *np = malloc(n);
	if (!np) {
		return;
	}
	snprintf(np, n, "%s%s%s", dir, path ? ":" : "", path ? path : "");
	setenv("PATH", np, 1);
	free(np);
}

/* wlroots asserts that no listeners remain when its objects are destroyed. */
#define SERVER_LISTENERS(X) X(new_xdg_toplevel) X(new_xdg_popup) X(new_layer_surface) \
	X(new_xwayland_surface) X(xwayland_ready) X(new_output) X(new_input) X(cursor_motion) \
	X(cursor_motion_abs) X(cursor_button) X(cursor_axis) X(cursor_frame) \
	X(cursor_swipe_begin) X(cursor_swipe_update) X(cursor_swipe_end) X(cursor_pinch_begin) \
	X(cursor_pinch_update) X(cursor_pinch_end) X(cursor_hold_begin) X(cursor_hold_end) \
	X(cursor_touch_down) X(cursor_touch_up) X(cursor_touch_motion) X(cursor_touch_cancel) \
	X(cursor_touch_frame) X(cursor_tool_proximity) X(cursor_tool_axis) X(cursor_tool_tip) \
	X(cursor_tool_button) X(kb_focus_pads) X(new_text_input) X(new_input_method) X(kb_focus_change) \
	X(im_commit) X(im_grab_keyboard) X(im_new_popup) X(im_destroy) X(im_grab_destroy) \
	X(request_cursor) X(request_set_selection) X(request_set_primary_selection) \
	X(request_start_drag) X(start_drag) X(drag_destroy) X(ext_ws_commit) \
	X(new_idle_inhibitor) X(new_constraint) X(new_kb_inhibitor) X(output_mgr_apply) \
	X(output_mgr_test) X(output_power_set_mode) X(layout_change) X(activation_request) \
	X(cursor_shape_request) X(new_virtual_keyboard) X(new_virtual_pointer) \
	X(new_xdg_decoration) X(toplevel_capture_request) X(drm_lease_request) X(new_lock) \
	X(lock_new_surface) X(lock_unlock) X(lock_destroy)

static void remove_listeners(struct wx_server *server) {
#define REMOVE(f) if (server->f.link.next) { wl_list_remove(&server->f.link); wl_list_init(&server->f.link); }
	SERVER_LISTENERS(REMOVE)
#undef REMOVE
}

static void usage(const char *argv0) {
	printf("Usage: %s [-c config.ini] [-s startup-cmd] [-d] [-v]\n"
		"  -c  config file (default ~/.config/waylandx/config.ini)\n"
		"  -s  command to run once the server is up\n"
		"  -d  debug logging\n"
		"  -v  print version\n", argv0);
}

int main(int argc, char *argv[]) {
	const char *startup_cmd = NULL;
	const char *config_path = NULL;
	enum wlr_log_importance log_level = WLR_INFO;
	int c;
	while ((c = getopt(argc, argv, "c:s:dvh")) != -1) {
		switch (c) {
		case 'c':
			config_path = optarg;
			break;
		case 's':
			startup_cmd = optarg;
			break;
		case 'd':
			log_level = WLR_DEBUG;
			break;
		case 'v':
			printf("waylandx %s\n", WX_VERSION);
			return 0;
		default:
			usage(argv[0]);
			return c == 'h' ? 0 : 1;
		}
	}
	wlr_log_init(log_level, NULL);

	static struct wx_server server = {0};
	server.config_path = config_path ? strdup(config_path) : config_default_path();
	config_defaults(&server.config);
	char *err = NULL;
	if (!config_load_file(&server.config, server.config_path, &err)) {
		wlr_log(WLR_ERROR, "config error, using defaults: %s", err);
		free(err);
	}
	server.nested = getenv("WAYLAND_DISPLAY") || getenv("DISPLAY");
	apply_mod(&server);

	server.display = wl_display_create();
	server.loop = wl_display_get_event_loop(server.display);
	server.backend = wlr_backend_autocreate(server.loop, &server.session);
	if (!server.backend) {
		wlr_log(WLR_ERROR, "failed to create backend");
		return 1;
	}
	server.renderer = wlr_renderer_autocreate(server.backend);
	if (!server.renderer) {
		wlr_log(WLR_ERROR, "failed to create renderer");
		return 1;
	}
	wlr_renderer_init_wl_shm(server.renderer, server.display);
	server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);
	if (!server.allocator) {
		wlr_log(WLR_ERROR, "failed to create allocator");
		return 1;
	}

	server.compositor = wlr_compositor_create(server.display, 6, server.renderer);
	wlr_subcompositor_create(server.display);

	server.output_layout = wlr_output_layout_create(server.display);
	server.scene = wlr_scene_create();
	server.scene_layout = wlr_scene_attach_output_layout(server.scene, server.output_layout);
	struct wlr_scene_tree *root = &server.scene->tree;
	server.desktop_bg = wlr_scene_tree_create(root);
	server.layer_bg = wlr_scene_tree_create(root);
	server.layer_bottom = wlr_scene_tree_create(root);
	server.win_below = wlr_scene_tree_create(root);
	server.win_normal = wlr_scene_tree_create(root);
	server.win_above = wlr_scene_tree_create(root);
	server.win_fullscreen = wlr_scene_tree_create(root);
	server.layer_top = wlr_scene_tree_create(root);
	server.unmanaged = wlr_scene_tree_create(root);
	server.layer_overlay = wlr_scene_tree_create(root);
	server.drag_icons = wlr_scene_tree_create(root);
	server.lock_tree = wlr_scene_tree_create(root);

	wl_list_init(&server.views);
	wl_list_init(&server.unmanaged_views);
	wl_list_init(&server.layers);
	wl_list_init(&server.binds);
	wl_list_init(&server.ipc_clients);
	server.next_view_id = 1;
	server.ipc_fd = -1;

	output_init(&server);
	seat_init(&server);
	input_init(&server);
	inject_init(&server);
	ime_init(&server);
	xdg_init(&server);
	layer_init(&server);
	workspace_init(&server);
	protocols_init(&server);
	wxproto_init(&server);
	lock_init(&server);

	server.socket = wl_display_add_socket_auto(server.display);
	if (!server.socket) {
		wlr_log(WLR_ERROR, "failed to add wayland socket");
		wlr_backend_destroy(server.backend);
		return 1;
	}
	if (!wlr_backend_start(server.backend)) {
		wlr_log(WLR_ERROR, "failed to start backend");
		wlr_backend_destroy(server.backend);
		wl_display_destroy(server.display);
		return 1;
	}

	setenv("WAYLAND_DISPLAY", server.socket, 1);
	unsetenv("DISPLAY"); /* set again once our Xwayland is up */
	setenv("XDG_CURRENT_DESKTOP", "WaylandX", 1);
	setenv("XDG_SESSION_DESKTOP", "WaylandX", 1);
	setenv("XDG_SESSION_TYPE", "wayland", 1);
	setup_shims_path();

	if (server.config.xwayland) {
		xwayland_init(&server);
	}
	if (!ipc_init(&server)) {
		wlr_log(WLR_ERROR, "IPC disabled");
	}
	binds_reload(&server);

	server.sighup_source = wl_event_loop_add_signal(server.loop, SIGHUP, handle_sighup, &server);
	server.sigterm_source = wl_event_loop_add_signal(server.loop, SIGTERM, handle_terminate, &server);
	server.sigint_source = wl_event_loop_add_signal(server.loop, SIGINT, handle_terminate, &server);
	signal(SIGPIPE, SIG_IGN);

	wlr_log(WLR_INFO, "WaylandX %s running on WAYLAND_DISPLAY=%s (%s, mod=%s)", WX_VERSION,
		server.socket, server.nested ? "nested" : "native",
		server.mod_mask == WLR_MODIFIER_ALT ? "Alt" :
		server.mod_mask == WLR_MODIFIER_CTRL ? "Ctrl" : "Super");

	for (size_t i = 0; i < server.config.autostart_len; i++) {
		server_spawn(&server, server.config.autostart[i]);
	}
	if (startup_cmd) {
		server_spawn(&server, startup_cmd);
	}

	wl_display_run(server.display);

	wlr_log(WLR_INFO, "shutting down");
	ipc_finish(&server);
	wl_display_destroy_clients(server.display);
	remove_listeners(&server);
	if (server.xwayland) {
		wlr_xwayland_destroy(server.xwayland);
	}
	inject_finish(&server);
	wl_event_source_remove(server.sighup_source);
	wl_event_source_remove(server.sigterm_source);
	wl_event_source_remove(server.sigint_source);
	/* the scene owns these rects; outputs are destroyed later with the backend */
	struct wx_output *output;
	wl_list_for_each(output, &server.outputs, link) {
		output->background = NULL;
		output->lock_bg = NULL;
	}
	wlr_scene_node_destroy(&server.scene->tree.node);
	wlr_xcursor_manager_destroy(server.cursor_mgr);
	wlr_cursor_destroy(server.cursor);
	wlr_allocator_destroy(server.allocator);
	wlr_renderer_destroy(server.renderer);
	wlr_backend_destroy(server.backend);
	wl_display_destroy(server.display);
	config_free(&server.config);
	free(server.config_path);
	return 0;
}
