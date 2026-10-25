/* kiterm - tabbed terminal emulator for KiDesktop, GTK2 + VTE 0.28.
 *
 * One process per window; Ctrl+Shift+N starts another. Settings live in
 * ~/.config/kiterm.conf (kt_config.c) and are edited from the context
 * menu's Preferences (kt_prefs.c). Tabs, find bar and the context menu are
 * in kt_tab.c / kt_search.c. */
#include "kiterm.h"
#include "../shared/xis_i18n.h"
#include "../shared/xis_winstate.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

static void usage(void)
{
    printf("kiterm %s - tabbed terminal emulator\n\n"
           "Usage: kiterm [options] [-e COMMAND [ARGS...]]\n\n"
           "  -e, -x, --command    run COMMAND (and its ARGS) instead of the shell;\n"
           "                       must be the last option\n"
           "  --working-directory=DIR  start in DIR\n"
           "  --title=TITLE        window title\n"
           "  --geometry=COLSxROWS initial size in characters\n"
           "  -h, --help           this text\n"
           "  -v, --version        version\n", KITERM_VERSION);
}

VteTerminal *kt_window_current(KtWindow *w)
{
    int n = gtk_notebook_get_current_page(GTK_NOTEBOOK(w->notebook));
    if (n < 0)
        return NULL;
    return VTE_TERMINAL(gtk_notebook_get_nth_page(GTK_NOTEBOOK(w->notebook), n));
}

void kt_window_update_title(KtWindow *w)
{
    VteTerminal *t = kt_window_current(w);
    if (w->title_override) {
        gtk_window_set_title(GTK_WINDOW(w->win), w->title_override);
        return;
    }
    char *title = g_strdup_printf("%s - kiterm", t ? kt_tab_title(t) : "kiterm");
    gtk_window_set_title(GTK_WINDOW(w->win), title);
    g_free(title);
}

static void update_tab_bar(KtWindow *w)
{
    GtkNotebook *nb = GTK_NOTEBOOK(w->notebook);
    gtk_notebook_set_show_tabs(nb, gtk_notebook_get_n_pages(nb) > 1);
}

void kt_window_apply_config(KtWindow *w)
{
    GtkNotebook *nb = GTK_NOTEBOOK(w->notebook);
    gtk_notebook_set_tab_pos(nb, (GtkPositionType)(w->cfg.tabs_pos == KT_TABS_TOP ? GTK_POS_TOP :
        w->cfg.tabs_pos == KT_TABS_BOTTOM ? GTK_POS_BOTTOM :
        w->cfg.tabs_pos == KT_TABS_LEFT ? GTK_POS_LEFT : GTK_POS_RIGHT));
    int n = gtk_notebook_get_n_pages(nb);
    for (int i = 0; i < n; i++)
        kt_tab_apply_config(w, VTE_TERMINAL(gtk_notebook_get_nth_page(nb, i)));
    kt_window_poll_update(w);
}

void kt_window_zoom(KtWindow *w, int dir)
{
    if (dir == 0)
        w->zoom = 1.0;
    else
        w->zoom = CLAMP(w->zoom * (dir > 0 ? 1.1 : 1.0 / 1.1), 0.4, 4.0);
    kt_window_apply_config(w);
}

void kt_window_spawn_self(const char *cwd)
{
    char self[4096];
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n <= 0)
        return;
    self[n] = 0;
    char *argv[] = { self, NULL };
    g_spawn_async(cwd, argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL, NULL);
}

void kt_window_new_tab(KtWindow *w, const char *cwd, char **argv)
{
    GtkWidget *term = kt_tab_create(w, cwd, argv);
    GtkNotebook *nb = GTK_NOTEBOOK(w->notebook);
    int pos = gtk_notebook_get_current_page(nb) + 1;
    int idx = gtk_notebook_insert_page(nb, term, kt_tab_make_label(w, VTE_TERMINAL(term)), pos);
    gtk_notebook_set_tab_reorderable(nb, term, TRUE);
    gtk_widget_show(term);
    gtk_notebook_set_current_page(nb, idx);
    update_tab_bar(w);
    kt_window_poll_update(w);
    gtk_widget_grab_focus(term);
    kt_window_update_title(w);
}

/* ---- closing ----------------------------------------------------------- */

