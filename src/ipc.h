#ifndef WX_IPC_H
#define WX_IPC_H

#include <stdbool.h>
#include <stddef.h>
#include <wayland-server-core.h>
#include "json.h"

struct wx_server;
struct wx_view;

struct wx_ipc_client {
	struct wl_list link;
	struct wx_server *server;
	int fd;
	struct wl_event_source *source;
	char *in;
	size_t in_len;
	struct wx_buf out;
	size_t out_off;
	bool subscribed;
	bool dead;
	bool eof;
	int pending; /* deferred replies outstanding (screenshots) */
};

void ipc_view_json(struct wx_buf *b, struct wx_view *view);

#endif
