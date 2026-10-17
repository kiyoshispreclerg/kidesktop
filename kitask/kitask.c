/* kitask - process list and system monitor for KiDesktop (GTK2).
 *
 * The window: optional graphs on top, then a filter bar and the process
 * list, a status line at the bottom. Two timers: the graphs' (system-wide
 * numbers, cheap) and the list's (every process), each with its own interval.
 * The list stops refreshing while the window is minimized; the graphs keep
 * their history going. */
#include "tsk.h"
#include "../shared/xis_i18n.h"
#include <gdk/gdkkeysyms.h>
#include <stdio.h>
#include <string.h>

static TskApp the_app;


/* ---- refresh ---------------------------------------------------------------------- */

static void update_status(TskApp *app)
{
    int total = (int)g_hash_table_size(app->procs.procs);
    int shown = gtk_tree_model_iter_n_children(app->sort_model, NULL);
    char *used = tsk_fmt_bytes(MAX(0, app->sys.mem_total - app->sys.mem_available));
    char *all = tsk_fmt_bytes(app->sys.mem_total);
    char *procs = shown == total ? g_strdup_printf(ngettext("%d process", "%d processes", total), total)
                                 : g_strdup_printf(ngettext("%d of %d process", "%d of %d processes", total), shown,
                                                   total);
    char *text = g_strdup_printf(_("%s    CPU %.0f%%    RAM %s of %s%s"), procs, app->sys.cpu, used, all,
                                 app->list_paused ? _("    (updates paused)") : "");
    gtk_label_set_text(GTK_LABEL(app->status), text);
    g_free(text);
    g_free(procs);
    g_free(used);
    g_free(all);
}

static void refresh_list(TskApp *app)
{
    tsk_procs_sample(&app->procs, tsk_list_need(app));
    tsk_list_update(app);
    update_status(app);
}

static gboolean on_list_timer(gpointer data)
{
    TskApp *app = data;
    GdkWindow *w = gtk_widget_get_window(app->win);
    gboolean iconified = w && (gdk_window_get_state(w) & GDK_WINDOW_STATE_ICONIFIED);
    if (!app->list_paused && !iconified)
        refresh_list(app);
    return TRUE;
}

static gboolean on_graph_timer(gpointer data)
{
    TskApp *app = data;
    tsk_sys_sample(&app->sys);
    if (app->cfg.show_graphs)
        tsk_gpu_sample();
    tsk_graphs_push(app);
    update_status(app);
    return TRUE;
}

void tsk_app_restart_timers(TskApp *app)
{
    if (app->list_timer)
        g_source_remove(app->list_timer);
    if (app->graph_timer)
        g_source_remove(app->graph_timer);
    app->list_timer = g_timeout_add((guint)(app->cfg.list_interval * 1000), on_list_timer, app);
    app->graph_timer = g_timeout_add((guint)(app->cfg.graph_interval * 1000), on_graph_timer, app);
}

void tsk_app_refresh_now(TskApp *app)
{
    refresh_list(app);
}

static gboolean save_now(gpointer data)
{
    TskApp *app = data;
    app->save_timer = 0;
    tsk_config_save(&app->cfg);
    return FALSE;
}

void tsk_app_save_soon(TskApp *app)
{
    if (app->save_timer)
        g_source_remove(app->save_timer);
    app->save_timer = g_timeout_add(800, save_now, app);
}

void tsk_app_apply_graphs_visible(TskApp *app)
{
    gtk_widget_set_visible(app->graphs_box, app->cfg.show_graphs);
    if (app->show_graphs_item &&
        gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(app->show_graphs_item)) != app->cfg.show_graphs)
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(app->show_graphs_item), app->cfg.show_graphs);
}

/* ---- menus -------------------------------------------------------------------------- */

static void on_quit(GtkWidget *w, TskApp *app)
{
    (void)w;
    (void)app;
    gtk_main_quit();
}

