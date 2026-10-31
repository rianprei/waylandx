#include <stdlib.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_alpha_modifier_v1.h>
#include <wlr/types/wlr_color_management_v1.h>
#include <wlr/types/wlr_color_representation_v1.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_content_type_v1.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_drm_lease_v1.h>
#include <wlr/types/wlr_export_dmabuf_v1.h>
#include <wlr/types/wlr_ext_data_control_v1.h>
#include <wlr/types/wlr_ext_foreign_toplevel_list_v1.h>
#include <wlr/types/wlr_ext_image_capture_source_v1.h>
#include <wlr/types/wlr_ext_image_copy_capture_v1.h>
#include <wlr/types/wlr_fixes.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_gamma_control_v1.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_keyboard_shortcuts_inhibit_v1.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/types/wlr_linux_drm_syncobj_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_security_context_v1.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_tearing_control_v1.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_virtual_pointer_v1.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_xdg_dialog_v1.h>
#include <wlr/types/wlr_xdg_foreign_registry.h>
#include <wlr/types/wlr_xdg_foreign_v1.h>
#include <wlr/types/wlr_xdg_foreign_v2.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_xdg_system_bell_v1.h>
#include <wlr/types/wlr_xdg_toplevel_icon_v1.h>
#include <wlr/types/wlr_xdg_toplevel_tag_v1.h>
#include <wlr/util/log.h>
#include <wlr/xwayland.h>
#include "server.h"

/* ---------- foreign toplevel (taskbars, panels, pagers) ---------- */

static void ftl_activate(struct wl_listener *listener, void *data) {
	struct wx_view *view = wl_container_of(listener, view, ftl_activate);
	seat_focus_view(view->server, view);
}

static void ftl_close(struct wl_listener *listener, void *data) {
	struct wx_view *view = wl_container_of(listener, view, ftl_close);
	view_close(view);
}

static void ftl_maximize(struct wl_listener *listener, void *data) {
	struct wx_view *view = wl_container_of(listener, view, ftl_maximize);
	struct wlr_foreign_toplevel_handle_v1_maximized_event *e = data;
	view_set_maximized(view, e->maximized);
}

static void ftl_minimize(struct wl_listener *listener, void *data) {
	struct wx_view *view = wl_container_of(listener, view, ftl_minimize);
	struct wlr_foreign_toplevel_handle_v1_minimized_event *e = data;
	view_set_minimized(view, e->minimized);
	if (!e->minimized) {
		seat_focus_view(view->server, view);
	}
}

static void ftl_fullscreen(struct wl_listener *listener, void *data) {
	struct wx_view *view = wl_container_of(listener, view, ftl_fullscreen);
	struct wlr_foreign_toplevel_handle_v1_fullscreen_event *e = data;
	view_set_fullscreen(view, e->fullscreen);
}

static void ftl_handle_destroy(struct wl_listener *listener, void *data) {
	struct wx_view *view = wl_container_of(listener, view, ftl_destroy);
	wl_list_remove(&view->ftl_activate.link);
	wl_list_remove(&view->ftl_close.link);
	wl_list_remove(&view->ftl_maximize.link);
	wl_list_remove(&view->ftl_minimize.link);
	wl_list_remove(&view->ftl_fullscreen.link);
	wl_list_remove(&view->ftl_destroy.link);
	view->ftl = NULL;
}

void view_ftl_update(struct wx_view *view) {
	if (view->ftl) {
		wlr_foreign_toplevel_handle_v1_set_title(view->ftl, view_title(view));
		wlr_foreign_toplevel_handle_v1_set_app_id(view->ftl, view_app_id(view));
		wlr_foreign_toplevel_handle_v1_set_activated(view->ftl, view->server->focused == view);
		wlr_foreign_toplevel_handle_v1_set_maximized(view->ftl, view->maximized);
		wlr_foreign_toplevel_handle_v1_set_minimized(view->ftl, view->minimized);
		wlr_foreign_toplevel_handle_v1_set_fullscreen(view->ftl, view->fullscreen);
	}
	if (view->ext_ftl) {
		struct wlr_ext_foreign_toplevel_handle_v1_state st = {
			.title = view_title(view), .app_id = view_app_id(view),
		};
		wlr_ext_foreign_toplevel_handle_v1_update_state(view->ext_ftl, &st);
	}
}

