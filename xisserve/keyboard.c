/*
 * keyboard.c - `xisserve --keyboard`: an on-screen keyboard docked to the
 * bottom of the screen, driven entirely by mouse clicks, that can switch
 * between several independent layouts (QWERTY, QWERTY ABNT2, Emoji, ...)
 * the same way an Android keyboard switches between ABC/123/emoji boards --
 * a menu on the header's layout button picks one and rebuilds the key rows
 * from that layout's own data file. Layouts are *.layout files loaded at
 * startup (see "layouts are data, not code" below), so adding one (AZERTY,
 * a math-symbol board, ...) never needs a rebuild -- nothing in the engine
 * names a specific layout.
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
#include <X11/extensions/Xrandr.h>

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
#define KEY_MIN_H 20 /* keys stretch with the window; this is only their floor */
#define KB_MIN_H 185
#define KEY_GAP 3
#define BASE_KEY_W 40

typedef enum {
    KK_CHAR,     /* lo/hi are the UTF-8 glyphs sent directly -- see kb_send_utf8() */
    KK_ACTION,   /* action_keysym sent as-is (Tab/Enter/BackSpace/Esc/arrows/Space) */
    KK_MODIFIER, /* one-shot latch (Shift/Ctrl/Alt/Super) */
    KK_CAPSLOCK, /* real toggle, own lamp */
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

/* ---- layouts are data, not code ---------------------------------------
 *
 * Every layout is a *.layout text file (see layouts/10-qwerty.layout for
 * the format) loaded at startup from, in this order: $XISSERVE_KBDIR (if
 * set), ~/.config/xisserve/keyboard, XISSERVE_KBDIR_DEFAULT, <exe>/layouts.
 * A file of the same name in an earlier directory shadows a later one, so
 * installing or overriding a layout never needs a rebuild. Layouts are
 * listed sorted by file name.
 */
typedef struct { const KbKeySpec *keys; int n; } KbRow;

typedef struct {
    char *name;
    gboolean emoji; /* kind = emoji: the picker UI, rows are only its bottom row */
    KbRow *rows;
    int n_rows;
} KbLayout;

static GPtrArray *g_layouts; /* KbLayout* */
static int g_layout_idx;
static GtkWidget *g_rows_container; /* rebuilt from scratch on every layout switch */
static GtkWidget *g_layout_btn;
static int g_win_w;
static GtkWidget *g_win;
static int g_out_x, g_out_y, g_out_h; /* output the dock sits on */
static int g_pref_h = KB_HEIGHT;      /* user's chosen height, persisted */
static int g_applied_h;               /* height the window/strut currently has */
static gboolean g_grip_dragging;
static void apply_height(int h);

#ifndef XISSERVE_KBDIR_DEFAULT
#define XISSERVE_KBDIR_DEFAULT "/usr/local/share/xisserve/keyboard"
#endif
#ifndef XISSERVE_EMOJI_DEFAULT
#define XISSERVE_EMOJI_DEFAULT "/usr/local/share/xisserve/emoji-test.txt"
#endif

static const struct {
    const char *tok;
    KbKeyType type;
    KeySym ks;
    const char *label;
    double w;
} kSpecialKeys[] = {
    {"esc", KK_ACTION, XK_Escape, "Esc", 1.3},
    {"tab", KK_ACTION, XK_Tab, "Tab", 1.6},
    {"bksp", KK_ACTION, XK_BackSpace, "\xe2\x8c\xab", 2.0}, /* U+232B */
    {"enter", KK_ACTION, XK_Return, "Enter", 2.0},
    {"space", KK_ACTION, XK_space, "", 5.0},
    {"left", KK_ACTION, XK_Left, "\xe2\x86\x90", 1},
    {"down", KK_ACTION, XK_Down, "\xe2\x86\x93", 1},
    {"up", KK_ACTION, XK_Up, "\xe2\x86\x91", 1},
    {"right", KK_ACTION, XK_Right, "\xe2\x86\x92", 1},
    {"caps", KK_CAPSLOCK, 0, "Caps", 1.9},
    {"shiftL", KK_MODIFIER, XK_Shift_L, "Shift", 2.4},
    {"shiftR", KK_MODIFIER, XK_Shift_R, "Shift", 2.4},
    {"ctrl", KK_MODIFIER, XK_Control_L, "Ctrl", 1.3},
    {"super", KK_MODIFIER, XK_Super_L, "Super", 1.1},
    {"altL", KK_MODIFIER, XK_Alt_L, "Alt", 1.1},
    {"altR", KK_MODIFIER, XK_Alt_R, "Alt", 1.1},
};

/* "tok" or "tok=1.5" -> key spec; FALSE (and a warning) when unusable. A
 * trailing =W is a width only when the '=' isn't the token's first char
 * and what follows is a number, so the "=+" key still parses as a key. */
static gboolean parse_key_token(const char *file, const char *tok, KbKeySpec *out)
{
    char *t = g_strdup(tok);
    double weight = 0;
    char *eq = strrchr(t, '=');
    if (eq && eq != t && eq[1]) {
        char *endp = NULL;
        double w = g_ascii_strtod(eq + 1, &endp);
        if (endp && !*endp && w > 0) {
            weight = w;
            *eq = 0;
        }
    }
    memset(out, 0, sizeof(*out));
    gboolean ok = FALSE;
    if (t[0] == '@') {
        for (size_t i = 0; i < sizeof(kSpecialKeys) / sizeof(kSpecialKeys[0]); i++) {
            if (strcmp(t + 1, kSpecialKeys[i].tok) != 0) continue;
            out->type = kSpecialKeys[i].type;
            out->action_keysym = kSpecialKeys[i].ks;
            out->label = kSpecialKeys[i].label;
            out->weight = weight > 0 ? weight : kSpecialKeys[i].w;
            ok = TRUE;
            break;
        }
    }
    if (!ok && g_utf8_validate(t, -1, NULL)) {
        glong n = g_utf8_strlen(t, -1);
        if (n == 1 || n == 2) {
            out->type = KK_CHAR;
            out->lo = g_strndup(t, (gsize)(g_utf8_offset_to_pointer(t, 1) - t));
            if (n == 2) {
                out->hi = g_strdup(g_utf8_offset_to_pointer(t, 1));
                out->is_letter = g_unichar_isalpha(g_utf8_get_char(out->lo)) &&
                                 g_unichar_toupper(g_utf8_get_char(out->lo)) == g_utf8_get_char(out->hi);
            }
            out->weight = weight > 0 ? weight : 1;
            ok = TRUE;
        }
    }
    if (!ok) fprintf(stderr, "xisserve --keyboard: %s: bad key '%s'\n", file, tok);
    g_free(t);
    return ok;
}

static KbLayout *load_layout_file(const char *path)
{
    gchar *text = NULL;
    if (!g_file_get_contents(path, &text, NULL, NULL)) return NULL;

    KbLayout *l = g_new0(KbLayout, 1);
    GArray *rows = g_array_new(FALSE, TRUE, sizeof(KbRow));
    gchar **lines = g_strsplit(text, "\n", -1);
    for (int i = 0; lines[i]; i++) {
        char *line = g_strstrip(lines[i]);
        if (!*line || *line == '#') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = g_strstrip(line), *val = g_strstrip(eq + 1);
        if (!strcmp(key, "name")) {
            g_free(l->name);
            l->name = g_strdup(val);
        } else if (!strcmp(key, "kind")) {
            l->emoji = !strcmp(val, "emoji");
        } else if (!strcmp(key, "row")) {
            gchar **toks = g_strsplit_set(val, " \t", -1);
            GArray *keys = g_array_new(FALSE, TRUE, sizeof(KbKeySpec));
            for (int j = 0; toks[j]; j++) {
                KbKeySpec spec;
                if (toks[j][0] && parse_key_token(path, toks[j], &spec)) g_array_append_val(keys, spec);
            }
            g_strfreev(toks);
            KbRow row = {.n = (int)keys->len};
            row.keys = (KbKeySpec *)g_array_free(keys, FALSE);
            if (row.n) g_array_append_val(rows, row);
        }
    }
    g_strfreev(lines);
    g_free(text);
    l->n_rows = (int)rows->len;
    l->rows = (KbRow *)g_array_free(rows, FALSE);
    if (!l->name) {
        char *base = g_path_get_basename(path);
        l->name = g_strdup(base);
        g_free(base);
    }
    if (!l->n_rows && !l->emoji) {
        fprintf(stderr, "xisserve --keyboard: %s: no rows, ignored\n", path);
        g_free(l->name);
        g_free(l);
        return NULL;
    }
    return l;
}

static void load_layouts(void)
{
    g_layouts = g_ptr_array_new();
    GPtrArray *dirs = g_ptr_array_new_with_free_func(g_free);
    const char *env = g_getenv("XISSERVE_KBDIR");
    if (env && *env) g_ptr_array_add(dirs, g_strdup(env));
    g_ptr_array_add(dirs, g_build_filename(g_get_user_config_dir(), "xisserve", "keyboard", NULL));
    g_ptr_array_add(dirs, g_strdup(XISSERVE_KBDIR_DEFAULT));
    char *exe = g_file_read_link("/proc/self/exe", NULL);
    if (exe) {
        char *d = g_path_get_dirname(exe);
        g_ptr_array_add(dirs, g_build_filename(d, "layouts", NULL));
        g_free(d);
        g_free(exe);
    }

    /* file name -> full path, first directory wins */
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    for (guint i = 0; i < dirs->len; i++) {
        GDir *d = g_dir_open(dirs->pdata[i], 0, NULL);
        if (!d) continue;
        const char *fn;
        while ((fn = g_dir_read_name(d))) {
            if (!g_str_has_suffix(fn, ".layout") || g_hash_table_contains(seen, fn)) continue;
            g_hash_table_insert(seen, g_strdup(fn), g_build_filename(dirs->pdata[i], fn, NULL));
        }
        g_dir_close(d);
    }
    GList *names = g_list_sort(g_hash_table_get_keys(seen), (GCompareFunc)strcmp);
    for (GList *n = names; n; n = n->next) {
        KbLayout *l = load_layout_file(g_hash_table_lookup(seen, n->data));
        if (l) g_ptr_array_add(g_layouts, l);
    }
    g_list_free(names);
    g_hash_table_destroy(seen);
    g_ptr_array_free(dirs, TRUE);
}

/* ---- emoji data (emoji-test.txt) ---------------------------------------
 *
 * The full Unicode emoji list, so the picker shows everything rather than
 * a hand-picked subset. Read from the system's unicode-data copy when
 * present, else the one this package installs. Only fully-qualified
 * entries without a skin-tone modifier are listed (the tone variants would
 * multiply the grid ~5x for no browsing value).
 */
typedef struct {
    char *glyph;
    char *name;
    int group;
} EmojiEntry;

static GArray *g_emoji;       /* EmojiEntry */
static GPtrArray *g_emoji_groups; /* char* names */

static gboolean emoji_has_skin_tone(const char *s)
{
    for (const char *p = s; *p; p = g_utf8_next_char(p)) {
        gunichar c = g_utf8_get_char(p);
        if (c >= 0x1F3FB && c <= 0x1F3FF) return TRUE;
    }
    return FALSE;
}

static void load_emoji(void)
{
    if (g_emoji) return;
    g_emoji = g_array_new(FALSE, TRUE, sizeof(EmojiEntry));
    g_emoji_groups = g_ptr_array_new();

    const char *paths[] = {"/usr/share/unicode/emoji/emoji-test.txt", XISSERVE_EMOJI_DEFAULT, NULL, NULL};
    char *local = g_build_filename(g_get_user_data_dir(), "xisserve", "emoji-test.txt", NULL);
    char *exe = g_file_read_link("/proc/self/exe", NULL), *exe_dir = exe ? g_path_get_dirname(exe) : NULL;
    char *beside = exe_dir ? g_build_filename(exe_dir, "data", "emoji-test.txt", NULL) : NULL;
    paths[2] = local;
    paths[3] = beside;
    /* user copy wins, so a newer emoji-test.txt can be dropped in */
    gchar *text = NULL;
    if (!g_file_get_contents(local, &text, NULL, NULL)) {
        for (int i = 0; i < 4 && !text; i++)
            if (paths[i]) g_file_get_contents(paths[i], &text, NULL, NULL);
    }
    g_free(local);
    g_free(beside);
    g_free(exe_dir);
    g_free(exe);
    if (!text) {
        fprintf(stderr, "xisserve --keyboard: emoji-test.txt not found, emoji list is empty\n");
        return;
    }

    int group = -1;
    gboolean skip_group = FALSE;
    gchar **lines = g_strsplit(text, "\n", -1);
    for (int i = 0; lines[i]; i++) {
        const char *ln = lines[i];
        if (g_str_has_prefix(ln, "# group: ")) {
            const char *name = ln + 9;
            skip_group = !strcmp(name, "Component");
            if (!skip_group) {
                g_ptr_array_add(g_emoji_groups, g_strdup(name));
                group = (int)g_emoji_groups->len - 1;
            }
            continue;
        }
        if (*ln == '#' || !*ln || skip_group || group < 0) continue;
        if (!strstr(ln, "; fully-qualified")) continue;
        /* "1F600 ; fully-qualified # 😀 E1.0 grinning face" */
        const char *hash = strchr(ln, '#');
        if (!hash) continue;
        gchar **f = g_strsplit(hash + 2, " ", 3); /* glyph, version, name */
        if (f[0] && f[1] && f[2] && !emoji_has_skin_tone(f[0])) {
            EmojiEntry e = {g_strdup(f[0]), g_strdup(f[2]), group};
            g_array_append_val(g_emoji, e);
        }
        g_strfreev(f);
    }
    g_strfreev(lines);
    g_free(text);
}
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

static void rebuild_rows(void);

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
    default:
        btn = gtk_button_new_with_label(spec->label);
        break;
    }

    gtk_widget_set_size_request(btn, (int)(BASE_KEY_W * spec->weight), KEY_MIN_H);
    return btn;
}

