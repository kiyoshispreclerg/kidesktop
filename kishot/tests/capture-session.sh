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
    DISPLAY=$HOST_DISPLAY Xephyr "$D" -screen 800x600 >"$TMP/xephyr.log" 2>&1 & XP=$!; PIDS+=($XP)
    sleep 1.5; export DISPLAY=$D
    "$ROOT/kiwm/kiwm" >"$TMP/kiwm.log" 2>&1 & KP=$!; PIDS+=($KP); sleep 0.8
    "$ROOT/kicomp/kicomp" -v >"$TMP/kicomp.log" 2>&1 & CP=$!; PIDS+=($CP); sleep 1.5
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
print('white' if p[0] > 200 and p[1] > 200 else 'black' if sum(p) < 30 else 'other:%s' % (p,))
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
start_stack "capture_censor = hide"
expect "offer: an unmarked window shows" "$(shot)" white
grep -q "capture: session" "$TMP/kicomp.log" && ok "a session was opened" || fail "no session opened"
grep -q "the screen is the censored picture" "$TMP/kishot.log" && ok "nothing hidden showing: the screen is used as is" || fail "the compositor's word was not used"
mark "$XC"; sleep 0.7
expect "offer: a marked window is hidden (visible>0, UNAVAILABLE -> rectangles)" "$(shot)" black
grep -q "own black rectangles" "$TMP/kishot.log" && ok "something hidden showing: own rectangles" || fail "rectangles were not used"
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

echo; if [ $fails -eq 0 ]; then echo "all passed"; else echo "$fails failed"; exit 1; fi
