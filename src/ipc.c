#define _GNU_SOURCE /* accept4 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <wlr/util/log.h>
#include "ipc.h"
#include "json.h"
#include "server.h"

#define IPC_MAX_LINE (1 << 16)
#define IPC_MAX_PENDING_OUT (8 << 20)

static void client_destroy(struct wx_ipc_client *c) {
	struct wx_server *server = c->server;
	struct wx_bind *b, *tmp;
	wl_list_for_each_safe(b, tmp, &server->binds, link) {
		if (b->owner == c) {
			wl_list_remove(&b->link);
			free(b->combo);
			free(b);
		}
	}
	capture_client_gone(server, c);
	wl_event_source_remove(c->source);
	close(c->fd);
	wl_list_remove(&c->link);
	buf_free(&c->out);
	free(c->in);
	free(c);
}

static bool client_flush(struct wx_ipc_client *c) {
	while (c->out_off < c->out.len) {
		ssize_t n = write(c->fd, c->out.data + c->out_off, c->out.len - c->out_off);
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				wl_event_source_fd_update(c->source, WL_EVENT_READABLE | WL_EVENT_WRITABLE);
				return true;
			}
			if (errno == EINTR) {
				continue;
			}
			return false;
		}
		c->out_off += (size_t)n;
	}
	buf_reset(&c->out);
	c->out_off = 0;
	wl_event_source_fd_update(c->source, WL_EVENT_READABLE);
	return true;
}

static void client_send(struct wx_ipc_client *c, const char *text) {
	if (c->out.len - c->out_off > IPC_MAX_PENDING_OUT) {
		c->dead = true; /* reader stopped consuming */
		return;
	}
	buf_puts(&c->out, text);
	size_t n = strlen(text);
	if (n == 0 || text[n - 1] != '\n') {
		buf_puts(&c->out, "\n");
	}
	buf_puts(&c->out, "\n"); /* blank line ends every message */
	if (!client_flush(c)) {
		c->dead = true;
	}
}

static void process_lines(struct wx_ipc_client *c) {
	struct wx_server *server = c->server;
	while (!c->dead && c->pending == 0) {
		char *nl = memchr(c->in, '\n', c->in_len);
		if (!nl) {
			break;
		}
		*nl = '\0';
		size_t line_len = (size_t)(nl - c->in);
		if (line_len > 0 && c->in[line_len - 1] == '\r') {
			c->in[line_len - 1] = '\0';
		}
		struct wx_buf out;
		buf_init(&out);
		bool done = ipc_cmd_run(server, c, c->in, &out);
		memmove(c->in, nl + 1, c->in_len - line_len - 1);
		c->in_len -= line_len + 1;
		if (done) {
			client_send(c, out.data ? out.data : "");
		} else {
			c->pending++;
		}
		buf_free(&out);
	}
}

void ipc_client_reply(struct wx_ipc_client *c, const char *text) {
	client_send(c, text);
}

void ipc_client_unref_pending(struct wx_ipc_client *c) {
	if (c->pending > 0) {
		c->pending--;
	}
	process_lines(c);
	if (c->dead || (c->eof && c->pending == 0)) {
		client_destroy(c);
	}
}

static int client_event(int fd, uint32_t mask, void *data) {
	struct wx_ipc_client *c = data;
	if (mask & (WL_EVENT_ERROR | WL_EVENT_HANGUP)) {
		client_destroy(c);
		return 0;
	}
	if (mask & WL_EVENT_WRITABLE) {
		if (!client_flush(c)) {
			client_destroy(c);
			return 0;
		}
	}
	if (mask & WL_EVENT_READABLE) {
		if (c->in_len >= IPC_MAX_LINE) {
			client_send(c, "{\"ok\":false,\"error\":\"line too long\"}");
			client_destroy(c);
			return 0;
		}
		ssize_t n = read(fd, c->in + c->in_len, IPC_MAX_LINE - c->in_len);
		if (n <= 0) {
			if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
				return 0;
			}
			/* EOF: finish deferred replies first, the reader may still be waiting */
			if (c->pending > 0) {
				c->eof = true;
				wl_event_source_fd_update(c->source, 0);
				return 0;
			}
			client_destroy(c);
			return 0;
		}
		c->in_len += (size_t)n;
		process_lines(c);
	}
	if (c->dead) {
		client_destroy(c);
	}
	return 0;
}

