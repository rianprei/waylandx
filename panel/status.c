#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifdef WX_HAVE_SDBUS
#include <systemd/sd-bus.h>
#endif
#include "panel.h"
#include "status.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

/*
 * Bar status items KDE shows in its system tray, without a tray: volume
 * (PipeWire through wpctl, refreshed on `pactl subscribe` events), network
 * (NetworkManager over D-Bus, or the kernel's view without it) and battery
 * (sysfs). Plus the volume/brightness OSD.
 */

#define OSD_MS 1500
#define NET_MS 5000
#define BAT_MS 30000

static struct {
	bool have_vol, muted;
	int vol;              /* percent */
	int net_kind;         /* 0 offline, 1 wired, 2 wifi, 3 other */
	int net_strength;     /* wifi 0..100 */
	char net_name[64];
	bool have_bat, charging;
	int bat;
	int sub_fd;           /* pactl subscribe */
	char sub_buf[1024];
	size_t sub_len;
	long long vol_due, net_due, bat_due, sub_retry;
	/* OSD */
	struct popup *osd;
	long long osd_until, osd_vol_due;
	int osd_kind;         /* 0 volume, 1 brightness */
	int osd_value;
	bool osd_muted;
#ifdef WX_HAVE_SDBUS
	sd_bus *sys;
#endif
} S = {.sub_fd = -1};