static GtkWidget *build_row(const KbRow *row)
{
    GtkWidget *hbox = gtk_hbox_new(FALSE, KEY_GAP);
    /* A key row is a picture of the physical keyboard, which doesn't
     * mirror under RTL -- keep Q on the left whatever the UI direction. */
    gtk_widget_set_direction(hbox, GTK_TEXT_DIR_LTR);
    for (int i = 0; i < row->n; i++) {
        GtkWidget *btn = make_key_button(&row->keys[i]);
        gtk_box_pack_start(GTK_BOX(hbox), btn, TRUE, TRUE, 0);
    }
    return hbox;
}

/* ---- emoji picker UI -----------------------------------------------------
 *
 * Category tabs over a scrollable grid of every emoji. This window never
 * takes focus, so a GtkEntry couldn't receive typing: search mode swaps
 * the grid for a one-row result strip plus a small on-screen letter pad
 * that feeds the query, and the bottom control row is hidden to make
 * room (the window is only as tall as the user dragged it). */
#define EMOJI_SEARCH_MAX 300

static int g_emoji_group;
static gboolean g_emoji_search;
static GString *g_emoji_query;
static GtkWidget *g_emoji_scroller, *g_emoji_query_label;
static GtkWidget *g_emoji_tabs[16];
static int g_emoji_ntabs;

