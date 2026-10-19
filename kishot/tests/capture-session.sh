#!/bin/bash
# kishot against kicomp's capture protocol, in a nested X server (kiwm +
# kicomp). Checks that a screenshot hides a marked window whatever the path
# (own rectangles, or the compositor's word that nothing hidden shows), and
# that a session is opened only when the compositor offers one.
#
#   kishot/tests/capture-session.sh        (from the repository root; needs $DISPLAY,
#                                           Xephyr, xterm, xprop, xdotool, python3-PIL)
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
TMP=$(mktemp -d)
D=:${TEST_DISPLAY_NUM:-80}
HOST_DISPLAY=${DISPLAY:?needs the host DISPLAY for Xephyr}
PIDS=(); fails=0
cleanup() { for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; done; sleep 0.5; rm -rf "$TMP"; }
trap cleanup EXIT
ok()   { echo "ok   - $1"; }
fail() { echo "FAIL - $1"; fails=$((fails + 1)); }
export HOME=$TMP; mkdir -p "$HOME/.config"

start_stack() { # start_stack <kicomp.conf body>
    printf '%s\n' "$1" > "$HOME/.config/kicomp.conf"
    DISPLAY=$HOST_DISPLAY Xephyr "$D" -screen 1600x900 >"$TMP/xephyr.log" 2>&1 & XP=$!; PIDS+=($XP)
    sleep 1.5; export DISPLAY=$D
    "$ROOT/kiwm/kiwm" >"$TMP/kiwm.log" 2>&1 & KP=$!; PIDS+=($KP); sleep 0.8
    "$ROOT/kicomp/kicomp" --renderer=xrender -v >"$TMP/kicomp.log" 2>&1 & CP=$!; PIDS+=($CP); sleep 1.5
    xsetroot -solid '#303030'
    xterm -bg white -fg black -geometry 30x8+100+100 -e sleep 120 & XT=$!; PIDS+=($XT); sleep 1.5
    XC=$(xdotool search --pid $XT | head -1)
}
stop_stack() { kill $XT $CP $KP $XP 2>/dev/null; wait $XT $CP $KP $XP 2>/dev/null; PIDS=(); sleep 0.5; }

