#include <stdlib.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_session_lock_v1.h>
#include <wlr/util/log.h>
#include "server.h"

/* ext-session-lock-v1 (swaylock & co). A crashed locker keeps the screen locked. */

struct wx_lock_surface {
	struct wx_server *server;
	struct wlr_session_lock_surface_v1 *ls;
	struct wlr_scene_tree *tree;
	struct wl_listener map;
	struct wl_listener destroy;
};

static void lock_surface_map(struct wl_listener *listener, void *data) {
	struct wx_lock_surface *s = wl_container_of(listener, s, map);
	seat_focus_surface(s->server, s->ls->surface);
}

static void lock_surface_destroy(struct wl_listener *listener, void *data) {
	struct wx_lock_surface *s = wl_container_of(listener, s, destroy);
	struct wx_server *server = s->server;
	wl_list_remove(&s->map.link);
	wl_list_remove(&s->destroy.link);
	wlr_scene_node_destroy(&s->tree->node);
	free(s);
	/* hand the keyboard to another lock surface if there is one */
	if (server->lock) {
		struct wlr_session_lock_surface_v1 *other;
		wl_list_for_each(other, &server->lock->surfaces, link) {
			if (other->surface->mapped) {
				seat_focus_surface(server, other->surface);
				break;
			}
		}
	}
}

static void lock_new_surface(struct wl_listener *listener, void *data) {
	struct wx_server *server = wl_container_of(listener, server, lock_new_surface);
	struct wlr_session_lock_surface_v1 *ls = data;
	struct wx_output *o = ls->output->data;
	struct wx_lock_surface *s = calloc(1, sizeof(*s));
	s->server = server;
	s->ls = ls;
	s->tree = wlr_scene_subsurface_tree_create(server->lock_tree, ls->surface);
	if (o) {
		wlr_scene_node_set_position(&s->tree->node, o->full.x, o->full.y);
		wlr_session_lock_surface_v1_configure(ls, (uint32_t)o->full.width,
			(uint32_t)o->full.height);
	}
	s->map.notify = lock_surface_map;
	wl_signal_add(&ls->surface->events.map, &s->map);
	s->destroy.notify = lock_surface_destroy;
	wl_signal_add(&ls->events.destroy, &s->destroy);
}

static void set_lock_backgrounds(struct wx_server *server, bool on) {
	struct wx_output *o;
	wl_list_for_each(o, &server->outputs, link) {
		if (on && !o->lock_bg) {
			float black[4] = {0, 0, 0, 1};
			o->lock_bg = wlr_scene_rect_create(server->lock_tree, o->full.width, o->full.height,
				black);
			wlr_scene_node_set_position(&o->lock_bg->node, o->full.x, o->full.y);
			wlr_scene_node_lower_to_bottom(&o->lock_bg->node);
		} else if (!on && o->lock_bg) {
			wlr_scene_node_destroy(&o->lock_bg->node);
			o->lock_bg = NULL;
		}
	}
}

static void lock_detach(struct wx_server *server) {
	wl_list_remove(&server->lock_new_surface.link);
	wl_list_remove(&server->lock_unlock.link);
	wl_list_remove(&server->lock_destroy.link);
	wl_list_init(&server->lock_new_surface.link);
	wl_list_init(&server->lock_unlock.link);
	wl_list_init(&server->lock_destroy.link);
	server->lock = NULL;
}

static void lock_unlock(struct wl_listener *listener, void *data) {
	struct wx_server *server = wl_container_of(listener, server, lock_unlock);
	lock_detach(server);
	server->locked = false;
	set_lock_backgrounds(server, false);
	wlr_log(WLR_INFO, "session unlocked");
	seat_refocus(server);
}

static void lock_destroy(struct wl_listener *listener, void *data) {
	struct wx_server *server = wl_container_of(listener, server, lock_destroy);
	lock_detach(server);
	if (server->locked) {
		wlr_log(WLR_ERROR, "screen locker died without unlocking; staying locked");
	}
}

static void new_lock(struct wl_listener *listener, void *data) {
	struct wx_server *server = wl_container_of(listener, server, new_lock);
	struct wlr_session_lock_v1 *lock = data;
	if (server->lock) {
		wlr_session_lock_v1_destroy(lock);
		return;
	}
	server->lock = lock;
	server->locked = true;
	set_lock_backgrounds(server, true);
	if (server->cursor_mode != WX_CURSOR_PASSTHROUGH) {
		input_end_interactive(server);
	}
	seat_focus_surface(server, NULL);
	wlr_seat_pointer_clear_focus(server->seat);
	server->lock_new_surface.notify = lock_new_surface;
	wl_signal_add(&lock->events.new_surface, &server->lock_new_surface);
	server->lock_unlock.notify = lock_unlock;
	wl_signal_add(&lock->events.unlock, &server->lock_unlock);
	server->lock_destroy.notify = lock_destroy;
	wl_signal_add(&lock->events.destroy, &server->lock_destroy);
	wlr_session_lock_v1_send_locked(lock);
	wlr_log(WLR_INFO, "session locked");
}

void lock_init(struct wx_server *server) {
	struct wlr_session_lock_manager_v1 *mgr = wlr_session_lock_manager_v1_create(server->display);
	server->new_lock.notify = new_lock;
	wl_signal_add(&mgr->events.new_lock, &server->new_lock);
}