static void on_show_graphs(GtkCheckMenuItem *mi, TskApp *app)
{
    if (app->cfg.show_graphs == gtk_check_menu_item_get_active(mi))
        return;
    app->cfg.show_graphs = gtk_check_menu_item_get_active(mi);
    tsk_app_apply_graphs_visible(app);
    tsk_app_save_soon(app);
}

static void on_pause(GtkCheckMenuItem *mi, TskApp *app)
{
    app->list_paused = gtk_check_menu_item_get_active(mi);
    update_status(app);
}

static void on_refresh(GtkWidget *w, TskApp *app)
{
    (void)w;
    tsk_app_refresh_now(app);
}

static void on_settings(GtkWidget *w, TskApp *app)
{
    (void)w;
    tsk_prefs_show(app, 0);
}

static void on_columns_menu_select(GtkMenuItem *mi, TskApp *app)
{
    GtkWidget *sub = gtk_menu_new();
    tsk_list_columns_menu(app, GTK_MENU_SHELL(sub));
    gtk_widget_show_all(sub);
    gtk_menu_item_set_submenu(mi, sub);
}

static void on_proc_signal(GtkWidget *w, TskApp *app)
{
    tsk_send_signal(app, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(w), "sig")));
}

static void on_proc_details(GtkWidget *w, TskApp *app)
{
    (void)w;
    GList *pids = tsk_list_selected_pids(app);
    for (GList *l = pids; l; l = l->next)
        tsk_details(app, GPOINTER_TO_INT(l->data));
    g_list_free(pids);
}

static void on_proc_renice(GtkWidget *w, TskApp *app)
{
    (void)w;
    tsk_renice(app);
}

static void on_proc_window(GtkWidget *w, TskApp *app)
{
    (void)w;
    tsk_activate_window(app);
}

/* the Process menu acts on the selection: greyed out without one */
static void on_process_menu_select(GtkMenuItem *mi, TskApp *app)
{
    GList *pids = tsk_list_selected_pids(app);
    TskProc *p = pids ? g_hash_table_lookup(app->procs.procs, pids->data) : NULL;
    GList *items = gtk_container_get_children(GTK_CONTAINER(gtk_menu_item_get_submenu(mi)));
    for (GList *l = items; l; l = l->next) {
        if (GTK_IS_SEPARATOR_MENU_ITEM(l->data))
            continue;
        gboolean win = g_object_get_data(G_OBJECT(l->data), "needs-window") != NULL;
        gtk_widget_set_sensitive(l->data, pids && (!win || (p && p->window && !pids->next)));
    }
    g_list_free(items);
    g_list_free(pids);
}

static void on_about(GtkWidget *w, TskApp *app)
{
    (void)w;
    gtk_show_about_dialog(GTK_WINDOW(app->win), "program-name", "kitask", "version", KITASK_VERSION, "comments",
                          _("Process list and system monitor for KiDesktop"), "logo-icon-name", "kitask",
                          "license", "GPL-3.0-or-later", NULL);
}

static GtkWidget *menu_item(GtkWidget *menu, const char *label, GCallback cb, TskApp *app, GtkAccelGroup *ag,
                            guint key, GdkModifierType mods)
{
    GtkWidget *mi = gtk_menu_item_new_with_mnemonic(label);
    if (cb)
        g_signal_connect(mi, "activate", cb, app);
    if (ag && key)
        gtk_widget_add_accelerator(mi, "activate", ag, key, mods, GTK_ACCEL_VISIBLE);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    return mi;
}

static GtkWidget *top_menu(GtkWidget *bar, const char *label)
{
    GtkWidget *mi = gtk_menu_item_new_with_mnemonic(label), *menu = gtk_menu_new();
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(mi), menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(bar), mi);
    return menu;
}

