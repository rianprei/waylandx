#ifndef WX_JSON_H
#define WX_JSON_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>

/* Growable string buffer used for IPC replies. */
struct wx_buf {
	char *data;
	size_t len, cap;
};

void buf_init(struct wx_buf *b);
void buf_free(struct wx_buf *b);
void buf_reset(struct wx_buf *b);
void buf_append(struct wx_buf *b, const char *s, size_t n);
void buf_puts(struct wx_buf *b, const char *s);
__attribute__((format(printf, 2, 3)))
void buf_printf(struct wx_buf *b, const char *fmt, ...);
/* Appends s as a JSON string literal (with quotes). NULL becomes null. */
void buf_json_str(struct wx_buf *b, const char *s);
/* Appends s with tabs/newlines replaced by spaces (for TSV output). */
void buf_plain_str(struct wx_buf *b, const char *s);

#endif
