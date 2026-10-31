#!/bin/bash
# HiDPI test: X11 apps get a device-pixel X screen and stay sharp.
# HEADLESS-1 is scale 2, HEADLESS-2 scale 1, so X11 renders at scale 2.
#   tests/run-hidpi.sh [build-dir]
set -euo pipefail
WX_BUILD=${1:-}
export WX_BUILD
# shellcheck source=tests/lib.sh
. "$(dirname "$0")/lib.sh"

cat > "$T/config.ini" <<'INI'
[general]
mod = super
terminal = true
[output:HEADLESS-1]
scale = 2
position = 0,0
[output:HEADLESS-2]
scale = 1
position = 640,0
INI
wx_start 2

echo "== X screen in device pixels"
spawn "$BUILD/tests/probe-x11" --title crisp --pos 200 100 --size 400 300 > "$T/x.txt" 2>&1
wait_for 5 'test -n "$(id_of crisp)"'
X=$(id_of crisp)
echo "   geometry: $("$CTL" info "$X" --plain)"
check "X screen covers both outputs at scale 2 (3840x1440)" \
	bash -c "xdpyinfo | grep -q 'dimensions:    3840x1440 pixels'"
check "RandR: scale-2 output is 1280x720 at 0,0" bash -c "xrandr | grep -q '1280x720+0+0'"
check "RandR: scale-1 output is 2560x1440 at 1280,0" bash -c "xrandr | grep -q '2560x1440+1280+0'"
check "Xft.dpi is 192" bash -c "xrdb -query | grep -q 'Xft.dpi:[[:space:]]*192'"
check "window asked for 200,100 400x300 px -> layout 100,50 200x150" \
	test "$(field "$X" 5),$(field "$X" 6),$(field "$X" 7),$(field "$X" 8)" = "100,50,200,150"

echo "== input lands on the right X pixel"
"$CTL" focus "$X" >/dev/null
"$CTL" mousemove 150 75 >/dev/null
"$CTL" click 1 >/dev/null
wait_for 3 'grep -q "button 1 at" "$T/x.txt"'
check "click at layout 150,75 is X pixel 100,50 in the window" grep -q "button 1 at 100 50" "$T/x.txt"
check "xdotool sees the pointer at X 300,150" \
	bash -c "/usr/bin/xdotool getmouselocation | grep -q 'x:300 y:150'"

echo "== moves and resizes are converted back"
"$CTL" move -w "$X" 300 100 >/dev/null
wait_for 3 'grep -q "configure 600 200 400 300" "$T/x.txt"'
check "IPC move 300,100 -> X configure 600,200" grep -q "configure 600 200 400 300" "$T/x.txt"
"$CTL" resize -w "$X" 250 125 >/dev/null
wait_for 3 'grep -q "configure 600 200 500 250" "$T/x.txt"'
check "IPC resize 250x125 -> X 500x250" grep -q "configure 600 200 500 250" "$T/x.txt"

echo "== screenshots keep every physical pixel"
sleep 0.5
"$CTL" screenshot "$T/shot.png" -o HEADLESS-1 >/dev/null
check "screenshot of the scale-2 output is native 1280x720" \
	bash -c "file '$T/shot.png' | grep -q '1280 x 720'"
"$CTL" screenshot "$T/win.png" -w "$X" >/dev/null
check "window screenshot at scale 2 (frame 254x153 -> 508x306)" \
	bash -c "file '$T/win.png' | grep -q '508 x 306'"

echo "== xwayland_scale = 1 goes back to stretched X11"
printf '[general]\nmod = super\nterminal = true\nxwayland_scale = 1\n[output:HEADLESS-1]\nscale = 2\nposition = 0,0\n[output:HEADLESS-2]\nscale = 1\nposition = 640,0\n' > "$T/config.ini"
"$CTL" reload >/dev/null
sleep 0.5
check "X screen back in layout units (1920x720)" \
	bash -c "xdpyinfo | grep -q 'dimensions:    1920x720 pixels'"
check "window keeps its layout box" \
	test "$(field "$X" 5),$(field "$X" 6),$(field "$X" 7),$(field "$X" 8)" = "300,100,250,125"
wait_for 3 'grep -q "configure 300 100 250 125" "$T/x.txt"'
check "...and the X11 client was told 300,100 250x125" grep -q "configure 300 100 250 125" "$T/x.txt"

wx_stop