static void on_emoji_clicked(GtkWidget *btn, gpointer data)
{
    (void)btn;
    /* a ZWJ/flag/keycap sequence is several codepoints; type them one
     * after another. The pause lets clients re-read a remapped scratch
     * keycode before the next one lands. */
    const char *s = data;
    for (const char *p = s; *p; p = g_utf8_next_char(p)) {
        char one[8] = { 0 };
        g_utf8_strncpy(one, p, 1);
        kb_send_utf8(one);
        if (*(g_utf8_next_char(p))) g_usleep(12000);
    }
}

static gboolean emoji_matches(const EmojiEntry *e, gchar **words)
{
    for (int i = 0; words[i]; i++)
        if (words[i][0] && !strstr(e->name, words[i])) return FALSE;
    return TRUE;
}

static GtkWidget *make_emoji_button(const EmojiEntry *e)
{
    GtkWidget *b = gtk_button_new_with_label(e->glyph);
    gtk_widget_set_size_request(b, BASE_KEY_W, KEY_H);
    gtk_widget_set_tooltip_text(b, e->name);
    g_signal_connect(b, "clicked", G_CALLBACK(on_emoji_clicked), e->glyph);
    return b;
}

/* (Re)fills the scroller for the current tab, or the current query. */
static void emoji_refill(void)
{
    GtkWidget *old = gtk_bin_get_child(GTK_BIN(g_emoji_scroller));
    if (old) gtk_widget_destroy(old);

    GtkWidget *content;
    if (g_emoji_search) {
        content = gtk_hbox_new(FALSE, KEY_GAP);
        gtk_widget_set_direction(content, GTK_TEXT_DIR_LTR);
        gchar *q = g_utf8_strdown(g_emoji_query->str, -1);
        gchar **words = g_strsplit(q, " ", -1);
        int shown = 0;
        if (q[0]) {
            for (guint i = 0; i < g_emoji->len && shown < EMOJI_SEARCH_MAX; i++) {
                const EmojiEntry *e = &g_array_index(g_emoji, EmojiEntry, i);
                if (!emoji_matches(e, words)) continue;
                gtk_box_pack_start(GTK_BOX(content), make_emoji_button(e), FALSE, FALSE, 0);
                shown++;
            }
        }
        g_strfreev(words);
        g_free(q);
    } else {
        int cols = (g_win_w - 24) / (BASE_KEY_W + KEY_GAP);
        if (cols < 4) cols = 4;
        content = gtk_table_new(1, cols, TRUE);
        gtk_table_set_row_spacings(GTK_TABLE(content), KEY_GAP);
        gtk_table_set_col_spacings(GTK_TABLE(content), KEY_GAP);
        gtk_widget_set_direction(content, GTK_TEXT_DIR_LTR);
        int n = 0;
        for (guint i = 0; i < g_emoji->len; i++) {
            const EmojiEntry *e = &g_array_index(g_emoji, EmojiEntry, i);
            if (e->group != g_emoji_group) continue;
            gtk_table_attach(GTK_TABLE(content), make_emoji_button(e), (guint)(n % cols), (guint)(n % cols) + 1,
                             (guint)(n / cols), (guint)(n / cols) + 1, GTK_FILL, GTK_FILL, 0, 0);
            n++;
        }
    }
    gtk_scrolled_window_add_with_viewport(GTK_SCROLLED_WINDOW(g_emoji_scroller), content);
    gtk_widget_show_all(g_emoji_scroller);
    GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(g_emoji_scroller));
    gtk_adjustment_set_value(adj, 0);
    adj = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(g_emoji_scroller));
    gtk_adjustment_set_value(adj, 0);
}

