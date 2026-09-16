/*
 * keyboard.c - `xisserve --keyboard`: an on-screen keyboard docked to the
 * bottom of the screen, driven entirely by mouse clicks, that can switch
 * between several independent layouts (QWERTY, QWERTY ABNT2, Emoji, ...)
 * the same way an Android keyboard switches between ABC/123/emoji boards --
 * a tap on the globe-ish "Layout" key cycles kLayouts[] and rebuilds the
 * key rows from that layout's own data table. Adding a layout (AZERTY, a
 * 12-key kana board, a math-symbol board, ...) is just one more KbRow[]
 * array and one more kLayouts[] entry -- everything below this comment
 * block is layout-agnostic engine, nothing in it names a specific layout.
 *
 * Every button synthesizes the real character via XTest, so whatever
 * window last had input focus keeps it and receives the keystrokes -- this
 * window itself never asks for focus (WM_HINTS input=False, the same
 * ICCCM contract xispanel's own panel windows use) and reserves its strip
 * of the screen via _NET_WM_STRUT_PARTIAL like any other dock.
 *
 * Unlike a plain QWERTY board (every key already exists as a real keycode
 * in the active X keymap), a layout like ABNT2's ç/Ç or the emoji board
 * needs characters the loaded keymap has no key for at all. There is no
 * core X11/XTest call to "just send this Unicode codepoint", so this file
 * borrows the same trick `xdotool type` uses: claim one otherwise-unused
 * keycode for the process's lifetime, and before sending a character that
 * has no native key, briefly remap that scratch keycode to the exact
 * keysym Unicode encodes it as (`gdk_unicode_to_keyval()`, which follows
 * the same "keysym = 0x01000000 + codepoint" convention XKB itself uses)
 * and send that instead. See kb_send_utf8().
 *
 * Because every character key already names its own exact unshifted/
 * shifted glyph (KbKeySpec::lo/hi) rather than relying on the receiving
 * application to resolve a physical Shift/Caps-Lock chord, letter case is
 * entirely this on-screen keyboard's own call -- Android-style, not
 * "physical keyboard style". Caps Lock is XOR'd with the on-screen Shift
 * latch to decide upper/lower for is_letter keys specifically (never for
 * digits/punctuation/emoji, matching a real keyboard where Caps Lock only
 * ever affects cased letters); clicking the Caps Lock key also still
 * toggles the *real* server-side lock (and its lamp), so a physical
 * keyboard plugged in at the same time stays in sync.
 *
 * Like --menu/--applications/--question, this stays outside the
 * launcher's own singleton/control-socket machinery -- relaying it into
 * that window would mean fighting over position and the input grab (see
 * appmenu.c's comment on why --menu stays separate; same reasoning). But
 * unlike those one-shot popups, an on-screen keyboard is meant to stay up
 * for a whole session, so it keeps a *small* singleton of its own: the
 * first `xisserve --keyboard` opens it and blocks in its own gtk_main()
 * for as long as it's shown; a second invocation while one is already up
 * reads that instance's PID from its own lock file and sends it SIGTERM
 * instead of opening a second keyboard -- run the same command again to
 * toggle it off, the usual shape a hotkey binding wants. Always an exact
 * PID signal, never pkill/pgrep -f (see the lock-file dance below).
 *
 * Ctrl/Alt/Super are one-shot latches, physically chorded around whatever
 * key is clicked next (real Ctrl+C, Alt+Tab, Super+X on the wire) -- see
 * kb_send_key().
 */
#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <gdk/gdkkeysyms.h>

#include "xisserve.h"

#include <X11/Xatom.h>
#include <X11/XKBlib.h>
#include <X11/extensions/XTest.h>

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#define KB_HEIGHT 224
#define KEY_H 38
#define KEY_GAP 3
#define BASE_KEY_W 40

typedef enum {
    KK_CHAR,     /* lo/hi are the UTF-8 glyphs sent directly -- see kb_send_utf8() */
    KK_ACTION,   /* action_keysym sent as-is (Tab/Enter/BackSpace/Esc/arrows/Space) */
    KK_MODIFIER, /* one-shot latch (Shift/Ctrl/Alt/Super) */
    KK_CAPSLOCK, /* real toggle, own lamp */
    KK_LAYOUT,   /* switches to the next registered layout, see kLayouts[] */
} KbKeyType;

typedef struct {
    KbKeyType type;
    const char *lo, *hi; /* KK_CHAR only: UTF-8 glyph, unshifted/shifted -- hi NULL means
                           * Shift has no effect on this key (most punctuation-free symbols,
                           * every emoji). */
    gboolean is_letter;  /* KK_CHAR only: true if hi is this key's real *case* pair -- see the
                           * file comment on why Caps Lock only applies to these. */
    KeySym action_keysym; /* KK_ACTION only; also identifies which modifier a KK_MODIFIER is */
    const char *label;   /* fixed label -- everything but KK_CHAR */
    double weight;       /* relative width, 1.0 = one normal key */
} KbKeySpec;

typedef struct { const KbKeySpec *keys; int n; } KbRow;
#define ROW(arr) {arr, (int)(sizeof(arr) / sizeof((arr)[0]))}

/* ---- QWERTY (also the base every other Latin-alphabet layout below
 * reuses rows from -- only the row that actually differs needs its own
 * copy, e.g. ABNT2 below only replaces row 2). ------------------------- */

static const KbKeySpec kQwertyRow0[] = {
    {.type = KK_ACTION, .action_keysym = XK_Escape, .label = "Esc", .weight = 1.3},
    {.type = KK_CHAR, .lo = "1", .hi = "!", .weight = 1},
    {.type = KK_CHAR, .lo = "2", .hi = "@", .weight = 1},
    {.type = KK_CHAR, .lo = "3", .hi = "#", .weight = 1},
    {.type = KK_CHAR, .lo = "4", .hi = "$", .weight = 1},
    {.type = KK_CHAR, .lo = "5", .hi = "%", .weight = 1},
    {.type = KK_CHAR, .lo = "6", .hi = "^", .weight = 1},
    {.type = KK_CHAR, .lo = "7", .hi = "&", .weight = 1},
    {.type = KK_CHAR, .lo = "8", .hi = "*", .weight = 1},
    {.type = KK_CHAR, .lo = "9", .hi = "(", .weight = 1},
    {.type = KK_CHAR, .lo = "0", .hi = ")", .weight = 1},
    {.type = KK_CHAR, .lo = "-", .hi = "_", .weight = 1},
    {.type = KK_CHAR, .lo = "=", .hi = "+", .weight = 1},
    {.type = KK_ACTION, .action_keysym = XK_BackSpace, .label = "\xe2\x8c\xab", .weight = 2.0}, /* U+232B */
};

