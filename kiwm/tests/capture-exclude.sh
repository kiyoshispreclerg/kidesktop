#!/bin/bash
# kiwm's capture exclusion (PROTOCOL.md): capture_exclude_classes= marks a
# window before it is mapped, and dialogs inherit the mark (value 2).
# Nested Xephyr + kiwm; needs $DISPLAY (the host), Xephyr, xprop.
#
#   kiwm/tests/capture-exclude.sh        (from the repository root)
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
TMP=$(mktemp -d)
D=:${TEST_DISPLAY_NUM:-83}
HOST_DISPLAY=${DISPLAY:?needs the host DISPLAY for Xephyr}
PIDS=(); fails=0
cleanup() { for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; done; sleep 0.5; rm -rf "$TMP"; }
trap cleanup EXIT
ok()   { echo "ok   - $1"; }
fail() { echo "FAIL - $1"; fails=$((fails + 1)); }
expect() { if [ "$2" = "$3" ]; then ok "$1"; else fail "$1 (got '$2', wanted '$3')"; fi; }
export HOME=$TMP XDG_CONFIG_HOME=$TMP/.config; mkdir -p "$XDG_CONFIG_HOME"
MM="$ROOT/kiwm/tests/mapmark"
[ -x "$MM" ] || cc -o "$MM" "$MM.c" -lX11 || exit 2
printf 'capture_exclude_classes = SecretApp, hidden-instance\n' > "$XDG_CONFIG_HOME/kiwm.conf"

DISPLAY=$HOST_DISPLAY Xephyr "$D" -screen 1000x700 >"$TMP/xephyr.log" 2>&1 & XP=$!; PIDS+=($XP)
sleep 1.5; export DISPLAY=$D
"$ROOT/kiwm/kiwm" >"$TMP/kiwm.log" 2>&1 & KP=$!; PIDS+=($KP); sleep 1

mark_of() { # run mapmark in the background, print its mark; leaves it up
    "$MM" "$@" > "$TMP/mm.out" & PIDS+=($!); sleep 1.2
    sed -E 's/.*mark=//' "$TMP/mm.out"
}
wid_of() { sed -E 's/window=(0x[0-9a-f]+).*/\1/' "$TMP/mm.out"; }

expect "a listed class is marked before its first MapNotify" "$(mark_of app SecretApp 0 20)" 1
PARENT=$(wid_of)
expect "...case-insensitively, by instance name too" "$(mark_of Hidden-Instance Other 0 20)" 1
expect "a class that is not listed is not marked" "$(mark_of app OrdinaryApp 0 20)" 0
UNMARKED=$(wid_of)
expect "a dialog of a marked window inherits (2), before it is mapped" "$(mark_of dlg Dialogs "$PARENT" 20)" 2
DIALOG=$(wid_of)
expect "...and so does a dialog of that dialog" "$(mark_of dlg2 Dialogs "$DIALOG" 20)" 2
expect "a dialog of an unmarked window is not marked" "$(mark_of dlg3 Dialogs "$UNMARKED" 20)" 0

# ---- the window menu's entry, through the real menu (kiwm's default theme:
# titlebar ~13 px above the client, entry 181 px below the click) -----------
MM_X=100 MM_Y=150 "$MM" app Plain 0 40 > "$TMP/p.out" & PIDS+=($!); sleep 1.2
PW=$(sed -E 's/window=(0x[0-9a-f]+).*/\1/' "$TMP/p.out")
MM_X=650 MM_Y=300 "$MM" dlg Dialog "$PW" 40 > "$TMP/d.out" & PIDS+=($!); sleep 1.2
DW=$(sed -E 's/window=(0x[0-9a-f]+).*/\1/' "$TMP/d.out")
m() { xprop -id "$1" _KIWM_CAPTURE_EXCLUDE | sed -E 's/.*= *//;s/.*not found.*/-/'; }
pick() { xdotool mousemove "$1" "$2"; sleep 0.3; xdotool click 3; sleep 0.8
         xdotool mousemove $(( $1 + 107 )) $(( $2 + 181 )); sleep 0.3; xdotool click 1; sleep 0.8; }
pick 150 163; expect "menu: marking a window marks its dialog (2)" "$(m "$PW")/$(m "$DW")" "1/2"
pick 150 163; expect "menu: un-marking it clears what it handed down" "$(m "$PW")/$(m "$DW")" "-/-"
pick 700 313; pick 150 163; pick 150 163
expect "menu: a dialog's own mark survives its parent's un-marking" "$(m "$PW")/$(m "$DW")" "-/1"
pick 700 313; pick 150 163; pick 700 313
expect "menu: an inherited mark cannot be unticked on the dialog" "$(m "$PW")/$(m "$DW")" "1/2"

echo; if [ $fails -eq 0 ]; then echo "all passed"; else echo "$fails failed"; exit 1; fi
