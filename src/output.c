#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_ext_workspace_v1.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_output_management_v1.h>
#include <wlr/types/wlr_output_power_management_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/util/log.h>
#include <wlr/xwayland.h>
#include "server.h"

static const struct wx_output_conf *find_conf(struct wx_server *server, const char *name) {
	const struct wx_output_conf *wild = NULL;
	for (size_t i = 0; i < server->config.outputs_len; i++) {
		const struct wx_output_conf *o = &server->config.outputs[i];
		if (!strcmp(o->name, name)) {
			return o;
		}
		if (!strcmp(o->name, "*")) {
			wild = o;
		}
	}
	return wild;
}

static void state_from_conf(struct wlr_output *wlr_output, const struct wx_output_conf *oc,
		struct wlr_output_state *state) {
	wlr_output_state_set_enabled(state, !oc || oc->enabled != 0);
	struct wlr_output_mode *mode = NULL;
	if (oc && oc->width > 0) {
		struct wlr_output_mode *m;
		int best = -1;
		wl_list_for_each(m, &wlr_output->modes, link) {
			if (m->width != oc->width || m->height != oc->height) {
				continue;
			}
			int diff = oc->refresh > 0 ? abs(m->refresh - (int)lround(oc->refresh * 1000)) : 0;
			if (best < 0 || diff < best) {
				best = diff;
				mode = m;
			}
		}
		if (!mode) {
			wlr_output_state_set_custom_mode(state, oc->width, oc->height,
				(int)lround(oc->refresh * 1000));
		}
	} else {
		mode = wlr_output_preferred_mode(wlr_output);
	}
	if (mode) {
		wlr_output_state_set_mode(state, mode);
	}
	if (oc && oc->scale > 0) {
		wlr_output_state_set_scale(state, (float)oc->scale);
	}
	if (oc && oc->transform >= 0) {
		wlr_output_state_set_transform(state, (enum wl_output_transform)oc->transform);
	}
	if (oc && oc->adaptive_sync >= 0) {
		wlr_output_state_set_adaptive_sync_enabled(state, oc->adaptive_sync);
	}
}

static void place_in_layout(struct wx_output *output, const struct wx_output_conf *oc) {
	struct wx_server *server = output->server;
	struct wlr_output_layout_output *lo;
	if (oc && oc->x != WX_UNSET && oc->y != WX_UNSET) {
		lo = wlr_output_layout_add(server->output_layout, output->wlr_output, oc->x, oc->y);
	} else {
		lo = wlr_output_layout_add_auto(server->output_layout, output->wlr_output);
	}
	if (!output->scene_output) {
		output->scene_output = wlr_scene_output_create(server->scene, output->wlr_output);
		wlr_scene_output_layout_add_output(server->scene_layout, lo, output->scene_output);
	}
}

void output_apply_config(struct wx_output *output) {
	struct wlr_output *wlr_output = output->wlr_output;
	const struct wx_output_conf *oc = find_conf(output->server, wlr_output->name);
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	state_from_conf(wlr_output, oc, &state);
	if (!wlr_output_commit_state(wlr_output, &state)) {
		/* VRR and custom modes are the usual culprits; retry conservatively. */
		wlr_log(WLR_ERROR, "output %s: config rejected, retrying with preferred mode",
			wlr_output->name);
		wlr_output_state_finish(&state);
		wlr_output_state_init(&state);
		wlr_output_state_set_enabled(&state, true);
		struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
		if (mode) {
			wlr_output_state_set_mode(&state, mode);
		}
		wlr_output_commit_state(wlr_output, &state);
	}
	wlr_output_state_finish(&state);
	if (wlr_output->enabled) {
		place_in_layout(output, oc);
	} else {
		wlr_output_layout_remove(output->server->output_layout, wlr_output);
	}
	wlr_xcursor_manager_load(output->server->cursor_mgr, wlr_output->scale);
}

