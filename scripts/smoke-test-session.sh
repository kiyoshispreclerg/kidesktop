#!/bin/sh
# smoke-test-session.sh - starts a real KiDesktop session on a virtual X
# server with a brand-new HOME (no config of any kind), lets it run for a
# few seconds and checks that every service came up and is still alive.
# Meant for CI after the .deb is installed; works on a dev machine too, as
# long as the KiDesktop binaries are in $PATH and Xvfb is installed.
#
#   SMOKE_SECONDS   how long to let the session run before the final check
#                   (default 15)
#   SMOKE_DISPLAY   X display number to use (default 99)
#
# Processes are matched by name *and* by DISPLAY in their environment, so
# running this inside someone's real session never touches their processes.
set -u

SECONDS_RUN=${SMOKE_SECONDS:-15}
DISP=":${SMOKE_DISPLAY:-99}"
WORK=$(mktemp -d)
LOG="$WORK/kisession.log"
XLOG="$WORK/xvfb.log"
NAMES="kisession kiwm kicomp kiconfd xismenu xisback xispanel xiskeys kimemoryd kistoryd xisserve xisguard"
fail=0
xpid=""
spid=""

export HOME="$WORK/home"
export XDG_RUNTIME_DIR="$WORK/run"
export DISPLAY="$DISP"
mkdir -p "$HOME" "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"
unset XDG_CONFIG_HOME XDG_DATA_HOME XDG_CACHE_HOME DBUS_SESSION_BUS_ADDRESS WAYLAND_DISPLAY

say() { printf 'smoke: %s\n' "$*"; }
bad() { printf 'smoke: FAIL: %s\n' "$*"; fail=1; }

# Prints the PIDs of processes called $1 that belong to our display.
pids_of() {
	for p in $(pgrep -x "$1" 2>/dev/null); do
		if tr '\0' '\n' < "/proc/$p/environ" 2>/dev/null | grep -qx "DISPLAY=$DISP"; then
			echo "$p"
		fi
	done
}

alive() { [ -n "$(pids_of "$1")" ]; }

kill_all() {
	for n in $NAMES; do
		for p in $(pids_of "$n"); do kill "-$1" "$p" 2>/dev/null; done
	done
}

cleanup() {
	# Everything is killed by PID / display match, never by bare name.
	kill_all TERM
	sleep 2
	kill_all KILL
	if [ -n "$xpid" ]; then kill -TERM "$xpid" 2>/dev/null; fi
	if [ "$fail" -ne 0 ]; then
		echo "----- kisession log -----"; cat "$LOG" 2>/dev/null
		echo "----- Xvfb log -----"; cat "$XLOG" 2>/dev/null
		echo "----- configs written -----"
		for f in "$HOME"/.config/*.conf; do echo "== $f"; cat "$f"; done 2>/dev/null
	fi
	rm -rf "$WORK"
}
trap cleanup EXIT

for b in kisession kiwm xisback xispanel xisserve Xvfb xwininfo xdpyinfo; do
	command -v "$b" >/dev/null 2>&1 || { echo "smoke: $b not found in PATH"; exit 2; }
done

Xvfb "$DISP" -screen 0 1280x800x24 +extension RANDR +extension Composite \
	-nolisten tcp >"$XLOG" 2>&1 &
xpid=$!
i=0
while ! xdpyinfo >/dev/null 2>&1 && [ "$i" -lt 50 ]; do
	sleep 0.2; i=$((i + 1))
done
xdpyinfo >/dev/null 2>&1 || { echo "smoke: Xvfb did not start"; fail=1; exit 1; }

say "starting kisession on $DISP with an empty HOME, running ${SECONDS_RUN}s"
kisession >"$LOG" 2>&1 &
spid=$!

sleep "$SECONDS_RUN"

# --- processes ------------------------------------------------------------
# xisguard is a oneshot that exits by itself without the XNOTIFY extension,
# and polkit/locker/audio depend on optional packages: not checked.
for n in kisession kiwm kicomp kiconfd xismenu xisback xispanel xiskeys kimemoryd; do
	if alive "$n"; then say "ok: $n running"; else bad "$n is not running"; fi
done

# --- first-run configs ----------------------------------------------------
for f in xisback.conf xispanel.conf kisession.conf; do
	if [ -s "$HOME/.config/$f" ]; then say "ok: $f created"; else bad "$f was not created"; fi
done

# --- the daemons actually put something on screen -------------------------
tree=$(xwininfo -root -tree 2>/dev/null)
if echo "$tree" | grep -q '"xisback:'; then say "ok: xisback wallpaper window"; else bad "no xisback wallpaper window"; fi
if echo "$tree" | grep -q '"xispanel:'; then say "ok: xispanel panel window"; else bad "no xispanel panel window"; fi

# --- xisserve (what the panel's launcher widget runs) ---------------------
xisserve >"$WORK/xisserve.log" 2>&1 &
sleep 4
if alive xisserve; then
	say "ok: xisserve launcher running"
else
	bad "xisserve launcher did not stay up"
	cat "$WORK/xisserve.log"
fi

if [ "$fail" -eq 0 ]; then say "PASS"; else say "FAILED"; fi
exit "$fail"
