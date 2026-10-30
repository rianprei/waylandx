#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <unistd.h>
#include "launcher.h"

char *apps_strip_field_codes(const char *exec) {
	size_t n = strlen(exec);
	char *out = malloc(n + 1), *o = out;
	for (size_t i = 0; i < n; i++) {
		if (exec[i] == '%' && i + 1 < n) {
			char c = exec[i + 1];
			if (c == '%') {
				*o++ = '%';
			}
			/* %f %F %u %U %i %c %k and deprecated codes are dropped */
			i++;
			continue;
		}
		*o++ = exec[i];
	}
	*o = '\0';
	/* trim trailing spaces left by removed codes */
	while (o > out && isspace((unsigned char)o[-1])) {
		*--o = '\0';
	}
	return out;
}

static bool has_entry(struct app_list *list, const char *id) {
	for (size_t i = 0; i < list->len; i++) {
		if (!strcmp(list->items[i].id, id)) {
			return true;
		}
	}
	return false;
}

static void parse_desktop(struct app_list *list, const char *path, const char *id) {
	FILE *f = fopen(path, "r");
	if (!f) {
		return;
	}
	char line[4096];
	bool in_entry = false, hidden = false, is_app = false, terminal = false;
	char *name = NULL, *exec = NULL;
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = '\0';
		if (line[0] == '[') {
			in_entry = !strcmp(line, "[Desktop Entry]");
			continue;
		}
		if (!in_entry) {
			continue;
		}
		if (!strncmp(line, "Name=", 5) && !name) {
			name = strdup(line + 5);
		} else if (!strncmp(line, "Exec=", 5) && !exec) {
			exec = strdup(line + 5);
		} else if (!strcmp(line, "NoDisplay=true") || !strcmp(line, "Hidden=true")) {
			hidden = true;
		} else if (!strcmp(line, "Type=Application")) {
			is_app = true;
		} else if (!strcmp(line, "Terminal=true")) {
			terminal = true;
		}
	}
	fclose(f);
	if (is_app && !hidden && name && exec) {
		struct app_entry e = {strdup(id), name, apps_strip_field_codes(exec), terminal};
		free(exec);
		struct app_entry *n = realloc(list->items, (list->len + 1) * sizeof(*n));
		if (!n) {
			abort();
		}
		list->items = n;
		list->items[list->len++] = e;
		return;
	}
	free(name);
	free(exec);
}

static void scan_dir(struct app_list *list, const char *dir) {
	DIR *d = opendir(dir);
	if (!d) {
		return;
	}
	struct dirent *de;
	while ((de = readdir(d))) {
		size_t n = strlen(de->d_name);
		if (n < 9 || strcmp(de->d_name + n - 8, ".desktop") || has_entry(list, de->d_name)) {
			continue;
		}
		char path[4096];
		snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
		parse_desktop(list, path, de->d_name);
	}
	closedir(d);
}

static int by_name(const void *a, const void *b) {
	return strcasecmp(((const struct app_entry *)a)->name, ((const struct app_entry *)b)->name);
}

void apps_load(struct app_list *list) {
	list->items = NULL;
	list->len = 0;
	char path[4096];
	const char *data_home = getenv("XDG_DATA_HOME");
	const char *home = getenv("HOME");
	/* user entries first so they shadow system ones with the same id */
	if (data_home && *data_home) {
		snprintf(path, sizeof(path), "%s/applications", data_home);
	} else {
		snprintf(path, sizeof(path), "%s/.local/share/applications", home ? home : "");
	}
	scan_dir(list, path);
	const char *dirs = getenv("XDG_DATA_DIRS");
	char *copy = strdup(dirs && *dirs ? dirs : "/usr/local/share:/usr/share");
	char *save = NULL;
	for (char *d = strtok_r(copy, ":", &save); d; d = strtok_r(NULL, ":", &save)) {
		snprintf(path, sizeof(path), "%s/applications", d);
		scan_dir(list, path);
	}
	free(copy);
	qsort(list->items, list->len, sizeof(*list->items), by_name);
}

void apps_free(struct app_list *list) {
	for (size_t i = 0; i < list->len; i++) {
		free(list->items[i].id);
		free(list->items[i].name);
		free(list->items[i].exec);
	}
	free(list->items);
	list->items = NULL;
	list->len = 0;
}

size_t apps_match(const struct app_list *list, const char *query, size_t *out, size_t max) {
	size_t n = 0;
	/* pass 0: name prefix, pass 1: substring in name or exec */
	for (int pass = 0; pass < 2 && n < max; pass++) {
		for (size_t i = 0; i < list->len && n < max; i++) {
			const struct app_entry *e = &list->items[i];
			bool prefix = !strncasecmp(e->name, query, strlen(query));
			bool sub = strcasestr(e->name, query) || strcasestr(e->exec, query);
			if ((pass == 0 && prefix) || (pass == 1 && !prefix && sub)) {
				out[n++] = i;
			}
		}
	}
	return n;
}

void launch_command(const char *cmd) {
	pid_t pid = fork();
	if (pid == 0) {
		setsid();
		signal(SIGCHLD, SIG_DFL);
		pid_t child = fork();
		if (child == 0) {
			execl("/bin/sh", "/bin/sh", "-c", cmd, (char *)NULL);
			_exit(127);
		}
		_exit(0);
	}
	if (pid > 0) {
		waitpid(pid, NULL, 0);
	}
}

void apps_launch(const struct app_entry *app) {
	if (!app->terminal) {
		launch_command(app->exec);
		return;
	}
	const char *term = getenv("TERMINAL");
	char *cmd;
	if (asprintf(&cmd, "%s -e %s", term && *term ? term : "alacritty", app->exec) < 0) {
		return;
	}
	launch_command(cmd);
	free(cmd);
}
