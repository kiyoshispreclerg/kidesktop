/*
 * keyboard.c - `xisserve --keyboard`: an on-screen QWERTY keyboard docked
 * to the bottom of the screen, driven entirely by mouse clicks. Every
 * button synthesizes the real key via XTest, so whatever window last had
 * input focus keeps it and receives the keystrokes -- this window itself
 * never asks for focus (WM_HINTS input=False, the same ICCCM contract
 * xispanel's own panel windows use) and reserves its strip of the screen
 * via _NET_WM_STRUT_PARTIAL like any other dock.
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
 * Modifiers (Shift/Ctrl/Alt/Super) are one-shot latches: click one,
 * click a key, the chord fires and every latch clears -- exactly what a
 * real chord (Ctrl+C, Alt+Tab, Shift+a) looks like from the X server's
 * point of view, which is also what lets Caps Lock's real, server-side
 * state (not anything this process tracks) decide letter case exactly
 * the way a physical keyboard would. Caps Lock itself is a real toggle
 * (one XK_Caps_Lock press/release) rather than a latch.
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
    KK_CHAR,   /* keysym is the unshifted base glyph; label is looked up live */
    KK_ACTION, /* keysym is sent as-is (Tab/Enter/BackSpace/Esc/arrows/Space) */
    KK_MODIFIER, /* one-shot latch (Shift/Ctrl/Alt/Super) */
    KK_CAPSLOCK, /* real toggle, own lamp */
} KbKeyType;

typedef struct {
    KbKeyType type;
    KeySym keysym;   /* 0 for a plain spacer */
    const char *label; /* fixed label -- KK_ACTION/KK_MODIFIER/KK_CAPSLOCK only */
    double weight;   /* relative width, 1.0 = one normal key */
} KbKeySpec;

static const KbKeySpec kRow0[] = {
    {KK_ACTION, XK_Escape, "Esc", 1.3},
    {KK_CHAR, XK_1, NULL, 1}, {KK_CHAR, XK_2, NULL, 1}, {KK_CHAR, XK_3, NULL, 1},
    {KK_CHAR, XK_4, NULL, 1}, {KK_CHAR, XK_5, NULL, 1}, {KK_CHAR, XK_6, NULL, 1},
    {KK_CHAR, XK_7, NULL, 1}, {KK_CHAR, XK_8, NULL, 1}, {KK_CHAR, XK_9, NULL, 1},
    {KK_CHAR, XK_0, NULL, 1},
    {KK_CHAR, XK_minus, NULL, 1}, {KK_CHAR, XK_equal, NULL, 1},
    {KK_ACTION, XK_BackSpace, "\xe2\x8c\xab", 2.0}, /* U+232B ERASE TO THE LEFT */
};

static const KbKeySpec kRow1[] = {
    {KK_ACTION, XK_Tab, "Tab", 1.6},
    {KK_CHAR, XK_q, NULL, 1}, {KK_CHAR, XK_w, NULL, 1}, {KK_CHAR, XK_e, NULL, 1},
    {KK_CHAR, XK_r, NULL, 1}, {KK_CHAR, XK_t, NULL, 1}, {KK_CHAR, XK_y, NULL, 1},
    {KK_CHAR, XK_u, NULL, 1}, {KK_CHAR, XK_i, NULL, 1}, {KK_CHAR, XK_o, NULL, 1},
    {KK_CHAR, XK_p, NULL, 1},
    {KK_CHAR, XK_bracketleft, NULL, 1}, {KK_CHAR, XK_bracketright, NULL, 1},
    {KK_CHAR, XK_backslash, NULL, 1.3},
};

static const KbKeySpec kRow2[] = {
    {KK_CAPSLOCK, 0, "Caps", 1.9},
    {KK_CHAR, XK_a, NULL, 1}, {KK_CHAR, XK_s, NULL, 1}, {KK_CHAR, XK_d, NULL, 1},
    {KK_CHAR, XK_f, NULL, 1}, {KK_CHAR, XK_g, NULL, 1}, {KK_CHAR, XK_h, NULL, 1},
    {KK_CHAR, XK_j, NULL, 1}, {KK_CHAR, XK_k, NULL, 1}, {KK_CHAR, XK_l, NULL, 1},
    {KK_CHAR, XK_semicolon, NULL, 1}, {KK_CHAR, XK_apostrophe, NULL, 1},
    {KK_ACTION, XK_Return, "Enter", 2.3},
};