static long long now_ms(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static bool in_path(const char *prog) {
	const char *path = getenv("PATH");
	if (!path) {
		return false;
	}
	char *copy = strdup(path), *save = NULL;
	bool found = false;
	for (char *d = strtok_r(copy, ":", &save); d && !found; d = strtok_r(NULL, ":", &save)) {
		char p[1024];
		snprintf(p, sizeof(p), "%s/%s", d, prog);
		found = access(p, X_OK) == 0;
	}
	free(copy);
	return found;
}

/* ---------- volume ---------- */

static void read_volume(void) {
	FILE *f = popen("wpctl get-volume @DEFAULT_AUDIO_SINK@ 2>/dev/null", "r");
	if (!f) {
		return;
	}
	char line[128] = "";
	bool ok = fgets(line, sizeof(line), f) != NULL;
	pclose(f);
	double v;
	if (!ok || sscanf(line, "Volume: %lf", &v) != 1) {
		S.have_vol = false;
		return;
	}
	int vol = (int)lround(v * 100);
	bool muted = strstr(line, "MUTED") != NULL;
	if (!S.have_vol || vol != S.vol || muted != S.muted) {
		S.have_vol = true;
		S.vol = vol;
		S.muted = muted;
		panel_redraw_bars();
	}
}

static void start_subscribe(void) {
	if (!in_path("pactl")) {
		return;
	}
	int p[2];
	if (pipe2(p, O_CLOEXEC) < 0) {
		return;
	}
	pid_t pid = fork();
	if (pid == 0) {
		dup2(p[1], 1);
		int devnull = open("/dev/null", O_WRONLY);
		if (devnull >= 0) {
			dup2(devnull, 2);
		}
		execlp("pactl", "pactl", "subscribe", (char *)NULL);
		_exit(127);
	}
	close(p[1]);
	if (pid < 0) {
		close(p[0]);
		return;
	}
	fcntl(p[0], F_SETFL, O_NONBLOCK);
	S.sub_fd = p[0];
	S.sub_len = 0;
}

int status_fd(void) {
	return S.sub_fd;
}

void status_dispatch(void) {
	ssize_t n = read(S.sub_fd, S.sub_buf + S.sub_len, sizeof(S.sub_buf) - 1 - S.sub_len);
	if (n <= 0) {
		if (n < 0 && errno == EAGAIN) {
			return;
		}
		close(S.sub_fd);
		S.sub_fd = -1;
		S.sub_retry = now_ms() + 5000; /* PipeWire restarting */
		return;
	}
	S.sub_len += (size_t)n;
	S.sub_buf[S.sub_len] = '\0';
	char *nl;
	while ((nl = strchr(S.sub_buf, '\n'))) {
		*nl = '\0';
		if (strstr(S.sub_buf, "sink") || strstr(S.sub_buf, "server")) {
			S.vol_due = now_ms() + 30; /* coalesce bursts */
		}
		size_t used = (size_t)(nl - S.sub_buf) + 1;
		memmove(S.sub_buf, nl + 1, S.sub_len - used + 1);
		S.sub_len -= used;
	}
	if (S.sub_len >= sizeof(S.sub_buf) - 1) {
		S.sub_len = 0;
	}
}

/* ---------- network ---------- */

#ifdef WX_HAVE_SDBUS
static char *nm_string(const char *path, const char *iface, const char *prop) {
	char *s = NULL;
	if (sd_bus_get_property_string(S.sys, "org.freedesktop.NetworkManager", path, iface, prop,
			NULL, &s) < 0) {
		return NULL;
	}
	return s;
}

static bool read_network_nm(void) {
	if (!S.sys) {
		return false;
	}
	uint32_t state = 0;
	if (sd_bus_get_property_trivial(S.sys, "org.freedesktop.NetworkManager",
			"/org/freedesktop/NetworkManager", "org.freedesktop.NetworkManager", "State", NULL, 'u',
			&state) < 0) {
		return false;
	}
	S.net_name[0] = '\0';
	S.net_strength = 0;
	if (state < 50) { /* NM_STATE_CONNECTED_LOCAL */
		S.net_kind = 0;
		return true;
	}
	char *primary = NULL;
	if (sd_bus_get_property_string(S.sys, "org.freedesktop.NetworkManager",
			"/org/freedesktop/NetworkManager", "org.freedesktop.NetworkManager",
			"PrimaryConnectionType", NULL, &primary) >= 0) {
		S.net_kind = strstr(primary, "wireless") ? 2 : strstr(primary, "ethernet") ? 1 : 3;
		free(primary);
	}
	sd_bus_message *reply = NULL;
	const char *conn = NULL;
	if (sd_bus_get_property(S.sys, "org.freedesktop.NetworkManager",
			"/org/freedesktop/NetworkManager", "org.freedesktop.NetworkManager",
			"PrimaryConnection", NULL, &reply, "o") >= 0 &&
			sd_bus_message_read(reply, "o", &conn) >= 0 && strcmp(conn, "/")) {
		char *id = nm_string(conn, "org.freedesktop.NetworkManager.Connection.Active", "Id");
		if (id) {
			snprintf(S.net_name, sizeof(S.net_name), "%s", id);
			free(id);
		}
		if (S.net_kind == 2) {
			sd_bus_message *devs = NULL;
			const char *dev = NULL;
			if (sd_bus_get_property(S.sys, "org.freedesktop.NetworkManager", conn,
					"org.freedesktop.NetworkManager.Connection.Active", "Devices", NULL, &devs,
					"ao") >= 0 && sd_bus_message_enter_container(devs, 'a', "o") > 0 &&
					sd_bus_message_read(devs, "o", &dev) > 0) {
				sd_bus_message *ap = NULL;
				const char *ap_path = NULL;
				if (sd_bus_get_property(S.sys, "org.freedesktop.NetworkManager", dev,
						"org.freedesktop.NetworkManager.Device.Wireless", "ActiveAccessPoint", NULL,
						&ap, "o") >= 0 && sd_bus_message_read(ap, "o", &ap_path) >= 0) {
					uint8_t strength = 0;
					if (sd_bus_get_property_trivial(S.sys, "org.freedesktop.NetworkManager", ap_path,
							"org.freedesktop.NetworkManager.AccessPoint", "Strength", NULL, 'y',
							&strength) >= 0) {
						S.net_strength = strength;
					}
				}
				sd_bus_message_unref(ap);
			}
			sd_bus_message_unref(devs);
		}
	}
	sd_bus_message_unref(reply);
	return true;
}
#endif

/* Without NetworkManager: any non-loopback interface that is up. */
static void read_network_sysfs(void) {
	S.net_kind = 0;
	S.net_name[0] = '\0';
	DIR *d = opendir("/sys/class/net");
	if (!d) {
		return;
	}
	struct dirent *e;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.' || !strcmp(e->d_name, "lo")) {
			continue;
		}
		char p[512], state[32] = "";
		snprintf(p, sizeof(p), "/sys/class/net/%s/operstate", e->d_name);
		FILE *f = fopen(p, "r");
		if (!f) {
			continue;
		}
		if (!fgets(state, sizeof(state), f)) {
			state[0] = '\0';
		}
		fclose(f);
		if (strncmp(state, "up", 2)) {
			continue;
		}
		snprintf(p, sizeof(p), "/sys/class/net/%s/wireless", e->d_name);
		S.net_kind = access(p, F_OK) == 0 ? 2 : 1;
		S.net_strength = 100;
		snprintf(S.net_name, sizeof(S.net_name), "%.63s", e->d_name);
		break;
	}
	closedir(d);
}

