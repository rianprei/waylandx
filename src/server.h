#ifndef WX_SERVER_H
#define WX_SERVER_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include <wlr/util/box.h>
#include <xkbcommon/xkbcommon.h>
#include "config.h"

#define WX_WORKSPACES 9

struct wlr_backend;
struct wlr_session;
struct wlr_renderer;
struct wlr_allocator;
struct wlr_compositor;
struct wlr_scene;
struct wlr_scene_tree;
struct wlr_scene_rect;
struct wlr_scene_buffer;
struct wlr_scene_output;
struct wlr_scene_output_layout;
struct wlr_output_layout;
struct wlr_output;
struct wlr_seat;
struct wlr_cursor;
struct wlr_xcursor_manager;
struct wlr_keyboard;
struct wlr_input_device;
struct wlr_surface;
struct wlr_xdg_shell;
struct wlr_xdg_toplevel;
struct wlr_xdg_toplevel_decoration_v1;
struct wlr_xwayland;
struct wlr_xwayland_surface;
struct wlr_layer_shell_v1;
struct wlr_layer_surface_v1;
struct wlr_scene_layer_surface_v1;
struct wlr_foreign_toplevel_manager_v1;
struct wlr_foreign_toplevel_handle_v1;
struct wlr_ext_foreign_toplevel_list_v1;
struct wlr_ext_foreign_toplevel_handle_v1;
struct wlr_ext_workspace_manager_v1;
struct wlr_ext_workspace_group_handle_v1;
struct wlr_ext_workspace_handle_v1;
struct wlr_ext_image_capture_source_v1;
struct wlr_idle_notifier_v1;
struct wlr_idle_inhibit_manager_v1;
struct wlr_pointer_constraints_v1;
struct wlr_pointer_constraint_v1;
struct wlr_relative_pointer_manager_v1;
struct wlr_keyboard_shortcuts_inhibit_manager_v1;
struct wlr_tearing_control_manager_v1;
struct wlr_output_manager_v1;
struct wlr_session_lock_v1;
struct wlr_xdg_activation_v1;
struct wlr_cursor_shape_manager_v1;
struct wlr_pointer_gestures_v1;
struct wlr_tablet_manager_v2;
struct wlr_input_method_v2;
struct wlr_input_method_keyboard_grab_v2;
struct wlr_ext_image_copy_capture_manager_v1;

/* What a scene node with non-NULL data belongs to. */
enum wx_node_kind {
	WX_NODE_VIEW,
	WX_NODE_LAYER,
	WX_NODE_LOCK,
};

#define WX_TAG_MAGIC 0x57584e54u

/* Stored in wlr_scene_node.data of trees we own. */
struct wx_node_tag {
	uint32_t magic;
	enum wx_node_kind kind;
	void *ptr;
};

enum wx_cursor_mode {
	WX_CURSOR_PASSTHROUGH,
	WX_CURSOR_MOVE,
	WX_CURSOR_RESIZE,
};

enum wx_view_type {
	WX_VIEW_XDG,
	WX_VIEW_XWAYLAND,
};

enum wx_stack {
	WX_STACK_BELOW,
	WX_STACK_NORMAL,
	WX_STACK_ABOVE,
};

struct wx_server;

struct wx_output {
	struct wl_list link; /* wx_server.outputs */
	struct wx_server *server;
	struct wlr_output *wlr_output;
	struct wlr_scene_output *scene_output;
	struct wlr_box full;   /* layout box */
	struct wlr_box usable; /* minus layer-shell exclusive zones */
	struct wlr_scene_rect *lock_bg;
	struct wlr_scene_rect *background; /* solid desktop color under layer-shell wallpapers */
	struct wl_list captures; /* wx_capture_part.link pending on this output */
	int workspace;           /* 1..WX_WORKSPACES shown on this output */
	bool tiling[WX_WORKSPACES + 1];
	double mfact[WX_WORKSPACES + 1];
	struct wlr_ext_workspace_group_handle_v1 *ws_group;
	struct wlr_ext_workspace_handle_v1 *ws_handles[WX_WORKSPACES + 1];
	struct wl_listener frame;
	struct wl_listener request_state;
	struct wl_listener commit;
	struct wl_listener destroy;
};

struct wx_keyboard {
	struct wl_list link;
	struct wx_server *server;
	struct wlr_keyboard *wlr_keyboard;
	bool is_virtual;
	struct wl_listener modifiers;
	struct wl_listener key;
	struct wl_listener destroy;
};

struct wx_touch_dev {
	struct wx_server *server;
	struct wl_listener destroy;
};

