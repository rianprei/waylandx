#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

/* waylandxctl: command-line client for the WaylandX IPC socket. */

static void usage(void) {
	fprintf(stderr,
		"usage: waylandxctl [-s SOCKET] COMMAND [ARGS...]\n"
		"       waylandxctl subscribe            # stream events\n"
		"       waylandxctl bind COMBO [CMD...]  # global hotkey; runs CMD or prints events\n"
		"       waylandxctl help                 # list server commands\n");
}

static char *socket_path(const char *override) {
	if (override) {
		return strdup(override);
	}
	const char *env = getenv("WAYLANDX_SOCK");
	if (env && *env) {
		return strdup(env);
	}
	const char *dir = getenv("XDG_RUNTIME_DIR");
	const char *wd = getenv("WAYLAND_DISPLAY");
	if (!dir || !wd) {
		return NULL;
	}
	char buf[512];
	snprintf(buf, sizeof(buf), "%s/waylandx-%s.sock", dir, wd);
	return strdup(buf);
}

static int connect_to(const char *path) {
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		return -1;
	}
	struct sockaddr_un addr = {.sun_family = AF_UNIX};
	if (strlen(path) >= sizeof(addr.sun_path)) {
		close(fd);
		errno = ENAMETOOLONG;
		return -1;
	}
	strcpy(addr.sun_path, path);
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static bool write_all(int fd, const char *s, size_t n) {
	while (n > 0) {
		ssize_t w = write(fd, s, n);
		if (w < 0) {
			if (errno == EINTR) {
				continue;
			}
			return false;
		}
		s += w;
		n -= (size_t)w;
	}
	return true;
}

/* Quotes one argument so the server tokenizer gets it back verbatim. */
static void append_quoted(char **line, size_t *len, size_t *cap, const char *arg) {
	size_t need = *len + strlen(arg) * 2 + 4;
	if (need > *cap) {
		*cap = need * 2;
		*line = realloc(*line, *cap);
		if (!*line) {
			abort();
		}
	}
	char *o = *line + *len;
	if (*len > 0) {
		*o++ = ' ';
	}
	*o++ = '"';
	for (const char *p = arg; *p; p++) {
		switch (*p) {
		case '"': *o++ = '\\'; *o++ = '"'; break;
		case '\\': *o++ = '\\'; *o++ = '\\'; break;
		case '\n': *o++ = '\\'; *o++ = 'n'; break;
		case '\t': *o++ = '\\'; *o++ = 't'; break;
		case '\r': *o++ = '\\'; *o++ = 'r'; break;
		default: *o++ = *p;
		}
	}
	*o++ = '"';
	*o = '\0';
	*len = (size_t)(o - *line);
}

struct reader {
	int fd;
	char buf[65536];
	size_t len;
};

/* Reads one message (lines up to a blank line). Returns malloc'd text or NULL on EOF. */
static char *read_message(struct reader *r) {
	for (;;) {
		char *end = NULL;
		if (r->len >= 1 && r->buf[0] == '\n') {
			end = r->buf;
		} else {
			for (size_t i = 0; i + 1 < r->len; i++) {
				if (r->buf[i] == '\n' && r->buf[i + 1] == '\n') {
					end = r->buf + i + 1;
					break;
				}
			}
		}
		if (end) {
			size_t n = (size_t)(end - r->buf);
			char *msg = malloc(n + 1);
			memcpy(msg, r->buf, n);
			msg[n] = '\0';
			memmove(r->buf, end + 1, r->len - n - 1);
			r->len -= n + 1;
			return msg;
		}
		if (r->len >= sizeof(r->buf) - 1) {
			/* oversized message: hand back what we have */
			char *msg = malloc(r->len + 1);
			memcpy(msg, r->buf, r->len);
			msg[r->len] = '\0';
			r->len = 0;
			return msg;
		}
		ssize_t n = read(r->fd, r->buf + r->len, sizeof(r->buf) - 1 - r->len);
		if (n < 0 && errno == EINTR) {
			continue;
		}
		if (n <= 0) {
			return NULL;
		}
		r->len += (size_t)n;
	}
}

static bool is_error(const char *msg) {
	return !strncmp(msg, "{\"ok\":false", 11);
}

int main(int argc, char *argv[]) {
	const char *sock_override = NULL;
	int i = 1;
	while (i < argc && argv[i][0] == '-' && argv[i][1]) {
		if (!strcmp(argv[i], "-s") && i + 1 < argc) {
			sock_override = argv[i + 1];
			i += 2;
		} else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			usage();
			return 0;
		} else {
			break;
		}
	}
	if (i >= argc) {
		usage();
		return 1;
	}
	char *path = socket_path(sock_override);
	if (!path) {
		fprintf(stderr, "waylandxctl: cannot find socket (set WAYLANDX_SOCK or WAYLAND_DISPLAY)\n");
		return 1;
	}
	int fd = connect_to(path);
	if (fd < 0) {
		fprintf(stderr, "waylandxctl: cannot connect to %s: %s\n", path, strerror(errno));
		free(path);
		return 1;
	}
	free(path);

	const char *cmd = argv[i];
	bool is_subscribe = !strcmp(cmd, "subscribe");
	bool is_bind = !strcmp(cmd, "bind");
	char *run = NULL;
	char *line = NULL;
	size_t len = 0, cap = 0;
	if (is_bind) {
		if (i + 1 >= argc) {
			usage();
			return 1;
		}
		append_quoted(&line, &len, &cap, "bind");
		append_quoted(&line, &len, &cap, argv[i + 1]);
		/* remaining args form a shell command run on each press */
		size_t rl = 0;
		for (int j = i + 2; j < argc; j++) {
			rl += strlen(argv[j]) + 1;
		}
		if (rl) {
			run = calloc(1, rl + 1);
			for (int j = i + 2; j < argc; j++) {
				strcat(run, argv[j]);
				if (j + 1 < argc) {
					strcat(run, " ");
				}
			}
		}
	} else {
		for (int j = i; j < argc; j++) {
			append_quoted(&line, &len, &cap, argv[j]);
		}
	}
	if (!is_subscribe && !is_bind) {
		struct timeval tv = {.tv_sec = 15};
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	}
	line = realloc(line, len + 2);
	line[len++] = '\n';
	line[len] = '\0';
	if (!write_all(fd, line, len)) {
		fprintf(stderr, "waylandxctl: write failed: %s\n", strerror(errno));
		return 1;
	}
	free(line);

	static struct reader r;
	r.fd = fd;
	char *msg = read_message(&r);
	if (!msg) {
		fprintf(stderr, "waylandxctl: no reply from server\n");
		return 1;
	}
	bool err = is_error(msg);
	if (!is_subscribe && !is_bind) {
		size_t n = strlen(msg);
		if (n > 0 && msg[n - 1] == '\n') {
			msg[n - 1] = '\0';
		}
		if (*msg) {
			puts(msg);
		}
		free(msg);
		close(fd);
		return err ? 1 : 0;
	}
	if (err) {
		fputs(msg, stderr);
		free(msg);
		return 1;
	}
	free(msg);
	fflush(stdout);
	while ((msg = read_message(&r))) {
		if (run && strstr(msg, "\"event\":\"binding\"")) {
			pid_t pid = fork();
			if (pid == 0) {
				setsid();
				execl("/bin/sh", "/bin/sh", "-c", run, (char *)NULL);
				_exit(127);
			}
			while (waitpid(-1, NULL, WNOHANG) > 0) {
			}
		} else {
			fputs(msg, stdout);
			fflush(stdout);
		}
		free(msg);
	}
	return 0;
}