static void read_network(void) {
	int kind = S.net_kind, strength = S.net_strength;
	char name[64];
	memcpy(name, S.net_name, sizeof(name));
#ifdef WX_HAVE_SDBUS
	if (!read_network_nm()) {
		read_network_sysfs();
	}
#else
	read_network_sysfs();
#endif
	if (kind != S.net_kind || strength / 25 != S.net_strength / 25 || strcmp(name, S.net_name)) {
		panel_redraw_bars();
	}
}

/* ---------- battery ---------- */

static void read_battery(void) {
	const char *dir = "/sys/class/power_supply";
	DIR *d = opendir(dir);
	bool have = false, charging = false;
	int total = 0, n = 0;
	if (d) {
		struct dirent *e;
		while ((e = readdir(d))) {
			if (e->d_name[0] == '.') {
				continue;
			}
			char p[512], buf[32] = "";
			snprintf(p, sizeof(p), "%s/%s/type", dir, e->d_name);
			FILE *f = fopen(p, "r");
			if (!f) {
				continue;
			}
			bool bat = fgets(buf, sizeof(buf), f) && !strncmp(buf, "Battery", 7);
			fclose(f);
			snprintf(p, sizeof(p), "%s/%s/scope", dir, e->d_name);
			f = fopen(p, "r");
			if (f) { /* mice and headsets report "Device" */
				if (fgets(buf, sizeof(buf), f) && !strncmp(buf, "Device", 6)) {
					bat = false;
				}
				fclose(f);
			}
			if (!bat) {
				continue;
			}
			snprintf(p, sizeof(p), "%s/%s/capacity", dir, e->d_name);
			f = fopen(p, "r");
			int cap;
			if (f && fscanf(f, "%d", &cap) == 1) {
				total += cap;
				n++;
				have = true;
			}
			if (f) {
				fclose(f);
			}
			snprintf(p, sizeof(p), "%s/%s/status", dir, e->d_name);
			f = fopen(p, "r");
			if (f) {
				if (fgets(buf, sizeof(buf), f) && (!strncmp(buf, "Charging", 8) ||
						!strncmp(buf, "Full", 4))) {
					charging = true;
				}
				fclose(f);
			}
		}
		closedir(d);
	}
	int bat = n ? total / n : 0;
	if (have != S.have_bat || bat != S.bat || charging != S.charging) {
		S.have_bat = have;
		S.bat = bat;
		S.charging = charging;
		panel_redraw_bars();
	}
}

/* ---------- drawing ---------- */

static void icon_speaker(cairo_t *cr, double x, double cy, int vol, bool muted, uint32_t c) {
	panel_set_rgb(cr, c, 1);
	cairo_move_to(cr, x, cy - 3);
	cairo_line_to(cr, x + 4, cy - 3);
	cairo_line_to(cr, x + 8, cy - 7);
	cairo_line_to(cr, x + 8, cy + 7);
	cairo_line_to(cr, x + 4, cy + 3);
	cairo_line_to(cr, x, cy + 3);
	cairo_close_path(cr);
	cairo_fill(cr);
	cairo_set_line_width(cr, 1.5);
	if (muted) {
		cairo_move_to(cr, x + 11, cy - 4);
		cairo_line_to(cr, x + 17, cy + 4);
		cairo_move_to(cr, x + 17, cy - 4);
		cairo_line_to(cr, x + 11, cy + 4);
		cairo_stroke(cr);
		return;
	}
	int waves = vol <= 0 ? 0 : vol < 34 ? 1 : vol < 67 ? 2 : 3;
	for (int i = 0; i < waves; i++) {
		cairo_arc(cr, x + 8, cy, 3.5 + i * 3.5, -G_PI / 4, G_PI / 4);
		cairo_stroke(cr);
	}
}