struct wx_deco {
	struct wlr_scene_tree *tree;
	struct wlr_scene_rect *border[4]; /* top, bottom, left, right */
	struct wlr_scene_buffer *title;
	int title_w, title_h;
	bool title_active;
	char *title_text;
};

struct wx_wxcontrol; /* waylandx-unstable-v1 control object */

struct wx_view {
	struct wl_list link;       /* wx_server.views, front = most recently focused */
	struct wx_server *server;
	enum wx_view_type type;
	uint32_t id;
	struct wx_node_tag tag;
	union {
		struct wlr_xdg_toplevel *xdg;
		struct wlr_xwayland_surface *xsurface;
	};
	struct wlr_scene_tree *tree;    /* frame origin == content origin */
	struct wlr_scene_tree *content; /* surface tree */
	struct wx_deco deco;

	int x, y, w, h;          /* content box in layout coords */
	struct wlr_box saved;    /* geometry before maximize/fullscreen/tiling */
	bool mapped;
	bool unmanaged;          /* X11 override-redirect */
	bool ssd;                /* server-side decorations */
	bool maximized, fullscreen, minimized, sticky, floating;
	bool skip_taskbar;
	bool allow_tearing;
	bool force_csd;          /* rule decorations=client */
	struct wlr_xdg_toplevel_decoration_v1 *xdg_deco;
	bool placed;             /* position chosen (by client, rule or us) */
	bool has_req_pos;        /* client asked for a position before map */
	int req_x, req_y, req_w, req_h;
	enum wx_stack stack;
	int workspace;           /* 1..WX_WORKSPACES, on view->output */
	struct wx_output *output; /* output whose workspaces this view belongs to */
	bool orphaned;            /* its output went away; adopt the next one */

	struct wlr_foreign_toplevel_handle_v1 *ftl;
	struct wlr_ext_foreign_toplevel_handle_v1 *ext_ftl;
	struct wlr_ext_image_capture_source_v1 *capture_source;
	struct wx_wxcontrol *control;

	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener commit;
	struct wl_listener destroy;
	struct wl_listener request_move;
	struct wl_listener request_resize;
	struct wl_listener request_maximize;
	struct wl_listener request_fullscreen;
	struct wl_listener request_minimize;
	struct wl_listener set_title;
	struct wl_listener set_app_id;
	/* xwayland only */
	struct wl_listener associate;
	struct wl_listener dissociate;
	struct wl_listener request_configure;
	struct wl_listener request_activate;
	struct wl_listener request_above;
	struct wl_listener request_below;
	struct wl_listener request_sticky;
	struct wl_listener request_close;
	struct wl_listener set_geometry;
	struct wl_listener set_decorations;
	struct wl_listener set_override_redirect;
	struct wl_listener map_request;
	/* foreign toplevel requests */
	struct wl_listener ftl_activate;
	struct wl_listener ftl_close;
	struct wl_listener ftl_maximize;
	struct wl_listener ftl_minimize;
	struct wl_listener ftl_fullscreen;
	struct wl_listener ftl_destroy;
};

struct wx_layer_surface {
	struct wl_list link;
	struct wx_server *server;
	struct wx_node_tag tag;
	struct wlr_layer_surface_v1 *layer_surface;
	struct wlr_scene_layer_surface_v1 *scene;
	struct wx_output *output;
	bool mapped;
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener commit;
	struct wl_listener destroy;
	struct wl_listener new_popup;
};

struct wx_bind {
	struct wl_list link;
	uint32_t mods;
	xkb_keysym_t sym;
	char *action;            /* command string; NULL when owned by IPC client */
	struct wx_ipc_client *owner;
	char *combo;
};

struct wx_ipc_client;

struct wx_server {
	struct wl_display *display;
	struct wl_event_loop *loop;
	struct wlr_backend *backend;
	struct wlr_session *session;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;
	struct wlr_compositor *compositor;
	struct wlr_scene *scene;
	struct wlr_scene_output_layout *scene_layout;
	struct wlr_output_layout *output_layout;
	const char *socket;
	bool nested;

	/* stacking order, bottom to top */
	struct wlr_scene_tree *desktop_bg;
	struct wlr_scene_tree *layer_bg;
	struct wlr_scene_tree *layer_bottom;
	struct wlr_scene_tree *win_below;
	struct wlr_scene_tree *win_normal;
	struct wlr_scene_tree *win_above;
	struct wlr_scene_tree *win_fullscreen;
	struct wlr_scene_tree *layer_top;
	struct wlr_scene_tree *unmanaged;
	struct wlr_scene_tree *layer_overlay;
	struct wlr_scene_tree *drag_icons;
	struct wlr_scene_tree *lock_tree;

