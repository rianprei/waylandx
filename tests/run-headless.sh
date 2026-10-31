#!/bin/bash
# End-to-end test: runs WaylandX headless and drives it like a user would.
#   tests/run-headless.sh [build-dir]
set -euo pipefail

WX_BUILD=${1:-}
export WX_BUILD
# shellcheck source=tests/lib.sh
. "$(dirname "$0")/lib.sh"

cat > "$T/config.ini" <<'EOF'
[general]
mod = super
terminal = true
gap = 4
[rule:ruled]
title = ruled
position = 15,25
layer = above
EOF

wx_start 1
check "IPC socket is private (0600)" test "$(stat -c %a "$WAYLANDX_SOCK")" = 600
echo "   WAYLAND_DISPLAY=$WAYLAND_DISPLAY DISPLAY=$DISPLAY"

echo "== Wayland clients"
spawn "$BUILD/tests/probe-wl" --title alpha > "$T/alpha.txt" 2>&1
wait_for 5 'test -n "$(id_of alpha)"'
spawn "$BUILD/tests/probe-wl" --title beta --pos 100 200 --size 400 300 > "$T/beta.txt" 2>&1
wait_for 5 'test -n "$(id_of beta)"'
A=$(id_of alpha)
B=$(id_of beta)
check "two windows listed" test "$("$CTL" list --plain | wc -l)" -eq 2
check "waylandx-unstable-v1: client placed itself at 100,200" \
	test "$(field "$B" 5),$(field "$B" 6)" = "100,200"
check "client received wx_position event" grep -q "wx_position 100 200" "$T/beta.txt"
check "newest window has focus" test "$("$CTL" focused --plain)" = "$B"

"$CTL" move -w "$B" 50 60 >/dev/null
check "move via IPC" test "$(field "$B" 5),$(field "$B" 6)" = "50,60"
"$CTL" resize -w "$B" 420 310 >/dev/null
wait_for 3 'test "$(field "$B" 7)" = 420'
check "resize via IPC (client acked)" test "$(field "$B" 7)x$(field "$B" 8)" = "420x310"

echo "== input injection (xdotool-like)"
"$CTL" type "olá Wörld!" >/dev/null
"$CTL" key ctrl+a shift+b >/dev/null
sleep 0.3
check "type: Unicode text arrives (á)" grep -q "key aacute down utf8=á" <(clean_log "$T/beta.txt")
check "type: Unicode text arrives (ö)" grep -q "key odiaeresis down utf8=ö" <(clean_log "$T/beta.txt")
check "key ctrl+a has ctrl modifier" grep -aq "key a down utf8=.* ctrl=1" <(clean_log "$T/beta.txt")
check "key shift+b gives B with shift" grep -q "key B down utf8=B ctrl=0 shift=1" <(clean_log "$T/beta.txt")
"$CTL" mousemove 150 160 >/dev/null
"$CTL" click 1 >/dev/null
"$CTL" scroll 0 15 >/dev/null
sleep 0.3
check "pointer enters at surface coords 100,100" grep -q "ptr_enter 100 100" <(clean_log "$T/beta.txt")
check "click 1 = BTN_LEFT down/up" grep -q "button 272 up" <(clean_log "$T/beta.txt")
check "scroll arrives" grep -q "axis 0 15.0" <(clean_log "$T/beta.txt")

echo "== screenshots (no portal, no prompt)"
check "full screenshot" "$CTL" screenshot "$T/full.png"
check "full screenshot is 1280x720" grep -q "1280 x 720" <(file "$T/full.png")
check "window screenshot" "$CTL" screenshot "$T/win.png" -w "$B"
check "window screenshot size 420x310" grep -q "420 x 310" <(file "$T/win.png")
check "region screenshot" "$CTL" screenshot "$T/reg.png" -r 0,0,64,32
check "relative path rejected" bash -c "! '$CTL' screenshot rel.png"

echo "== X11 clients (Xwayland)"
spawn "$BUILD/tests/probe-x11" --pos 700 400 --size 320 200 --title xprobe > "$T/x1.txt" 2>&1
wait_for 10 'test -n "$(id_of xprobe)"'
X=$(id_of xprobe)
check "X11 window listed" test -n "$X"
check "X11 window kept its requested position 700,400" \
	test "$(field "$X" 5),$(field "$X" 6)" = "700,400"