static void icon_wifi(cairo_t *cr, double x, double cy, int strength, uint32_t c) {
	int bars = strength >= 75 ? 4 : strength >= 50 ? 3 : strength >= 25 ? 2 : 1;
	double cx = x + 8, by = cy + 6;
	cairo_set_line_width(cr, 1.8);
	for (int i = 0; i < 4; i++) {
		panel_set_rgb(cr, c, i < bars ? 1 : 0.3);
		if (i == 0) {
			cairo_arc(cr, cx, by - 1, 1.6, 0, 2 * G_PI);
			cairo_fill(cr);
			continue;
		}
		cairo_arc(cr, cx, by, i * 4.0, -3 * G_PI / 4, -G_PI / 4);
		cairo_stroke(cr);
	}
}

static void icon_wired(cairo_t *cr, double x, double cy, bool online, uint32_t c) {
	panel_set_rgb(cr, c, online ? 1 : 0.4);
	cairo_rectangle(cr, x + 5, cy - 7, 6, 5);
	cairo_rectangle(cr, x, cy + 3, 6, 5);
	cairo_rectangle(cr, x + 10, cy + 3, 6, 5);
	cairo_fill(cr);
	cairo_set_line_width(cr, 1.2);
	cairo_move_to(cr, x + 8, cy - 2);
	cairo_line_to(cr, x + 8, cy + 1);
	cairo_move_to(cr, x + 3, cy + 3);
	cairo_line_to(cr, x + 3, cy + 1);
	cairo_line_to(cr, x + 13, cy + 1);
	cairo_line_to(cr, x + 13, cy + 3);
	cairo_stroke(cr);
	if (!online) {
		panel_set_rgb(cr, 0xe0605a, 1);
		cairo_set_line_width(cr, 1.6);
		cairo_move_to(cr, x, cy - 7);
		cairo_line_to(cr, x + 16, cy + 8);
		cairo_stroke(cr);
	}
}

static void icon_battery(cairo_t *cr, double x, double cy, int pct, bool charging, uint32_t c) {
	panel_set_rgb(cr, c, 1);
	cairo_set_line_width(cr, 1.2);
	cairo_rectangle(cr, x + 0.5, cy - 5.5, 18, 11);
	cairo_stroke(cr);
	cairo_rectangle(cr, x + 19, cy - 2.5, 2, 5);
	cairo_fill(cr);
	uint32_t fill = pct <= 15 && !charging ? 0xe0605a : charging ? 0x6cc36c : c;
	panel_set_rgb(cr, fill, 1);
	cairo_rectangle(cr, x + 2, cy - 4, 15.0 * (pct < 0 ? 0 : pct > 100 ? 100 : pct) / 100, 8);
	cairo_fill(cr);
	if (charging) {
		panel_set_rgb(cr, 0x16181d, 1);
		cairo_move_to(cr, x + 10, cy - 5);
		cairo_line_to(cr, x + 6, cy + 1);
		cairo_line_to(cr, x + 9, cy + 1);
		cairo_line_to(cr, x + 8, cy + 5);
		cairo_line_to(cr, x + 12, cy - 1);
		cairo_line_to(cr, x + 9, cy - 1);
		cairo_close_path(cr);
		cairo_fill(cr);
	}
}

static double label_width(PangoLayout *l, const char *s) {
	pango_layout_set_text(l, s, -1);
	pango_layout_set_width(l, -1);
	int w, h;
	pango_layout_get_pixel_size(l, &w, &h);
	return w;
}

static void label(cairo_t *cr, PangoLayout *l, int h, const char *s, double x, uint32_t c) {
	pango_layout_set_text(l, s, -1);
	pango_layout_set_width(l, -1);
	int w, th;
	pango_layout_get_pixel_size(l, &w, &th);
	panel_set_rgb(cr, c, 1);
	cairo_move_to(cr, x, (h - th) / 2.0);
	pango_cairo_show_layout(cr, l);
}