	struct wl_list outputs;
	struct wl_list views;
	struct wl_list unmanaged_views;
	struct wl_list layers;
	struct wl_list keyboards;
	struct wl_list binds;
	uint32_t next_view_id;

	struct wlr_xdg_shell *xdg_shell;
	struct wlr_layer_shell_v1 *layer_shell;
	struct wlr_xwayland *xwayland;
	struct wl_listener new_xdg_toplevel;
	struct wl_listener new_xdg_popup;
	struct wl_listener new_layer_surface;
	struct wl_listener new_xwayland_surface;
	struct wl_listener xwayland_ready;
	struct wl_listener new_output;
	struct wl_listener new_input;

	/* seat and input */
	struct wlr_seat *seat;
	struct wlr_cursor *cursor;
	struct wlr_xcursor_manager *cursor_mgr;
	struct wx_keyboard *last_keyboard;
	struct wlr_keyboard *inject_kb;
	char *inject_keymap_cache;
	enum wx_cursor_mode cursor_mode;
	struct wx_view *grab_view;
	double grab_x, grab_y;
	struct wlr_box grab_box;
	uint32_t resize_edges;
	int snap;                      /* pending snap while moving: 0 none, 1 left, 2 right, 3 max */
	struct wx_view *focused;
	struct wx_layer_surface *focused_layer;
	struct wlr_pointer_constraint_v1 *active_constraint;
	uint32_t pressed_buttons;
	struct wl_listener cursor_motion;
	struct wl_listener cursor_motion_abs;
	struct wl_listener cursor_button;
	struct wl_listener cursor_axis;
	struct wl_listener cursor_frame;
	struct wl_listener cursor_swipe_begin, cursor_swipe_update, cursor_swipe_end;
	struct wl_listener cursor_pinch_begin, cursor_pinch_update, cursor_pinch_end;
	struct wl_listener cursor_hold_begin, cursor_hold_end;
	struct wl_listener cursor_touch_down, cursor_touch_up, cursor_touch_motion;
	struct wl_listener cursor_touch_cancel, cursor_touch_frame;
	struct wl_listener cursor_tool_proximity, cursor_tool_axis, cursor_tool_tip, cursor_tool_button;
	struct wlr_tablet_manager_v2 *tablet_manager;

	/* input method relay (ime.c) */
	struct wl_list text_inputs;
	struct wl_list ime_popups;
	struct wlr_input_method_v2 *input_method;
	struct wlr_input_method_keyboard_grab_v2 *im_grab;
	struct wl_listener new_text_input, new_input_method, kb_focus_change;
	struct wl_listener im_commit, im_grab_keyboard, im_new_popup, im_destroy, im_grab_destroy;
	int touch_devices;
	struct wl_listener request_cursor;
	struct wl_listener request_set_selection;
	struct wl_listener request_set_primary_selection;
	struct wl_listener request_start_drag;
	struct wl_listener start_drag;
	struct wl_listener drag_destroy;

	/* workspaces (per output, see wx_output) */
	struct wx_output *active_output; /* where workspace commands apply */
	struct wlr_ext_workspace_manager_v1 *ext_ws;
	struct wl_listener ext_ws_commit;

	/* protocols that need hooks */
	struct wlr_foreign_toplevel_manager_v1 *ftl_mgr;
	struct wlr_ext_foreign_toplevel_list_v1 *ext_ftl_list;
	struct wlr_idle_notifier_v1 *idle_notifier;
	struct wlr_idle_inhibit_manager_v1 *idle_inhibit;
	struct wlr_pointer_constraints_v1 *pointer_constraints;
	struct wlr_relative_pointer_manager_v1 *relative_pointer;
	struct wlr_keyboard_shortcuts_inhibit_manager_v1 *kb_inhibit;
	struct wlr_tearing_control_manager_v1 *tearing;
	struct wlr_output_manager_v1 *output_mgr;
	struct wlr_xdg_activation_v1 *activation;
	struct wlr_cursor_shape_manager_v1 *cursor_shape;
	struct wlr_pointer_gestures_v1 *gestures;
	struct wl_listener new_idle_inhibitor;
	struct wl_listener new_constraint;
	struct wl_listener new_kb_inhibitor;
	struct wl_listener output_mgr_apply;
	struct wl_listener output_mgr_test;
	struct wl_listener output_power_set_mode;
	struct wl_listener layout_change;
	struct wl_listener activation_request;
	struct wl_listener cursor_shape_request;
	struct wl_listener new_virtual_keyboard;
	struct wl_listener new_virtual_pointer;
	struct wl_listener new_xdg_decoration;
	struct wl_listener toplevel_capture_request;
	struct wl_listener drm_lease_request;
	struct wl_listener new_lock;

