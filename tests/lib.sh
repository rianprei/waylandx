# Shared helpers for the end-to-end tests. Optionally set WX_BUILD before
# sourcing; call wx_start OUTPUTS after writing "$T/config.ini".
SRC=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BUILD=$(cd "${WX_BUILD:-$SRC/build}" && pwd)
CTL="$BUILD/waylandxctl"
T=$(mktemp -d /tmp/waylandx-test.XXXXXX)
PASS=0
FAIL=0
PIDS=()

cleanup() {
	for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null || true; done
	[ -n "${WX_PID:-}" ] && kill "$WX_PID" 2>/dev/null || true
	wait 2>/dev/null || true
}
trap cleanup EXIT

ok() { PASS=$((PASS + 1)); printf '  \033[32mPASS\033[0m %s\n' "$1"; }
bad() { FAIL=$((FAIL + 1)); printf '  \033[31mFAIL\033[0m %s\n' "$1"; [ $# -gt 1 ] && printf '       %s\n' "$2"; return 0; }
check() { local name=$1; shift; if "$@" >/dev/null 2>&1; then ok "$name"; else bad "$name"; fi; }
field() { # field ID COLUMN  (list --plain: id ws pid app x y w h title)
	"$CTL" info "$1" --plain | cut -f"$2"
}
wait_for() { # wait_for SECONDS 'shell condition' (re-evaluated each try)
	local end=$((SECONDS + $1))
	until eval "$2" >/dev/null 2>&1; do
		[ $SECONDS -ge $end ] && return 0
		sleep 0.1
	done
}
id_of() { "$CTL" list --plain | awk -F'\t' -v t="$1" '$9 == t { print $1; exit }'; }
spawn() { "$@" & PIDS+=($!); }
clean_log() { tr -d '\000' < "$1"; }


wx_start() { # wx_start NUM_OUTPUTS
	echo "== starting WaylandX headless with $1 output(s) (logs: $T/server.log)"
	env -u WAYLAND_DISPLAY -u DISPLAY -u WAYLANDX_SOCK \
		WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS="$1" WLR_LIBINPUT_NO_DEVICES=1 \
		"$BUILD/waylandx" ${WX_DEBUG:+-d} -c "$T/config.ini" > "$T/server.log" 2>&1 &
	WX_PID=$!
	local line=""
	for _ in $(seq 50); do
		line=$(grep -o 'IPC listening on .*' "$T/server.log" || true)
		[ -n "$line" ] && break
		sleep 0.1
	done
	export WAYLANDX_SOCK=${line#IPC listening on }
	# Xwayland started with the session (HiDPI): clients wait for its WM, like autostart does
	if grep -q "Starting Xwayland" "$T/server.log"; then
		wait_for 10 'grep -q "Xwayland ready" "$T/server.log"'
	fi
	check "IPC socket is up and answers ping" "$CTL" ping
	local status
	status=$("$CTL" status)
	WAYLAND_DISPLAY=$(echo "$status" | sed -n 's/.*"wayland_display":"\([^"]*\)".*/\1/p')
	DISPLAY=$(echo "$status" | sed -n 's/.*"x11_display":"\([^"]*\)".*/\1/p')
	export WAYLAND_DISPLAY DISPLAY
}

wx_stop() {
	echo "== shutdown"
	"$CTL" exit >/dev/null || true
	for _ in $(seq 50); do kill -0 "$WX_PID" 2>/dev/null || break; sleep 0.1; done
	if kill -0 "$WX_PID" 2>/dev/null; then bad "exit command"; else
		wait "$WX_PID" && ok "clean exit (status 0)" || bad "exit status $?"
	fi
	WX_PID=""
	if grep -q "AddressSanitizer\|runtime error" "$T/server.log"; then bad "sanitizer report in server log"; fi
	echo
	echo "passed: $PASS  failed: $FAIL  (artifacts in $T)"
	[ "$FAIL" -eq 0 ]
}