static gboolean idle_rebuild(gpointer data)
{
    (void)data;
    rebuild_rows();
    return FALSE;
}

static void on_emoji_tab(GtkToggleButton *btn, gpointer data)
{
    static gboolean busy;
    if (busy || !gtk_toggle_button_get_active(btn)) {
        /* a click on the already-active tab would untoggle it */
        if (!busy && !gtk_toggle_button_get_active(btn)) {
            busy = TRUE;
            gtk_toggle_button_set_active(btn, TRUE);
            busy = FALSE;
        }
        return;
    }
    busy = TRUE;
    g_emoji_group = GPOINTER_TO_INT(data);
    for (int i = 0; i < g_emoji_ntabs; i++)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_emoji_tabs[i]), i == g_emoji_group);
    busy = FALSE;
    emoji_refill();
}

static void on_emoji_search_toggle(GtkWidget *btn, gpointer data)
{
    (void)btn;
    g_emoji_search = GPOINTER_TO_INT(data);
    if (!g_emoji_search) g_string_truncate(g_emoji_query, 0);
    g_idle_add(idle_rebuild, NULL);
}

static void emoji_query_changed(void)
{
    gtk_label_set_text(GTK_LABEL(g_emoji_query_label), g_emoji_query->len ? g_emoji_query->str : _("Buscar emoji (em inglês)..."));
    emoji_refill();
}