static gboolean confirm(KtWindow *w, const char *text)
{
    if (!w->cfg.confirm_close)
        return TRUE;
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(w->win), GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION,
                                          GTK_BUTTONS_NONE, "%s", text);
    gtk_dialog_add_buttons(GTK_DIALOG(d), GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
                           GTK_STOCK_CLOSE, GTK_RESPONSE_ACCEPT, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_CANCEL);
    int r = gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
    return r == GTK_RESPONSE_ACCEPT;
}

static void remove_tab(KtWindow *w, GtkWidget *term)
{
    GtkNotebook *nb = GTK_NOTEBOOK(w->notebook);
    int idx = gtk_notebook_page_num(nb, term);
    if (idx < 0)
        return;
    gtk_notebook_remove_page(nb, idx);
    if (gtk_notebook_get_n_pages(nb) == 0) {
        gtk_main_quit();
        return;
    }
    update_tab_bar(w);
    kt_window_poll_update(w);
    kt_window_update_title(w);
}

/* From the close button / menu / shortcut: asks first when a program runs.
 * (A shell that exited by itself comes through here too; it is not busy.) */
void kt_window_close_tab(KtWindow *w, GtkWidget *term)
{
    if (!term)
        return;
    if (kt_tab_busy(VTE_TERMINAL(term)) &&
        !confirm(w, _("A program is still running in this tab. Close it anyway?")))
        return;
    remove_tab(w, term);
}

static gboolean on_delete(GtkWidget *win, GdkEvent *ev, gpointer data)
{
    (void)win; (void)ev;
    KtWindow *w = data;
    GtkNotebook *nb = GTK_NOTEBOOK(w->notebook);
    for (int i = 0; i < gtk_notebook_get_n_pages(nb); i++) {
        if (kt_tab_busy(VTE_TERMINAL(gtk_notebook_get_nth_page(nb, i)))) {
            return !confirm(w, _("Programs are still running in this window. Close it anyway?"));
        }
    }
    return FALSE;
}

/* ---- keyboard ---------------------------------------------------------- */

static gboolean on_key(GtkWidget *win, GdkEventKey *ev, gpointer data)
{
    (void)win;
    KtWindow *w = data;
    guint mods = ev->state & gtk_accelerator_get_default_mod_mask();
    guint key = gdk_keyval_to_lower(ev->keyval);
    GtkNotebook *nb = GTK_NOTEBOOK(w->notebook);
    VteTerminal *t = kt_window_current(w);
    const guint cs = GDK_CONTROL_MASK | GDK_SHIFT_MASK;

    if (mods == cs) {
        switch (key) {
        case GDK_KEY_t: {
            char *cwd = t ? kt_tab_cwd(t) : NULL;
            kt_window_new_tab(w, cwd, NULL);
            g_free(cwd);
            return TRUE;
        }
        case GDK_KEY_n: {
            char *cwd = t ? kt_tab_cwd(t) : NULL;
            kt_window_spawn_self(cwd);
            g_free(cwd);
            return TRUE;
        }
        case GDK_KEY_w: kt_window_close_tab(w, GTK_WIDGET(t)); return TRUE;
        case GDK_KEY_c: if (t) vte_terminal_copy_clipboard(t); return TRUE;
        case GDK_KEY_v: if (t) vte_terminal_paste_clipboard(t); return TRUE;
        case GDK_KEY_f: kt_search_show(w); return TRUE;
        case GDK_KEY_g: kt_search_step(w, TRUE); return TRUE;
        case GDK_KEY_r: if (t) kt_tab_rename(w, t); return TRUE;
        case GDK_KEY_comma: case GDK_KEY_less: kt_prefs_show(w); return TRUE;
        case GDK_KEY_Page_Up: case GDK_KEY_Page_Down: {
            int cur = gtk_notebook_get_current_page(nb), n = gtk_notebook_get_n_pages(nb);
            int to = CLAMP(cur + (key == GDK_KEY_Page_Down ? 1 : -1), 0, n - 1);
            if (to != cur)
                gtk_notebook_reorder_child(nb, gtk_notebook_get_nth_page(nb, cur), to);
            return TRUE;
        }
        case GDK_KEY_plus: case GDK_KEY_equal: kt_window_zoom(w, 1); return TRUE;
        }
    }
    if (mods == GDK_CONTROL_MASK) {
        switch (key) {
        case GDK_KEY_Page_Up: gtk_notebook_prev_page(nb); return TRUE;
        case GDK_KEY_Page_Down: gtk_notebook_next_page(nb); return TRUE;
        case GDK_KEY_plus: case GDK_KEY_equal: case GDK_KEY_KP_Add: kt_window_zoom(w, 1); return TRUE;
        case GDK_KEY_minus: case GDK_KEY_KP_Subtract: kt_window_zoom(w, -1); return TRUE;
        case GDK_KEY_0: case GDK_KEY_KP_0: kt_window_zoom(w, 0); return TRUE;
        }
    }
    if (mods == GDK_MOD1_MASK && key >= GDK_KEY_1 && key <= GDK_KEY_9) {
        int n = key - GDK_KEY_1;
        if (n < gtk_notebook_get_n_pages(nb))
            gtk_notebook_set_current_page(nb, n);
        return TRUE;
    }
    if (mods == GDK_SHIFT_MASK && key == GDK_KEY_Insert) {
        if (t) vte_terminal_paste_clipboard(t);
        return TRUE;
    }
    if (mods == GDK_CONTROL_MASK && key == GDK_KEY_Insert) {
        if (t) vte_terminal_copy_clipboard(t);
        return TRUE;
    }
    if (ev->keyval == GDK_KEY_F11 && !mods) {
        GdkWindowState st = gdk_window_get_state(gtk_widget_get_window(w->win));
        if (st & GDK_WINDOW_STATE_FULLSCREEN)
            gtk_window_unfullscreen(GTK_WINDOW(w->win));
        else
            gtk_window_fullscreen(GTK_WINDOW(w->win));
        return TRUE;
    }
    return FALSE;
}

