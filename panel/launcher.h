#ifndef WX_LAUNCHER_H
#define WX_LAUNCHER_H

#include <stdbool.h>
#include <stddef.h>

struct app_entry {
	char *id;      /* desktop file id, e.g. firefox.desktop */
	char *name;
	char *exec;    /* Exec= with field codes stripped */
	bool terminal;
	char *icon;    /* Icon= (theme name or path), may be NULL */
	char *wmclass; /* StartupWMClass=, may be NULL */
	bool nodisplay; /* hidden from the launcher, kept for icon lookup */
};

struct app_list {
	struct app_entry *items;
	size_t len;
};

void apps_load(struct app_list *list);
void apps_free(struct app_list *list);
/* Fills out[] with indexes of entries matching query (prefix matches first). */
size_t apps_match(const struct app_list *list, const char *query, size_t *out, size_t max);
char *apps_strip_field_codes(const char *exec);
void apps_launch(const struct app_entry *app);
/* Desktop entry for a window's app_id / X11 class, or NULL. */
const struct app_entry *apps_for_app_id(const struct app_list *list, const char *app_id);
void launch_command(const char *cmd);

#endif