check "X11 window gets server-side decorations" bash -c "'$CTL' info $X | grep -q '\"decorations\":\"server\"'"
check "X11 window flagged xwayland" bash -c "'$CTL' info $X | grep -q '\"xwayland\":true'"
spawn "$BUILD/tests/probe-x11" --pos 900 100 --size 150 80 --override --title menu > "$T/x2.txt" 2>&1
sleep 1
check "override-redirect window is unmanaged (not listed)" test -z "$(id_of menu)"
spawn "$BUILD/tests/probe-x11" --pos 300 450 --size 200 120 --above --title ontop > "$T/x3.txt" 2>&1
wait_for 10 'test -n "$(id_of ontop)"'
O=$(id_of ontop)
check "_NET_WM_STATE_ABOVE honoured" bash -c "'$CTL' info $O | grep -q '\"layer\":\"above\"'"
"$CTL" focus "$X" >/dev/null
sleep 0.3
"$CTL" key a >/dev/null
sleep 0.5
check "key injection reaches X11 window" grep -q "^key " "$T/x1.txt"
"$CTL" move -w "$X" 720 410 >/dev/null
sleep 0.5
check "moving an X11 window sends it a ConfigureNotify" grep -q "configure 720 410" "$T/x1.txt"

echo "== window management with the mouse"
X1=$(field "$X" 5); Y1=$(field "$X" 6); W1=$(field "$X" 7); H1=$(field "$X" 8)
"$CTL" mousemove $((X1 + W1 + 1)) $((Y1 + H1 / 2)) >/dev/null   # right border
"$CTL" mousedown 1 >/dev/null
"$CTL" mousemove $((X1 + W1 + 81)) $((Y1 + H1 / 2)) >/dev/null
"$CTL" mouseup 1 >/dev/null
sleep 0.4
check "drag right border resizes X11 window (+80px)" test "$(field "$X" 7)" = "$((W1 + 80))"
"$CTL" mousemove $((X1 + 60)) $((Y1 - 10)) >/dev/null           # title bar
"$CTL" mousedown 1 >/dev/null
"$CTL" mousemove $((X1 + 60 - 100)) $((Y1 - 10 + 30)) >/dev/null
"$CTL" mouseup 1 >/dev/null
sleep 0.3
check "drag title bar moves window" test "$(field "$X" 5),$(field "$X" 6)" = "$((X1 - 100)),$((Y1 + 30))"
X2=$(field "$X" 5); Y2=$(field "$X" 6)
"$CTL" mousemove $((X2 + 50)) $((Y2 + 50)) >/dev/null
"$CTL" keydown super >/dev/null
"$CTL" mousedown 1 >/dev/null
"$CTL" mousemove $((X2 + 50 + 40)) $((Y2 + 50 + 20)) >/dev/null
"$CTL" mouseup 1 >/dev/null
"$CTL" keyup super >/dev/null
sleep 0.3
check "Super+drag moves window from anywhere inside it" \
	test "$(field "$X" 5),$(field "$X" 6)" = "$((X2 + 40)),$((Y2 + 20))"

echo "== rules"
spawn "$BUILD/tests/probe-wl" --title ruled > "$T/ruled.txt" 2>&1
wait_for 5 'test -n "$(id_of ruled)"'
R=$(id_of ruled)
check "rule position 15,25" test "$(field "$R" 5),$(field "$R" 6)" = "15,25"
check "rule layer above" bash -c "'$CTL' info $R | grep -q '\"layer\":\"above\"'"

