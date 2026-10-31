#!/bin/bash
# Desktop shell pieces of the panel: notification server, OSD, status items.
# Runs on a private D-Bus with fake wpctl/backlight, so nothing real changes.
#   tests/run-desktop.sh [build-dir]
set -euo pipefail
WX_BUILD=${1:-}
export WX_BUILD
# shellcheck source=tests/lib.sh
. "$(dirname "$0")/lib.sh"

if ! command -v dbus-daemon >/dev/null || ! command -v notify-send >/dev/null || ! command -v gdbus >/dev/null; then
	echo "SKIP: needs dbus-daemon, notify-send and gdbus"
	exit 77
fi

dbus-daemon --session --nofork --print-address=3 3>"$T/bus.addr" >/dev/null 2>&1 &
PIDS+=($!)
wait_for 5 'test -s "$T/bus.addr"'
export DBUS_SESSION_BUS_ADDRESS=$(head -1 "$T/bus.addr")

# fake PipeWire tools and backlight
mkdir -p "$T/bin" "$T/bl/fake"
cat > "$T/bin/wpctl" <<'SH'
#!/bin/sh
echo "$*" >> "$WX_FAKE_LOG"
case "$1" in get-volume) echo "Volume: 0.40";; esac
SH
printf '#!/bin/sh\nexec sleep 1000\n' > "$T/bin/pactl"
chmod +x "$T/bin/wpctl" "$T/bin/pactl"
export PATH="$T/bin:$PATH" WX_FAKE_LOG="$T/wpctl.log"
echo raw > "$T/bl/fake/type"; echo 1000 > "$T/bl/fake/max_brightness"; echo 500 > "$T/bl/fake/brightness"
export WAYLANDX_BACKLIGHT_DIR="$T/bl"

printf '[general]\nmod = super\nterminal = true\n[autostart]\npanel = %s/waylandx-panel > %s/panel.log 2>&1\n' \
	"$BUILD" "$T" > "$T/config.ini"
wx_start 1
wait_for 5 'grep -q "notification server ready" "$T/panel.log"'
check "panel owns org.freedesktop.Notifications" grep -q "notification server ready" "$T/panel.log"
check "server information says WaylandX" bash -c "gdbus call --session -d org.freedesktop.Notifications \
	-o /org/freedesktop/Notifications -m org.freedesktop.Notifications.GetServerInformation | grep -q WaylandX"
check "status bar reads the volume (fake wpctl)" grep -q "get-volume" "$T/wpctl.log"

CARDS="900,36,372,140"
"$CTL" screenshot "$T/empty.png" -r "$CARDS" >/dev/null

echo "== notifications"
notify-send -a Test -t 600 "expires soon" "body text" &
wait_for 3 'grep -q "from Test: expires soon" "$T/panel.log"'
check "notification arrives" grep -q "from Test: expires soon" "$T/panel.log"
sleep 0.3
"$CTL" screenshot "$T/card.png" -r "$CARDS" >/dev/null
check "a card is drawn in the top right" bash -c "! cmp -s '$T/empty.png' '$T/card.png'"
sleep 1
"$CTL" screenshot "$T/after.png" -r "$CARDS" >/dev/null
check "it expires after its timeout" cmp -s "$T/empty.png" "$T/after.png"

notify-send -a Mail -A default=Open -A archive=Archive --wait "New mail" "from Ana" > "$T/action.txt" &
NS=$!
wait_for 3 'grep -q "from Mail: New mail" "$T/panel.log"'
sleep 0.3
"$CTL" mousemove 1000 60 >/dev/null
"$CTL" click 1 >/dev/null
wait "$NS" 2>/dev/null || true
check "clicking the card runs the default action" grep -qx "default" "$T/action.txt"

ID=$(gdbus call --session -d org.freedesktop.Notifications -o /org/freedesktop/Notifications \
	-m org.freedesktop.Notifications.Notify -- "x" 0 "" "sticky" "critical, stays" "[]" \
	"{'urgency': <byte 2>}" -1 | sed 's/.*uint32 \([0-9]*\).*/\1/')
sleep 1.2
"$CTL" screenshot "$T/crit.png" -r "$CARDS" >/dev/null
check "critical notifications stay" bash -c "! cmp -s '$T/empty.png' '$T/crit.png'"
gdbus call --session -d org.freedesktop.Notifications -o /org/freedesktop/Notifications \
	-m org.freedesktop.Notifications.CloseNotification "$ID" >/dev/null
sleep 0.3
"$CTL" screenshot "$T/closed.png" -r "$CARDS" >/dev/null
check "CloseNotification removes it" cmp -s "$T/empty.png" "$T/closed.png"

echo "== OSD and media keys"
OSD="480,560,320,80"
"$CTL" screenshot "$T/osd0.png" -r "$OSD" >/dev/null
"$CTL" volume up >/dev/null
wait_for 3 'grep -q "set-volume -l 1.5 @DEFAULT_AUDIO_SINK@ 5%+" "$T/wpctl.log"'
check "volume up runs wpctl" grep -q "set-volume -l 1.5 @DEFAULT_AUDIO_SINK@ 5%+" "$T/wpctl.log"
sleep 0.3
"$CTL" screenshot "$T/osd1.png" -r "$OSD" >/dev/null
check "volume OSD appears" bash -c "! cmp -s '$T/osd0.png' '$T/osd1.png'"
sleep 1.8
"$CTL" screenshot "$T/osd2.png" -r "$OSD" >/dev/null
check "...and goes away" cmp -s "$T/osd0.png" "$T/osd2.png"
check "brightness 30% writes the backlight" bash -c "'$CTL' brightness 30% | grep -q '\"percent\":30' && \
	test \"\$(cat '$T/bl/fake/brightness')\" = 300"