static GtkWidget *build_menubar(TskApp *app, GtkAccelGroup *ag)
{
    GtkWidget *bar = gtk_menu_bar_new();
    GtkWidget *m = top_menu(bar, _("_File"));
    menu_item(m, _("_Quit"), G_CALLBACK(on_quit), app, ag, GDK_q, GDK_CONTROL_MASK);

    m = top_menu(bar, _("_View"));
    app->show_graphs_item = gtk_check_menu_item_new_with_mnemonic(_("Show _Graphs"));
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(app->show_graphs_item), app->cfg.show_graphs);
    gtk_widget_add_accelerator(app->show_graphs_item, "activate", ag, GDK_g, GDK_CONTROL_MASK, GTK_ACCEL_VISIBLE);
    g_signal_connect(app->show_graphs_item, "toggled", G_CALLBACK(on_show_graphs), app);
    gtk_menu_shell_append(GTK_MENU_SHELL(m), app->show_graphs_item);
    GtkWidget *cols = menu_item(m, _("_Columns"), NULL, app, NULL, 0, 0);
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(cols), gtk_menu_new());
    g_signal_connect(cols, "select", G_CALLBACK(on_columns_menu_select), app);
    gtk_menu_shell_append(GTK_MENU_SHELL(m), gtk_separator_menu_item_new());
    menu_item(m, _("_Refresh Now"), G_CALLBACK(on_refresh), app, ag, GDK_F5, 0);
    GtkWidget *pause = gtk_check_menu_item_new_with_mnemonic(_("_Pause Updates"));
    gtk_widget_add_accelerator(pause, "activate", ag, GDK_p, GDK_CONTROL_MASK, GTK_ACCEL_VISIBLE);
    g_signal_connect(pause, "toggled", G_CALLBACK(on_pause), app);
    gtk_menu_shell_append(GTK_MENU_SHELL(m), pause);
    gtk_menu_shell_append(GTK_MENU_SHELL(m), gtk_separator_menu_item_new());
    menu_item(m, _("_Settings..."), G_CALLBACK(on_settings), app, ag, GDK_comma, GDK_CONTROL_MASK);

    m = top_menu(bar, _("_Process"));
    GtkWidget *proc_top = gtk_menu_get_attach_widget(GTK_MENU(m));
    g_signal_connect(proc_top, "select", G_CALLBACK(on_process_menu_select), app);
    menu_item(m, _("_Details"), G_CALLBACK(on_proc_details), app, NULL, 0, 0);
    GtkWidget *w = menu_item(m, _("_Switch to Window"), G_CALLBACK(on_proc_window), app, NULL, 0, 0);
    g_object_set_data(G_OBJECT(w), "needs-window", GINT_TO_POINTER(1));
    gtk_menu_shell_append(GTK_MENU_SHELL(m), gtk_separator_menu_item_new());
    static const struct { const char *label; int sig; } sigs[] = {
        {N_("_End (SIGTERM)"), 15}, {N_("_Kill (SIGKILL)"), 9}, {N_("_Pause (SIGSTOP)"), 19},
        {N_("Co_ntinue (SIGCONT)"), 18}};
    for (size_t i = 0; i < G_N_ELEMENTS(sigs); i++) {
        w = menu_item(m, _(sigs[i].label), G_CALLBACK(on_proc_signal), app, NULL, 0, 0);
        g_object_set_data(G_OBJECT(w), "sig", GINT_TO_POINTER(sigs[i].sig));
    }
    gtk_menu_shell_append(GTK_MENU_SHELL(m), gtk_separator_menu_item_new());
    menu_item(m, _("Change _Priority..."), G_CALLBACK(on_proc_renice), app, NULL, 0, 0);

    m = top_menu(bar, _("_Help"));
    menu_item(m, _("_About"), G_CALLBACK(on_about), app, NULL, 0, 0);
    return bar;
}

/* ---- filter bar ---------------------------------------------------------------------- */

static void on_filter_changed(GtkEditable *e, TskApp *app)
{
    (void)e;
    tsk_list_refilter(app);
    update_status(app);
}