void protocols_view_map(struct wx_view *view) {
	struct wx_server *server = view->server;
	if (!view->skip_taskbar && !(view->type == WX_VIEW_XWAYLAND && view->xsurface->skip_taskbar)) {
		view->ftl = wlr_foreign_toplevel_handle_v1_create(server->ftl_mgr);
		view->ftl->data = view;
#define FTL(sig, field, fn) view->field.notify = fn; wl_signal_add(&view->ftl->events.sig, &view->field)
		FTL(request_activate, ftl_activate, ftl_activate);
		FTL(request_close, ftl_close, ftl_close);
		FTL(request_maximize, ftl_maximize, ftl_maximize);
		FTL(request_minimize, ftl_minimize, ftl_minimize);
		FTL(request_fullscreen, ftl_fullscreen, ftl_fullscreen);
		FTL(destroy, ftl_destroy, ftl_handle_destroy);
#undef FTL
		struct wx_output *o = view_output(view);
		if (o) {
			wlr_foreign_toplevel_handle_v1_output_enter(view->ftl, o->wlr_output);
		}
	}
	struct wlr_ext_foreign_toplevel_handle_v1_state st = {
		.title = view_title(view), .app_id = view_app_id(view),
	};
	view->ext_ftl = wlr_ext_foreign_toplevel_handle_v1_create(server->ext_ftl_list, &st);
	if (view->ext_ftl) {
		view->ext_ftl->data = view;
	}
	view_ftl_update(view);
}

void protocols_view_output_changed(struct wx_view *view, struct wx_output *old) {
	if (!view->ftl) {
		return;
	}
	if (old) {
		wlr_foreign_toplevel_handle_v1_output_leave(view->ftl, old->wlr_output);
	}
	if (view->output) {
		wlr_foreign_toplevel_handle_v1_output_enter(view->ftl, view->output->wlr_output);
	}
}

void protocols_view_unmap(struct wx_view *view) {
	if (view->ftl) {
		wlr_foreign_toplevel_handle_v1_destroy(view->ftl); /* fires ftl_handle_destroy */
	}
	if (view->ext_ftl) {
		wlr_ext_foreign_toplevel_handle_v1_destroy(view->ext_ftl);
		view->ext_ftl = NULL;
	}
	/* capture_source stays: it follows view->tree, which lives until destroy */
}

/* ---------- capture of a single toplevel (ext-image-capture-source) ---------- */

static void toplevel_capture_request(struct wl_listener *listener, void *data) {
	struct wx_server *server = wl_container_of(listener, server, toplevel_capture_request);
	struct wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request *req = data;
	struct wx_view *view = req->toplevel_handle ? req->toplevel_handle->data : NULL;
	if (!view || !view->mapped) {
		return;
	}
	if (!view->capture_source) {
		view->capture_source = wlr_ext_image_capture_source_v1_create_with_scene_node(
			&view->tree->node, server->loop, server->allocator, server->renderer);
	}
	if (view->capture_source) {
		wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request_accept(req,
			view->capture_source);
	}
}

/* ---------- idle inhibit ---------- */

struct wx_idle_inhibitor {
	struct wx_server *server;
	struct wl_listener destroy;
};

void protocols_update_idle_inhibit(struct wx_server *server) {
	bool inhibited = false;
	struct wlr_idle_inhibitor_v1 *inh;
	wl_list_for_each(inh, &server->idle_inhibit->inhibitors, link) {
		struct wx_view *view = view_from_surface(server, inh->surface);
		/* inhibitors on hidden windows don't count (like sway's "visible") */
		if (!view || view_is_visible(view)) {
			inhibited = true;
			break;
		}
	}
	wlr_idle_notifier_v1_set_inhibited(server->idle_notifier, inhibited);
	idle_set_inhibited(server, inhibited);
}

static void idle_inhibit_recheck(void *data) {
	protocols_update_idle_inhibit(data);
}

static void idle_inhibitor_destroy(struct wl_listener *listener, void *data) {
	struct wx_idle_inhibitor *i = wl_container_of(listener, i, destroy);
	struct wx_server *server = i->server;
	wl_list_remove(&i->destroy.link);
	free(i);
	/* the inhibitor is still in the list during its destroy signal */
	wl_event_loop_add_idle(server->loop, idle_inhibit_recheck, server);
}

static void new_idle_inhibitor(struct wl_listener *listener, void *data) {
	struct wx_server *server = wl_container_of(listener, server, new_idle_inhibitor);
	struct wlr_idle_inhibitor_v1 *inh = data;
	struct wx_idle_inhibitor *i = calloc(1, sizeof(*i));
	i->server = server;
	i->destroy.notify = idle_inhibitor_destroy;
	wl_signal_add(&inh->events.destroy, &i->destroy);
	protocols_update_idle_inhibit(server);
}

/* ---------- pointer constraints (games, 3D apps) ---------- */

struct wx_constraint {
	struct wx_server *server;
	struct wlr_pointer_constraint_v1 *constraint;
	struct wl_listener destroy;
};

static void constraint_destroy(struct wl_listener *listener, void *data) {
	struct wx_constraint *c = wl_container_of(listener, c, destroy);
	if (c->server->active_constraint == c->constraint) {
		c->server->active_constraint = NULL;
	}
	wl_list_remove(&c->destroy.link);
	free(c);
}

