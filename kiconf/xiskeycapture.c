/* xiskeycapture - "Pressione as teclas..." dialog: records one key
 * combination and prints it as a key spec (shared/xis_keyspec.h) on
 * stdout. Any program configuring a hotkey runs it and reads the answer;
 * GTK callers have shared/xis_keycapture.h for that.
 *
 *   xiskeycapture [--title TEXT] [--current SPEC] [--modkey alt|meta]
 *                 [--allow-modifier-only] [--transient-for XID]
 *
 * Exit status: 0 = spec printed, 1 = cancelled, 2 = "Limpar" (unbind),
 * 3 = "Editar como texto" (the caller falls back to its text field),
 * 64 = bad arguments.
 *
 * Keys: anything with a modifier, or any single key, becomes the
 * candidate shown in the dialog -- nothing is accepted until OK/Enter, so
 * Ctrl+Esc and Alt+Enter are capturable and a single key needs no
 * special mode. Bare Esc cancels; bare Enter confirms the candidate.
 * The only combinations the keyboard can't express (bare Esc/Enter) go
 * through "Editar como texto".
 *
 * Why a separate process: the keyboard has to be actively grabbed, or
 * kiwm/xiskeys/xispanel's passive grabs would fire instead (Alt+Tab
 * would open the switcher). A grab held by a process that dies is
 * released by the X server with its connection, so:
 *   - only the keyboard is grabbed, never the pointer -- Cancelar stays
 *     clickable whatever happens to the keyboard;
 *   - a hung main loop is killed by SIGALRM's default action: alarm() is
 *     re-armed only from a main-loop timeout, so if the loop stops the
 *     process dies and the grab goes with it;
 *   - the caller also kills it by PID past a deadline.
 * While open it owns the _XIS_KEYCAPTURE selection, which xispanel's
 * modtap checks so a Meta tap here doesn't open the launcher (X Record
 * sees keys regardless of grabs). Ownership also dies with the process. */
#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <gdk/gdkkeysyms.h>
#include <X11/XKBlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../shared/xis_i18n.h"
#include "../shared/xis_keyspec.h"

enum { EXIT_OK = 0, EXIT_CANCEL = 1, EXIT_CLEAR = 2, EXIT_EDIT_TEXT = 3, EXIT_USAGE = 64 };
enum { RESP_CLEAR = 1, RESP_EDIT_TEXT = 2 };

#define WATCHDOG_SECS 10    /* main loop stalled this long -> SIGALRM kills us */
#define IDLE_SECS 60        /* no key at all this long -> cancel, giving the keyboard back */
#define GRAB_TRIES 20       /* x 100 ms: someone else (kiwm mid-Alt+Tab) may hold it */

static struct {
    GtkWidget *dialog, *keys_label, *hint_label, *ok_btn;
    char candidate[128];
    unsigned int modkey;
    gboolean allow_modifier_only;
    gboolean grabbed;
    int grab_tries;
    guint idle_id;
    /* Modifier-only tap tracking: the modifiers pressed since all were
     * last up, and whether a real key came in between. */
    unsigned int tap_mods;
    gboolean tap_saw_key;
} S;

static gboolean watchdog_feed(gpointer d)
{
    (void)d;
    alarm(WATCHDOG_SECS);
    return TRUE;
}

static gboolean idle_expired(gpointer d)
{
    (void)d;
    S.idle_id = 0;
    gtk_dialog_response(GTK_DIALOG(S.dialog), GTK_RESPONSE_CANCEL);
    return FALSE;
}

static void idle_reset(void)
{
    if (S.idle_id)
        g_source_remove(S.idle_id);
    S.idle_id = g_timeout_add_seconds(IDLE_SECS, idle_expired, NULL);
}

static void set_hint(const char *text)
{
    gtk_label_set_text(GTK_LABEL(S.hint_label), text);
}

static void show_keys(unsigned int held)
{
    char buf[160];
    if (held) {
        char mods[128];
        xis_keyspec_format(held, 0, S.modkey, mods, sizeof(mods));
        snprintf(buf, sizeof(buf), "%s+...", mods);
    } else if (S.candidate[0]) {
        snprintf(buf, sizeof(buf), "%s", S.candidate);
    } else {
        snprintf(buf, sizeof(buf), "%s", _("Pressione as teclas..."));
    }
    char *markup = g_markup_printf_escaped("<big><b>%s</b></big>", buf);
    gtk_label_set_markup(GTK_LABEL(S.keys_label), markup);
    g_free(markup);
    gtk_widget_set_sensitive(S.ok_btn, S.candidate[0] != '\0');
}