static void on_filter_icon(GtkEntry *e, GtkEntryIconPosition pos, GdkEvent *ev, TskApp *app)
{
    (void)ev;
    (void)app;
    if (pos == GTK_ENTRY_ICON_SECONDARY)
        gtk_entry_set_text(e, "");
}

static gboolean on_filter_key(GtkWidget *w, GdkEventKey *ev, TskApp *app)
{
    if (ev->keyval == GDK_Escape) {
        gtk_entry_set_text(GTK_ENTRY(w), "");
        return TRUE;
    }
    if (ev->keyval == GDK_Down || ev->keyval == GDK_Return || ev->keyval == GDK_KP_Enter) {
        gtk_widget_grab_focus(app->list);
        return TRUE;
    }
    return FALSE;
}

static void on_scope(GtkComboBox *c, TskApp *app)
{
    app->cfg.scope = (TskScope)gtk_combo_box_get_active(c);
    /* "with windows" needs window data the list may not be reading yet */
    tsk_app_refresh_now(app);
    tsk_list_refilter(app);
    update_status(app);
    tsk_app_save_soon(app);
}

/* Ctrl+F: through the window's key handler, which runs before the list's
 * own Ctrl+F (its interactive search popup) and needs no accelerator closure */
static gboolean on_window_key(GtkWidget *w, GdkEventKey *ev, TskApp *app)
{
    (void)w;
    if ((ev->keyval == GDK_f || ev->keyval == GDK_F) && (ev->state & GDK_CONTROL_MASK)) {
        gtk_widget_grab_focus(app->filter);
        gtk_editable_select_region(GTK_EDITABLE(app->filter), 0, -1);
        return TRUE;
    }
    return FALSE;
}

static GtkWidget *build_filter_bar(TskApp *app, GtkAccelGroup *ag)
{
    GtkWidget *box = gtk_hbox_new(FALSE, 6);
    gtk_container_set_border_width(GTK_CONTAINER(box), 4);
    app->filter = gtk_entry_new();
    gtk_entry_set_icon_from_icon_name(GTK_ENTRY(app->filter), GTK_ENTRY_ICON_PRIMARY, "edit-find");
    gtk_entry_set_icon_from_icon_name(GTK_ENTRY(app->filter), GTK_ENTRY_ICON_SECONDARY, "edit-clear");
    gtk_entry_set_icon_tooltip_text(GTK_ENTRY(app->filter), GTK_ENTRY_ICON_SECONDARY, _("Clear"));
    gtk_widget_set_tooltip_text(app->filter, _("Filter by name, command line, user, window title or PID (Ctrl+F).\n"
                                               "Separate several with | or , to show any of them: kiplay|kitask"));
    atk_object_set_name(gtk_widget_get_accessible(app->filter), _("Filter processes"));
    g_signal_connect(app->filter, "changed", G_CALLBACK(on_filter_changed), app);
    g_signal_connect(app->filter, "icon-press", G_CALLBACK(on_filter_icon), app);
    g_signal_connect(app->filter, "key-press-event", G_CALLBACK(on_filter_key), app);
    (void)ag;
    gtk_box_pack_start(GTK_BOX(box), app->filter, TRUE, TRUE, 0);

    app->scope = gtk_combo_box_new_text();
    gtk_combo_box_append_text(GTK_COMBO_BOX(app->scope), _("All processes"));
    gtk_combo_box_append_text(GTK_COMBO_BOX(app->scope), _("My processes"));
    gtk_combo_box_append_text(GTK_COMBO_BOX(app->scope), _("With windows"));
    gtk_combo_box_set_active(GTK_COMBO_BOX(app->scope), app->cfg.scope);
    atk_object_set_name(gtk_widget_get_accessible(app->scope), _("Which processes"));
    g_signal_connect(app->scope, "changed", G_CALLBACK(on_scope), app);
    gtk_box_pack_start(GTK_BOX(box), app->scope, FALSE, FALSE, 0);
    return box;
}