static const KbKeySpec kQwertyRow1[] = {
    {.type = KK_ACTION, .action_keysym = XK_Tab, .label = "Tab", .weight = 1.6},
    {.type = KK_CHAR, .lo = "q", .hi = "Q", .is_letter = TRUE, .weight = 1},
    {.type = KK_CHAR, .lo = "w", .hi = "W", .is_letter = TRUE, .weight = 1},
    {.type = KK_CHAR, .lo = "e", .hi = "E", .is_letter = TRUE, .weight = 1},
    {.type = KK_CHAR, .lo = "r", .hi = "R", .is_letter = TRUE, .weight = 1},
    {.type = KK_CHAR, .lo = "t", .hi = "T", .is_letter = TRUE, .weight = 1},
    {.type = KK_CHAR, .lo = "y", .hi = "Y", .is_letter = TRUE, .weight = 1},
    {.type = KK_CHAR, .lo = "u", .hi = "U", .is_letter = TRUE, .weight = 1},
    {.type = KK_CHAR, .lo = "i", .hi = "I", .is_letter = TRUE, .weight = 1},
    {.type = KK_CHAR, .lo = "o", .hi = "O", .is_letter = TRUE, .weight = 1},
    {.type = KK_CHAR, .lo = "p", .hi = "P", .is_letter = TRUE, .weight = 1},
    {.type = KK_CHAR, .lo = "[", .hi = "{", .weight = 1},
    {.type = KK_CHAR, .lo = "]", .hi = "}", .weight = 1},
    {.type = KK_CHAR, .lo = "\\", .hi = "|", .weight = 1.3},
};

static const KbKeySpec kQwertyRow2[] = {
    {.type = KK_CAPSLOCK, .label = "Caps", .weight = 1.9},
    {.type = KK_CHAR, .lo = "a", .hi = "A", .is_letter = TRUE, .weight = 1},
    {.type = KK_CHAR, .lo = "s", .hi = "S", .is_letter = TRUE, .weight = 1},
    {.type = KK_CHAR, .lo = "d", .hi = "D", .is_letter = TRUE, .weight = 1},
    {.type = KK_CHAR, .lo = "f", .hi = "F", .is_letter = TRUE, .weight = 1},
    {.type = KK_CHAR, .lo = "g", .hi = "G", .is_letter = TRUE, .weight = 1},
    {.type = KK_CHAR, .lo = "h", .hi = "H", .is_letter = TRUE, .weight = 1},
    {.type = KK_CHAR, .lo = "j", .hi = "J", .is_letter = TRUE, .weight = 1},
    {.type = KK_CHAR, .lo = "k", .hi = "K", .is_letter = TRUE, .weight = 1},
    {.type = KK_CHAR, .lo = "l", .hi = "L", .is_letter = TRUE, .weight = 1},
    {.type = KK_CHAR, .lo = ";", .hi = ":", .weight = 1},
    {.type = KK_CHAR, .lo = "'", .hi = "\"", .weight = 1},
    {.type = KK_ACTION, .action_keysym = XK_Return, .label = "Enter", .weight = 2.3},
};

static const KbKeySpec kQwertyRow3[] = {
    {.type = KK_MODIFIER, .action_keysym = XK_Shift_L, .label = "Shift", .weight = 2.4},
    {.type = KK_CHAR, .lo = "z", .hi = "Z", .is_letter = TRUE, .weight = 1},
    {.type = KK_CHAR, .lo = "x", .hi = "X", .is_letter = TRUE, .weight = 1},
    {.type = KK_CHAR, .lo = "c", .hi = "C", .is_letter = TRUE, .weight = 1},
    {.type = KK_CHAR, .lo = "v", .hi = "V", .is_letter = TRUE, .weight = 1},
    {.type = KK_CHAR, .lo = "b", .hi = "B", .is_letter = TRUE, .weight = 1},
    {.type = KK_CHAR, .lo = "n", .hi = "N", .is_letter = TRUE, .weight = 1},
    {.type = KK_CHAR, .lo = "m", .hi = "M", .is_letter = TRUE, .weight = 1},
    {.type = KK_CHAR, .lo = ",", .hi = "<", .weight = 1},
    {.type = KK_CHAR, .lo = ".", .hi = ">", .weight = 1},
    {.type = KK_CHAR, .lo = "/", .hi = "?", .weight = 1},
    {.type = KK_MODIFIER, .action_keysym = XK_Shift_R, .label = "Shift", .weight = 2.4},
};

static const KbKeySpec kQwertyRow4[] = {
    {.type = KK_MODIFIER, .action_keysym = XK_Control_L, .label = "Ctrl", .weight = 1.3},
    {.type = KK_MODIFIER, .action_keysym = XK_Super_L, .label = "Super", .weight = 1.1},
    {.type = KK_MODIFIER, .action_keysym = XK_Alt_L, .label = "Alt", .weight = 1.1},
    {.type = KK_ACTION, .action_keysym = XK_space, .label = "", .weight = 5.0},
    {.type = KK_MODIFIER, .action_keysym = XK_Alt_R, .label = "Alt", .weight = 1.1},
    {.type = KK_LAYOUT, .label = "Layout", .weight = 1.4},
    {.type = KK_ACTION, .action_keysym = XK_Left, .label = "\xe2\x86\x90", .weight = 1},
    {.type = KK_ACTION, .action_keysym = XK_Down, .label = "\xe2\x86\x93", .weight = 1},
    {.type = KK_ACTION, .action_keysym = XK_Up, .label = "\xe2\x86\x91", .weight = 1},
    {.type = KK_ACTION, .action_keysym = XK_Right, .label = "\xe2\x86\x92", .weight = 1},
};

