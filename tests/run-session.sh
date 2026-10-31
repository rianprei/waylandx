#!/bin/bash
# Session integration against a mock logind + MPRIS player on a private bus:
# lock before sleep, loginctl lock-session, LockedHint, brightness, media keys.
#   tests/run-session.sh [build-dir]
set -euo pipefail
WX_BUILD=${1:-}
export WX_BUILD
# shellcheck source=tests/lib.sh
. "$(dirname "$0")/lib.sh"

if [ ! -x "$BUILD/tests/mock-logind" ] || ! command -v dbus-daemon >/dev/null || ! command -v swaylock >/dev/null; then
	echo "SKIP: needs sd-bus, dbus-daemon and swaylock"
	exit 77
fi

dbus-daemon --session --nofork --print-address=3 3>"$T/bus.addr" >/dev/null 2>&1 &
PIDS+=($!)
wait_for 5 'test -s "$T/bus.addr"'
DBUS_SYSTEM_BUS_ADDRESS=$(head -1 "$T/bus.addr")
DBUS_SESSION_BUS_ADDRESS=$DBUS_SYSTEM_BUS_ADDRESS
export DBUS_SYSTEM_BUS_ADDRESS DBUS_SESSION_BUS_ADDRESS

# two fake backlights: the firmware one must win over the raw one
mkdir -p "$T/bl/acpi_video0" "$T/bl/intel_backlight"
echo firmware > "$T/bl/acpi_video0/type"; echo 1000 > "$T/bl/acpi_video0/max_brightness"
echo 500 > "$T/bl/acpi_video0/brightness"
echo raw > "$T/bl/intel_backlight/type"; echo 100 > "$T/bl/intel_backlight/max_brightness"
echo 10 > "$T/bl/intel_backlight/brightness"

mkfifo "$T/mock.in"
"$BUILD/tests/mock-logind" "$T/bl" < "$T/mock.in" > "$T/mock.txt" 2>&1 &
PIDS+=($!)
exec 7>"$T/mock.in"
wait_for 5 'grep -q ready "$T/mock.txt"'

printf '[general]\nmod = super\nterminal = true\nidle_lock = 0\nidle_dpms = 0\nlock_command = swaylock -c 202020\n' > "$T/config.ini"
export WAYLANDX_LOGIND=1 WAYLANDX_BACKLIGHT_DIR="$T/bl"
wx_start 1
wait_for 3 'grep -q "inhibit sleep WaylandX delay" "$T/mock.txt"'
check "takes a delay inhibitor for sleep" grep -q "inhibit sleep WaylandX delay" "$T/mock.txt"

echo "== brightness through logind (no root)"
check "reads the firmware backlight (50%)" bash -c "'$CTL' brightness | grep -q '\"percent\":50'"
"$CTL" brightness +10% >/dev/null
check "+10% asks logind for 600/1000" grep -q "brightness backlight acpi_video0 600" "$T/mock.txt"
"$CTL" brightness 0% >/dev/null
check "never fully dark (1%)" grep -q "brightness backlight acpi_video0 10$" "$T/mock.txt"

echo "== media keys over MPRIS"
"$CTL" media play-pause >/dev/null
"$CTL" media next >/dev/null
wait_for 3 'grep -q "mpris Next" "$T/mock.txt"'
check "play-pause reaches the player" grep -q "mpris PlayPause" "$T/mock.txt"
check "next reaches the player" grep -q "mpris Next" "$T/mock.txt"

echo "== lock before sleep"
echo sleep >&7
wait_for 5 'grep -q "inhibitor released" "$T/mock.txt"'
check "suspend starts the locker" bash -c "'$CTL' status | grep -q '\"locked\":true'"
check "logind is told the session is locked" grep -q "lockedhint 1" "$T/mock.txt"
check "the sleep inhibitor is released once the locker shows" grep -q "inhibitor released" "$T/mock.txt"
echo resume >&7
wait_for 3 'test "$(grep -c "inhibit sleep" "$T/mock.txt")" -ge 2'
check "resume takes a new inhibitor" test "$(grep -c "inhibit sleep" "$T/mock.txt")" -ge 2

echo "== loginctl lock-session"
pkill -f "^swaylock -c 202020" || true
sleep 3.2 # relaunch guard
echo lock >&7
wait_for 5 'pgrep -f "^swaylock -c 202020" >/dev/null'
check "Lock signal starts the locker" pgrep -f "^swaylock -c 202020"
pkill -f "^swaylock -c 202020" || true

exec 7>&-
wx_stop