/* ---- window ------------------------------------------------------------------------ */

static gboolean on_configure(GtkWidget *w, GdkEventConfigure *ev, TskApp *app)
{
    (void)w;
    if (app->cfg.win_w != ev->width || app->cfg.win_h != ev->height) {
        app->cfg.win_w = ev->width;
        app->cfg.win_h = ev->height;
        tsk_app_save_soon(app);
    }
    return FALSE;
}

static gboolean on_window_state(GtkWidget *w, GdkEventWindowState *ev, TskApp *app)
{
    (void)w;
    if ((ev->changed_mask & GDK_WINDOW_STATE_ICONIFIED) && !(ev->new_window_state & GDK_WINDOW_STATE_ICONIFIED) &&
        !app->list_paused)
        refresh_list(app);   /* coming back: don't show numbers from before */
    return FALSE;
}

static void usage(void)
{
    printf(_("kitask %s - process list and system monitor\n\n"
             "Usage: kitask [OPTION]...\n"
             "  -f, --filter TEXT    start with TEXT in the filter\n"
             "  -h, --help           show this help\n"
             "  -v, --version        show the version\n"),
           KITASK_VERSION);
}

int main(int argc, char **argv)
{
    xis_i18n_init("kitask");
    const char *filter = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage();
            return 0;
        }
        if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--version")) {
            printf("kitask %s\n", KITASK_VERSION);
            return 0;
        }
        if ((!strcmp(argv[i], "-f") || !strcmp(argv[i], "--filter")) && i + 1 < argc)
            filter = argv[++i];
    }
    gtk_init(&argc, &argv);
    setlocale(LC_NUMERIC, "C");   /* gtk_init() reset it; the config has '.' decimals */

    TskApp *app = &the_app;
    tsk_gpu_init();
    tsk_config_load(&app->cfg);
    tsk_procs_init(&app->procs);
    tsk_sys_init(&app->sys);

    app->win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app->win), _("Processes - kitask"));
    gtk_window_set_icon_name(GTK_WINDOW(app->win), "kitask");
    gtk_window_set_default_size(GTK_WINDOW(app->win), app->cfg.win_w, app->cfg.win_h);
    gtk_window_set_role(GTK_WINDOW(app->win), "kitask-main");
    GtkAccelGroup *ag = gtk_accel_group_new();
    gtk_window_add_accel_group(GTK_WINDOW(app->win), ag);

    GtkWidget *vbox = gtk_vbox_new(FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), build_menubar(app, ag), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), tsk_graphs_new(app), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), build_filter_bar(app, ag), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), tsk_list_new(app), TRUE, TRUE, 0);
    app->status = gtk_label_new("");
    gtk_misc_set_alignment(GTK_MISC(app->status), 0, 0.5);
    gtk_misc_set_padding(GTK_MISC(app->status), 6, 3);
    gtk_box_pack_start(GTK_BOX(vbox), app->status, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(app->win), vbox);

    g_signal_connect(app->win, "destroy", G_CALLBACK(on_quit), app);
    g_signal_connect(app->win, "key-press-event", G_CALLBACK(on_window_key), app);
    g_signal_connect(app->win, "configure-event", G_CALLBACK(on_configure), app);
    g_signal_connect(app->win, "window-state-event", G_CALLBACK(on_window_state), app);

    if (filter)
        gtk_entry_set_text(GTK_ENTRY(app->filter), filter);
    refresh_list(app);
    tsk_gpu_sample();
    tsk_graphs_push(app);
    gtk_widget_show_all(app->win);
    tsk_app_apply_graphs_visible(app);
    gtk_widget_grab_focus(app->list);
    tsk_app_restart_timers(app);
    gtk_main();

    if (app->save_timer)
        g_source_remove(app->save_timer);
    tsk_config_save(&app->cfg);
    return 0;
}