void constraint_check_focus(struct wx_server *server) {
	struct wlr_surface *focused = server->seat->keyboard_state.focused_surface;
	struct wlr_pointer_constraint_v1 *want = focused ?
		wlr_pointer_constraints_v1_constraint_for_surface(server->pointer_constraints, focused,
			server->seat) : NULL;
	if (want == server->active_constraint) {
		return;
	}
	if (server->active_constraint) {
		wlr_pointer_constraint_v1_send_deactivated(server->active_constraint);
	}
	server->active_constraint = want;
	if (want) {
		wlr_pointer_constraint_v1_send_activated(want);
	}
}

static void new_constraint(struct wl_listener *listener, void *data) {
	struct wx_server *server = wl_container_of(listener, server, new_constraint);
	struct wlr_pointer_constraint_v1 *constraint = data;
	struct wx_constraint *c = calloc(1, sizeof(*c));
	c->server = server;
	c->constraint = constraint;
	c->destroy.notify = constraint_destroy;
	wl_signal_add(&constraint->events.destroy, &c->destroy);
	constraint_check_focus(server);
}

/* ---------- keyboard shortcuts inhibit (VMs, remote desktops) ---------- */

static void new_kb_inhibitor(struct wl_listener *listener, void *data) {
	struct wlr_keyboard_shortcuts_inhibitor_v1 *inh = data;
	/* open model: any client may grab the shortcuts while it is focused */
	wlr_keyboard_shortcuts_inhibitor_v1_activate(inh);
}

/* ---------- virtual input devices (wtype, ydotool-like, remote desktop) ---------- */

static void new_virtual_keyboard(struct wl_listener *listener, void *data) {
	struct wx_server *server = wl_container_of(listener, server, new_virtual_keyboard);
	struct wlr_virtual_keyboard_v1 *vk = data;
	input_add_keyboard(server, &vk->keyboard, true);
}

static void new_virtual_pointer(struct wl_listener *listener, void *data) {
	struct wx_server *server = wl_container_of(listener, server, new_virtual_pointer);
	struct wlr_virtual_pointer_v1_new_pointer_event *e = data;
	input_add_pointer(server, &e->new_pointer->pointer.base);
	if (e->suggested_output) {
		wlr_cursor_map_input_to_output(server->cursor, &e->new_pointer->pointer.base,
			e->suggested_output);
	}
}

/* ---------- activation: no focus-stealing prevention (X11 behaviour) ---------- */

static void activation_request(struct wl_listener *listener, void *data) {
	struct wx_server *server = wl_container_of(listener, server, activation_request);
	struct wlr_xdg_activation_v1_request_activate_event *e = data;
	struct wx_view *view = view_from_surface(server, e->surface);
	if (view && view->mapped) {
		seat_focus_view(server, view);
	}
}

/* ---------- tearing ---------- */

bool protocols_view_wants_tearing(struct wx_view *view) {
	if (view->allow_tearing) {
		return true;
	}
	struct wlr_surface *s = view_surface(view);
	return s && wlr_tearing_control_manager_v1_surface_hint_from_surface(view->server->tearing, s) ==
		WP_TEARING_CONTROL_V1_PRESENTATION_HINT_ASYNC;
}

/* ---------- DRM leasing (VR headsets) ---------- */

static void drm_lease_request(struct wl_listener *listener, void *data) {
	struct wlr_drm_lease_request_v1 *req = data;
	if (!wlr_drm_lease_request_v1_grant(req)) {
		wlr_drm_lease_request_v1_reject(req);
	}
}