shot() { # shot <name>: prints "white" if the xterm shows, "black" if hidden
    rm -rf "$HOME/Screenshots"
    KISHOT_DEBUG=1 "$ROOT/kishot/kishot" -f -b >"$TMP/kishot.log" 2>&1
    local f; f=$(ls "$HOME"/Screenshots/*.png 2>/dev/null | head -1)
    [ -n "$f" ] || { echo none; return; }
    python3 - "$f" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert('RGB')
p = im.getpixel((160, 160))
print('white' if p[0] > 200 and p[1] > 200 else 'black' if sum(p) < 30
      else 'gray' if p == (28, 28, 28) else 'other:%s' % (p,))
PY
}
expect() { # expect <name> <got> <want>
    if [ "$2" = "$3" ]; then ok "$1"; else fail "$1 (got $2, wanted $3)"; fi
}
mark()   { xprop -id "$1" -f _KIWM_CAPTURE_EXCLUDE 32c -set _KIWM_CAPTURE_EXCLUDE 1; }
unmark() { xprop -id "$1" -remove _KIWM_CAPTURE_EXCLUDE; }

# ---- the compositor does not offer capture ----------------------------------
start_stack "capture_censor = off"
expect "no offer: an unmarked window shows" "$(shot)" white
mark "$XC"; sleep 0.7
expect "no offer: a marked window is hidden (own rectangles)" "$(shot)" black
grep -q "capture: session" "$TMP/kicomp.log" && fail "a session was opened without an offer" || ok "no session without an offer"
stop_stack

# ---- the compositor offers it -------------------------------------------------
start_stack "capture_censor = black"

# ---- the window must keep hearing its input -----------------------------------
# (watching the marks once replaced the event mask GDK chose for kishot's own
# window, which froze it: no clicks, no keys)
"$ROOT/kishot/kishot" >"$TMP/gui.log" 2>&1 & GUI=$!; PIDS+=($GUI); sleep 3
GW=$(xdotool search --pid $GUI --onlyvisible | tail -1)
xdotool windowactivate --sync "$GW"; sleep 0.5
panel() { # the options panel only: the preview changes by itself
    xwd -silent -id "$GW" -out "$TMP/g.xwd"
    python3 - "$TMP/g.xwd" <<'PY'
import struct, sys, hashlib
d = open(sys.argv[1], "rb").read()
hs = struct.unpack(">I", d[:4])[0]; w, h = struct.unpack(">II", d[16:24])
bpl = struct.unpack(">I", d[48:52])[0]; nc = struct.unpack(">I", d[76:80])[0]
off = hs + nc * 12
rows = [d[off + y * bpl + (w - 340) * 4: off + y * bpl + w * 4] for y in range(30, 400)]
print(hashlib.md5(b"".join(rows)).hexdigest())
PY
}
p1=$(panel); xdotool key Tab; sleep 0.3; xdotool key ctrl+Next; sleep 1; p2=$(panel)   # focus into the page, then the next tab
if [ "$p1" = "$p2" ]; then fail "kishot's window ignores the keyboard (no tab switch)"; else ok "kishot's window still switches tabs from the keyboard"; fi
kill $GUI 2>/dev/null; sleep 0.5

expect "offer: an unmarked window shows" "$(shot)" white
grep -q "capture: session" "$TMP/kicomp.log" && ok "a session was opened" || fail "no session opened"
grep -q "the screen is the censored picture" "$TMP/kishot.log" && ok "nothing hidden showing: the screen is used as is" || fail "the compositor's word was not used"
mark "$XC"; sleep 0.7
expect "offer: a marked window is hidden" "$(shot)" black
grep -q "the compositor's censored picture (slot)" "$TMP/kishot.log" && ok "something hidden showing: read from the compositor's slot" || { fail "the slot was not used"; cat "$TMP/kishot.log"; }
unmark "$XC"; sleep 0.7
expect "offer: unmarked again, it shows" "$(shot)" white

# many shots racing a window being marked and unmarked: whatever the timing,
# a shot taken while it is marked and settled must not show it
bad=0
for i in 1 2 3 4 5 6; do
    mark "$XC"; sleep 0.5
    [ "$(shot)" = black ] || bad=$((bad + 1))
    unmark "$XC"; sleep 0.5
done
[ $bad -eq 0 ] && ok "6 mark/shot cycles: never leaked" || fail "$bad of 6 shots leaked a marked window"

# hide mode: what is behind the window shows (the compositor's default ground, 0x1c1c1c), not black
kill $CP; wait $CP 2>/dev/null; sleep 0.5
printf 'capture_censor = hide\n' > "$HOME/.config/kicomp.conf"
"$ROOT/kicomp/kicomp" --renderer=xrender -v >"$TMP/kicomp.log" 2>&1 & CP=$!; PIDS+=($CP); sleep 1.5
mark "$XC"; sleep 0.7
expect "hide mode: the desktop shows where the window was" "$(shot)" gray
grep -q "the compositor's censored picture (slot)" "$TMP/kishot.log" && ok "... read from the slot" || fail "hide: the slot was not used"
unmark "$XC"; sleep 0.7

# kishot has a target of its own (censor_classes): the compositor's "nothing
# hidden shows" cannot be trusted to mean the shot is clean
printf 'censor_classes=XTerm\n' > "$HOME/.config/kishot.conf"
expect "offer + censor_classes: an unmarked listed window is still hidden" "$(shot)" black
rm -f "$HOME/.config/kishot.conf"

# a compositor that disappears: kishot must not trust a stale STATE
kill -9 $CP; wait $CP 2>/dev/null; sleep 0.5
mark "$XC"; sleep 0.5
expect "compositor gone: a marked window is still hidden" "$(shot)" black
stop_stack

# ---- a recording (the SHM path in a nested server): the compositor's colour --
if command -v ffmpeg >/dev/null; then
    start_stack "capture_censor = #204060"
    mark "$XC"; sleep 0.7
    mkdir -p "$HOME/Videos"
    "$ROOT/kishot/kishot" -fbR >"$TMP/rec1.log" 2>&1 &
    sleep 3
    "$ROOT/kishot/kishot" -fbR >"$TMP/rec2.log" 2>&1; sleep 3
    V=$(ls "$HOME"/Videos/*.mkv "$HOME"/*.mkv 2>/dev/null | head -1)
    if [ -n "$V" ] && ffmpeg -loglevel error -y -ss 1 -i "$V" -frames:v 1 "$TMP/frame.png"; then
        got=$(python3 - "$TMP/frame.png" <<'PY'
import sys
from PIL import Image
p = Image.open(sys.argv[1]).convert('RGB').getpixel((160, 160))
print('fill' if all(abs(a - b) <= 8 for a, b in zip(p, (0x20, 0x40, 0x60))) else 'black' if sum(p) < 30 else 'other:%s' % (p,))
PY
)
        expect "recording: the marked window is the compositor's fill colour, not black" "$got" fill
    else
        fail "no recording was made"; cat "$TMP/rec1.log" "$TMP/rec2.log"
    fi
    stop_stack
fi

echo; if [ $fails -eq 0 ]; then echo "all passed"; else echo "$fails failed"; exit 1; fi