static const KbRow kQwertyRows[] = {
    ROW(kQwertyRow0), ROW(kQwertyRow1), ROW(kQwertyRow2), ROW(kQwertyRow3), ROW(kQwertyRow4),
};

/* ---- QWERTY ABNT2: same board, only the home row differs -- the
 * Brazilian ABNT2 keyboard's most-missed key by far is Ç, sitting where
 * US QWERTY has `;`. Full ABNT2 fidelity would also mean dead keys for
 * ´ ` ^ ~ (a separate press-then-press-vowel step to compose á/à/â/ã) --
 * skipped here: a virtual keyboard can just offer each precomposed
 * accented letter directly on its own key far more easily than a real
 * keyboard's dead-key dance, but that's a bigger per-key data-entry job
 * than this pass covers. Cedilla was the specific ask, so that's what's
 * here; everything else stays byte-for-byte the QWERTY row above. */
static const KbKeySpec kAbnt2Row2[] = {
    {.type = KK_CAPSLOCK, .label = "Caps", .weight = 1.9},
    {.type = KK_CHAR, .lo = "a", .hi = "A", .is_letter = TRUE, .weight = 1},
    {.type = KK_CHAR, .lo = "s", .hi = "S", .is_letter = TRUE, .weight = 1},
    {.type = KK_CHAR, .lo = "d", .hi = "D", .is_letter = TRUE, .weight = 1},
    {.type = KK_CHAR, .lo = "f", .hi = "F", .is_letter = TRUE, .weight = 1},
    {.type = KK_CHAR, .lo = "g", .hi = "G", .is_letter = TRUE, .weight = 1},
    {.type = KK_CHAR, .lo = "h", .hi = "H", .is_letter = TRUE, .weight = 1},
    {.type = KK_CHAR, .lo = "j", .hi = "J", .is_letter = TRUE, .weight = 1},
    {.type = KK_CHAR, .lo = "k", .hi = "K", .is_letter = TRUE, .weight = 1},
    {.type = KK_CHAR, .lo = "l", .hi = "L", .is_letter = TRUE, .weight = 1},
    {.type = KK_CHAR, .lo = "\xc3\xa7", .hi = "\xc3\x87", .is_letter = TRUE, .weight = 1}, /* ç / Ç */
    {.type = KK_CHAR, .lo = "'", .hi = "\"", .weight = 1},
    {.type = KK_ACTION, .action_keysym = XK_Return, .label = "Enter", .weight = 2.3},
};

static const KbRow kAbnt2Rows[] = {
    ROW(kQwertyRow0), ROW(kQwertyRow1), ROW(kAbnt2Row2), ROW(kQwertyRow3), ROW(kQwertyRow4),
};

/* ---- Emoji: no case, no modifiers -- just five rows of common emoji
 * plus a small control row. Each is sent as a single Unicode codepoint
 * (see kb_send_utf8()), so entries needing a combining/ZWJ sequence
 * (skin-tone modifiers, family emoji, flags) are left out on purpose --
 * only the base character would be sent otherwise. */
