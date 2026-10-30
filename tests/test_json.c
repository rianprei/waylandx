#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "json.h"

int main(void) {
	struct wx_buf b;
	buf_init(&b);
	buf_json_str(&b, "a\"b\\c\nd\te\x01");
	assert(!strcmp(b.data, "\"a\\\"b\\\\c\\nd\\te\\u0001\""));
	buf_reset(&b);
	buf_json_str(&b, NULL);
	assert(!strcmp(b.data, "null"));
	buf_reset(&b);
	buf_json_str(&b, "olá 日本");
	assert(!strcmp(b.data, "\"olá 日本\""));
	buf_reset(&b);
	buf_plain_str(&b, "tab\there\nnl");
	assert(!strcmp(b.data, "tab here nl"));
	buf_reset(&b);
	for (int i = 0; i < 1000; i++) {
		buf_printf(&b, "%d,", i);
	}
	assert(b.len == strlen(b.data) && !strncmp(b.data + b.len - 4, "999,", 4));
	buf_free(&b);
	puts("json: all tests passed");
	return 0;
}