static gboolean title_idle(gpointer data)
{
    KtWindow *w = data;
    kt_window_update_title(w);
    VteTerminal *t = kt_window_current(w);
    if (t && w->focused)
        kt_tab_set_attention(t, FALSE);     /* seen now */
    return FALSE;
}

static void on_switch_page(GtkNotebook *nb, gpointer page, guint n, gpointer data)
{
    (void)nb; (void)page; (void)n;
    /* The page is not "current" yet inside the signal: update afterwards. */
    g_idle_add(title_idle, data);
}

/* Background-job watch: while the window is unfocused or has several tabs, look
 * every 2 s for a foreground program that has returned to the shell. With one
 * focused tab there is nothing to watch and the timer isn't running at all. */
static gboolean poll_jobs(gpointer data)
{
    KtWindow *w = data;
    GtkNotebook *nb = GTK_NOTEBOOK(w->notebook);
    gint64 now = g_get_monotonic_time();
    VteTerminal *cur = kt_window_current(w);
    for (int i = 0; i < gtk_notebook_get_n_pages(nb); i++) {
        VteTerminal *t = VTE_TERMINAL(gtk_notebook_get_nth_page(nb, i));
        gint64 since = (gint64)GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(t), "kt-busy-since"));
        gboolean busy = kt_tab_busy(t);
        if (busy && !since) {
            g_object_set_data(G_OBJECT(t), "kt-busy-since", GSIZE_TO_POINTER((gsize)now));
        } else if (!busy && since) {
            g_object_set_data(G_OBJECT(t), "kt-busy-since", NULL);
            if (w->cfg.notify_done && (!w->focused || t != cur) &&
                now - since >= (gint64)w->cfg.notify_done_secs * G_USEC_PER_SEC)
                kt_tab_job_done(w, t);
        }
    }
    return TRUE;
}

void kt_window_poll_update(KtWindow *w)
{
    gboolean need = w->cfg.notify_done &&
        (!w->focused || gtk_notebook_get_n_pages(GTK_NOTEBOOK(w->notebook)) > 1);
    if (need && !w->poll_id)
        w->poll_id = g_timeout_add_seconds(2, poll_jobs, w);
    else if (!need && w->poll_id) {
        g_source_remove(w->poll_id);
        w->poll_id = 0;
    }
}

static gboolean on_focus_in(GtkWidget *win, GdkEvent *ev, gpointer data)
{
    (void)ev;
    KtWindow *w = data;
    w->focused = TRUE;
    gtk_window_set_urgency_hint(GTK_WINDOW(win), FALSE);
    VteTerminal *t = kt_window_current(w);
    if (t)
        kt_tab_set_attention(t, FALSE);
    kt_window_poll_update(w);
    return FALSE;
}