static const KbKeySpec kEmojiRow0[] = {
    {.type = KK_CHAR, .lo = "\xf0\x9f\x98\x80", .weight = 1}, {.type = KK_CHAR, .lo = "\xf0\x9f\x98\x82", .weight = 1},
    {.type = KK_CHAR, .lo = "\xf0\x9f\x98\x85", .weight = 1}, {.type = KK_CHAR, .lo = "\xf0\x9f\x98\x8a", .weight = 1},
    {.type = KK_CHAR, .lo = "\xf0\x9f\x98\x8d", .weight = 1}, {.type = KK_CHAR, .lo = "\xf0\x9f\x98\x98", .weight = 1},
    {.type = KK_CHAR, .lo = "\xf0\x9f\x98\x9c", .weight = 1}, {.type = KK_CHAR, .lo = "\xf0\x9f\xa4\x94", .weight = 1},
    {.type = KK_CHAR, .lo = "\xf0\x9f\x98\x8e", .weight = 1}, {.type = KK_CHAR, .lo = "\xf0\x9f\x98\xa2", .weight = 1},
    {.type = KK_CHAR, .lo = "\xf0\x9f\x98\xa1", .weight = 1}, {.type = KK_CHAR, .lo = "\xf0\x9f\x98\xb1", .weight = 1},
    {.type = KK_CHAR, .lo = "\xf0\x9f\xa5\xb3", .weight = 1},
};
static const KbKeySpec kEmojiRow1[] = {
    {.type = KK_CHAR, .lo = "\xf0\x9f\x91\x8d", .weight = 1}, {.type = KK_CHAR, .lo = "\xf0\x9f\x91\x8e", .weight = 1},
    {.type = KK_CHAR, .lo = "\xf0\x9f\x91\x8f", .weight = 1}, {.type = KK_CHAR, .lo = "\xf0\x9f\x99\x8f", .weight = 1},
    {.type = KK_CHAR, .lo = "\xf0\x9f\x92\xaa", .weight = 1}, {.type = KK_CHAR, .lo = "\xf0\x9f\x91\x8b", .weight = 1},
    {.type = KK_CHAR, .lo = "\xe2\x9c\x8c", .weight = 1}, {.type = KK_CHAR, .lo = "\xf0\x9f\xa4\x9d", .weight = 1},
    {.type = KK_CHAR, .lo = "\xe2\x9d\xa4", .weight = 1}, {.type = KK_CHAR, .lo = "\xf0\x9f\x92\x94", .weight = 1},
    {.type = KK_CHAR, .lo = "\xf0\x9f\x92\x95", .weight = 1}, {.type = KK_CHAR, .lo = "\xf0\x9f\x94\xa5", .weight = 1},
    {.type = KK_CHAR, .lo = "\xe2\x9c\xa8", .weight = 1},
};
static const KbKeySpec kEmojiRow2[] = {
    {.type = KK_CHAR, .lo = "\xf0\x9f\x90\xb6", .weight = 1}, {.type = KK_CHAR, .lo = "\xf0\x9f\x90\xb1", .weight = 1},
    {.type = KK_CHAR, .lo = "\xf0\x9f\x90\xad", .weight = 1}, {.type = KK_CHAR, .lo = "\xf0\x9f\x90\xb9", .weight = 1},
    {.type = KK_CHAR, .lo = "\xf0\x9f\x90\xb0", .weight = 1}, {.type = KK_CHAR, .lo = "\xf0\x9f\xa6\x8a", .weight = 1},
    {.type = KK_CHAR, .lo = "\xf0\x9f\x90\xbb", .weight = 1}, {.type = KK_CHAR, .lo = "\xf0\x9f\x90\xbc", .weight = 1},
    {.type = KK_CHAR, .lo = "\xf0\x9f\x90\xb8", .weight = 1}, {.type = KK_CHAR, .lo = "\xf0\x9f\x90\xb5", .weight = 1},
    {.type = KK_CHAR, .lo = "\xf0\x9f\x8c\xb8", .weight = 1}, {.type = KK_CHAR, .lo = "\xf0\x9f\x8c\x9e", .weight = 1},
    {.type = KK_CHAR, .lo = "\xf0\x9f\x8c\x99", .weight = 1},
};
static const KbKeySpec kEmojiRow3[] = {
    {.type = KK_CHAR, .lo = "\xf0\x9f\x8d\x8e", .weight = 1}, {.type = KK_CHAR, .lo = "\xf0\x9f\x8d\x95", .weight = 1},
    {.type = KK_CHAR, .lo = "\xf0\x9f\x8d\x94", .weight = 1}, {.type = KK_CHAR, .lo = "\xf0\x9f\x8d\x9f", .weight = 1},
    {.type = KK_CHAR, .lo = "\xf0\x9f\x8d\xa9", .weight = 1}, {.type = KK_CHAR, .lo = "\xf0\x9f\x8d\xa6", .weight = 1},
    {.type = KK_CHAR, .lo = "\xf0\x9f\x8d\xba", .weight = 1}, {.type = KK_CHAR, .lo = "\xe2\x98\x95", .weight = 1},
    {.type = KK_CHAR, .lo = "\xf0\x9f\x8d\xab", .weight = 1}, {.type = KK_CHAR, .lo = "\xf0\x9f\x8d\x87", .weight = 1},
    {.type = KK_CHAR, .lo = "\xf0\x9f\x8d\x89", .weight = 1}, {.type = KK_CHAR, .lo = "\xf0\x9f\xa5\x91", .weight = 1},
    {.type = KK_CHAR, .lo = "\xf0\x9f\x8d\x93", .weight = 1},
};
static const KbKeySpec kEmojiRow4[] = {
    {.type = KK_CHAR, .lo = "\xe2\xad\x90", .weight = 1}, {.type = KK_CHAR, .lo = "\xe2\x9a\xa1", .weight = 1},
    {.type = KK_CHAR, .lo = "\xf0\x9f\x8e\x89", .weight = 1}, {.type = KK_CHAR, .lo = "\xf0\x9f\x8e\x81", .weight = 1},
    {.type = KK_CHAR, .lo = "\xf0\x9f\x93\xb1", .weight = 1}, {.type = KK_CHAR, .lo = "\xf0\x9f\x92\xa1", .weight = 1},
    {.type = KK_CHAR, .lo = "\xf0\x9f\x94\x92", .weight = 1}, {.type = KK_CHAR, .lo = "\xe2\x8f\xb0", .weight = 1},
    {.type = KK_CHAR, .lo = "\xf0\x9f\x93\x8c", .weight = 1}, {.type = KK_CHAR, .lo = "\xe2\x9c\x85", .weight = 1},
    {.type = KK_CHAR, .lo = "\xe2\x9d\x8c", .weight = 1}, {.type = KK_CHAR, .lo = "\xe2\x9d\x93", .weight = 1},
    {.type = KK_CHAR, .lo = "\xe2\x9d\x97", .weight = 1},
};
static const KbKeySpec kEmojiControlRow[] = {
    {.type = KK_LAYOUT, .label = "Layout", .weight = 2.0},
    {.type = KK_ACTION, .action_keysym = XK_space, .label = "", .weight = 5.0},
    {.type = KK_ACTION, .action_keysym = XK_BackSpace, .label = "\xe2\x8c\xab", .weight = 2.0},
    {.type = KK_ACTION, .action_keysym = XK_Return, .label = "Enter", .weight = 2.0},
};

static const KbRow kEmojiRows[] = {
    ROW(kEmojiRow0), ROW(kEmojiRow1), ROW(kEmojiRow2), ROW(kEmojiRow3), ROW(kEmojiRow4), ROW(kEmojiControlRow),
};

/* ---- layout registry: add a layout by adding one more KbRow[] table
 * above and one more entry here -- nothing else in this file needs to
 * know it exists. */
typedef struct {
    const char *name;
    const KbRow *rows;
    int n_rows;
} KbLayout;

static const KbLayout kLayouts[] = {
    {"QWERTY", kQwertyRows, (int)(sizeof(kQwertyRows) / sizeof(kQwertyRows[0]))},
    {"ABNT2", kAbnt2Rows, (int)(sizeof(kAbnt2Rows) / sizeof(kAbnt2Rows[0]))},
    {"Emoji", kEmojiRows, (int)(sizeof(kEmojiRows) / sizeof(kEmojiRows[0]))},
};
#define N_LAYOUTS ((int)(sizeof(kLayouts) / sizeof(kLayouts[0])))
static int g_layout_idx;
static GtkWidget *g_rows_container; /* rebuilt from scratch on every layout switch */

/* One-shot latches -- cleared after the next non-modifier key. Widget
 * pointers are re-resolved on every rebuild_rows() (a layout is free to
 * not offer a given modifier at all -- the Emoji layout offers none of
 * them -- so every use below is NULL-guarded). */
static gboolean g_shift_latched, g_ctrl_latched, g_alt_latched, g_super_latched;
static GtkWidget *g_shift_btns[2], *g_alt_btns[2];
static GtkWidget *g_ctrl_btn, *g_super_btn;
static GtkWidget *g_lamp_caps, *g_lamp_num, *g_lamp_scroll;
static gboolean g_capslock_on; /* mirrors real server state, see poll_indicators() */

