#!/bin/bash
# kishot's censoring on a scaled output (kicomp scale=2, X-INPUT-SCALE): the
# compositor's censored picture is read at the scanout size, and, with the
# compositor out of the way, kishot's own rectangles land on the scaled
# geometry. Needs the fork's Xephyr (XEPHYR=...; the system one has no
# X-INPUT-SCALE), plus xterm, xprop, xdotool, python3-PIL.
#
#   kishot/tests/capture-scaled.sh        (from the repository root; needs $DISPLAY)
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
XEPHYR=${XEPHYR:-$HOME/Downloads/kiyoshi-forks/xserver/build-test-debug/hw/kdrive/ephyr/Xephyr}
[ -x "$XEPHYR" ] || { echo "skip - no Xephyr with X-INPUT-SCALE (set XEPHYR)"; exit 0; }
HOST_DISPLAY=${DISPLAY:?needs the host DISPLAY for Xephyr}
REAL_HOME=$HOME
TMP=$(mktemp -d)
D=:${TEST_DISPLAY_NUM:-82}
PIDS=(); fails=0
cleanup() { for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; done; sleep 0.5; rm -rf "$TMP"; }
trap cleanup EXIT
ok()   { echo "ok   - $1"; }
fail() { echo "FAIL - $1"; fails=$((fails + 1)); }
export HOME=$TMP; mkdir -p "$HOME/.config"

# stack <capture_censor>; the white xterm is at logical +100+126 (184x108),
# the green one at +450+100: on the 2x output, physical 200..568 x 252..468
printf 'capture_censor = %s\ncapture_sessions = 2\n[output:default]\nscale=2\n' "${1:-hide}" > "$HOME/.config/kicomp.conf"
DISPLAY=$HOST_DISPLAY "$XEPHYR" "$D" -screen 1600x900 -extension XNOTIFY >"$TMP/xephyr.log" 2>&1 & PIDS+=($!)
sleep 1.5; export DISPLAY=$D
"$ROOT/kiwm/kiwm" >"$TMP/kiwm.log" 2>&1 & PIDS+=($!); sleep 0.8
"$ROOT/kicomp/kicomp" --renderer=xrender -v >"$TMP/kicomp.log" 2>&1 & PIDS+=($!); sleep 1.5
grep -q "scale 2.00x" "$TMP/kicomp.log" || { fail "the output is not scaled"; exit 1; }
xsetroot -solid '#303030'
xterm -bg white -fg black -geometry 30x8+100+100 -e sleep 120 & XT=$!; PIDS+=($XT)
xterm -bg '#00ff00' -geometry 20x6+450+100 -e sleep 120 & PIDS+=($!); sleep 2
XC=$(xdotool search --pid $XT | head -1)

shot() { # shot <white-point> <green-point>: "<white pixel>|<green pixel>"
    rm -rf "$HOME/Kishot" "$HOME/Screenshots"
    KISHOT_DEBUG=1 "$ROOT/kishot/kishot" -f -b >"$TMP/kishot.log" 2>&1
    python3 - "$(find "$HOME" -name '*.png' | head -1)" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert('RGB')
print(im.size == (1600, 900), im.getpixel((400, 400)), im.getpixel((1000, 300)))
PY
}
r=$(shot)
[ "$r" = "True (255, 255, 255) (0, 255, 0)" ] && ok "unmarked: both windows show" || fail "unmarked ($r)"
xprop -id "$XC" -f _KIWM_CAPTURE_EXCLUDE 32c -set _KIWM_CAPTURE_EXCLUDE 1; sleep 0.7
r=$(shot)
[ "$r" = "True (28, 28, 28) (0, 255, 0)" ] && ok "marked: hidden where it is on the scaled screen, the other stays" || fail "marked ($r)"
grep -q "censored picture (slot)" "$TMP/kishot.log" && ok "the compositor's picture was used" || fail "slot not used"
exit $((fails != 0))