static void set_candidate(unsigned int mods, unsigned long keysym)
{
    if (xis_keyspec_format(mods, keysym, S.modkey, S.candidate, sizeof(S.candidate)))
        set_hint(_("Enter ou OK confirma; Esc cancela."));
}

/* Level 0 of group 0, so Shift+1 is "Shift+1" and not "Shift+exclam",
 * whatever the active layout -- the parsers grab by keycode anyway. */
static unsigned long base_keysym(GdkEventKey *ev)
{
    Display *xdpy = GDK_DISPLAY_XDISPLAY(gdk_display_get_default());
    KeySym ks = XkbKeycodeToKeysym(xdpy, (KeyCode)ev->hardware_keycode, 0, 0);
    return ks != NoSymbol ? (unsigned long)ks : (unsigned long)ev->keyval;
}

static gboolean on_key_press(GtkWidget *w, GdkEventKey *ev, gpointer d)
{
    (void)w;
    (void)d;
    idle_reset();
    unsigned long ks = base_keysym(ev);
    unsigned int mods = ev->state & XIS_KEYSPEC_MODS;

    if (IsModifierKey(ks)) {
        unsigned int bit = xis_keyspec_modifier_bit(ks);
        if (!mods)
            S.tap_mods = 0, S.tap_saw_key = FALSE;
        S.tap_mods |= bit;
        show_keys(mods | bit);
        return TRUE;
    }
    S.tap_saw_key = TRUE;

    /* AltGr (ISO_Level3, usually Mod5) and Mod3 have no name in the
     * grammar; dropping them silently would record the wrong combo. */
    if (ev->state & (Mod3Mask | Mod5Mask)) {
        set_hint(_("AltGr nao pode fazer parte de um atalho."));
        return TRUE;
    }
    if (!mods && ks == XK_Escape) {
        gtk_dialog_response(GTK_DIALOG(S.dialog), GTK_RESPONSE_CANCEL);
        return TRUE;
    }
    if (!mods && (ks == XK_Return || ks == XK_KP_Enter)) {
        if (S.candidate[0])
            gtk_dialog_response(GTK_DIALOG(S.dialog), GTK_RESPONSE_OK);
        else
            set_hint(_("Esc ou Enter sozinhos: use \"Editar como texto\"."));
        return TRUE;
    }
    set_candidate(mods, ks);
    show_keys(mods);
    return TRUE;
}

static gboolean on_key_release(GtkWidget *w, GdkEventKey *ev, gpointer d)
{
    (void)w;
    (void)d;
    unsigned long ks = base_keysym(ev);
    if (!IsModifierKey(ks))
        return TRUE;
    unsigned int held = (ev->state & XIS_KEYSPEC_MODS) & ~xis_keyspec_modifier_bit(ks);
    if (!held) {
        /* A modifier tapped alone (xispanel's hotkey=Meta): only one, and
         * no real key in between. */
        unsigned int t = S.tap_mods;
        if (S.allow_modifier_only && !S.tap_saw_key && t && !(t & (t - 1)))
            set_candidate(t, 0);
        S.tap_mods = 0;
        S.tap_saw_key = FALSE;
    }
    show_keys(held);
    return TRUE;
}

static gboolean try_grab(gpointer d)
{
    (void)d;
    GdkWindow *win = gtk_widget_get_window(S.dialog);
    if (win && gdk_keyboard_grab(win, FALSE, GDK_CURRENT_TIME) == GDK_GRAB_SUCCESS) {
        S.grabbed = TRUE;
        return FALSE;
    }
    if (++S.grab_tries < GRAB_TRIES)
        return TRUE;
    /* Still keys reach us while focused, but shortcuts other programs
     * already grab will fire there instead of being recorded. */
    set_hint(_("Nao foi possivel capturar o teclado: atalhos ja em uso por outros programas vao disparar."));
    return FALSE;
}

static gboolean on_map(GtkWidget *w, GdkEvent *ev, gpointer d)
{
    (void)ev;
    (void)d;
    GdkWindow *win = gtk_widget_get_window(w);
    gdk_selection_owner_set(win, gdk_atom_intern_static_string("_XIS_KEYCAPTURE"), GDK_CURRENT_TIME, FALSE);
    if (try_grab(NULL))
        g_timeout_add(100, try_grab, NULL);
    return FALSE;
}

static void usage(void)
{
    fprintf(stderr,
            "usage: xiskeycapture [--title TEXT] [--current SPEC] [--modkey alt|meta]\n"
            "                     [--allow-modifier-only] [--transient-for XID]\n");
}