	/* session lock */
	bool locked;
	struct wlr_session_lock_v1 *lock;
	struct wl_listener lock_new_surface;
	struct wl_listener lock_unlock;
	struct wl_listener lock_destroy;

	/* config & ipc */
	struct wx_config config;
	char *config_path;
	uint32_t mod_mask;
	int ipc_fd;
	char *ipc_path;
	struct wl_event_source *ipc_source;
	struct wl_list ipc_clients;
	struct wl_event_source *sighup_source;
	struct wl_event_source *sigterm_source;
	struct wl_event_source *sigint_source;
};

/* main.c */
void server_spawn(struct wx_server *server, const char *cmd);
bool server_reload(struct wx_server *server, char **err);

/* output.c */
void output_init(struct wx_server *server);
struct wx_output *output_at(struct wx_server *server, double lx, double ly);
struct wx_output *output_for_view(struct wx_view *view);
struct wx_output *output_focused(struct wx_server *server);
struct wx_output *output_by_name(struct wx_server *server, const char *name);
void output_apply_config(struct wx_output *output);
void outputs_apply_config(struct wx_server *server);
void output_update_manager(struct wx_server *server);
void output_update_background(struct wx_output *output);

/* input.c */
void input_init(struct wx_server *server);
void input_apply_config(struct wx_server *server);
void input_add_keyboard(struct wx_server *server, struct wlr_keyboard *kb, bool is_virtual);
void input_add_pointer(struct wx_server *server, struct wlr_input_device *device);
void input_add_touch(struct wx_server *server, struct wlr_input_device *device);
void input_add_tablet(struct wx_server *server, struct wlr_input_device *device);
struct wx_node_tag *input_node_at(struct wx_server *server, double lx, double ly,
	struct wlr_surface **surface, double *sx, double *sy);
void input_begin_interactive(struct wx_view *view, enum wx_cursor_mode mode, uint32_t edges);
void input_end_interactive(struct wx_server *server);
void input_process_motion(struct wx_server *server, uint32_t time);
void input_pointer_button(struct wx_server *server, uint32_t time, uint32_t button, bool pressed);
void input_pointer_axis(struct wx_server *server, uint32_t time, bool horizontal, double delta);
uint32_t input_now(void);

/* seat.c */
void seat_init(struct wx_server *server);
void seat_focus_view(struct wx_server *server, struct wx_view *view);
void seat_focus_surface(struct wx_server *server, struct wlr_surface *surface);
void seat_focus_layer(struct wx_server *server, struct wx_layer_surface *layer);
void seat_refocus(struct wx_server *server);
bool seat_shortcuts_inhibited(struct wx_server *server);

/* ime.c */
void ime_init(struct wx_server *server);
bool ime_handle_key(struct wx_server *server, struct wx_keyboard *kb, uint32_t time,
	uint32_t keycode, uint32_t state);
bool ime_handle_modifiers(struct wx_server *server, struct wx_keyboard *kb);

/* xdg.c */
void xdg_init(struct wx_server *server);

/* xwayland.c */
void xwayland_init(struct wx_server *server);

/* layer.c */
void layer_init(struct wx_server *server);
void layers_arrange(struct wx_output *output);
void layers_update_xwayland_workareas(struct wx_server *server);

/* lock.c */
void lock_init(struct wx_server *server);

/* view.c */
struct wx_view *view_create(struct wx_server *server, enum wx_view_type type);
void view_destroy(struct wx_view *view);
void view_map(struct wx_view *view);
void view_unmap(struct wx_view *view);
const char *view_app_id(struct wx_view *view);
const char *view_title(struct wx_view *view);
int view_pid(struct wx_view *view);
struct wlr_surface *view_surface(struct wx_view *view);
struct wx_view *view_by_id(struct wx_server *server, uint32_t id);
struct wx_view *view_at(struct wx_server *server, double lx, double ly,
	struct wlr_surface **surface, double *sx, double *sy);