void output_set_enabled(struct wx_output *output, bool enabled) {
	if (enabled) {
		output_apply_config(output);
		return;
	}
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, false);
	if (wlr_output_commit_state(output->wlr_output, &state)) {
		wlr_output_layout_remove(output->server->output_layout, output->wlr_output);
	}
	wlr_output_state_finish(&state);
}

void outputs_apply_config(struct wx_server *server) {
	struct wx_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		if (output->lid_disabled) {
			continue; /* stays off until the lid opens */
		}
		output_apply_config(output);
		output_update_background(output);
	}
}

static struct wx_view *fullscreen_view_on(struct wx_output *output) {
	struct wx_view *view;
	wl_list_for_each(view, &output->server->views, link) {
		if (view->fullscreen && view_is_visible(view) && output_for_view(view) == output) {
			return view;
		}
	}
	return NULL;
}

static void output_frame(struct wl_listener *listener, void *data) {
	struct wx_output *output = wl_container_of(listener, output, frame);
	struct wlr_scene_output *so = output->scene_output;
	if (!so) {
		return;
	}
	const struct wx_output_conf *oc = find_conf(output->server, output->wlr_output->name);
	struct wx_view *fs = fullscreen_view_on(output);
	bool tearing = fs && (!oc || oc->allow_tearing != 0) && protocols_view_wants_tearing(fs);

	if (tearing && wlr_scene_output_needs_frame(so)) {
		struct wlr_output_state state;
		wlr_output_state_init(&state);
		if (wlr_scene_output_build_state(so, &state, NULL)) {
			state.tearing_page_flip = true;
			if (!wlr_output_test_state(output->wlr_output, &state)) {
				state.tearing_page_flip = false;
			}
			wlr_output_commit_state(output->wlr_output, &state);
		}
		wlr_output_state_finish(&state);
	} else {
		wlr_scene_output_commit(so, NULL);
	}

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(so, &now);
}

static void output_request_state(struct wl_listener *listener, void *data) {
	struct wx_output *output = wl_container_of(listener, output, request_state);
	const struct wlr_output_event_request_state *event = data;
	wlr_output_commit_state(output->wlr_output, event->state);
}

static void output_commit(struct wl_listener *listener, void *data) {
	struct wx_output *output = wl_container_of(listener, output, commit);
	capture_output_commit(output, data);
}

static void output_destroy(struct wl_listener *listener, void *data) {
	struct wx_output *output = wl_container_of(listener, output, destroy);
	struct wx_server *server = output->server;
	capture_output_destroy(output);
	workspace_output_finish(output);
	if (server->active_output == output) {
		server->active_output = NULL;
	}
	struct wx_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (view->output == output) {
			view->output = NULL;
			view->orphaned = true;
		}
	}
	struct wx_layer_surface *layer, *tmp;
	wl_list_for_each_safe(layer, tmp, &server->layers, link) {
		if (layer->output == output) {
			layer->output = NULL;
			wlr_layer_surface_v1_destroy(layer->layer_surface);
		}
	}
	if (output->lock_bg) {
		wlr_scene_node_destroy(&output->lock_bg->node);
	}
	if (output->background) {
		wlr_scene_node_destroy(&output->background->node);
	}
	wl_list_remove(&output->frame.link);
	wl_list_remove(&output->request_state.link);
	wl_list_remove(&output->commit.link);
	wl_list_remove(&output->destroy.link);
	wl_list_remove(&output->link);
	free(output);
}

