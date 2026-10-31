#ifndef WX_NOTIFY_H
#define WX_NOTIFY_H

/* org.freedesktop.Notifications server, drawn as cards in the top right. */
void notify_init(void);
int notify_fd(void);      /* D-Bus fd to poll, or -1 */
void notify_dispatch(void);
void notify_tick(void);   /* expire cards */
int notify_next_ms(void); /* -1 when nothing expires */

#endif
