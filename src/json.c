#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "json.h"

void buf_init(struct wx_buf *b) {
	b->data = NULL;
	b->len = b->cap = 0;
}

void buf_free(struct wx_buf *b) {
	free(b->data);
	buf_init(b);
}

void buf_reset(struct wx_buf *b) {
	b->len = 0;
	if (b->data) {
		b->data[0] = '\0';
	}
}

static void buf_reserve(struct wx_buf *b, size_t extra) {
	if (b->len + extra + 1 <= b->cap) {
		return;
	}
	size_t cap = b->cap ? b->cap : 256;
	while (cap < b->len + extra + 1) {
		cap *= 2;
	}
	char *d = realloc(b->data, cap);
	if (!d) {
		abort();
	}
	b->data = d;
	b->cap = cap;
}

void buf_append(struct wx_buf *b, const char *s, size_t n) {
	buf_reserve(b, n);
	memcpy(b->data + b->len, s, n);
	b->len += n;
	b->data[b->len] = '\0';
}

void buf_puts(struct wx_buf *b, const char *s) {
	buf_append(b, s, strlen(s));
}

void buf_printf(struct wx_buf *b, const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	if (n < 0) {
		return;
	}
	buf_reserve(b, (size_t)n);
	va_start(ap, fmt);
	vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap);
	va_end(ap);
	b->len += (size_t)n;
}

void buf_json_str(struct wx_buf *b, const char *s) {
	if (!s) {
		buf_puts(b, "null");
		return;
	}
	buf_append(b, "\"", 1);
	for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
		switch (*p) {
		case '"': buf_puts(b, "\\\""); break;
		case '\\': buf_puts(b, "\\\\"); break;
		case '\n': buf_puts(b, "\\n"); break;
		case '\r': buf_puts(b, "\\r"); break;
		case '\t': buf_puts(b, "\\t"); break;
		case '\b': buf_puts(b, "\\b"); break;
		case '\f': buf_puts(b, "\\f"); break;
		default:
			if (*p < 0x20) {
				buf_printf(b, "\\u%04x", *p);
			} else {
				buf_append(b, (const char *)p, 1);
			}
		}
	}
	buf_append(b, "\"", 1);
}

void buf_plain_str(struct wx_buf *b, const char *s) {
	if (!s) {
		return;
	}
	for (const char *p = s; *p; p++) {
		char c = (*p == '\t' || *p == '\n' || *p == '\r') ? ' ' : *p;
		buf_append(b, &c, 1);
	}
}
