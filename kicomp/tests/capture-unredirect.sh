#!/bin/bash
# An output handed to a fullscreen window (unredirect.c) and the capture
# protocol: the compositor does not take the output back for a recorder, a
# marked window *behind* the fullscreen one does not count (it is not in the
# picture), and a marked fullscreen window gives a flat picture -- the fill
# colour, or black in hide mode.
#
#   RENDERER=glx kicomp/tests/capture-unredirect.sh   (from the repository root)
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
TMP=$(mktemp -d)
D=:${TEST_DISPLAY_NUM:-82}
HOST_DISPLAY=${DISPLAY:?needs the host DISPLAY for Xephyr}
PIDS=(); fails=0
cleanup() { for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; done; sleep 0.5; rm -rf "$TMP"; }
trap cleanup EXIT
ok()   { echo "ok   - $1"; }
fail() { echo "FAIL - $1"; fails=$((fails + 1)); }
expect() { if [ "$2" = "$3" ]; then ok "$1"; else fail "$1 (got '$2', wanted '$3')"; fi; }
export HOME=$TMP; mkdir -p "$HOME/.config"
PROBE="$ROOT/kicomp/tests/capture-probe"; FS="$ROOT/kicomp/tests/fullscreen"
[ -x "$PROBE" ] || cc -o "$PROBE" "$PROBE.c" -lX11 -lXrandr || exit 2
[ -x "$FS" ] || cc -o "$FS" "$FS.c" -lX11 || exit 2
mark()   { xprop -id "$1" -f _KIWM_CAPTURE_EXCLUDE 32c -set _KIWM_CAPTURE_EXCLUDE 1; }
unmark() { xprop -id "$1" -remove _KIWM_CAPTURE_EXCLUDE; }
pix() { grep -E "^PIXEL" "$1" | head -1 | sed -E 's/.*=//'; }
root_pix() { grep -E "^ROOT" "$1" | head -1 | sed -E 's/.*=//'; }
last_state() { grep -E "^STATE" "$1" | tail -1 | sed -E 's/.*(visible=[0-9]+).*/\1/'; }

run_mode() { # run_mode <capture_censor value> <expected flat colour>
    printf 'capture_censor = %s\nunredirect_fullscreen = 1\n' "$1" > "$HOME/.config/kicomp.conf"
    DISPLAY=$HOST_DISPLAY Xephyr "$D" -screen 800x600 >"$TMP/xephyr.log" 2>&1 & XP=$!; PIDS+=($XP)
    sleep 1.5; export DISPLAY=$D
    "$ROOT/kiwm/kiwm" >"$TMP/kiwm.log" 2>&1 & KP=$!; PIDS+=($KP); sleep 0.8
    "$ROOT/kicomp/kicomp" --renderer=${RENDERER:-xrender} -v >"$TMP/kicomp.log" 2>&1 & CP=$!; PIDS+=($CP); sleep 1.5
    xsetroot -solid '#303030'
    xterm -bg '#00ff00' -geometry 30x8+100+100 -e sleep 120 & XB=$!; PIDS+=($XB); sleep 1.2   # behind
    xterm -bg white -fg black -geometry 40x10+0+0 -e sleep 120 & XF=$!; PIDS+=($XF); sleep 1.5  # will be fullscreen
    CB=$(xdotool search --pid $XB | head -1); CF=$(xdotool search --pid $XF | head -1)
    mark "$CB"                                  # the one BEHIND is marked, the fullscreen one is not
    "$FS" "$CF" 1; sleep 2.5
    grep -q "handed to window" "$TMP/kicomp.log" && ok "[$1] the output is handed to the fullscreen window" \
        || { fail "[$1] no handover"; tail -5 "$TMP/kicomp.log"; }

    "$PROBE" --copy 400,300 --seconds 3 > "$TMP/a.out"
    expect "[$1] a marked window behind a fullscreen one does not count" "$(last_state "$TMP/a.out")" "visible=0"

    mark "$CF"; sleep 1
    "$PROBE" --copy 400,300 --seconds 3 > "$TMP/b.out"
    expect "[$1] the fullscreen window marked: it counts" "$(last_state "$TMP/b.out")" "visible=1"
    expect "[$1] ...and its picture is flat" "$(pix "$TMP/b.out")" "$2"
    expect "[$1] ...while the screen still shows it" "$(root_pix "$TMP/b.out")" "ffffff"
    grep -q "taken back" "$TMP/kicomp.log" && fail "[$1] the output was taken back for the recorder" \
        || ok "[$1] the output stays handed over (nothing is composited for the recorder)"

    unmark "$CF"; sleep 1
    "$PROBE" --copy 400,300 --seconds 3 > "$TMP/c.out"
    expect "[$1] unmarked again: nothing excluded" "$(last_state "$TMP/c.out")" "visible=0"
    kill $XF $XB $CP $KP $XP 2>/dev/null; wait $XF $XB $CP $KP $XP 2>/dev/null; PIDS=(); sleep 1
}
run_mode "#204060" 204060
run_mode "hide" 000000

echo; if [ $fails -eq 0 ]; then echo "all passed"; else echo "$fails failed"; exit 1; fi