sleep 0.3
"$CTL" screenshot "$T/osd3.png" -r "$OSD" >/dev/null
check "brightness OSD appears" bash -c "! cmp -s '$T/osd0.png' '$T/osd3.png'"

echo "== Alt+Tab switcher"
for t in sw-one sw-two sw-three; do
	spawn "$BUILD/tests/probe-wl" --title "$t" > /dev/null 2>&1
	wait_for 5 'test -n "$(id_of '"$t"')"'
done
title_of_focused() { "$CTL" info "$("$CTL" focused --plain)" --plain | cut -f9; }
check "newest window has focus" test "$(title_of_focused)" = sw-three
"$CTL" keydown alt >/dev/null
"$CTL" key Tab >/dev/null
"$CTL" key Tab >/dev/null
wait_for 3 'grep -q "switcher: .* picked 2 (sw-one)" "$T/panel.log"'
check "holding Alt, Tab walks the windows (overlay shows sw-one)" grep -q "picked 2 (sw-one)" "$T/panel.log"
check "nothing switches before Alt is released" test "$(title_of_focused)" = sw-three
"$CTL" keyup alt >/dev/null
sleep 0.2
check "releasing Alt switches to the picked window" test "$(title_of_focused)" = sw-one
check "...and the overlay closes" grep -q "switcher closed" "$T/panel.log"
"$CTL" keydown alt >/dev/null
"$CTL" key Tab >/dev/null
"$CTL" key Escape >/dev/null
"$CTL" keyup alt >/dev/null
sleep 0.2
check "Escape cancels" test "$(title_of_focused)" = sw-one
"$CTL" key alt+Tab >/dev/null
sleep 0.2
check "a quick Alt+Tab flips to the previous window" test "$(title_of_focused)" = sw-three

echo "== system tray (StatusNotifierItem + DBusMenu)"
if [ -x "$BUILD/tests/mock-sni" ]; then
	check "panel is the StatusNotifierWatcher" grep -q "tray: watcher ready" "$T/panel.log"
	spawn "$BUILD/tests/mock-sni" > "$T/sni.txt" 2>&1
	wait_for 5 'grep -q "tray: org.kde.StatusNotifierItem.* at " "$T/panel.log"'
	check "an app's tray item shows up in the bar" grep -q "tray: org.kde.StatusNotifierItem.* at " "$T/panel.log"
	POS=$(grep -o ' at [0-9]*-[0-9]*' "$T/panel.log" | tail -1 | tr -d ' at')
	X0=${POS%-*}; X1=${POS#*-}; CX=$(((X0 + X1) / 2))
	sleep 0.3
	"$CTL" mousemove "$CX" 14 >/dev/null
	"$CTL" click 1 >/dev/null
	wait_for 3 'grep -q "item Activate" "$T/sni.txt"'
	check "left click activates the app" grep -q "item Activate" "$T/sni.txt"
	"$CTL" click 2 >/dev/null
	wait_for 3 'grep -q "item SecondaryActivate" "$T/sni.txt"'
	check "middle click is the secondary action" grep -q "item SecondaryActivate" "$T/sni.txt"
	"$CTL" click 3 >/dev/null
	wait_for 3 'grep -q "tray: menu open with 4 rows" "$T/panel.log"'
	check "right click opens its DBusMenu" grep -q "tray: menu open with 4 rows" "$T/panel.log"
	sleep 0.3
	"$CTL" mousemove $((X1 - 40)) 49 >/dev/null
	"$CTL" click 1 >/dev/null
	wait_for 3 'grep -q "menu event 1 clicked" "$T/sni.txt"'
	check "clicking a menu entry reaches the app" grep -q "menu event 1 clicked" "$T/sni.txt"
	check "mnemonics are stripped (_Open window)" grep -q "(Open window)" "$T/panel.log"
	"$CTL" mousemove "$CX" 14 >/dev/null
	"$CTL" click 3 >/dev/null
	sleep 0.4
	"$CTL" mousemove $((X1 - 40)) 84 >/dev/null
	"$CTL" click 1 >/dev/null
	wait_for 3 'grep -q "tray: menu open with 1 rows" "$T/panel.log"'
	check "submenus open" grep -q "tray: menu open with 1 rows" "$T/panel.log"
	sleep 0.3
	"$CTL" mousemove $((X1 - 40)) 75 >/dev/null
	"$CTL" click 1 >/dev/null
	wait_for 3 'grep -q "menu event 5 clicked" "$T/sni.txt"'
	check "...and their entries work" grep -q "menu event 5 clicked" "$T/sni.txt"
	"$CTL" mousemove "$CX" 14 >/dev/null
	"$CTL" click 3 >/dev/null
	sleep 0.4
	CLOSED=$(grep -c "tray: menu closed" "$T/panel.log")
	"$CTL" mousemove 300 400 >/dev/null
	"$CTL" click 1 >/dev/null
	wait_for 3 'test "$(grep -c "tray: menu closed" "$T/panel.log")" -gt "$CLOSED"'
	check "clicking elsewhere closes the menu" test "$(grep -c "tray: menu closed" "$T/panel.log")" -gt "$CLOSED"
	kill "${PIDS[-1]}" 2>/dev/null || true
	wait_for 3 'grep -q "tray: .* removed" "$T/panel.log"'
	check "the item goes away with its app" grep -q "tray: .* removed" "$T/panel.log"
fi

wx_stop