static const KbKeySpec kRow3[] = {
    {KK_MODIFIER, XK_Shift_L, "Shift", 2.4},
    {KK_CHAR, XK_z, NULL, 1}, {KK_CHAR, XK_x, NULL, 1}, {KK_CHAR, XK_c, NULL, 1},
    {KK_CHAR, XK_v, NULL, 1}, {KK_CHAR, XK_b, NULL, 1}, {KK_CHAR, XK_n, NULL, 1},
    {KK_CHAR, XK_m, NULL, 1},
    {KK_CHAR, XK_comma, NULL, 1}, {KK_CHAR, XK_period, NULL, 1}, {KK_CHAR, XK_slash, NULL, 1},
    {KK_MODIFIER, XK_Shift_R, "Shift", 2.4},
};

static const KbKeySpec kRow4[] = {
    {KK_MODIFIER, XK_Control_L, "Ctrl", 1.4},
    {KK_MODIFIER, XK_Super_L, "Super", 1.2},
    {KK_MODIFIER, XK_Alt_L, "Alt", 1.2},
    {KK_ACTION, XK_space, "", 6.0},
    {KK_MODIFIER, XK_Alt_R, "Alt", 1.2},
    {KK_ACTION, XK_Left, "\xe2\x86\x90", 1},
    {KK_ACTION, XK_Down, "\xe2\x86\x93", 1},
    {KK_ACTION, XK_Up, "\xe2\x86\x91", 1},
    {KK_ACTION, XK_Right, "\xe2\x86\x92", 1},
};

typedef struct { const KbKeySpec *keys; int n; } KbRow;
static const KbRow kRows[] = {
    {kRow0, (int)(sizeof(kRow0) / sizeof(kRow0[0]))},
    {kRow1, (int)(sizeof(kRow1) / sizeof(kRow1[0]))},
    {kRow2, (int)(sizeof(kRow2) / sizeof(kRow2[0]))},
    {kRow3, (int)(sizeof(kRow3) / sizeof(kRow3[0]))},
    {kRow4, (int)(sizeof(kRow4) / sizeof(kRow4[0]))},
};
#define N_ROWS ((int)(sizeof(kRows) / sizeof(kRows[0])))

/* One-shot modifier latches -- cleared after the next non-modifier key.
 * Real Caps Lock state lives on the server, not here; g_capslock_lamp_on
 * only mirrors it for the lamp (see poll_indicators()). */
static gboolean g_shift_latched, g_ctrl_latched, g_alt_latched, g_super_latched;
static GtkWidget *g_shift_btns[2]; /* left + right Shift both reflect/clear together */
static GtkWidget *g_alt_btns[2];   /* left + right Alt, same deal */
static GtkWidget *g_ctrl_btn, *g_super_btn;
static GtkWidget *g_capslock_btn;
static GtkWidget *g_lamp_caps, *g_lamp_num, *g_lamp_scroll;

/* Every KK_CHAR button, so a Shift latch toggle can refresh their glyphs
 * to the shifted level live (cosmetic only -- what's actually sent is
 * decided at click time by kb_send_key(), see its comment). */
typedef struct { GtkWidget *btn; KeyCode kc; } CharKeyWidget;
static CharKeyWidget g_char_keys[128];
static int g_n_char_keys;

static Display *g_dpy;
static KeyCode g_kc_shift, g_kc_ctrl, g_kc_alt, g_kc_super, g_kc_capslock;

static void set_lamp(GtkWidget *lamp, gboolean on)
{
    GdkColor c;
    if (on) gdk_color_parse("#4caf50", &c);
    else gdk_color_parse("#3a3a3a", &c);
    gtk_widget_modify_bg(lamp, GTK_STATE_NORMAL, &c);
}

