#!/bin/bash
# Acceptance test for the capture protocol's control plane (kicomp 0.3.66+):
# offer, sessions, the exclusion mark, STATE/epoch, hide_pid, session limits,
# a recorder that dies. Runs a nested Xephyr with kiwm + kicomp; needs
# $DISPLAY (the host X server), Xephyr, xterm, xprop, xdotool.
#
#   kicomp/tests/capture-control.sh            (from the repository root)
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
TMP=$(mktemp -d)
D=:${TEST_DISPLAY_NUM:-79}
HOST_DISPLAY=${DISPLAY:?needs the host DISPLAY for Xephyr}
PIDS=()
fails=0

cleanup() {
    for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; done
    sleep 0.5
    rm -rf "$TMP"
}
trap cleanup EXIT

ok()   { echo "ok   - $1"; }
fail() { echo "FAIL - $1"; fails=$((fails + 1)); }
check() { # check "name" file pattern
    if grep -Eq "$3" "$2"; then ok "$1"; else fail "$1 (wanted /$3/ in $2)"; sed 's/^/       | /' "$2"; fi
}
absent() { if grep -Eq "$3" "$2"; then fail "$1 (unexpected /$3/)"; else ok "$1"; fi; }
wait_for() { # wait_for file pattern [seconds]
    local t=$(( ${3:-5} * 10 ))
    while [ $t -gt 0 ]; do grep -Eq "$2" "$1" 2>/dev/null && return 0; sleep 0.1; t=$((t - 1)); done
    return 1
}

export HOME=$TMP; mkdir -p "$HOME/.config"
start_stack() { # start_stack <kicomp.conf body>
    printf '%s\n' "$1" > "$HOME/.config/kicomp.conf"
    DISPLAY=$HOST_DISPLAY Xephyr "$D" -screen 800x600 >"$TMP/xephyr.log" 2>&1 & XP=$!; PIDS+=($XP)
    sleep 1.5
    export DISPLAY=$D
    "$ROOT/kiwm/kiwm" >"$TMP/kiwm.log" 2>&1 & KP=$!; PIDS+=($KP)
    sleep 0.8
    "$ROOT/kicomp/kicomp" --renderer=xrender -v >"$TMP/kicomp.log" 2>&1 & CP=$!; PIDS+=($CP)
    sleep 1.5
}
stop_stack() {
    kill $CP $KP $XP 2>/dev/null; wait $CP $KP $XP 2>/dev/null
    PIDS=()
    sleep 0.5
}
PROBE="$ROOT/kicomp/tests/capture-probe"
[ -x "$PROBE" ] || cc -o "$PROBE" "$PROBE.c" -lX11 -lXrandr || exit 2

client_of() { xdotool search --pid "$1" 2>/dev/null | head -1; }
mark()   { xprop -id "$1" -f _KIWM_CAPTURE_EXCLUDE 32c -set _KIWM_CAPTURE_EXCLUDE 1; }
unmark() { xprop -id "$1" -remove _KIWM_CAPTURE_EXCLUDE; }

# ---- off: no offer ---------------------------------------------------------
start_stack "[shadow]
enabled=0"
"$PROBE" --seconds 2 > "$TMP/off.out"
check "capture_censor unset: the offer is absent" "$TMP/off.out" '^NO-OFFER'
stop_stack

# ---- on --------------------------------------------------------------------
start_stack "capture_censor = #204060
capture_sessions = 1"
"$PROBE" --seconds 2 > "$TMP/on.out"
check "offer published with the fill colour" "$TMP/on.out" '^OFFER version=1 features=0x1 mode=1 colour=0x204060'
check "session accepted" "$TMP/on.out" '^STATUS state=1 error=0 outputs=1 version=1'
check "TARGETS: one 13-word record, available, 3 slots" "$TMP/on.out" '^TARGET 0 flags=0x1 pos=0,0 size=800x600 pixels=800x600 depth=24 slots=3'
check "STATE starts with nothing visible" "$TMP/on.out" '^STATE 0 epoch=1 visible=0 flags=0x0'
check "CLOSE is processed" "$TMP/on.out" '^CLOSED'

# bad output name -> error 3 (probe has no flag for it: use xprop on a fake S)
# a window of ours, marked and unmarked, seen by a running session
xterm -geometry 30x8+100+100 -e sleep 120 & XT=$!; PIDS+=($XT)
sleep 1.5
XC=$(client_of $XT)
"$PROBE" --request --seconds 7 > "$TMP/live.out" & PR=$!; PIDS+=($PR)
wait_for "$TMP/live.out" '^STATE 0' 3 || fail "session did not start"
absent "unmarked window: not visible" "$TMP/live.out" 'visible=[1-9]'
mark "$XC"
wait_for "$TMP/live.out" 'visible=1' 3 && ok "marking the window makes it visible" || fail "mark not seen"
check "epoch moved on 0 -> 1, the output can still be rendered censored" "$TMP/live.out" 'epoch=2 visible=1 flags=0x0'
unmark "$XC"
wait_for "$TMP/live.out" 'epoch=3 visible=0' 3 && ok "unmarking: back to visible=0, epoch 3" || fail "unmark not seen"
check "REQUEST is answered with a slot" "$TMP/live.out" '^FRAME output=0 slot=0x[0-3] request=[0-9]+ flags=0x0'

# second session while the first runs: capture_sessions = 1
"$PROBE" --seconds 1 > "$TMP/second.out"
check "a second session is refused (out of resources)" "$TMP/second.out" '^STATUS state=0 error=4'

# a recorder that dies costs nothing: its slot is free again
kill -9 $PR; wait $PR 2>/dev/null
sleep 0.5
"$PROBE" --seconds 1 > "$TMP/third.out"
check "after the recorder died, a new session is accepted" "$TMP/third.out" '^STATUS state=1 error=0'

# unknown output name -> error 3
"$PROBE" --output NOPE-9 --seconds 1 > "$TMP/badout.out"
check "an unknown output is refused" "$TMP/badout.out" '^STATUS state=0 error=3'

# the popup rule: an override-redirect window of a marked window's process
# counts, one of another process does not
OW="$ROOT/kicomp/tests/override-window"
[ -x "$OW" ] || cc -o "$OW" "$OW.c" -lX11
mark "$XC"
"$PROBE" --seconds 6 > "$TMP/popup.out" & PR=$!; PIDS+=($PR)
wait_for "$TMP/popup.out" 'epoch=2 visible=1' 3 || fail "marked window not counted before the popup"
"$OW" "$$" 4 & OP=$!; PIDS+=($OP)      # some other process's popup
sleep 1.2
absent "a popup of another process is not excluded" "$TMP/popup.out" 'visible=2'
"$OW" "$XT" 3 & OP2=$!; PIDS+=($OP2)   # the marked window's own popup
wait_for "$TMP/popup.out" 'visible=2' 3 && ok "a popup of the marked window's process is excluded" || fail "popup of the marked process not counted"
wait $PR 2>/dev/null
unmark "$XC"

# hide_pid: windows of that process count too, with no mark
"$PROBE" --hide-pid "$XT" --seconds 2 > "$TMP/hide.out"
check "HIDE_PID counts the process's windows as visible" "$TMP/hide.out" 'visible=1'

stop_stack
echo
if [ $fails -eq 0 ]; then echo "all passed"; else echo "$fails failed"; exit 1; fi
