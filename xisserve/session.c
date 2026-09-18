/*
 * session.c - `xisserve --session`: a centered, always-on-top picker for
 * "what do you want to do with this session" -- Desligar/Reiniciar/
 * Suspender/Sair/Trocar usuario/Bloquear tela, one button each, drawing
 * from the exact same table and confirmation dialogs the launcher's own
 * footer buttons use (xisserve_n_power_actions() et al., see xisserve.c
 * and xisserve.h) so there is exactly one place each of those commands
 * lives, not two copies to keep in sync by hand.
 *
 * Meant to replace binding a hotkey straight to `systemctl poweroff` (or
 * reboot/suspend/...): xiskeys' defaults point every such action at
 * `xisserve --session` instead, so a stray Ctrl+Alt+Delete tap (or any
 * other power key) always asks first, through one shared picker, rather
 * than each key having its own direct-and-immediate command (some of
 * which -- systemctl poweroff/reboot -- already confirm via the
 * launcher's own footer, but not when bound as a bare hotkey).
 *
 * Like --menu/--applications/--question, this is a one-shot popup,
 * entirely outside the launcher's singleton/control-socket machinery:
 * main() dispatches here before the flock is even opened, so it never
 * disturbs a running launcher and any number of instances could exist at
 * once (though in practice a hotkey daemon only ever fires one at a
 * time).
 *
 * Unlike --question's plain centered dialog, this picker has an opinion
 * about *which* screen "centered" means on a multi-monitor session: the
 * one the pointer (or focused window) is actually on, via the same
 * xisserve_resolve_active_output() lookup --keyboard already uses to
 * dock itself (see keyboard.c) -- falling back to the whole default
 * screen if that lookup fails for any reason.
 */
#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <gdk/gdkkeysyms.h>

#include "xisserve.h"

static gboolean g_action_taken;

static void on_power_pick(GtkWidget *btn, gpointer user_data)
{
    GtkWidget *window = gtk_widget_get_toplevel(btn);
    int idx = GPOINTER_TO_INT(user_data);
    if (xisserve_power_action_run(idx, window)) {
        g_action_taken = TRUE;
        gtk_widget_destroy(window);
    }
    /* No/dismissed: leave the picker up so another action can be chosen. */
}

static void on_cancel_clicked(GtkWidget *btn, gpointer data)
{
    (void)data;
    gtk_widget_destroy(gtk_widget_get_toplevel(btn));
}

static void on_destroy(GtkWidget *w, gpointer data)
{
    (void)w;
    (void)data;
    gtk_main_quit();
}

static gboolean on_key_press(GtkWidget *w, GdkEventKey *ev, gpointer data)
{
    (void)data;
    if (ev->keyval == GDK_Escape) {
        gtk_widget_destroy(w);
        return TRUE;
    }
    return FALSE;
}

/* Same "prefer the RandR monitor the pointer/focused window is actually
 * on, fall back to the whole default screen" reasoning keyboard.c's
 * --keyboard docking already uses -- see xisserve_resolve_active_output()
 * in xisserve.h. */
static void get_current_output_rect(int *ox, int *oy, int *ow, int *oh)
{
    Display *dpy = GDK_DISPLAY_XDISPLAY(gdk_display_get_default());
    Window root = GDK_ROOT_WINDOW();
    if (xisserve_resolve_active_output(dpy, root, ox, oy, ow, oh)) return;

    GdkScreen *screen = gdk_screen_get_default();
    *ox = 0;
    *oy = 0;
    *ow = gdk_screen_get_width(screen);
    *oh = gdk_screen_get_height(screen);
}

int session_run(void)
{
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "Sessao");
    gtk_window_set_type_hint(GTK_WINDOW(window), GDK_WINDOW_TYPE_HINT_DIALOG);
    gtk_window_set_keep_above(GTK_WINDOW(window), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(window), FALSE);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(window), TRUE);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(window), TRUE);
    gtk_container_set_border_width(GTK_CONTAINER(window), 12);
    g_signal_connect(window, "destroy", G_CALLBACK(on_destroy), NULL);
    g_signal_connect(window, "key-press-event", G_CALLBACK(on_key_press), NULL);

    GtkWidget *vbox = gtk_vbox_new(FALSE, 10);
    gtk_container_add(GTK_CONTAINER(window), vbox);

    GtkWidget *header = gtk_hbox_new(FALSE, 8);
    GtkWidget *title = gtk_label_new("O que voce deseja fazer?");
    gtk_misc_set_alignment(GTK_MISC(title), 0.0, 0.5);
    gtk_box_pack_start(GTK_BOX(header), title, TRUE, TRUE, 0);
    GtkWidget *close_btn = gtk_button_new_with_label("\xc3\x97"); /* U+00D7 MULTIPLICATION SIGN */
    gtk_widget_set_size_request(close_btn, 28, 22);
    g_signal_connect(close_btn, "clicked", G_CALLBACK(on_cancel_clicked), NULL);
    gtk_box_pack_end(GTK_BOX(header), close_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), header, FALSE, FALSE, 0);

    /* One button per visible power action, same table (and same
     * probe_bin-driven skipping) the launcher's own footer uses. */
    GtkWidget *actions = gtk_vbox_new(TRUE, 4);
    int n = xisserve_n_power_actions();
    for (int i = 0; i < n; i++) {
        if (!xisserve_power_action_visible(i)) continue;
        GtkWidget *btn = gtk_button_new_with_label(xisserve_power_action_label(i));
        g_signal_connect(btn, "clicked", G_CALLBACK(on_power_pick), GINT_TO_POINTER(i));
        gtk_box_pack_start(GTK_BOX(actions), btn, TRUE, TRUE, 0);
    }
    gtk_box_pack_start(GTK_BOX(vbox), actions, FALSE, FALSE, 0);

    GtkWidget *sep = gtk_hseparator_new();
    gtk_box_pack_start(GTK_BOX(vbox), sep, FALSE, FALSE, 0);

    GtkWidget *cancel_btn = gtk_button_new_with_label("Cancelar");
    g_signal_connect(cancel_btn, "clicked", G_CALLBACK(on_cancel_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(vbox), cancel_btn, FALSE, FALSE, 0);

    gtk_widget_set_size_request(window, 220, -1);

    /* gtk_widget_show_all(vbox) -- not the window -- flags every button
     * visible (an invisible widget's requisition is (0,0) in GTK2, so
     * skipping this would under-measure the window) without mapping
     * anything to the X server yet, since vbox's parent (the window)
     * isn't shown/realized itself yet: only gtk_widget_show(window)
     * below actually maps it. That ordering matters here (unlike
     * xisserve.c's own reposition_window(), which shows the real window
     * first) because this window is WM-managed -- showing it before
     * gtk_window_move() would let the WM place it once, then jump it,
     * flickering visibly. */
    gtk_widget_show_all(vbox);

    int ox, oy, ow, oh;
    get_current_output_rect(&ox, &oy, &ow, &oh);
    GtkRequisition req;
    gtk_widget_size_request(window, &req);
    gtk_window_move(GTK_WINDOW(window), ox + (ow - req.width) / 2, oy + (oh - req.height) / 2);

    gtk_widget_show(window);
    gtk_window_present(GTK_WINDOW(window));
    gtk_widget_grab_focus(cancel_btn);

    gtk_main();

    return g_action_taken ? 0 : 1;
}