/* Polled rather than event-driven, same call as xisserve.c's own
 * check_parent_alive() -- there's no portable low-overhead "notify me
 * when XKB indicator state changes" short of subscribing to the Xkb
 * extension's own event stream, which isn't worth it for a lamp that
 * only needs to be eventually-consistent. Bits 0/1/2 are Caps/Num/
 * Scroll Lock on every XKB base ruleset this project targets. */
static gboolean poll_indicators(gpointer data)
{
    (void)data;
    unsigned int state = 0;
    XkbGetIndicatorState(g_dpy, XkbUseCoreKbd, &state);
    set_lamp(g_lamp_caps, (state & 0x01) != 0);
    set_lamp(g_lamp_num, (state & 0x02) != 0);
    set_lamp(g_lamp_scroll, (state & 0x04) != 0);
    return TRUE;
}

/* Live glyph for a KK_CHAR key's keycode at the current Shift level --
 * queried from the active keymap (whatever layout is actually loaded,
 * not a hardcoded US table) so the label always matches what will
 * really be typed, non-US layouts included. */
static void char_key_glyph(KeyCode kc, gboolean shifted, char *out, size_t outsz)
{
    KeySym ks = XkbKeycodeToKeysym(g_dpy, kc, 0, shifted ? 1 : 0);
    if (ks == NoSymbol) ks = XkbKeycodeToKeysym(g_dpy, kc, 0, 0);
    guint32 uc = ks ? gdk_keyval_to_unicode((guint)ks) : 0;
    if (uc) {
        gchar buf[8];
        gint n = g_unichar_to_utf8((gunichar)uc, buf);
        buf[n] = 0;
        snprintf(out, outsz, "%s", buf);
    } else {
        snprintf(out, outsz, "?");
    }
}

static void refresh_char_labels(void)
{
    char glyph[8];
    for (int i = 0; i < g_n_char_keys; i++) {
        char_key_glyph(g_char_keys[i].kc, g_shift_latched, glyph, sizeof(glyph));
        gtk_button_set_label(GTK_BUTTON(g_char_keys[i].btn), glyph);
    }
}

/* Clears every one-shot latch (visually too -- toggling the buttons off
 * re-enters their own "toggled" handlers, which is what actually flips
 * the g_*_latched booleans back to FALSE). */
static void clear_latches(void)
{
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_shift_btns[0]), FALSE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_shift_btns[1]), FALSE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_ctrl_btn), FALSE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_alt_btns[0]), FALSE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_alt_btns[1]), FALSE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_super_btn), FALSE);
}

/* Sends one physical key, wrapped in whatever modifiers are currently
 * latched -- the same shape a real chorded keypress takes on the wire,
 * which is exactly why this needs no separate "compute the shifted
 * keysym" logic: the receiving application (and the server's own Caps
 * Lock state) resolves the keycode+modifiers into a character the same
 * way it would for a physical keyboard. */
static void kb_send_key(KeyCode kc)
{
    if (!kc) return;
    if (g_ctrl_latched && g_kc_ctrl) XTestFakeKeyEvent(g_dpy, g_kc_ctrl, True, 0);
    if (g_alt_latched && g_kc_alt) XTestFakeKeyEvent(g_dpy, g_kc_alt, True, 0);
    if (g_super_latched && g_kc_super) XTestFakeKeyEvent(g_dpy, g_kc_super, True, 0);
    if (g_shift_latched && g_kc_shift) XTestFakeKeyEvent(g_dpy, g_kc_shift, True, 0);

    XTestFakeKeyEvent(g_dpy, kc, True, 0);
    XTestFakeKeyEvent(g_dpy, kc, False, 0);

    if (g_shift_latched && g_kc_shift) XTestFakeKeyEvent(g_dpy, g_kc_shift, False, 0);
    if (g_super_latched && g_kc_super) XTestFakeKeyEvent(g_dpy, g_kc_super, False, 0);
    if (g_alt_latched && g_kc_alt) XTestFakeKeyEvent(g_dpy, g_kc_alt, False, 0);
    if (g_ctrl_latched && g_kc_ctrl) XTestFakeKeyEvent(g_dpy, g_kc_ctrl, False, 0);
    XFlush(g_dpy);

    clear_latches();
}