static void free_glyph_closure(gpointer data, GClosure *c)
{
    (void)c;
    g_free(data);
}

static void on_search_char(GtkWidget *btn, gpointer data)
{
    (void)btn;
    g_string_append(g_emoji_query, (const char *)data);
    emoji_query_changed();
}

static void on_search_bksp(GtkWidget *btn, gpointer data)
{
    (void)btn;
    (void)data;
    if (!g_emoji_query->len) return;
    char *prev = g_utf8_prev_char(g_emoji_query->str + g_emoji_query->len);
    g_string_truncate(g_emoji_query, (gsize)(prev - g_emoji_query->str));
    emoji_query_changed();
}

static void build_emoji_layout(const KbLayout *layout)
{
    load_emoji();
    if (!g_emoji_query) g_emoji_query = g_string_new("");
    if (g_emoji_group >= (int)g_emoji_groups->len) g_emoji_group = 0;

    GtkWidget *top = gtk_hbox_new(FALSE, KEY_GAP);
    gtk_widget_set_direction(top, GTK_TEXT_DIR_LTR);
    g_emoji_ntabs = 0;
    g_emoji_query_label = NULL;

    if (g_emoji_search) {
        g_emoji_query_label = gtk_label_new(NULL);
        gtk_misc_set_alignment(GTK_MISC(g_emoji_query_label), 0, 0.5);
        gtk_misc_set_padding(GTK_MISC(g_emoji_query_label), 8, 0);
        gtk_box_pack_start(GTK_BOX(top), g_emoji_query_label, TRUE, TRUE, 0);
        GtkWidget *bk = gtk_button_new_with_label("\xe2\x8c\xab");
        gtk_widget_set_size_request(bk, BASE_KEY_W * 2, 28);
        g_signal_connect(bk, "clicked", G_CALLBACK(on_search_bksp), NULL);
        gtk_box_pack_start(GTK_BOX(top), bk, FALSE, FALSE, 0);
        GtkWidget *done = gtk_button_new_with_label(_("Voltar"));
        gtk_widget_set_size_request(done, BASE_KEY_W * 2, 28);
        g_signal_connect(done, "clicked", G_CALLBACK(on_emoji_search_toggle), GINT_TO_POINTER(FALSE));
        gtk_box_pack_start(GTK_BOX(top), done, FALSE, FALSE, 0);
    } else {
        for (guint i = 0; i < g_emoji_groups->len && i < 16; i++) {
            /* a group's tab shows its first emoji; the name is the tooltip */
            const char *glyph = "?";
            for (guint j = 0; j < g_emoji->len; j++) {
                const EmojiEntry *e = &g_array_index(g_emoji, EmojiEntry, j);
                if (e->group == (int)i) { glyph = e->glyph; break; }
            }
            GtkWidget *t = gtk_toggle_button_new_with_label(glyph);
            gtk_widget_set_size_request(t, BASE_KEY_W, KEY_MIN_H);
            gtk_widget_set_tooltip_text(t, g_ptr_array_index(g_emoji_groups, i));
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(t), (int)i == g_emoji_group);
            g_signal_connect(t, "toggled", G_CALLBACK(on_emoji_tab), GINT_TO_POINTER((int)i));
            gtk_box_pack_start(GTK_BOX(top), t, TRUE, TRUE, 0);
            g_emoji_tabs[g_emoji_ntabs++] = t;
        }
        GtkWidget *s = gtk_button_new_with_label(_("Buscar"));
        gtk_widget_set_size_request(s, BASE_KEY_W * 2, KEY_MIN_H);
        g_signal_connect(s, "clicked", G_CALLBACK(on_emoji_search_toggle), GINT_TO_POINTER(TRUE));
        gtk_box_pack_start(GTK_BOX(top), s, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(g_rows_container), top, FALSE, FALSE, 0);

    g_emoji_scroller = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(g_emoji_scroller),
                                   g_emoji_search ? GTK_POLICY_AUTOMATIC : GTK_POLICY_NEVER,
                                   g_emoji_search ? GTK_POLICY_NEVER : GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(g_emoji_scroller), GTK_SHADOW_NONE);
    gtk_box_pack_start(GTK_BOX(g_rows_container), g_emoji_scroller, TRUE, TRUE, 0);
    if (g_emoji_search) gtk_widget_set_size_request(g_emoji_scroller, -1, KEY_H + 8);

    if (g_emoji_search) {
        static const char *pad[] = {"qwertyuiop", "asdfghjkl", "zxcvbnm "};
        for (int r = 0; r < 3; r++) {
            GtkWidget *hb = gtk_hbox_new(FALSE, KEY_GAP);
            gtk_widget_set_direction(hb, GTK_TEXT_DIR_LTR);
            for (const char *c = pad[r]; *c; c++) {
                char *glyph = g_strndup(c, 1);
                GtkWidget *b = gtk_button_new_with_label(*c == ' ' ? "\xe2\x90\xa3" : glyph); /* U+2423 */
                gtk_widget_set_size_request(b, *c == ' ' ? BASE_KEY_W * 3 : BASE_KEY_W, 28);
                g_signal_connect_data(b, "clicked", G_CALLBACK(on_search_char), glyph, free_glyph_closure, 0);
                gtk_box_pack_start(GTK_BOX(hb), b, TRUE, TRUE, 0);
            }
            gtk_box_pack_start(GTK_BOX(g_rows_container), hb, FALSE, FALSE, 0);
        }
        emoji_query_changed();
    } else {
        for (int i = 0; i < layout->n_rows; i++)
            gtk_box_pack_start(GTK_BOX(g_rows_container), build_row(&layout->rows[i]), FALSE, FALSE, 0);
        emoji_refill();
    }
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

    const KbLayout *layout = g_ptr_array_index(g_layouts, g_layout_idx);
    if (layout->emoji) {
        build_emoji_layout(layout);
    } else {
        for (int i = 0; i < layout->n_rows; i++)
            gtk_box_pack_start(GTK_BOX(g_rows_container), build_row(&layout->rows[i]), TRUE, TRUE, 0);
    }
    gtk_widget_show_all(g_rows_container);

    /* e.g. leaving emoji search, which can have grown the window */
    if (g_win && !g_grip_dragging) apply_height(g_pref_h);

    if (g_layout_btn) {
        char *label = g_strdup_printf("%s \xe2\x96\xbe", layout->name); /* U+25BE */
        gtk_button_set_label(GTK_BUTTON(g_layout_btn), label);
        g_free(label);
    }
}