double status_draw(cairo_t *cr, PangoLayout *l, int h, double x_right,
		void (*hit)(void *ctx, int item, double x0, double x1), void *ctx) {
	double x = x_right, cy = h / 2.0;
	const uint32_t fg = 0xdddddd;
	char buf[96];
	if (S.have_bat) {
		snprintf(buf, sizeof(buf), "%d%%", S.bat);
		double w = 22 + 4 + label_width(l, buf);
		x -= w;
		icon_battery(cr, x, cy, S.bat, S.charging, fg);
		label(cr, l, h, buf, x + 26, fg);
		hit(ctx, ST_BATTERY, x - 4, x + w + 4);
		x -= 14;
	}
	{
		const char *name = S.net_kind ? S.net_name : "offline";
		snprintf(buf, sizeof(buf), "%.24s", *name ? name : "online");
		double tw = label_width(l, buf);
		double w = 16 + 5 + tw;
		x -= w;
		if (S.net_kind == 2) {
			icon_wifi(cr, x, cy, S.net_strength, fg);
		} else {
			icon_wired(cr, x, cy, S.net_kind != 0, fg);
		}
		label(cr, l, h, buf, x + 21, S.net_kind ? fg : 0x8a8f99);
		hit(ctx, ST_NETWORK, x - 4, x + w + 4);
		x -= 14;
	}
	if (S.have_vol) {
		snprintf(buf, sizeof(buf), "%d%%", S.vol);
		double w = 18 + 4 + label_width(l, buf);
		x -= w;
		icon_speaker(cr, x, cy, S.vol, S.muted, S.muted ? 0x8a8f99 : fg);
		label(cr, l, h, buf, x + 22, S.muted ? 0x8a8f99 : fg);
		hit(ctx, ST_VOLUME, x - 4, x + w + 4);
		x -= 10;
	}
	return x;
}

/* ---------- actions ---------- */

static void open_first(const char *const *cmds) {
	for (int i = 0; cmds[i]; i++) {
		char prog[128];
		snprintf(prog, sizeof(prog), "%s", cmds[i]);
		prog[strcspn(prog, " ")] = '\0';
		if (in_path(prog)) {
			panel_spawn(cmds[i]);
			return;
		}
	}
}

static void volume_cmd(const char *args) {
	char cmd[256];
	snprintf(cmd, sizeof(cmd), "wpctl %s", args);
	panel_spawn(cmd);
	S.vol_due = now_ms() + 80;
}

void status_click(int item, uint32_t button) {
	bool left = button == 0x110; /* BTN_LEFT */
	if (item == ST_VOLUME) {
		if (left) {
			volume_cmd("set-mute @DEFAULT_AUDIO_SINK@ toggle");
		} else {
			static const char *const mixers[] = {"pavucontrol", "pwvucontrol",
				"kcmshell6 kcm_pulseaudio", "systemsettings kcm_pulseaudio", NULL};
			open_first(mixers);
		}
	} else if (item == ST_NETWORK) {
		static const char *const nets[] = {"nm-connection-editor",
			"kcmshell6 kcm_networkmanagement", "systemsettings kcm_networkmanagement",
			"alacritty -e nmtui", "foot nmtui", "xterm -e nmtui", NULL};
		open_first(nets);
	} else if (item == ST_BATTERY) {
		static const char *const power[] = {"kcmshell6 kcm_powerdevilprofilesconfig",
			"systemsettings kcm_powerdevilprofilesconfig", NULL};
		open_first(power);
	}
}

void status_scroll(int item, double delta) {
	if (item == ST_VOLUME) {
		volume_cmd(delta < 0 ? "set-volume -l 1.5 @DEFAULT_AUDIO_SINK@ 5%+" :
			"set-volume @DEFAULT_AUDIO_SINK@ 5%-");
	}
}

/* ---------- OSD ---------- */