static void on_key_clicked(GtkWidget *btn, gpointer data)
{
    (void)btn;
    KeyCode kc = (KeyCode)(guintptr)data;
    kb_send_key(kc);
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
    if (GTK_WIDGET(btn) == g_shift_btns[0])
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_shift_btns[1]), active);
    else if (GTK_WIDGET(btn) == g_shift_btns[1])
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
    if (GTK_WIDGET(btn) == g_alt_btns[0])
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_alt_btns[1]), active);
    else if (GTK_WIDGET(btn) == g_alt_btns[1])
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

static GtkWidget *make_key_button(const KbKeySpec *spec)
{
    GtkWidget *btn;
    KeyCode kc = spec->keysym ? XKeysymToKeycode(g_dpy, spec->keysym) : 0;

    switch (spec->type) {
    case KK_CHAR: {
        char glyph[8];
        char_key_glyph(kc, FALSE, glyph, sizeof(glyph));
        btn = gtk_button_new_with_label(glyph);
        if (g_n_char_keys < (int)(sizeof(g_char_keys) / sizeof(g_char_keys[0]))) {
            g_char_keys[g_n_char_keys].btn = btn;
            g_char_keys[g_n_char_keys].kc = kc;
            g_n_char_keys++;
        }
        g_signal_connect(btn, "clicked", G_CALLBACK(on_key_clicked), (gpointer)(guintptr)kc);
        break;
    }
    case KK_ACTION:
        btn = gtk_button_new_with_label(spec->label);
        g_signal_connect(btn, "clicked", G_CALLBACK(on_key_clicked), (gpointer)(guintptr)kc);
        break;
    case KK_MODIFIER:
        btn = gtk_toggle_button_new_with_label(spec->label);
        if (spec->keysym == XK_Shift_L) {
            g_shift_btns[0] = btn;
            g_signal_connect(btn, "toggled", G_CALLBACK(on_shift_toggled), NULL);
        } else if (spec->keysym == XK_Shift_R) {
            g_shift_btns[1] = btn;
            g_signal_connect(btn, "toggled", G_CALLBACK(on_shift_toggled), NULL);
        } else if (spec->keysym == XK_Control_L) {
            g_ctrl_btn = btn;
            g_signal_connect(btn, "toggled", G_CALLBACK(on_ctrl_toggled), NULL);
        } else if (spec->keysym == XK_Alt_L) {
            g_alt_btns[0] = btn;
            g_signal_connect(btn, "toggled", G_CALLBACK(on_alt_toggled), NULL);
        } else if (spec->keysym == XK_Alt_R) {
            g_alt_btns[1] = btn;
            g_signal_connect(btn, "toggled", G_CALLBACK(on_alt_toggled), NULL);
        } else if (spec->keysym == XK_Super_L) {
            g_super_btn = btn;
            g_signal_connect(btn, "toggled", G_CALLBACK(on_super_toggled), NULL);
        }
        break;
    case KK_CAPSLOCK:
    default:
        btn = gtk_button_new_with_label(spec->label);
        g_capslock_btn = btn;
        g_signal_connect(btn, "clicked", G_CALLBACK(on_capslock_clicked), NULL);
        break;
    }

    gtk_widget_set_size_request(btn, (int)(BASE_KEY_W * spec->weight), KEY_H);
    /* GTK buttons grab GTK-internal focus on click for keyboard
     * activation (Space/Enter re-triggering them) -- harmless for a
     * mouse-only on-screen keyboard, but GTK_CAN_FOCUS still lets Tab
     * land on them if some other input ever reaches this window. Since
     * this window never holds X input focus (see build below), that
     * never happens in practice; left as GTK's default. */
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

    for (int i = 0; i < N_ROWS; i++) {
        gtk_box_pack_start(GTK_BOX(vbox), build_row(&kRows[i]), TRUE, TRUE, 0);
    }

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
