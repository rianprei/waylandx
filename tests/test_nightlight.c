#include <assert.h>
#include <stdio.h>
#include "server.h"

/* nightlight.c only needs this from the rest of the server */
void ipc_broadcast_event(struct wx_server *server, const char *json) {
}

int main(void) {
	const char *s = "20:00-07:00";
	assert(nightlight_target(s, 4000, 12 * 60) == 6500);      /* noon */
	assert(nightlight_target(s, 4000, 20 * 60) == 6500);      /* fade starts */
	assert(nightlight_target(s, 4000, 20 * 60 + 15) == 5250); /* halfway in */
	assert(nightlight_target(s, 4000, 23 * 60) == 4000);      /* night */
	assert(nightlight_target(s, 4000, 3 * 60) == 4000);       /* past midnight */
	assert(nightlight_target(s, 4000, 6 * 60 + 45) == 5250);  /* fading out */
	assert(nightlight_target(s, 4000, 7 * 60) == 6500);       /* morning */
	assert(nightlight_target("08:00-09:00", 3000, 8 * 60 + 40) == 4167); /* short night */
	assert(nightlight_target("on", 3500, 12 * 60) == 3500);
	assert(nightlight_target("off", 3500, 23 * 60) == 6500);
	assert(nightlight_target("bogus", 3500, 23 * 60) == 6500);
	puts("nightlight ok");
	return 0;
}