static void new_output(struct wl_listener *listener, void *data) {
	struct wx_server *server = wl_container_of(listener, server, new_output);
	struct wlr_output *wlr_output = data;
	if (!wlr_output_init_render(wlr_output, server->allocator, server->renderer)) {
		wlr_log(WLR_ERROR, "output %s: cannot init rendering", wlr_output->name);
		return;
	}
	struct wx_output *output = calloc(1, sizeof(*output));
	output->server = server;
	output->wlr_output = wlr_output;
	wlr_output->data = output;
	wl_list_init(&output->captures);
	output->frame.notify = output_frame;
	wl_signal_add(&wlr_output->events.frame, &output->frame);
	output->request_state.notify = output_request_state;
	wl_signal_add(&wlr_output->events.request_state, &output->request_state);
	output->commit.notify = output_commit;
	wl_signal_add(&wlr_output->events.commit, &output->commit);
	output->destroy.notify = output_destroy;
	wl_signal_add(&wlr_output->events.destroy, &output->destroy);
	wl_list_insert(server->outputs.prev, &output->link);

	workspace_output_init(output);
	output_apply_config(output);
	if (server->locked) {
		float black[4] = {0, 0, 0, 1};
		output->lock_bg = wlr_scene_rect_create(server->lock_tree, 0, 0, black);
	}
	wlr_log(WLR_INFO, "output %s added (%s %s), scale %.2f", wlr_output->name,
		wlr_output->make ? wlr_output->make : "?", wlr_output->model ? wlr_output->model : "?",
		wlr_output->scale);
}

void output_update_manager(struct wx_server *server) {
	if (!server->output_mgr) {
		return;
	}
	struct wlr_output_configuration_v1 *config = wlr_output_configuration_v1_create();
	struct wx_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		struct wlr_output_configuration_head_v1 *head =
			wlr_output_configuration_head_v1_create(config, output->wlr_output);
		struct wlr_box box;
		wlr_output_layout_get_box(server->output_layout, output->wlr_output, &box);
		head->state.enabled = output->wlr_output->enabled && !wlr_box_empty(&box);
		head->state.x = box.x;
		head->state.y = box.y;
	}
	wlr_output_manager_v1_set_configuration(server->output_mgr, config);
}

void output_update_background(struct wx_output *output) {
	uint32_t c = output->server->config.color_background;
	float color[4] = {((c >> 24) & 0xff) / 255.0f, ((c >> 16) & 0xff) / 255.0f,
		((c >> 8) & 0xff) / 255.0f, (c & 0xff) / 255.0f};
	if (!output->background) {
		output->background = wlr_scene_rect_create(output->server->desktop_bg, 0, 0, color);
	}
	wlr_scene_rect_set_color(output->background, color);
	wlr_scene_node_set_position(&output->background->node, output->full.x, output->full.y);
	wlr_scene_rect_set_size(output->background, output->full.width, output->full.height);
}

static void layout_change(struct wl_listener *listener, void *data) {
	struct wx_server *server = wl_container_of(listener, server, layout_change);
	struct wx_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		wlr_output_layout_get_box(server->output_layout, output->wlr_output, &output->full);
		output_update_background(output);
		if (output->lock_bg) {
			wlr_scene_node_set_position(&output->lock_bg->node, output->full.x, output->full.y);
			wlr_scene_rect_set_size(output->lock_bg, output->full.width, output->full.height);
		}
		layers_arrange(output);
	}
	/* Rescue windows that ended up outside every output. */
	struct wx_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (!view->mapped) {
			continue;
		}
		double cx = view->x + view->w / 2.0, cy = view->y + view->h / 2.0;
		if (!wlr_output_layout_output_at(server->output_layout, cx, cy)) {
			double nx, ny;
			wlr_output_layout_closest_point(server->output_layout, NULL, cx, cy, &nx, &ny);
			view_move(view, (int)nx - view->w / 2, (int)ny - view->h / 2);
		} else if (view->orphaned) {
			view_update_output(view);
		}
	}
	output_update_manager(server);
	layout_arrange(server);
}