static gboolean on_focus_out(GtkWidget *win, GdkEvent *ev, gpointer data)
{
    (void)win; (void)ev;
    KtWindow *w = data;
    w->focused = FALSE;
    kt_window_poll_update(w);
    return FALSE;
}

/* With an RGBA colormap GTK would paint the theme background opaquely over
 * everything; the window is cleared to transparent only under the terminal
 * (VTE paints its own translucent background there) and keeps the theme
 * background everywhere else: tab bar, find bar, borders. */
static gboolean on_expose_clear(GtkWidget *win, GdkEventExpose *ev, gpointer data)
{
    KtWindow *w = data;
    cairo_t *cr = gdk_cairo_create(gtk_widget_get_window(win));
    gdk_cairo_region(cr, ev->region);
    cairo_clip(cr);
    gdk_cairo_set_source_color(cr, &gtk_widget_get_style(win)->bg[GTK_STATE_NORMAL]);
    cairo_paint(cr);
    VteTerminal *t = kt_window_current(w);
    if (t && gtk_widget_get_mapped(GTK_WIDGET(t))) {
        GtkAllocation a;
        gint x, y;
        gtk_widget_get_allocation(GTK_WIDGET(t), &a);
        if (gtk_widget_translate_coordinates(GTK_WIDGET(t), win, 0, 0, &x, &y)) {
            cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
            cairo_rectangle(cr, x, y, a.width, a.height);
            cairo_fill(cr);
        }
    }
    cairo_destroy(cr);
    return FALSE;
}

/* The terminal whose tab label is under the pointer, or NULL. */
static GtkWidget *tab_at(GtkWidget *nb, GdkEventButton *ev)
{
    GtkNotebook *n = GTK_NOTEBOOK(nb);
    for (int i = 0; i < gtk_notebook_get_n_pages(n); i++) {
        GtkWidget *page = gtk_notebook_get_nth_page(n, i);
        GtkWidget *lab = gtk_notebook_get_tab_label(n, page);
        GtkAllocation a;
        gtk_widget_get_allocation(lab, &a);
        gint x, y;
        gtk_widget_translate_coordinates(nb, lab, (gint)ev->x, (gint)ev->y, &x, &y);
        if (x >= 0 && y >= 0 && x < a.width && y < a.height)
            return page;
    }
    return NULL;
}

static gboolean on_tab_click(GtkWidget *nb, GdkEventButton *ev, gpointer data)
{
    KtWindow *w = data;
    /* Middle click on a tab closes it, double click renames it, right click
     * opens its menu. */
    GtkWidget *page;
    if (ev->button == 2 && ev->type == GDK_BUTTON_PRESS) {
        if ((page = tab_at(nb, ev))) {
            kt_window_close_tab(w, page);
            return TRUE;
        }
    } else if (ev->button == 1 && ev->type == GDK_2BUTTON_PRESS) {
        if ((page = tab_at(nb, ev))) {
            kt_tab_rename(w, VTE_TERMINAL(page));
            return TRUE;
        }
    } else if (ev->button == 3 && ev->type == GDK_BUTTON_PRESS) {
        if ((page = tab_at(nb, ev))) {
            kt_tab_label_menu(w, VTE_TERMINAL(page), ev);
            return TRUE;
        }
    }
    return FALSE;
}

static void on_new_tab_clicked(GtkButton *b, gpointer data)
{
    (void)b;
    KtWindow *w = data;
    char *cwd = kt_tab_cwd(kt_window_current(w));
    kt_window_new_tab(w, cwd, NULL);
    g_free(cwd);
}