static void on_layout_picked(GtkWidget *item, gpointer data)
{
    (void)item;
    g_layout_idx = GPOINTER_TO_INT(data);
    g_emoji_search = FALSE;
    g_idle_add(idle_rebuild, NULL);
}

static void on_layout_clicked(GtkWidget *btn, gpointer data)
{
    (void)data;
    GtkWidget *menu = gtk_menu_new();
    for (guint i = 0; i < g_layouts->len; i++) {
        const KbLayout *l = g_ptr_array_index(g_layouts, i);
        GtkWidget *item = gtk_check_menu_item_new_with_label(l->name);
        gtk_check_menu_item_set_draw_as_radio(GTK_CHECK_MENU_ITEM(item), TRUE);
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), (int)i == g_layout_idx);
        g_signal_connect(item, "activate", G_CALLBACK(on_layout_picked), GINT_TO_POINTER((int)i));
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    }
    gtk_widget_show_all(menu);
    gtk_menu_popup(GTK_MENU(menu), NULL, NULL, NULL, NULL, 1, gtk_get_current_event_time());
    (void)btn;
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

/* Resizes the dock to h (clamped), keeps it on the output's bottom edge
 * and updates how much of the work area it reserves. */
static void apply_height(int h)
{
    int max_h = g_out_h * 7 / 10;
    if (h > max_h) h = max_h;
    if (h < KB_MIN_H) h = KB_MIN_H;
    g_applied_h = h;
    gtk_widget_set_size_request(g_win, g_win_w, h);
    gtk_window_move(GTK_WINDOW(g_win), g_out_x, g_out_y + g_out_h - h);
    if (GTK_WIDGET_REALIZED(g_win)) reserve_strut(g_win->window, g_out_x, 0, g_win_w, h);
}

/* The children can force the window taller than asked (emoji search needs
 * room for its letter pad): follow the real height so the strut and the
 * bottom anchoring stay truthful. */
static gboolean on_win_size_allocate(GtkWidget *w, GtkAllocation *a, gpointer data)
{
    (void)w;
    (void)data;
    if (a->height > 1 && a->height != g_applied_h && GTK_WIDGET_MAPPED(g_win)) {
        g_applied_h = a->height;
        gtk_window_move(GTK_WINDOW(g_win), g_out_x, g_out_y + g_out_h - g_applied_h);
        reserve_strut(g_win->window, g_out_x, 0, g_win_w, g_applied_h);
    }
    return FALSE;
}

static char *kb_conf_path(void) { return g_build_filename(g_get_user_config_dir(), "xisserve", "keyboard.conf", NULL); }

static void load_height_pref(void)
{
    char *path = kb_conf_path();
    GKeyFile *kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
        int h = g_key_file_get_integer(kf, "keyboard", "height", NULL);
        if (h > 0) g_pref_h = h;
    }
    g_key_file_free(kf);
    g_free(path);
}