struct wx_view *view_from_surface(struct wx_server *server, struct wlr_surface *surface);
struct wx_view *view_from_xdg_toplevel(struct wlr_xdg_toplevel *toplevel);
void view_move(struct wx_view *view, int x, int y);
void view_resize(struct wx_view *view, int w, int h);
void view_set_box(struct wx_view *view, int x, int y, int w, int h);
void view_close(struct wx_view *view);
void view_kill(struct wx_view *view);
void view_set_maximized(struct wx_view *view, bool on);
void view_set_fullscreen(struct wx_view *view, bool on);
void view_set_minimized(struct wx_view *view, bool on);
void view_set_stack(struct wx_view *view, enum wx_stack stack);
void view_set_sticky(struct wx_view *view, bool on);
void view_set_floating(struct wx_view *view, bool on);
void view_set_workspace(struct wx_view *view, int ws);
void view_set_activated(struct wx_view *view, bool on);
void view_raise(struct wx_view *view);
void view_update_visibility(struct wx_view *view);
bool view_is_visible(struct wx_view *view);
struct wx_output *view_output(struct wx_view *view);
void view_update_output(struct wx_view *view);
bool view_is_tiled(struct wx_view *view);
void view_frame_extents(struct wx_view *view, int *left, int *right, int *top, int *bottom);
void view_frame_box(struct wx_view *view, struct wlr_box *box);
void view_title_changed(struct wx_view *view);
void view_ftl_update(struct wx_view *view);
void view_cycle_focus(struct wx_server *server, bool forward);

/* deco.c */
#define WX_DECO_NONE   0
#define WX_DECO_TITLE  1
#define WX_DECO_CLOSE  2
#define WX_DECO_MAX    3
#define WX_DECO_MIN    4
#define WX_DECO_BORDER 5
void deco_create(struct wx_view *view);
void deco_update(struct wx_view *view);
void deco_destroy(struct wx_view *view);
/* hit-test in coords relative to content origin; sets resize edges for border */
int deco_hit(struct wx_view *view, double vx, double vy, uint32_t *edges);

/* layout.c */
void layout_master_stack(struct wlr_box area, int n, double mfact, int gap,
	struct wlr_box *out);
void layout_arrange(struct wx_server *server);

/* workspace.c */
void workspace_init(struct wx_server *server);
void workspace_output_init(struct wx_output *output);
void workspace_output_finish(struct wx_output *output);
struct wx_output *workspace_active_output(struct wx_server *server);
void workspace_switch(struct wx_server *server, struct wx_output *output, int ws);

/* rules.c */
void rules_apply(struct wx_view *view);

/* ipc.c */
bool ipc_init(struct wx_server *server);
void ipc_finish(struct wx_server *server);
void ipc_broadcast_event(struct wx_server *server, const char *json_line);
void ipc_event_window(struct wx_server *server, const char *event, struct wx_view *view);
bool ipc_binding_triggered(struct wx_server *server, struct wx_bind *bind);
void ipc_client_reply(struct wx_ipc_client *client, const char *text);
void ipc_client_unref_pending(struct wx_ipc_client *client);

/* ipc_cmd.c */
struct wx_buf;
/* Runs one command line; returns false if the reply is deferred (async). */
bool ipc_cmd_run(struct wx_server *server, struct wx_ipc_client *client,
	const char *line, struct wx_buf *out);
bool keybind_parse(struct wx_server *server, const char *combo, uint32_t *mods,
	xkb_keysym_t *sym);
void binds_reload(struct wx_server *server);
bool binds_handle(struct wx_server *server, uint32_t mods, xkb_keysym_t sym);

/* inject.c */
void inject_init(struct wx_server *server);
void inject_finish(struct wx_server *server);
bool inject_type(struct wx_server *server, const char *utf8, char **err);
bool inject_key_combo(struct wx_server *server, const char *combo, int press_mode, char **err);
void inject_mouse_move(struct wx_server *server, double x, double y, bool relative);

/* capture.c */
bool capture_start(struct wx_server *server, struct wx_ipc_client *client,
	const char *path, struct wlr_box box, char **err);
void capture_output_commit(struct wx_output *output, void *event);
void capture_output_destroy(struct wx_output *output);
void capture_client_gone(struct wx_server *server, struct wx_ipc_client *client);

/* wxproto.c */
void wxproto_init(struct wx_server *server);
void wxproto_view_mapped(struct wx_view *view);
void wxproto_view_moved(struct wx_view *view);
void wxproto_view_destroyed(struct wx_view *view);

/* protocols.c */
void protocols_init(struct wx_server *server);
void protocols_view_map(struct wx_view *view);
void protocols_view_unmap(struct wx_view *view);
void protocols_view_output_changed(struct wx_view *view, struct wx_output *old);
void protocols_update_idle_inhibit(struct wx_server *server);
bool protocols_view_wants_tearing(struct wx_view *view);
void constraint_check_focus(struct wx_server *server);

#endif