echo "== X11 tool shims (wmctrl/xdotool see Wayland windows too)"
export PATH="$SRC/shims:$BUILD:$PATH"
check "wmctrl -l lists Wayland window 'alpha'" bash -c "wmctrl -l | grep -q ' alpha$'"
check "wmctrl -l lists X11 window 'xprobe'" bash -c "wmctrl -l | grep -q ' xprobe$'"
check "xdotool search --name beta" test "$(xdotool search --name beta)" = "$B"
xdotool search --name alpha windowactivate
check "xdotool search ... windowactivate (chained)" test "$("$CTL" focused --plain)" = "$A"
check "xdotool getactivewindow" test "$(xdotool getactivewindow)" = "$A"
wmctrl -r beta -e 0,30,40,-1,-1
check "wmctrl -r beta -e moves it" test "$(field "$B" 5),$(field "$B" 6)" = "30,40"
wmctrl -r beta -b add,above
check "wmctrl -b add,above" bash -c "'$CTL' info $B | grep -q '\"layer\":\"above\"'"
check "xdotool getwindowname" test "$(xdotool getwindowname "$B")" = beta

echo "== clipboard bridge Wayland <-> X11"
if command -v wl-copy >/dev/null && [ -x /usr/bin/xclip ]; then
	wl-copy "fusion-clip-123"
	sleep 0.5
	check "xclip shim reads the clipboard from anywhere" \
		test "$(timeout 3 xclip -o -selection clipboard 2>/dev/null)" = "fusion-clip-123"
	printf 'shim-in-789' | xclip -selection clipboard
	sleep 0.3
	check "xclip shim writes the clipboard" test "$(timeout 3 wl-paste -n 2>/dev/null)" = "shim-in-789"
	check "xsel shim reads it back" test "$(timeout 3 xsel -b -o 2>/dev/null)" = "shim-in-789"
	# native path: a real X11 client with focus, through Xwayland's bridge
	"$CTL" focus "$X" >/dev/null
	sleep 0.3
	wl-copy "native-wl-123"
	sleep 0.5
	check "Wayland -> focused X11 app (real xclip)" \
		test "$(timeout 3 /usr/bin/xclip -o -selection clipboard 2>/dev/null)" = "native-wl-123"
	printf 'native-x11-456' | timeout 3 /usr/bin/xclip -i -selection clipboard -loops 1 &
	sleep 0.8
	check "focused X11 app -> Wayland (real xclip)" test "$(timeout 3 wl-paste -n 2>/dev/null)" = "native-x11-456"
	# X11-style: background X11 clients read the clipboard with a Wayland window focused
	"$CTL" focus "$B" >/dev/null
	sleep 0.3
	wl-copy "background-read-789"
	sleep 0.5
	check "unfocused X11 client reads the Wayland clipboard (real xclip)" \
		test "$(timeout 3 /usr/bin/xclip -o -selection clipboard 2>/dev/null)" = "background-read-789"
	check "...TARGETS lists text types" \
		bash -c "timeout 3 /usr/bin/xclip -o -selection clipboard -t TARGETS | grep -q UTF8_STRING"
	wl-copy --primary "primary-sel-321"
	sleep 0.5
	check "...and the primary selection" \
		test "$(timeout 3 /usr/bin/xclip -o -selection primary 2>/dev/null)" = "primary-sel-321"
	# X11-style: a background X11 client takes the clipboard with a Wayland window focused
	printf 'bg-owner-555' | timeout 5 /usr/bin/xclip -i -selection clipboard &
	sleep 0.8
	check "unfocused X11 client sets the Wayland clipboard (real xclip)" \
		test "$(timeout 3 wl-paste -n 2>/dev/null)" = "bg-owner-555"
	check "...the content outlives the X11 owner" bash -c '! pgrep -fx "/usr/bin/xclip -i -selection clipboard" >/dev/null'
	check "...and X11 clients read it back" \
		test "$(timeout 3 /usr/bin/xclip -o -selection clipboard 2>/dev/null)" = "bg-owner-555"
	printf 'bg-primary-666' | timeout 5 /usr/bin/xclip -i -selection primary &
	sleep 0.8
	check "...same for PRIMARY" test "$(timeout 3 wl-paste -p -n 2>/dev/null)" = "bg-primary-666"
	head -c 3000000 /dev/urandom | base64 -w0 > "$T/big.txt"
	timeout 10 /usr/bin/xclip -i -selection clipboard "$T/big.txt" &
	sleep 2
	check "...large payloads (INCR, 4 MB)" \
		bash -c "cmp -s <(timeout 5 wl-paste -n) '$T/big.txt'"
else
	bad "clipboard test skipped: wl-clipboard/xclip missing"
fi

