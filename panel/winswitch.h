#ifndef WX_WINSWITCH_H
#define WX_WINSWITCH_H

#include "launcher.h"

/* IPC "switcher" event from the compositor (Alt+Tab). */
void winswitch_event(const char *line, const struct app_list *apps);

#endif
