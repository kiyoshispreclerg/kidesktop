#!/bin/bash
# The censored picture itself (kicomp 0.3.67, XRender): a recorder asks for a
# slot, copies one pixel out of it and compares with what the screen shows.
# Nested Xephyr with kiwm + kicomp --renderer=xrender; needs $DISPLAY (host),
# Xephyr, xterm, xprop, xdotool.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
TMP=$(mktemp -d)
D=:${TEST_DISPLAY_NUM:-81}
HOST_DISPLAY=${DISPLAY:?needs the host DISPLAY for Xephyr}
PIDS=(); fails=0
cleanup() { for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; done; sleep 0.5; rm -rf "$TMP"; }
trap cleanup EXIT
ok()   { echo "ok   - $1"; }
fail() { echo "FAIL - $1"; fails=$((fails + 1)); }
export HOME=$TMP; mkdir -p "$HOME/.config"
PROBE="$ROOT/kicomp/tests/capture-probe"
[ -x "$PROBE" ] || cc -o "$PROBE" "$PROBE.c" -lX11 -lXrandr || exit 2

start_stack() { # start_stack <mode>
    printf 'capture_censor = %s\ncapture_sessions = 2\n' "$1" > "$HOME/.config/kicomp.conf"
    # SHADOW=1 ./capture-render.sh: the same, with shadows and the usual effects on
    [ -n "${SHADOW:-}" ] && printf '[shadow]\nenabled=1\nradius=14\nopacity=0.5\n' >> "$HOME/.config/kicomp.conf"
    DISPLAY=$HOST_DISPLAY Xephyr "$D" -screen 800x600 >"$TMP/xephyr.log" 2>&1 & XP=$!; PIDS+=($XP)
    sleep 1.5; export DISPLAY=$D
    "$ROOT/kiwm/kiwm" >"$TMP/kiwm.log" 2>&1 & KP=$!; PIDS+=($KP); sleep 0.8
    "$ROOT/kicomp/kicomp" --renderer=${RENDERER:-xrender} -v >"$TMP/kicomp.log" 2>&1 & CP=$!; PIDS+=($CP); sleep 1.5
    xsetroot -solid '#303030'
    # B underneath (green), A above it (white), overlapping
    xterm -bg '#00ff00' -geometry 30x8+100+100 -e sleep 120 & XB=$!; PIDS+=($XB); sleep 1
    xterm -bg white -fg black -geometry 30x8+140+140 -e sleep 120 & XA=$!; PIDS+=($XA); sleep 1.5
    CA=$(xdotool search --pid $XA | head -1)
}
stop_stack() { kill $XA $XB $CP $KP $XP 2>/dev/null; wait $XA $XB $CP $KP $XP 2>/dev/null; PIDS=(); sleep 0.5; }
mark() { xprop -id "$1" -f _KIWM_CAPTURE_EXCLUDE 32c -set _KIWM_CAPTURE_EXCLUDE 1; }

probe() { # probe <out> <x,y> [extra args...]: request on the first state, copy a pixel
    local out=$1 xy=$2; shift 2
    "$PROBE" --copy "$xy" --seconds 3 "$@" > "$out"
}
pix() { grep -E "^PIXEL" "$1" | head -1 | sed -E 's/.*=//'; }
root_pix() { grep -E "^ROOT" "$1" | head -1 | sed -E 's/.*=//'; }
expect() { if [ "$2" = "$3" ]; then ok "$1"; else fail "$1 (got '$2', wanted '$3')"; fi; }

# ---- fill ---------------------------------------------------------------------
start_stack "#204060"
mark "$CA"; sleep 0.7
# (190,190) is inside A, which also covers B there
probe "$TMP/f1" 190,190
expect "fill: the excluded window is a flat #204060 in the slot" "$(pix "$TMP/f1")" 204060
expect "fill: ... while the screen still shows it (white)" "$(root_pix "$TMP/f1")" ffffff
# (110,110) is B alone: the slot must equal the screen where nothing is excluded
probe "$TMP/f2" 110,110
expect "fill: a window that is not excluded is as on screen" "$(pix "$TMP/f2")" "$(root_pix "$TMP/f2")"
probe "$TMP/f3" 700,500
expect "fill: the desktop is as on screen" "$(pix "$TMP/f3")" "$(root_pix "$TMP/f3")"
# asking again with nothing changed: the same picture, UNCHANGED
probe "$TMP/f4" 190,190 --again
grep -Eq '^FRAME .* flags=0x1' "$TMP/f4" && ok "a second request with nothing changed is UNCHANGED" || { fail "no UNCHANGED frame"; sed 's/^/       | /' "$TMP/f4"; }
# hide_pid: windows of that process are not drawn at all, mark or no mark
probe "$TMP/f5" 190,190 --hide-pid "$XA"
expect "HIDE_PID: the process's window is not drawn (what is behind it shows)" "$(pix "$TMP/f5")" 00ff00
# The slots are redrawn only where something changed since each was last
# drawn: a window moved between two pictures has to be where it went in every
# slot after that, and gone from where it was.
"$PROBE" --copy 600,450 --every 250 --seconds 5 > "$TMP/f6" &
P6=$!; sleep 1.6
CB=$(xdotool search --pid $XB | head -1)
xdotool windowmove "$CB" 520 380; sleep 0.3
xdotool windowraise "$CB" 2>/dev/null
wait $P6
first=$(grep -E '^PIXEL' "$TMP/f6" | head -1 | sed -E 's/.*=//')
last=$(grep -E '^PIXEL' "$TMP/f6" | tail -3 | sed -E 's/.*=//' | sort -u | tr '\n' ' ')
expect "partial redraw: before the move, the desktop is there" "$first" 1c1c1c
expect "partial redraw: after it, every slot shows the window that moved in" "$last" "00ff00 "
# ...and a change of contents alone, which damages only that window (a
# terminal switching its background colour by itself, 2 s in)
xterm -bg '#00ff00' -geometry 20x4+40+520 -e sh -c "sleep 2.5; printf '\\033]11;#ff0000\\007'; sleep 60" & XC2=$!; PIDS+=($XC2)
sleep 1
eval "$(xdotool getwindowgeometry --shell "$(xdotool search --pid $XC2 | head -1)")"
"$PROBE" --copy $((X + 20)),$((Y + 20)) --every 250 --seconds 5 > "$TMP/f7"
first=$(grep -E '^PIXEL' "$TMP/f7" | head -1 | sed -E 's/.*=//')
last=$(grep -E '^PIXEL' "$TMP/f7" | tail -3 | sed -E 's/.*=//' | sort -u | tr '\n' ' ')
expect "partial redraw: contents before the change" "$first" 00ff00
expect "partial redraw: every slot picks up a change of contents" "$last" "ff0000 "
kill $XC2 2>/dev/null
stop_stack

# ---- hide ---------------------------------------------------------------------
start_stack "hide"
mark "$CA"; sleep 0.7
probe "$TMP/h1" 190,190
expect "hide: the window below shows through where the excluded one was" "$(pix "$TMP/h1")" 00ff00
probe "$TMP/h2" 700,500
expect "hide: the desktop is as on screen" "$(pix "$TMP/h2")" "$(root_pix "$TMP/h2")"
# a window with a mark that is later removed: back to what the screen shows
xprop -id "$CA" -remove _KIWM_CAPTURE_EXCLUDE; sleep 0.7
probe "$TMP/h3" 190,190
expect "unmarked again: the slot shows the window" "$(pix "$TMP/h3")" ffffff
stop_stack

echo; if [ $fails -eq 0 ]; then echo "all passed"; else echo "$fails failed"; exit 1; fi