echo "== workspaces, tiling, states"
"$CTL" move_to_workspace -w "$A" 2 >/dev/null
check "move_to_workspace" test "$(field "$A" 2)" = 2
"$CTL" workspace 2 >/dev/null
check "workspace switch" bash -c "'$CTL' status | grep -q '\"workspace\":2'"
check "focus follows to window on new workspace" test "$("$CTL" focused --plain)" = "$A"
"$CTL" workspace 1 >/dev/null
"$CTL" tiling on >/dev/null
sleep 0.5
check "tiling on" bash -c "'$CTL' status | grep -q '\"tiling\":true'"
check "beta is tiled" bash -c "'$CTL' info $B | grep -q '\"tiled\":false'" # above layer floats
"$CTL" above off -w "$B" >/dev/null
sleep 0.5
check "beta tiled after leaving the above layer" bash -c "'$CTL' info $B | grep -q '\"tiled\":true'"
"$CTL" tiling off >/dev/null
"$CTL" maximize on -w "$B" >/dev/null
sleep 0.4
check "maximize" bash -c "'$CTL' info $B | grep -q '\"maximized\":true'"
check "client got maximized configure" grep -q "max=1" <(clean_log "$T/beta.txt")
"$CTL" maximize off -w "$B" >/dev/null
"$CTL" fullscreen on -w "$B" >/dev/null
sleep 0.4
check "fullscreen covers the output" test "$(field "$B" 7)x$(field "$B" 8)" = "1280x720"
"$CTL" fullscreen off -w "$B" >/dev/null
"$CTL" minimize on -w "$B" >/dev/null
check "minimize" bash -c "'$CTL' info $B | grep -q '\"minimized\":true'"
"$CTL" focus "$B" >/dev/null
check "focusing a minimized window restores it" bash -c "'$CTL' info $B | grep -q '\"minimized\":false'"

echo "== global hotkeys over IPC"
"$CTL" bind ctrl+F12 > "$T/bind.txt" 2>&1 &
PIDS+=($!)
sleep 0.4
"$CTL" key ctrl+F12 >/dev/null
sleep 0.4
check "bind: owner receives the binding event" grep -q '"combo":"ctrl+F12"' "$T/bind.txt"

echo "== live config reload"
cp "$T/config.ini" "$T/config.good"
printf '[general]\ngap = notanumber\n' > "$T/config.ini"
check "broken config is rejected" bash -c "! '$CTL' reload"
check "server still alive after bad reload" "$CTL" ping
cp "$T/config.good" "$T/config.ini"
check "good config reloads" "$CTL" reload

echo "== input methods (text-input-v3 <-> input-method-v2)"
spawn "$BUILD/tests/probe-wl" --text-input --title imeapp > "$T/imeapp.txt" 2>&1
wait_for 5 'test -n "$(id_of imeapp)"'
wait_for 3 'grep -q ti_enter "$T/imeapp.txt"'
check "app gets text-input enter on focus" grep -q ti_enter "$T/imeapp.txt"
spawn "$BUILD/tests/probe-im" > "$T/im1.txt" 2>&1
wait_for 5 'grep -q "ti_commit" "$T/imeapp.txt"'
check "input method is activated for the focused app" grep -q im_activate "$T/im1.txt"
check "IME preedit reaches the app" grep -q "ti_preedit pre" "$T/imeapp.txt"
check "IME commit string reaches the app" grep -q "ti_commit IME✓" "$T/imeapp.txt"
spawn "$BUILD/tests/probe-im" > "$T/im2.txt" 2>&1
wait_for 3 'grep -q im_unavailable "$T/im2.txt"'
check "a second input method is refused" grep -q im_unavailable "$T/im2.txt"
"$CTL" focus "$B" >/dev/null
wait_for 3 'grep -q im_deactivate "$T/im1.txt"'
check "focus change deactivates the IME" grep -q im_deactivate "$T/im1.txt"
check "...and the app gets text-input leave" grep -q ti_leave "$T/imeapp.txt"