static void osd_draw(struct popup *p, cairo_t *cr, PangoLayout *l) {
	int w = p->width, h = p->height;
	panel_round_rect(cr, 0.5, 0.5, w - 1, h - 1, 12);
	panel_set_rgb(cr, 0x16181d, 0.94);
	cairo_fill_preserve(cr);
	panel_set_rgb(cr, 0x3a3d45, 1);
	cairo_set_line_width(cr, 1);
	cairo_stroke(cr);
	double cy = h / 2.0;
	if (S.osd_kind == 0) {
		icon_speaker(cr, 18, cy, S.osd_value, S.osd_muted, 0xffffff);
	} else { /* sun */
		panel_set_rgb(cr, 0xffffff, 1);
		cairo_arc(cr, 27, cy, 4.5, 0, 2 * G_PI);
		cairo_fill(cr);
		cairo_set_line_width(cr, 1.6);
		for (int i = 0; i < 8; i++) {
			double a = i * G_PI / 4;
			cairo_move_to(cr, 27 + cos(a) * 7, cy + sin(a) * 7);
			cairo_line_to(cr, 27 + cos(a) * 9.5, cy + sin(a) * 9.5);
		}
		cairo_stroke(cr);
	}
	double bx = 50, bw = w - 50 - 58, bh = 6;
	panel_round_rect(cr, bx, cy - bh / 2, bw, bh, 3);
	panel_set_rgb(cr, 0x3a3d45, 1);
	cairo_fill(cr);
	double frac = S.osd_value / 100.0;
	if (frac > 1) {
		frac = 1;
	}
	if (frac > 0) {
		panel_round_rect(cr, bx, cy - bh / 2, bw * frac, bh, 3);
		panel_set_rgb(cr, S.osd_muted ? 0x6c717c : 0x3d7edd, 1);
		cairo_fill(cr);
	}
	char buf[16];
	snprintf(buf, sizeof(buf), "%d%%", S.osd_value);
	label(cr, l, h, buf, w - 48, 0xffffff);
}

static void osd_show(int kind, int value, bool muted) {
	S.osd_kind = kind;
	S.osd_value = value;
	S.osd_muted = muted;
	S.osd_until = now_ms() + OSD_MS;
	if (!S.osd) {
		int margin[4] = {0, 0, 96, 0};
		S.osd = popup_create(POPUP_OVERLAY, ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM, margin, 300, 52,
			"waylandx-osd", osd_draw, NULL, NULL);
	} else {
		popup_damage(S.osd);
	}
}

void status_osd_volume(void) {
	S.osd_vol_due = now_ms() + 60; /* let wpctl finish first */
}

void status_osd_brightness(int percent) {
	osd_show(1, percent, false);
}

/* ---------- lifecycle ---------- */

void status_init(void) {
#ifdef WX_HAVE_SDBUS
	if (sd_bus_open_system(&S.sys) < 0) {
		S.sys = NULL;
	}
#endif
	if (in_path("wpctl")) {
		read_volume();
		start_subscribe();
	}
	read_network();
	read_battery();
	long long now = now_ms();
	S.net_due = now + NET_MS;
	S.bat_due = now + BAT_MS;
	if (S.sub_fd < 0 && S.have_vol) {
		S.vol_due = now + 5000; /* no event source: poll */
	}
}

void status_tick(void) {
	long long now = now_ms();
	if (S.vol_due && now >= S.vol_due) {
		S.vol_due = 0;
		read_volume();
		if (S.sub_fd < 0 && S.have_vol) {
			S.vol_due = now + 5000;
		}
	}
	if (S.osd_vol_due && now >= S.osd_vol_due) {
		S.osd_vol_due = 0;
		read_volume();
		if (S.have_vol) {
			osd_show(0, S.vol, S.muted);
		}
	}
	if (now >= S.net_due) {
		S.net_due = now + NET_MS;
		read_network();
	}
	if (now >= S.bat_due) {
		S.bat_due = now + BAT_MS;
		read_battery();
	}
	if (S.sub_retry && now >= S.sub_retry) {
		S.sub_retry = 0;
		start_subscribe();
	}
	if (S.osd && now >= S.osd_until) {
		popup_destroy(S.osd);
		S.osd = NULL;
	}
}

int status_next_ms(void) {
	long long now = now_ms(), next = -1;
	long long due[] = {S.vol_due, S.osd_vol_due, S.net_due, S.bat_due, S.sub_retry,
		S.osd ? S.osd_until : 0};
	for (size_t i = 0; i < sizeof(due) / sizeof(due[0]); i++) {
		if (due[i] && (next < 0 || due[i] < next)) {
			next = due[i];
		}
	}
	if (next < 0) {
		return -1;
	}
	return next <= now ? 0 : (int)(next - now);
}