int main(int argc, char **argv)
{
    /* SIGALRM must kill (its default): that's the hung-main-loop exit. */
    signal(SIGALRM, SIG_DFL);
    alarm(WATCHDOG_SECS);

    xis_i18n_init("kiconf");
    gtk_init(&argc, &argv);

    const char *title = NULL, *current = NULL;
    unsigned long transient_for = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--title") && v) {
            title = v, i++;
        } else if (!strcmp(a, "--current") && v) {
            current = v, i++;
        } else if (!strcmp(a, "--modkey") && v) {
            S.modkey = xis_keyspec_parse_modifier(v), i++;
            if (!S.modkey) {
                usage();
                return EXIT_USAGE;
            }
        } else if (!strcmp(a, "--allow-modifier-only")) {
            S.allow_modifier_only = TRUE;
        } else if (!strcmp(a, "--transient-for") && v) {
            transient_for = strtoul(v, NULL, 0), i++;
        } else {
            usage();
            return EXIT_USAGE;
        }
    }

    S.dialog = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(S.dialog), title && *title ? title : _("Definir atalho"));
    gtk_window_set_position(GTK_WINDOW(S.dialog), GTK_WIN_POS_MOUSE);
    gtk_window_set_keep_above(GTK_WINDOW(S.dialog), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(S.dialog), FALSE);
    gtk_dialog_set_has_separator(GTK_DIALOG(S.dialog), FALSE);

    GtkWidget *clear_btn = gtk_dialog_add_button(GTK_DIALOG(S.dialog), _("_Limpar"), RESP_CLEAR);
    gtk_widget_set_tooltip_text(clear_btn, _("Deixar sem atalho"));
    gtk_dialog_add_button(GTK_DIALOG(S.dialog), _("Editar como _texto"), RESP_EDIT_TEXT);
    gtk_dialog_add_button(GTK_DIALOG(S.dialog), _("_Cancelar"), GTK_RESPONSE_CANCEL);
    S.ok_btn = gtk_dialog_add_button(GTK_DIALOG(S.dialog), _("_OK"), GTK_RESPONSE_OK);

    GtkWidget *box = gtk_vbox_new(FALSE, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    S.keys_label = gtk_label_new(NULL);
    gtk_widget_set_size_request(S.keys_label, 320, -1);
    S.hint_label = gtk_label_new(NULL);
    gtk_label_set_line_wrap(GTK_LABEL(S.hint_label), TRUE);
    gtk_box_pack_start(GTK_BOX(box), S.keys_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), S.hint_label, FALSE, FALSE, 0);
    if (current && *current) {
        char *cur = g_strdup_printf(_("Atual: %s"), current);
        GtkWidget *cur_label = gtk_label_new(cur);
        g_free(cur);
        gtk_widget_set_sensitive(cur_label, FALSE);
        gtk_box_pack_start(GTK_BOX(box), cur_label, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(S.dialog)->vbox), box, TRUE, TRUE, 0);

    set_hint(_("Esc cancela. Uma tecla sozinha tambem vale: aperte-a e depois Enter."));
    show_keys(0);

    g_signal_connect(S.dialog, "key-press-event", G_CALLBACK(on_key_press), NULL);
    g_signal_connect(S.dialog, "key-release-event", G_CALLBACK(on_key_release), NULL);
    g_signal_connect(S.dialog, "map-event", G_CALLBACK(on_map), NULL);

    gtk_widget_realize(S.dialog);
    if (transient_for) {
        GdkWindow *parent = gdk_window_foreign_new((GdkNativeWindow)transient_for);
        if (parent)
            gdk_window_set_transient_for(gtk_widget_get_window(S.dialog), parent);
    }
    gtk_widget_show_all(S.dialog);

    g_timeout_add_seconds(1, watchdog_feed, NULL);
    idle_reset();

    gint resp = gtk_dialog_run(GTK_DIALOG(S.dialog));
    if (S.grabbed)
        gdk_keyboard_ungrab(GDK_CURRENT_TIME);
    gtk_widget_destroy(S.dialog);
    gdk_flush();

    switch (resp) {
    case GTK_RESPONSE_OK:
        if (!S.candidate[0])
            return EXIT_CANCEL;
        printf("%s\n", S.candidate);
        fflush(stdout);
        return EXIT_OK;
    case RESP_CLEAR:
        return EXIT_CLEAR;
    case RESP_EDIT_TEXT:
        return EXIT_EDIT_TEXT;
    default:
        return EXIT_CANCEL;
    }
}