static void output_mgr_apply_or_test(struct wx_server *server,
		struct wlr_output_configuration_v1 *config, bool test_only) {
	size_t len = 0;
	struct wlr_backend_output_state *states = wlr_output_configuration_v1_build_state(config, &len);
	bool ok = states && wlr_backend_test(server->backend, states, len);
	if (ok && !test_only) {
		ok = wlr_backend_commit(server->backend, states, len);
		if (ok) {
			struct wlr_output_configuration_head_v1 *head;
			wl_list_for_each(head, &config->heads, link) {
				struct wx_output *output = head->state.output->data;
				if (!output) {
					continue;
				}
				if (head->state.enabled) {
					struct wlr_output_layout_output *lo = wlr_output_layout_add(
						server->output_layout, head->state.output, head->state.x, head->state.y);
					if (!output->scene_output) {
						output->scene_output = wlr_scene_output_create(server->scene,
							head->state.output);
						wlr_scene_output_layout_add_output(server->scene_layout, lo,
							output->scene_output);
					}
				} else {
					wlr_output_layout_remove(server->output_layout, head->state.output);
				}
			}
		}
	}
	free(states);
	if (ok) {
		wlr_output_configuration_v1_send_succeeded(config);
	} else {
		wlr_output_configuration_v1_send_failed(config);
	}
	wlr_output_configuration_v1_destroy(config);
	if (!test_only) {
		output_update_manager(server);
	}
}

static void output_mgr_apply(struct wl_listener *listener, void *data) {
	struct wx_server *server = wl_container_of(listener, server, output_mgr_apply);
	output_mgr_apply_or_test(server, data, false);
}

static void output_mgr_test(struct wl_listener *listener, void *data) {
	struct wx_server *server = wl_container_of(listener, server, output_mgr_test);
	output_mgr_apply_or_test(server, data, true);
}

static void output_power_set_mode(struct wl_listener *listener, void *data) {
	struct wlr_output_power_v1_set_mode_event *event = data;
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, event->mode == ZWLR_OUTPUT_POWER_V1_MODE_ON);
	wlr_output_commit_state(event->output, &state);
	wlr_output_state_finish(&state);
}

void output_init(struct wx_server *server) {
	wl_list_init(&server->outputs);
	server->new_output.notify = new_output;
	wl_signal_add(&server->backend->events.new_output, &server->new_output);
	server->layout_change.notify = layout_change;
	wl_signal_add(&server->output_layout->events.change, &server->layout_change);

	server->output_mgr = wlr_output_manager_v1_create(server->display);
	server->output_mgr_apply.notify = output_mgr_apply;
	wl_signal_add(&server->output_mgr->events.apply, &server->output_mgr_apply);
	server->output_mgr_test.notify = output_mgr_test;
	wl_signal_add(&server->output_mgr->events.test, &server->output_mgr_test);

	struct wlr_output_power_manager_v1 *power = wlr_output_power_manager_v1_create(server->display);
	server->output_power_set_mode.notify = output_power_set_mode;
	wl_signal_add(&power->events.set_mode, &server->output_power_set_mode);
}

struct wx_output *output_at(struct wx_server *server, double lx, double ly) {
	struct wlr_output *o = wlr_output_layout_output_at(server->output_layout, lx, ly);
	return o ? o->data : NULL;
}

struct wx_output *output_for_view(struct wx_view *view) {
	struct wx_server *server = view->server;
	struct wx_output *o = output_at(server, view->x + view->w / 2.0, view->y + view->h / 2.0);
	if (!o) {
		double nx, ny;
		wlr_output_layout_closest_point(server->output_layout, NULL,
			view->x + view->w / 2.0, view->y + view->h / 2.0, &nx, &ny);
		o = output_at(server, nx, ny);
	}
	return o;
}

struct wx_output *output_focused(struct wx_server *server) {
	struct wx_output *o = output_at(server, server->cursor->x, server->cursor->y);
	if (!o && !wl_list_empty(&server->outputs)) {
		o = wl_container_of(server->outputs.next, o, link);
	}
	return o;
}

struct wx_output *output_by_name(struct wx_server *server, const char *name) {
	struct wx_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		if (!strcmp(output->wlr_output->name, name)) {
			return output;
		}
	}
	return NULL;
}