int main(int argc, char **argv)
{
    xis_i18n_init("kiterm");
    gtk_init(&argc, &argv);
    setlocale(LC_NUMERIC, "C");     /* gtk_init() resets LC_ALL */

    KtWindow *w = g_new0(KtWindow, 1);
    w->zoom = 1.0;
    kt_config_load(&w->cfg);
    char *workdir = NULL;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(); return 0; }
        if (!strcmp(a, "-v") || !strcmp(a, "--version")) { printf("kiterm %s\n", KITERM_VERSION); return 0; }
        if (!strcmp(a, "-e") || !strcmp(a, "-x") || !strcmp(a, "--command")) {
            if (i + 1 < argc)
                w->cmd_argv = &argv[i + 1];
            break;
        }
        if (!strncmp(a, "--working-directory=", 20)) workdir = g_strdup(a + 20);
        else if (!strncmp(a, "--title=", 8)) w->title_override = g_strdup(a + 8);
        else if (!strncmp(a, "--geometry=", 11)) {
            int c, r;
            if (sscanf(a + 11, "%dx%d", &c, &r) == 2) {
                w->cfg.cols = CLAMP(c, 20, 500);
                w->cfg.rows = CLAMP(r, 5, 200);
            }
        }
    }

    w->win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_icon_name(GTK_WINDOW(w->win), "utilities-terminal");
    gtk_window_set_role(GTK_WINDOW(w->win), "kiterm");
    gtk_widget_set_name(w->win, "kiterm");
    if (w->cfg.opacity < 1.0) {
        GdkScreen *scr = gtk_widget_get_screen(w->win);
        GdkColormap *rgba = gdk_screen_get_rgba_colormap(scr);
        if (rgba && gdk_screen_is_composited(scr)) {
            gtk_widget_set_colormap(w->win, rgba);
            gtk_widget_set_app_paintable(w->win, TRUE);
            g_signal_connect(w->win, "expose-event", G_CALLBACK(on_expose_clear), w);
            w->argb = TRUE;
        }
    }

    GtkWidget *vbox = gtk_vbox_new(FALSE, 0);
    gtk_container_add(GTK_CONTAINER(w->win), vbox);
    w->notebook = gtk_notebook_new();
    GtkNotebook *nb = GTK_NOTEBOOK(w->notebook);
    gtk_notebook_set_scrollable(nb, TRUE);
    gtk_notebook_set_show_border(nb, FALSE);
    gtk_notebook_set_show_tabs(nb, FALSE);
    GtkWidget *plus = gtk_button_new();
    gtk_button_set_relief(GTK_BUTTON(plus), GTK_RELIEF_NONE);
    gtk_button_set_focus_on_click(GTK_BUTTON(plus), FALSE);
    gtk_container_add(GTK_CONTAINER(plus), gtk_image_new_from_stock(GTK_STOCK_ADD, GTK_ICON_SIZE_MENU));
    gtk_widget_set_tooltip_text(plus, _("New tab (Ctrl+Shift+T)"));
    g_signal_connect(plus, "clicked", G_CALLBACK(on_new_tab_clicked), w);
    gtk_widget_show_all(plus);
    gtk_notebook_set_action_widget(nb, plus, GTK_PACK_END);
    gtk_box_pack_start(GTK_BOX(vbox), w->notebook, TRUE, TRUE, 0);
    kt_search_init(w, vbox);

    g_signal_connect(w->win, "key-press-event", G_CALLBACK(on_key), w);
    g_signal_connect(w->win, "delete-event", G_CALLBACK(on_delete), w);
    g_signal_connect(w->win, "focus-in-event", G_CALLBACK(on_focus_in), w);
    g_signal_connect(w->win, "focus-out-event", G_CALLBACK(on_focus_out), w);
    w->focused = TRUE;
    g_signal_connect(w->notebook, "switch-page", G_CALLBACK(on_switch_page), w);
    g_signal_connect(w->notebook, "button-press-event", G_CALLBACK(on_tab_click), w);
    g_signal_connect(w->win, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    /* Position and maximized only: the size follows the terminal grid. */
    xis_winstate_restore(GTK_WINDOW(w->win), "kiterm", XIS_WINSTATE_POS);
    xis_winstate_track(GTK_WINDOW(w->win), "kiterm", XIS_WINSTATE_POS);

    kt_window_apply_config(w);
    kt_window_new_tab(w, workdir, w->cmd_argv);
    g_free(workdir);
    gtk_widget_show_all(vbox);
    gtk_widget_hide(w->search_bar);
    /* The window opens at the grid's size, then stops asking for it. */
    GtkRequisition req;
    gtk_widget_size_request(w->win, &req);
    gtk_window_set_default_size(GTK_WINDOW(w->win), req.width, req.height);
    w->shrinkable = TRUE;
    for (int i = 0; i < gtk_notebook_get_n_pages(nb); i++)
        kt_tab_allow_shrink(VTE_TERMINAL(gtk_notebook_get_nth_page(nb, i)));
    gtk_widget_show(w->win);
    gtk_main();
    return 0;
}