/* Every KK_CHAR button on the current layout, so a Shift/Caps change can
 * refresh their glyphs live -- cosmetic only, kb_send_utf8() recomputes
 * the same lo/hi choice independently at click time. */
typedef struct { GtkWidget *btn; const KbKeySpec *spec; } CharKeyWidget;
static CharKeyWidget g_char_keys[256];
static int g_n_char_keys;

static Display *g_dpy;
static KeyCode g_kc_shift, g_kc_ctrl, g_kc_alt, g_kc_super, g_kc_capslock, g_scratch_kc;

static void set_lamp(GtkWidget *lamp, gboolean on)
{
    GdkColor c;
    if (on) gdk_color_parse("#4caf50", &c);
    else gdk_color_parse("#3a3a3a", &c);
    gtk_widget_modify_bg(lamp, GTK_STATE_NORMAL, &c);
}

static void refresh_char_labels(void);

/* Polled rather than event-driven, same call as xisserve.c's own
 * check_parent_alive() -- there's no portable low-overhead "notify me
 * when XKB indicator state changes" short of subscribing to the Xkb
 * extension's own event stream, which isn't worth it for a lamp (and the
 * Caps-Lock-driven letter case, see the file comment) that only need to
 * be eventually-consistent. Bits 0/1/2 are Caps/Num/Scroll Lock on every
 * XKB base ruleset this project targets. */
static gboolean poll_indicators(gpointer data)
{
    (void)data;
    unsigned int state = 0;
    XkbGetIndicatorState(g_dpy, XkbUseCoreKbd, &state);
    gboolean caps = (state & 0x01) != 0;
    set_lamp(g_lamp_caps, caps);
    set_lamp(g_lamp_num, (state & 0x02) != 0);
    set_lamp(g_lamp_scroll, (state & 0x04) != 0);
    if (caps != g_capslock_on) {
        g_capslock_on = caps;
        refresh_char_labels();
    }
    return TRUE;
}

/* Which of a KK_CHAR key's glyphs is currently in effect: for a letter,
 * Caps Lock and the on-screen Shift latch XOR together (Shift cancels
 * Caps for a letter, exactly like a real keyboard); for anything else
 * (digits, punctuation, emoji) only the on-screen Shift latch matters,
 * since real Caps Lock never touches symbol rows either. */
static gboolean char_key_is_hi(const KbKeySpec *spec)
{
    if (!spec->hi) return FALSE;
    if (spec->is_letter) return (g_shift_latched != g_capslock_on);
    return g_shift_latched;
}

static void refresh_char_labels(void)
{
    for (int i = 0; i < g_n_char_keys; i++) {
        const KbKeySpec *spec = g_char_keys[i].spec;
        const char *glyph = char_key_is_hi(spec) ? spec->hi : spec->lo;
        gtk_button_set_label(GTK_BUTTON(g_char_keys[i].btn), glyph);
    }
}

/* Clears every one-shot latch (visually too -- toggling a button off
 * re-enters its own "toggled" handler, which is what actually flips the
 * matching g_*_latched boolean back to FALSE). NULL-guarded since the
 * active layout may not offer a given modifier at all. */
static void clear_latches(void)
{
    if (g_shift_btns[0]) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_shift_btns[0]), FALSE);
    if (g_shift_btns[1]) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_shift_btns[1]), FALSE);
    if (g_ctrl_btn) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_ctrl_btn), FALSE);
    if (g_alt_btns[0]) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_alt_btns[0]), FALSE);
    if (g_alt_btns[1]) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_alt_btns[1]), FALSE);
    if (g_super_btn) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_super_btn), FALSE);
    /* Shift itself has no physical chord to send (see kb_send_utf8()),
     * but it's still a one-shot latch UI-wise. */
    g_shift_latched = FALSE;
}

/* Sends one physical key, wrapped in whatever of Ctrl/Alt/Super is
 * currently latched -- the real chord shape (Ctrl+C, Alt+Tab, Super+X)
 * on the wire. No Shift here: which glyph to send was already decided by
 * the caller (kb_send_utf8()) or is simply not applicable (action keys),
 * see the file comment on why this keyboard resolves case itself rather
 * than delegating to the receiving end's modifier handling. */
static void kb_send_key(KeyCode kc)
{
    if (!kc) return;
    if (g_ctrl_latched && g_kc_ctrl) XTestFakeKeyEvent(g_dpy, g_kc_ctrl, True, 0);
    if (g_alt_latched && g_kc_alt) XTestFakeKeyEvent(g_dpy, g_kc_alt, True, 0);
    if (g_super_latched && g_kc_super) XTestFakeKeyEvent(g_dpy, g_kc_super, True, 0);

    XTestFakeKeyEvent(g_dpy, kc, True, 0);
    XTestFakeKeyEvent(g_dpy, kc, False, 0);

    if (g_super_latched && g_kc_super) XTestFakeKeyEvent(g_dpy, g_kc_super, False, 0);
    if (g_alt_latched && g_kc_alt) XTestFakeKeyEvent(g_dpy, g_kc_alt, False, 0);
    if (g_ctrl_latched && g_kc_ctrl) XTestFakeKeyEvent(g_dpy, g_kc_ctrl, False, 0);
    XFlush(g_dpy);

    clear_latches();
}

/* Resolves one UTF-8 character (one codepoint -- see the emoji tables'
 * comment on why multi-codepoint sequences are avoided) to a keycode and
 * sends it through kb_send_key().
 *
 * XKeysymToKeycode() only promises that *some* level of the keycode it
 * returns carries the keysym we asked for -- on a real layout, a letter
 * or ABNT2's Ç live at level 1 (the *shifted* level) of the exact same
 * keycode their lowercase/unshifted sibling lives on at level 0. Sending
 * that keycode bare would produce level 0's glyph regardless of which
 * one we actually asked for, so this checks which level the target
 * keysym is actually at and physically holds Shift around the event
 * when it isn't level 0 -- the one place this file still synthesizes a
 * real Shift chord, purely as an implementation detail of "how do I
 * reach this exact already-decided glyph", not to decide the glyph
 * itself (that's still entirely char_key_is_hi()'s call, see the file
 * comment). Falls back to the scratch keycode, remapped to exactly this
 * keysym at level 0, when the active keymap has no key for it at all. */