echo "== panel (layer-shell, exclusive zone)"
spawn "$BUILD/waylandx-panel" > "$T/panel.txt" 2>&1
wait_for 5 '"$CTL" outputs --plain | grep -q "	0	0	1280	720"'
sleep 0.5
check "panel reserves 28px at the top" bash -c "'$CTL' outputs | grep -q '\"usable\":{\"x\":0,\"y\":28,\"width\":1280,\"height\":692}'"
"$CTL" screenshot "$T/panel.png" -r 0,0,1280,28 >/dev/null
check "panel is drawn (screenshot of the bar)" test -s "$T/panel.png"
spawn "$BUILD/tests/probe-x11" --pos 400 0 --size 200 100 --title undertop > "$T/x4.txt" 2>&1
wait_for 10 'test -n "$(id_of undertop)"'
U=$(id_of undertop)
check "window asking for y=0 keeps its title bar below the panel" \
	test "$(field "$U" 6)" = "$((28 + 2 + 24))"

echo "== network transparency (waypipe transport)"
if command -v waypipe >/dev/null; then
	waypipe --socket "$T/wp.sock" client > "$T/wp-client.txt" 2>&1 &
	PIDS+=($!)
	sleep 0.5
	waypipe --socket "$T/wp.sock" server -- "$BUILD/tests/probe-wl" --title remote-app > "$T/wp-server.txt" 2>&1 &
	PIDS+=($!)
	wait_for 10 'test -n "$(id_of remote-app)"'
	check "app through waypipe shows up as a window" test -n "$(id_of remote-app)"
else
	echo "   SKIP waypipe not installed"
fi
if command -v waypipe >/dev/null && timeout 5 ssh -o BatchMode=yes -o ConnectTimeout=3 localhost true 2>/dev/null; then
	check "waylandx-remote localhost" timeout 15 "$SRC/session/waylandx-remote" -o BatchMode=yes localhost true
else
	echo "   SKIP waylandx-remote over ssh (needs sshd with key login to localhost)"
fi

echo "== idle: screens off, inhibitors, auto-lock"
cp "$T/config.ini" "$T/config.base"
printf '[general]\nidle_dpms = 1\nidle_lock = 0\n' >> "$T/config.ini"
"$CTL" reload >/dev/null
screens_on() { "$CTL" outputs | grep -q '"enabled":true'; }
sleep 1.5
check "screens turn off after idle_dpms" bash -c "! '$CTL' outputs | grep -q '\"enabled\":true'"
"$CTL" mousemove_rel 1 0 >/dev/null
check "input turns them back on" screens_on
spawn "$BUILD/tests/probe-wl" --title inhibitor --inhibit-idle > /dev/null 2>&1
wait_for 5 'test -n "$(id_of inhibitor)"'
sleep 1.5
check "a visible idle inhibitor (video player) keeps them on" screens_on
kill "${PIDS[-1]}" 2>/dev/null || true
sleep 1.6
check "...and idling resumes once it is gone" bash -c "! '$CTL' outputs | grep -q '\"enabled\":true'"
"$CTL" mousemove_rel -1 0 >/dev/null
cp "$T/config.base" "$T/config.ini"

echo "== session lock (ext-session-lock-v1)"
if command -v swaylock >/dev/null; then
	printf '[general]\nidle_lock = 1\nidle_dpms = 0\nlock_command = swaylock -c 202020\n' >> "$T/config.ini"
	"$CTL" reload >/dev/null
	wait_for 5 '"$CTL" status | grep -q "\"locked\":true"'
	check "idle_lock starts swaylock and locks the session" bash -c "'$CTL' status | grep -q '\"locked\":true'"
	: > "$T/beta.txt"
	"$CTL" type "secret" >/dev/null
	sleep 0.3
	check "keys do not reach normal windows while locked" bash -c "! grep -aq 'key s down' '$T/beta.txt'"
	pkill -f "^swaylock -c 202020" || true
	sleep 0.5
	check "a crashed locker leaves the session locked" bash -c "'$CTL' status | grep -q '\"locked\":true'"
	sleep 3 # the relaunch guard
	"$CTL" lock >/dev/null
	wait_for 5 'pgrep -f "^swaylock -c 202020" >/dev/null'
	check "'lock' brings a crashed locker back" pgrep -f "^swaylock -c 202020"
	pkill -f "^swaylock -c 202020" || true
else
	echo "   SKIP swaylock not installed"
fi

wx_stop
