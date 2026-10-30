#!/bin/bash
# Two-monitor test: workspaces are per output and windows follow monitors.
#   tests/run-multihead.sh [build-dir]
set -euo pipefail
WX_BUILD=${1:-}
export WX_BUILD
# shellcheck source=tests/lib.sh
. "$(dirname "$0")/lib.sh"

printf '[general]\nmod = super\nterminal = true\ninternal_output = HEADLESS-1\n' > "$T/config.ini"
wx_start 2

info() { "$CTL" info "$1"; }
has() { info "$1" | grep -q "$2"; }

O1=$("$CTL" outputs --plain | sed -n 1p | cut -f1)
O2=$("$CTL" outputs --plain | sed -n 2p | cut -f1)
X2=$("$CTL" outputs --plain | sed -n 2p | cut -f2)
check "two outputs side by side" test "$X2" = 1280

echo "== one window per monitor"
spawn "$BUILD/tests/probe-wl" --title left --pos 100 100 > "$T/left.txt" 2>&1
wait_for 5 'test -n "$(id_of left)"'
spawn "$BUILD/tests/probe-wl" --title right --pos $((X2 + 100)) 100 > "$T/right.txt" 2>&1
wait_for 5 'test -n "$(id_of right)"'
L=$(id_of left)
R=$(id_of right)
check "left window is on $O1" has "$L" "\"output\":\"$O1\""
check "right window is on $O2" has "$R" "\"output\":\"$O2\""

echo "== independent workspaces"
"$CTL" workspace 2 -o "$O2" >/dev/null
check "$O2 switched to workspace 2" bash -c "'$CTL' status -o $O2 | grep -q '\"workspace\":2'"
check "$O1 still on workspace 1" bash -c "'$CTL' status -o $O1 | grep -q '\"workspace\":1'"
check "right window hidden" has "$R" '"visible":false'
check "left window still visible" has "$L" '"visible":true'

echo "== windows follow the monitor they are moved to"
"$CTL" move -w "$L" $((X2 + 300)) 200 >/dev/null
check "moved window now belongs to $O2" has "$L" "\"output\":\"$O2\""
check "...and adopted its workspace (2)" has "$L" '"workspace":2'
check "...so it stays visible" has "$L" '"visible":true'
"$CTL" workspace 1 -o "$O2" >/dev/null
check "switching $O2 back shows the right window" has "$R" '"visible":true'
check "...and hides the moved one" has "$L" '"visible":false'
"$CTL" focus "$L" >/dev/null
check "focusing a hidden window switches its monitor's workspace" has "$L" '"visible":true'

echo "== tiling is per output and workspace"
"$CTL" tiling on -o "$O1" >/dev/null
check "tiling on $O1" bash -c "'$CTL' status -o $O1 | grep -q '\"tiling\":true'"
check "$O2 untouched" bash -c "'$CTL' status -o $O2 | grep -q '\"tiling\":false'"

echo "== panel: one bar per monitor"
spawn "$BUILD/waylandx-panel" > "$T/panel.txt" 2>&1
PANEL_PID=${PIDS[-1]}
wait_for 5 '[ "$("$CTL" outputs | grep -o "\"usable\":{\"x\":[0-9]*,\"y\":28" | wc -l)" = 2 ]'
check "both monitors reserve 28px for their bar" \
	test "$("$CTL" outputs | grep -o '"usable":{"x":[0-9]*,"y":28' | wc -l)" = 2

echo "== laptop lid (HEADLESS-1 plays the built-in panel)"
"$CTL" move -w "$L" 200 200 >/dev/null
"$CTL" lid closed >/dev/null
wait_for 3 '[ "$("$CTL" outputs --plain | wc -l)" = 1 ] || ! "$CTL" outputs | grep -q "\"name\":\"HEADLESS-1\".*\"enabled\":true"'
check "closing the lid turns the internal panel off" \
	bash -c "! '$CTL' outputs | grep -o '{\"name\":\"HEADLESS-1\"[^}]*' | grep -q '\"enabled\":true'"
check "windows from the internal panel move to the external one" has "$L" '"output":"HEADLESS-2"'
check "...and stay visible" has "$L" '"visible":true'
"$CTL" lid open >/dev/null
sleep 0.5
check "opening the lid brings the panel back" \
	bash -c "'$CTL' outputs | grep -o '{\"name\":\"HEADLESS-1\"[^}]*' | grep -q '\"enabled\":true'"
wait_for 3 '[ "$("$CTL" outputs | grep -o "\"usable\":{\"x\":[0-9]*,\"y\":28" | wc -l)" = 2 ]'
check "panel survived the monitor going away and back" kill -0 "$PANEL_PID"
check "...and put a bar on the returning monitor" \
	test "$("$CTL" outputs | grep -o '"usable":{"x":[0-9]*,"y":28' | wc -l)" = 2

echo "== screenshot across both monitors"
check "full screenshot" "$CTL" screenshot "$T/both.png"
check "covers 2560x720" grep -q "2560 x 720" <(file "$T/both.png")

wx_stop