static void kb_send_utf8(const char *utf8)
{
    if (!utf8 || !utf8[0]) return;
    gunichar uc = g_utf8_get_char(utf8);
    guint keyval = gdk_unicode_to_keyval(uc);
    if (!keyval) return;
    KeySym target = (KeySym)keyval;

    KeyCode kc = XKeysymToKeycode(g_dpy, target);
    gboolean need_shift = FALSE;
    if (kc) {
        need_shift = (XkbKeycodeToKeysym(g_dpy, kc, 0, 0) != target);
    } else if (g_scratch_kc) {
        XChangeKeyboardMapping(g_dpy, g_scratch_kc, 1, &target, 1);
        XSync(g_dpy, False);
        kc = g_scratch_kc;
    }
    if (!kc) return;

    if (need_shift && g_kc_shift) XTestFakeKeyEvent(g_dpy, g_kc_shift, True, 0);
    kb_send_key(kc);
    if (need_shift && g_kc_shift) XTestFakeKeyEvent(g_dpy, g_kc_shift, False, 0);
}

/* Claims one keycode we can freely remap for characters the active
 * keymap has no native key for -- prefers one with no symbol at any
 * level in the current map; falls back to the highest keycode in range
 * if the map happens to be completely full (very unlikely, but then
 * XChangeKeyboardMapping just overwrites whatever was there for this
 * process's lifetime, which is still harmless -- nothing else runs
 * between this claim and the process exiting). */
static KeyCode claim_scratch_keycode(void)
{
    int min_kc, max_kc;
    XDisplayKeycodes(g_dpy, &min_kc, &max_kc);
    int per_kc = 0;
    KeySym *map = XGetKeyboardMapping(g_dpy, (KeyCode)min_kc, max_kc - min_kc + 1, &per_kc);
    KeyCode found = 0;
    for (int kc = max_kc; kc >= min_kc && !found; kc--) {
        gboolean empty = TRUE;
        for (int j = 0; j < per_kc; j++) {
            if (map[(kc - min_kc) * per_kc + j] != NoSymbol) {
                empty = FALSE;
                break;
            }
        }
        if (empty) found = (KeyCode)kc;
    }
    XFree(map);
    return found ? found : (KeyCode)max_kc;
}

static void on_action_clicked(GtkWidget *btn, gpointer data)
{
    (void)btn;
    kb_send_key((KeyCode)(guintptr)data);
}

static void on_char_clicked(GtkWidget *btn, gpointer data)
{
    (void)btn;
    const KbKeySpec *spec = data;
    kb_send_utf8(char_key_is_hi(spec) ? spec->hi : spec->lo);
}

static void on_capslock_clicked(GtkWidget *btn, gpointer data)
{
    (void)btn;
    (void)data;
    XTestFakeKeyEvent(g_dpy, g_kc_capslock, True, 0);
    XTestFakeKeyEvent(g_dpy, g_kc_capslock, False, 0);
    XFlush(g_dpy);
    /* Lamp/label state is left to poll_indicators() -- it'll pick up the
     * real change within one tick, same as if a physical keyboard had
     * toggled it. */
}

static void on_shift_toggled(GtkToggleButton *btn, gpointer data)
{
    (void)data;
    gboolean active = gtk_toggle_button_get_active(btn);
    /* Both Shift buttons mirror each other -- clicking either one is
     * "Shift is latched", not two independent modifiers. */
    g_shift_latched = active;
    if (GTK_WIDGET(btn) == g_shift_btns[0] && g_shift_btns[1])
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_shift_btns[1]), active);
    else if (GTK_WIDGET(btn) == g_shift_btns[1] && g_shift_btns[0])
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_shift_btns[0]), active);
    refresh_char_labels();
}

static void on_ctrl_toggled(GtkToggleButton *btn, gpointer data)
{
    (void)data;
    g_ctrl_latched = gtk_toggle_button_get_active(btn);
}

static void on_alt_toggled(GtkToggleButton *btn, gpointer data)
{
    (void)data;
    gboolean active = gtk_toggle_button_get_active(btn);
    g_alt_latched = active;
    if (GTK_WIDGET(btn) == g_alt_btns[0] && g_alt_btns[1])
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_alt_btns[1]), active);
    else if (GTK_WIDGET(btn) == g_alt_btns[1] && g_alt_btns[0])
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_alt_btns[0]), active);
}

static void on_super_toggled(GtkToggleButton *btn, gpointer data)
{
    (void)data;
    g_super_latched = gtk_toggle_button_get_active(btn);
}

static void on_close_clicked(GtkWidget *btn, gpointer data)
{
    (void)btn;
    (void)data;
    gtk_main_quit();
}

static void rebuild_rows(void); /* forward -- on_layout_clicked needs it below */

static void on_layout_clicked(GtkWidget *btn, gpointer data)
{
    (void)btn;
    (void)data;
    g_layout_idx = (g_layout_idx + 1) % N_LAYOUTS;
    rebuild_rows();
}