static int server_event(int fd, uint32_t mask, void *data) {
	struct wx_server *server = data;
	int cfd = accept4(fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
	if (cfd < 0) {
		return 0;
	}
	struct wx_ipc_client *c = calloc(1, sizeof(*c));
	c->server = server;
	c->fd = cfd;
	c->in = malloc(IPC_MAX_LINE + 1);
	buf_init(&c->out);
	c->source = wl_event_loop_add_fd(server->loop, cfd, WL_EVENT_READABLE, client_event, c);
	wl_list_insert(&server->ipc_clients, &c->link);
	return 0;
}

bool ipc_init(struct wx_server *server) {
	const char *dir = getenv("XDG_RUNTIME_DIR");
	if (!dir) {
		wlr_log(WLR_ERROR, "XDG_RUNTIME_DIR not set");
		return false;
	}
	char path[108];
	int n = snprintf(path, sizeof(path), "%s/waylandx-%s.sock", dir, server->socket);
	if (n < 0 || (size_t)n >= sizeof(path)) {
		wlr_log(WLR_ERROR, "IPC socket path too long");
		return false;
	}
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		wlr_log_errno(WLR_ERROR, "IPC socket");
		return false;
	}
	struct sockaddr_un addr = {.sun_family = AF_UNIX};
	memcpy(addr.sun_path, path, (size_t)n + 1);
	unlink(path);
	mode_t old = umask(0077); /* only our user may drive the session */
	int r = bind(fd, (struct sockaddr *)&addr, sizeof(addr));
	umask(old);
	if (r < 0 || listen(fd, 16) < 0) {
		wlr_log_errno(WLR_ERROR, "IPC bind/listen %s", path);
		close(fd);
		return false;
	}
	chmod(path, 0600);
	server->ipc_fd = fd;
	server->ipc_path = strdup(path);
	server->ipc_source = wl_event_loop_add_fd(server->loop, fd, WL_EVENT_READABLE, server_event,
		server);
	setenv("WAYLANDX_SOCK", path, 1);
	wlr_log(WLR_INFO, "IPC listening on %s", path);
	return true;
}

void ipc_finish(struct wx_server *server) {
	struct wx_ipc_client *c, *tmp;
	wl_list_for_each_safe(c, tmp, &server->ipc_clients, link) {
		client_destroy(c);
	}
	if (server->ipc_source) {
		wl_event_source_remove(server->ipc_source);
	}
	if (server->ipc_fd >= 0) {
		close(server->ipc_fd);
		unlink(server->ipc_path);
	}
	free(server->ipc_path);
}

void ipc_broadcast_event(struct wx_server *server, const char *json_line) {
	struct wx_ipc_client *c, *tmp;
	wl_list_for_each_safe(c, tmp, &server->ipc_clients, link) {
		if (c->subscribed && !c->dead) {
			client_send(c, json_line);
			if (c->dead) {
				/* destroyed from its own fd callback once the hangup arrives */
				shutdown(c->fd, SHUT_RDWR);
			}
		}
	}
}

void ipc_event_window(struct wx_server *server, const char *event, struct wx_view *view) {
	if (wl_list_empty(&server->ipc_clients)) {
		return;
	}
	struct wx_buf b;
	buf_init(&b);
	buf_puts(&b, "{\"event\":\"window\",\"change\":");
	buf_json_str(&b, event);
	buf_puts(&b, ",\"window\":");
	if (view) {
		ipc_view_json(&b, view);
	} else {
		buf_puts(&b, "null");
	}
	buf_puts(&b, "}");
	ipc_broadcast_event(server, b.data);
	buf_free(&b);
}

bool ipc_binding_triggered(struct wx_server *server, struct wx_bind *bind) {
	struct wx_ipc_client *c = bind->owner;
	struct wx_buf b;
	buf_init(&b);
	buf_puts(&b, "{\"event\":\"binding\",\"combo\":");
	buf_json_str(&b, bind->combo);
	buf_puts(&b, "}");
	client_send(c, b.data);
	buf_free(&b);
	if (c->dead) {
		shutdown(c->fd, SHUT_RDWR);
	}
	return true;
}