void protocols_init(struct wx_server *server) {
	struct wl_display *d = server->display;

	/* buffers, sync and color: the Wayland side of the fusion */
	if (wlr_renderer_get_texture_formats(server->renderer, WLR_BUFFER_CAP_DMABUF)) {
		struct wlr_linux_dmabuf_v1 *dmabuf =
			wlr_linux_dmabuf_v1_create_with_renderer(d, 5, server->renderer);
		wlr_scene_set_linux_dmabuf_v1(server->scene, dmabuf);
	}
	if (server->renderer->features.timeline && server->backend->features.timeline) {
		int fd = wlr_renderer_get_drm_fd(server->renderer);
		if (fd >= 0) {
			wlr_linux_drm_syncobj_manager_v1_create(d, 1, fd);
		}
	}
	if (server->renderer->features.output_color_transform) {
		static const enum wp_color_manager_v1_render_intent intents[] = {
			WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL,
		};
		/* no TRANSFER_FUNCTION_SRGB: deprecated, and wlroots asserts on it */
		static const enum wp_color_manager_v1_transfer_function tfs[] = {
			WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ,
			WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_LINEAR,
			WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22,
			WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_BT1886,
		};
		static const enum wp_color_manager_v1_primaries prims[] = {
			WP_COLOR_MANAGER_V1_PRIMARIES_SRGB,
			WP_COLOR_MANAGER_V1_PRIMARIES_BT2020,
		};
		struct wlr_color_manager_v1_options opts = {
			.features = {.parametric = true, .set_mastering_display_primaries = true},
			.render_intents = intents, .render_intents_len = 1,
			.transfer_functions = tfs, .transfer_functions_len = sizeof(tfs) / sizeof(tfs[0]),
			.primaries = prims, .primaries_len = 2,
		};
		wlr_color_manager_v1_create(d, 1, &opts);
	}
	wlr_color_representation_manager_v1_create_with_renderer(d, 1, server->renderer);
	wlr_presentation_create(d, server->backend, 2);
	wlr_viewporter_create(d);
	wlr_single_pixel_buffer_manager_v1_create(d);
	wlr_fractional_scale_manager_v1_create(d, 1);
	wlr_alpha_modifier_v1_create(d);
	wlr_content_type_manager_v1_create(d, 1);
	wlr_fixes_create(d, 1);
	server->tearing = wlr_tearing_control_manager_v1_create(d, 1);

	/* shell helpers */
	xwlscale_init(server);
	server->activation = wlr_xdg_activation_v1_create(d);
	server->activation_request.notify = activation_request;
	wl_signal_add(&server->activation->events.request_activate, &server->activation_request);
	wlr_xdg_wm_dialog_v1_create(d, 1);
	wlr_xdg_system_bell_v1_create(d, 1);
	wlr_xdg_toplevel_icon_manager_v1_create(d, 1);
	wlr_xdg_toplevel_tag_manager_v1_create(d, 1);
	struct wlr_xdg_foreign_registry *reg = wlr_xdg_foreign_registry_create(d);
	wlr_xdg_foreign_v1_create(d, reg);
	wlr_xdg_foreign_v2_create(d, reg);
	wlr_security_context_manager_v1_create(d);

	/* clipboard managers */
	wlr_data_control_manager_v1_create(d);
	wlr_ext_data_control_manager_v1_create(d, 1);

	/* the X11 side of the fusion: open capture, open input, open window lists */
	wlr_screencopy_manager_v1_create(d);
	wlr_export_dmabuf_manager_v1_create(d);
	wlr_ext_image_copy_capture_manager_v1_create(d, 1);
	wlr_ext_output_image_capture_source_manager_v1_create(d, 1);
	struct wlr_ext_foreign_toplevel_image_capture_source_manager_v1 *tcap =
		wlr_ext_foreign_toplevel_image_capture_source_manager_v1_create(d, 1);
	server->toplevel_capture_request.notify = toplevel_capture_request;
	wl_signal_add(&tcap->events.new_request, &server->toplevel_capture_request);
	server->gamma_mgr = wlr_gamma_control_manager_v1_create(d);
	wlr_scene_set_gamma_control_manager_v1(server->scene, server->gamma_mgr);
	server->ftl_mgr = wlr_foreign_toplevel_manager_v1_create(d);
	server->ext_ftl_list = wlr_ext_foreign_toplevel_list_v1_create(d, 1);

	struct wlr_virtual_keyboard_manager_v1 *vkm = wlr_virtual_keyboard_manager_v1_create(d);
	server->new_virtual_keyboard.notify = new_virtual_keyboard;
	wl_signal_add(&vkm->events.new_virtual_keyboard, &server->new_virtual_keyboard);
	struct wlr_virtual_pointer_manager_v1 *vpm = wlr_virtual_pointer_manager_v1_create(d);
	server->new_virtual_pointer.notify = new_virtual_pointer;
	wl_signal_add(&vpm->events.new_virtual_pointer, &server->new_virtual_pointer);

	server->kb_inhibit = wlr_keyboard_shortcuts_inhibit_v1_create(d);
	server->new_kb_inhibitor.notify = new_kb_inhibitor;
	wl_signal_add(&server->kb_inhibit->events.new_inhibitor, &server->new_kb_inhibitor);

	server->pointer_constraints = wlr_pointer_constraints_v1_create(d);
	server->new_constraint.notify = new_constraint;
	wl_signal_add(&server->pointer_constraints->events.new_constraint, &server->new_constraint);

	server->idle_notifier = wlr_idle_notifier_v1_create(d);
	server->idle_inhibit = wlr_idle_inhibit_v1_create(d);
	server->new_idle_inhibitor.notify = new_idle_inhibitor;
	wl_signal_add(&server->idle_inhibit->events.new_inhibitor, &server->new_idle_inhibitor);

	struct wlr_drm_lease_v1_manager *lease = wlr_drm_lease_v1_manager_create(d, server->backend);
	if (lease) {
		server->drm_lease_request.notify = drm_lease_request;
		wl_signal_add(&lease->events.request, &server->drm_lease_request);
	}
}