static GtkWidget *make_key_button(const KbKeySpec *spec)
{
    GtkWidget *btn;

    switch (spec->type) {
    case KK_CHAR:
        btn = gtk_button_new_with_label(char_key_is_hi(spec) ? spec->hi : spec->lo);
        if (g_n_char_keys < (int)(sizeof(g_char_keys) / sizeof(g_char_keys[0]))) {
            g_char_keys[g_n_char_keys].btn = btn;
            g_char_keys[g_n_char_keys].spec = spec;
            g_n_char_keys++;
        }
        g_signal_connect(btn, "clicked", G_CALLBACK(on_char_clicked), (gpointer)spec);
        break;
    case KK_ACTION: {
        KeyCode kc = XKeysymToKeycode(g_dpy, spec->action_keysym);
        btn = gtk_button_new_with_label(spec->label);
        g_signal_connect(btn, "clicked", G_CALLBACK(on_action_clicked), (gpointer)(guintptr)kc);
        break;
    }
    case KK_MODIFIER:
        btn = gtk_toggle_button_new_with_label(spec->label);
        if (spec->action_keysym == XK_Shift_L) {
            g_shift_btns[0] = btn;
            g_signal_connect(btn, "toggled", G_CALLBACK(on_shift_toggled), NULL);
        } else if (spec->action_keysym == XK_Shift_R) {
            g_shift_btns[1] = btn;
            g_signal_connect(btn, "toggled", G_CALLBACK(on_shift_toggled), NULL);
        } else if (spec->action_keysym == XK_Control_L) {
            g_ctrl_btn = btn;
            g_signal_connect(btn, "toggled", G_CALLBACK(on_ctrl_toggled), NULL);
        } else if (spec->action_keysym == XK_Alt_L) {
            g_alt_btns[0] = btn;
            g_signal_connect(btn, "toggled", G_CALLBACK(on_alt_toggled), NULL);
        } else if (spec->action_keysym == XK_Alt_R) {
            g_alt_btns[1] = btn;
            g_signal_connect(btn, "toggled", G_CALLBACK(on_alt_toggled), NULL);
        } else if (spec->action_keysym == XK_Super_L) {
            g_super_btn = btn;
            g_signal_connect(btn, "toggled", G_CALLBACK(on_super_toggled), NULL);
        }
        break;
    case KK_CAPSLOCK:
        btn = gtk_button_new_with_label(spec->label);
        g_signal_connect(btn, "clicked", G_CALLBACK(on_capslock_clicked), NULL);
        break;
    case KK_LAYOUT:
    default:
        btn = gtk_button_new_with_label(spec->label);
        g_signal_connect(btn, "clicked", G_CALLBACK(on_layout_clicked), NULL);
        break;
    }

    gtk_widget_set_size_request(btn, (int)(BASE_KEY_W * spec->weight), KEY_H);
    return btn;
}

static GtkWidget *build_row(const KbRow *row)
{
    GtkWidget *hbox = gtk_hbox_new(FALSE, KEY_GAP);
    for (int i = 0; i < row->n; i++) {
        GtkWidget *btn = make_key_button(&row->keys[i]);
        gtk_box_pack_start(GTK_BOX(hbox), btn, TRUE, TRUE, 0);
    }
    return hbox;
}

/* Tears down the previous layout's row widgets and builds the new one's
 * from scratch -- every per-layout widget pointer (modifier buttons,
 * char-key list) is invalid the moment this starts, so they're all reset
 * before build_row() repopulates whichever of them the new layout uses. */
static void rebuild_rows(void)
{
    GList *children = gtk_container_get_children(GTK_CONTAINER(g_rows_container));
    for (GList *l = children; l; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(children);

    g_n_char_keys = 0;
    g_shift_btns[0] = g_shift_btns[1] = NULL;
    g_alt_btns[0] = g_alt_btns[1] = NULL;
    g_ctrl_btn = g_super_btn = NULL;
    g_shift_latched = g_ctrl_latched = g_alt_latched = g_super_latched = FALSE;

    const KbLayout *layout = &kLayouts[g_layout_idx];
    for (int i = 0; i < layout->n_rows; i++) {
        gtk_box_pack_start(GTK_BOX(g_rows_container), build_row(&layout->rows[i]), TRUE, TRUE, 0);
    }
    gtk_widget_show_all(g_rows_container);
}

static GtkWidget *build_lamp(const char *label_text)
{
    GtkWidget *vbox = gtk_vbox_new(FALSE, 1);
    GtkWidget *lamp = gtk_event_box_new();
    gtk_widget_set_size_request(lamp, 14, 14);
    set_lamp(lamp, FALSE);
    GtkWidget *label = gtk_label_new(label_text);
    gtk_box_pack_start(GTK_BOX(vbox), lamp, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), label, FALSE, FALSE, 0);
    g_object_set_data(G_OBJECT(vbox), "lamp", lamp);
    return vbox;
}

static void reserve_strut(GdkWindow *gw, int x, int y, int w, int h)
{
    (void)y;
    Window xwin = GDK_WINDOW_XID(gw);
    Atom a_partial = XInternAtom(g_dpy, "_NET_WM_STRUT_PARTIAL", False);
    Atom a_strut = XInternAtom(g_dpy, "_NET_WM_STRUT", False);
    Atom a_desktop = XInternAtom(g_dpy, "_NET_WM_DESKTOP", False);

    long partial[12] = {0, 0, 0, h, 0, 0, 0, 0, 0, 0, x, x + w - 1};
    long simple[4] = {0, 0, 0, h};
    XChangeProperty(g_dpy, xwin, a_partial, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)partial, 12);
    XChangeProperty(g_dpy, xwin, a_strut, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)simple, 4);

    long all_desktops = -1;
    XChangeProperty(g_dpy, xwin, a_desktop, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&all_desktops, 1);
}

/* ---- toggle-on-second-invocation singleton ------------------------------
 *
 * Deliberately its own tiny lock, separate from the launcher's
 * xisserve.lock/xisserve.sock -- this mode has nothing to do with that
 * singleton (see the file comment) and would make no sense sharing its
 * state. The lock file's content is the owning PID in decimal, read back
 * by a second invocation so it can signal that *exact* process -- never
 * pkill/pgrep by name or command line, which can hit an unrelated
 * process that happens to share the pattern.
 */
static gboolean g_quit_requested;

static void on_term(int sig)
{
    (void)sig;
    g_quit_requested = TRUE;
}

static gboolean check_quit_requested(gpointer data)
{
    (void)data;
    if (g_quit_requested) gtk_main_quit();
    return TRUE;
}

static void keyboard_lock_path(char *out, size_t outsz)
{
    const char *rundir = getenv("XDG_RUNTIME_DIR");
    if (!rundir || !*rundir) rundir = "/tmp";
    snprintf(out, outsz, "%s/xisserve-keyboard.lock", rundir);
}