static void save_height_pref(void)
{
    char *path = kb_conf_path();
    char *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0755);
    GKeyFile *kf = g_key_file_new();
    g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL); /* keep unknown keys */
    g_key_file_set_integer(kf, "keyboard", "height", g_pref_h);
    g_key_file_save_to_file(kf, path, NULL);
    g_key_file_free(kf);
    g_free(dir);
    g_free(path);
}

/* Drag handle on the top edge: the bottom stays put, the top follows the
 * pointer. Live-resized; saved when the button is released. */

static gboolean on_grip_event(GtkWidget *w, GdkEvent *ev, gpointer data)
{
    (void)w;
    (void)data;
    switch (ev->type) {
    case GDK_BUTTON_PRESS:
        if (ev->button.button == 1) g_grip_dragging = TRUE;
        return TRUE;
    case GDK_MOTION_NOTIFY:
        if (g_grip_dragging) {
            g_pref_h = g_out_y + g_out_h - (int)ev->motion.y_root;
            apply_height(g_pref_h);
            g_pref_h = g_applied_h; /* clamped */
        }
        return TRUE;
    case GDK_BUTTON_RELEASE:
        if (g_grip_dragging) {
            g_grip_dragging = FALSE;
            save_height_pref();
        }
        return TRUE;
    default:
        return FALSE;
    }
}

static void on_grip_realize(GtkWidget *w, gpointer data)
{
    (void)data;
    GdkCursor *c = gdk_cursor_new(GDK_SB_V_DOUBLE_ARROW);
    gdk_window_set_cursor(w->window, c);
    gdk_cursor_unref(c);
}

/* ---- follow the active window's own output on a multi-monitor session --
 *
 * Nothing hands --keyboard an --output-x/-y/-w/-h the way xispanel does
 * for the launcher's other pages (there's no panel button it's anchored
 * to -- it's opened by a hotkey, from anywhere); a caller that doesn't
 * pass those flags at all falls back to parse_argv()'s hardcoded
 * "0,0,1920,1080" default, which on a real multi-monitor session can
 * dock the keyboard on a screen the user isn't even looking at. So
 * before trusting that default, look up which RandR output actually
 * contains the currently focused window (falling back to wherever the
 * pointer is if there's no usable _NET_ACTIVE_WINDOW) and use that
 * instead when found -- an explicit --output-* still wins if this
 * lookup fails for any reason (no RandR, nothing focused, ...).
 *
 * xisserve_resolve_active_output() below is exported (see xisserve.h) so
 * session.c's --session picker can center itself on the same monitor
 * rather than duplicating this lookup. */
/* RandR 1.5 "Monitor" objects (what `xrandr --listmonitors`/--setmonitor
 * shows), not the older per-CRTC geometry -- kiwm's own output tracking
 * (output.c's xcb_randr_get_monitors() call) already made this same call
 * for this project, since a Monitor is the thing that's actually allowed
 * to be a carved-out region of one CRTC (or span more than one), which is
 * exactly how a WM would expose more logical outputs than the hardware
 * has physical ones. */
static gboolean find_output_containing(Display *dpy, Window root, int px, int py, int *ox, int *oy, int *ow,
                                        int *oh)
{
    int nmon = 0;
    XRRMonitorInfo *mons = XRRGetMonitors(dpy, root, True, &nmon);
    if (!mons) return FALSE;
    gboolean found = FALSE;
    long best_area = 0;
    /* More than one monitor can legitimately contain the same point (a
     * "combined" whole-screen entry alongside the real per-output ones
     * some setups expose) -- the smallest one that does is the more
     * specific, more useful answer. */
    for (int i = 0; i < nmon; i++) {
        XRRMonitorInfo *m = &mons[i];
        if (px < m->x || px >= m->x + m->width || py < m->y || py >= m->y + m->height) continue;
        long area = (long)m->width * (long)m->height;
        if (!found || area < best_area) {
            *ox = m->x;
            *oy = m->y;
            *ow = m->width;
            *oh = m->height;
            best_area = area;
            found = TRUE;
        }
    }
    XRRFreeMonitors(mons);
    return found;
}

static Window get_active_window(Display *dpy, Window root)
{
    Atom prop = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
    Atom type;
    int format;
    unsigned long nitems, bytes_after;
    unsigned char *data = NULL;
    Window w = None;
    if (XGetWindowProperty(dpy, root, prop, 0, 1, False, XA_WINDOW, &type, &format, &nitems, &bytes_after, &data) ==
            Success &&
        data) {
        if (nitems >= 1) w = *(Window *)data;
        XFree(data);
    }
    return w;
}