int keyboard_run(int output_x, int output_y, int output_w, int output_h)
{
    char lockpath[512];
    keyboard_lock_path(lockpath, sizeof(lockpath));

    int lockfd = open(lockpath, O_CREAT | O_RDWR, 0600);
    if (lockfd < 0) {
        perror("xisserve --keyboard: open lock");
        return 1;
    }

    if (flock(lockfd, LOCK_EX | LOCK_NB) != 0) {
        /* Already running -- read its PID and ask it to close instead of
         * opening a second keyboard. */
        char buf[32] = "";
        ssize_t n = read(lockfd, buf, sizeof(buf) - 1);
        close(lockfd);
        if (n > 0) {
            buf[n] = 0;
            pid_t pid = (pid_t)atol(buf);
            if (pid > 0) kill(pid, SIGTERM);
        }
        return 0;
    }

    /* We hold the lock: record our own PID for the next invocation to
     * read back and signal. */
    char pidbuf[32];
    int pn = snprintf(pidbuf, sizeof(pidbuf), "%ld\n", (long)getpid());
    ssize_t written = write(lockfd, pidbuf, (size_t)pn);
    (void)written;

    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);
    g_timeout_add(200, check_quit_requested, NULL);

    g_dpy = GDK_DISPLAY_XDISPLAY(gdk_display_get_default());
    int xtest_event, xtest_error, xtest_major, xtest_minor;
    if (!XTestQueryExtension(g_dpy, &xtest_event, &xtest_error, &xtest_major, &xtest_minor)) {
        fprintf(stderr, "xisserve --keyboard: X server has no XTEST extension, can't type\n");
        flock(lockfd, LOCK_UN);
        close(lockfd);
        unlink(lockpath);
        return 1;
    }

    g_kc_shift = XKeysymToKeycode(g_dpy, XK_Shift_L);
    g_kc_ctrl = XKeysymToKeycode(g_dpy, XK_Control_L);
    g_kc_alt = XKeysymToKeycode(g_dpy, XK_Alt_L);
    g_kc_super = XKeysymToKeycode(g_dpy, XK_Super_L);
    g_kc_capslock = XKeysymToKeycode(g_dpy, XK_Caps_Lock);
    g_scratch_kc = claim_scratch_keycode();

    if (output_w <= 0) output_w = gdk_screen_get_width(gdk_screen_get_default());
    if (output_h <= 0) output_h = gdk_screen_get_height(gdk_screen_get_default());
    int win_x = output_x, win_y = output_y + output_h - KB_HEIGHT, win_w = output_w, win_h = KB_HEIGHT;

    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), "xisserve-keyboard");
    gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
    gtk_window_set_resizable(GTK_WINDOW(win), FALSE);
    gtk_window_set_type_hint(GTK_WINDOW(win), GDK_WINDOW_TYPE_HINT_DOCK);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(win), TRUE);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(win), TRUE);
    gtk_window_set_keep_above(GTK_WINDOW(win), TRUE);
    /* ICCCM WM_HINTS input=False: this window must never receive input
     * focus, so mouse clicks on its buttons keep typing into whatever
     * window had focus before -- the whole point of an on-screen
     * keyboard. gtk_window_set_accept_focus() is GTK's portable spelling
     * of that hint (xispanel's own dock windows set it the same way, via
     * raw XSetWMHints since xispanel isn't GTK -- see its
     * panel_create_window()). */
    gtk_window_set_accept_focus(GTK_WINDOW(win), FALSE);
    gtk_window_set_focus_on_map(GTK_WINDOW(win), FALSE);
    gtk_window_set_gravity(GTK_WINDOW(win), GDK_GRAVITY_STATIC);
    /* set_default_size() is only a *default* for a resizable window --
     * with resizable(FALSE) above, GTK instead shrink-wraps to the
     * children's natural size unless the window itself is given an
     * explicit minimum, which set_size_request() does. */
    gtk_widget_set_size_request(win, win_w, win_h);
    gtk_window_move(GTK_WINDOW(win), win_x, win_y);

    GtkWidget *vbox = gtk_vbox_new(FALSE, KEY_GAP);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), KEY_GAP);
    gtk_container_add(GTK_CONTAINER(win), vbox);

    GtkWidget *header = gtk_hbox_new(FALSE, 8);
    GtkWidget *lamp_caps_box = build_lamp("Caps");
    GtkWidget *lamp_num_box = build_lamp("Num");
    GtkWidget *lamp_scroll_box = build_lamp("Scroll");
    g_lamp_caps = GTK_WIDGET(g_object_get_data(G_OBJECT(lamp_caps_box), "lamp"));
    g_lamp_num = GTK_WIDGET(g_object_get_data(G_OBJECT(lamp_num_box), "lamp"));
    g_lamp_scroll = GTK_WIDGET(g_object_get_data(G_OBJECT(lamp_scroll_box), "lamp"));
    gtk_box_pack_start(GTK_BOX(header), lamp_caps_box, FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(header), lamp_num_box, FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(header), lamp_scroll_box, FALSE, FALSE, 4);
    GtkWidget *spacer = gtk_label_new(NULL);
    gtk_box_pack_start(GTK_BOX(header), spacer, TRUE, TRUE, 0);
    GtkWidget *close_btn = gtk_button_new_with_label("\xc3\x97"); /* U+00D7 MULTIPLICATION SIGN */
    gtk_widget_set_size_request(close_btn, 28, 22);
    g_signal_connect(close_btn, "clicked", G_CALLBACK(on_close_clicked), NULL);
    gtk_box_pack_end(GTK_BOX(header), close_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), header, FALSE, FALSE, 0);

    g_rows_container = gtk_vbox_new(FALSE, KEY_GAP);
    gtk_box_pack_start(GTK_BOX(vbox), g_rows_container, TRUE, TRUE, 0);
    g_layout_idx = 0;
    rebuild_rows();

    gtk_widget_realize(win);
    reserve_strut(win->window, win_x, win_y, win_w, win_h);

    gtk_widget_show_all(win);
    gtk_window_move(GTK_WINDOW(win), win_x, win_y); /* some WMs only honor this once mapped */

    poll_indicators(NULL);
    g_timeout_add(400, poll_indicators, NULL);

    gtk_main();

    flock(lockfd, LOCK_UN);
    close(lockfd);
    unlink(lockpath);
    return 0;
}