gboolean xisserve_resolve_active_output(Display *dpy, Window root, int *ox, int *oy, int *ow, int *oh)
{
    Window active = get_active_window(dpy, root);
    if (active != None) {
        XWindowAttributes attrs;
        Window child;
        int rx, ry;
        /* XGetWindowAttributes()/XTranslateCoordinates() can legitimately
         * fail here -- the "active" window from a stale property can
         * already be gone by the time this runs. */
        if (XGetWindowAttributes(dpy, active, &attrs) &&
            XTranslateCoordinates(dpy, active, attrs.root, 0, 0, &rx, &ry, &child)) {
            int cx = rx + attrs.width / 2, cy = ry + attrs.height / 2;
            if (find_output_containing(dpy, root, cx, cy, ox, oy, ow, oh)) return TRUE;
        }
    }

    Window root_ret, child_ret;
    int rx, ry, wxr, wyr;
    unsigned int mask;
    if (XQueryPointer(dpy, root, &root_ret, &child_ret, &rx, &ry, &wxr, &wyr, &mask)) {
        if (find_output_containing(dpy, root, rx, ry, ox, oy, ow, oh)) return TRUE;
    }
    return FALSE;
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

    int lockfd = open(lockpath, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
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

    load_layouts();
    if (!g_layouts->len) {
        fprintf(stderr, "xisserve --keyboard: no *.layout files found (looked in $XISSERVE_KBDIR, "
                        "~/.config/xisserve/keyboard, %s)\n", XISSERVE_KBDIR_DEFAULT);
        flock(lockfd, LOCK_UN);
        close(lockfd);
        unlink(lockpath);
        return 1;
    }

    /* Whatever --output-x/-y/-w/-h (or parse_argv()'s own defaults for
     * them) handed us, prefer the RandR output the currently focused
     * window (or failing that, the pointer) actually sits on -- see
     * xisserve_resolve_active_output()'s comment. */
    {
        int aox, aoy, aow, aoh;
        if (xisserve_resolve_active_output(g_dpy, DefaultRootWindow(g_dpy), &aox, &aoy, &aow, &aoh)) {
            output_x = aox;
            output_y = aoy;
            output_w = aow;
            output_h = aoh;
        }
    }

    if (output_w <= 0) output_w = gdk_screen_get_width(gdk_screen_get_default());
    if (output_h <= 0) output_h = gdk_screen_get_height(gdk_screen_get_default());
    g_win_w = output_w;
    g_out_x = output_x;
    g_out_y = output_y;
    g_out_h = output_h;
    load_height_pref();

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
    g_win = win;
    g_signal_connect(win, "size-allocate", G_CALLBACK(on_win_size_allocate), NULL);
    apply_height(g_pref_h);
    g_pref_h = g_applied_h;

    GtkWidget *vbox = gtk_vbox_new(FALSE, KEY_GAP);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), KEY_GAP);
    gtk_container_add(GTK_CONTAINER(win), vbox);

    GtkWidget *grip = gtk_event_box_new();
    gtk_widget_set_size_request(grip, -1, 10);
    gtk_widget_add_events(grip, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK);
    gtk_container_add(GTK_CONTAINER(grip), gtk_hseparator_new());
    g_signal_connect(grip, "realize", G_CALLBACK(on_grip_realize), NULL);
    g_signal_connect(grip, "event", G_CALLBACK(on_grip_event), NULL);
    gtk_widget_set_tooltip_text(grip, _("Arraste para ajustar a altura"));
    gtk_box_pack_start(GTK_BOX(vbox), grip, FALSE, FALSE, 0);

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
    GtkWidget *close_btn = gtk_button_new_with_label(_("\xc3\x97")); /* U+00D7 MULTIPLICATION SIGN */
    gtk_widget_set_size_request(close_btn, 28, 22);
    g_signal_connect(close_btn, "clicked", G_CALLBACK(on_close_clicked), NULL);
    xisserve_a11y(close_btn, _("Fechar"), NULL);
    gtk_box_pack_end(GTK_BOX(header), close_btn, FALSE, FALSE, 0);
    /* layout picker: left of Close with a gap so the two can't be mis-hit */
    g_layout_btn = gtk_button_new_with_label("");
    gtk_widget_set_size_request(g_layout_btn, -1, 22);
    g_signal_connect(g_layout_btn, "clicked", G_CALLBACK(on_layout_clicked), NULL);
    xisserve_a11y(g_layout_btn, _("Alterar layout do teclado"), NULL);
    gtk_widget_set_tooltip_text(g_layout_btn, _("Alterar layout"));
    gtk_box_pack_end(GTK_BOX(header), g_layout_btn, FALSE, FALSE, 14);
    gtk_box_pack_start(GTK_BOX(vbox), header, FALSE, FALSE, 0);

    g_rows_container = gtk_vbox_new(FALSE, KEY_GAP);
    gtk_box_pack_start(GTK_BOX(vbox), g_rows_container, TRUE, TRUE, 0);
    g_layout_idx = 0;
    rebuild_rows();

    gtk_widget_realize(win);
    apply_height(g_pref_h); /* realized now: also sets the strut */

    gtk_widget_show_all(win);
    gtk_window_move(GTK_WINDOW(win), g_out_x, g_out_y + g_out_h - g_applied_h); /* some WMs only honor this once mapped */

    poll_indicators(NULL);
    g_timeout_add(400, poll_indicators, NULL);

    gtk_main();

    flock(lockfd, LOCK_UN);
    close(lockfd);
    unlink(lockpath);
    return 0;
}
