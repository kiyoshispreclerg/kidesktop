/* kifiles - file manager for KiDesktop (GTK2 + GIO).
 *
 * The window: menu bar, tabs (kf_tabs.c) holding the toolbar, places sidebar
 * and one or two panes (split view), status bar with totals, free space and
 * the zoom slider. The panes (kf_pane.c) report back through
 * kf_app_pane_changed(); file operations live in kf_ops.c / kf_clip.c, the
 * sidebar in kf_places.c, thumbnails in kf_thumbs.c. */
#include "kf.h"
#include "../shared/xis_i18n.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <glib-unix.h>

static KfApp *the_app;

static KfPane *active(KfApp *app) { return app->active; }
static gboolean in_trash(KfPane *p) { return p->dir && g_file_has_uri_scheme(p->dir, "trash"); }
static GtkAction *act(KfApp *app, const char *name) { return gtk_action_group_get_action(app->actions, name); }

/* ---- status bar ---------------------------------------------------------- */

static char *format_counts(guint folders, guint files, guint64 bytes)
{
    char *fo = g_strdup_printf(ngettext("%u folder", "%u folders", folders), folders);
    char *fi = g_strdup_printf(ngettext("%u file", "%u files", files), files);
    char *sz = g_format_size_full(bytes, G_FORMAT_SIZE_IEC_UNITS);
    char *out = files ? g_strdup_printf("%s, %s (%s)", fo, fi, sz) : g_strdup_printf("%s, %s", fo, fi);
    g_free(fo);
    g_free(fi);
    g_free(sz);
    return out;
}

static void update_status(KfApp *app)
{
    KfPane *p = active(app);
    if (!p)
        return;
    guint folders, files;
    guint64 bytes;
    guint sel = kf_pane_selection_count(p);
    char *txt;
    if (sel > 0) {
        kf_pane_stats(p, TRUE, &folders, &files, &bytes);
        char *counts = format_counts(folders, files, bytes);
        char *head = g_strdup_printf(ngettext("%u item selected", "%u items selected", sel), sel);
        txt = g_strdup_printf("%s -- %s", head, counts);
        g_free(head);
        g_free(counts);
    } else {
        kf_pane_stats(p, FALSE, &folders, &files, &bytes);
        txt = p->loading ? g_strdup(_("Loading...")) : format_counts(folders, files, bytes);
    }
    gtk_label_set_text(GTK_LABEL(app->status_left), txt);
    g_free(txt);
}

static GCancellable *free_cancel;

static void on_free_info(GObject *src, GAsyncResult *res, gpointer data)
{
    (void)data;
    GFileInfo *info = g_file_query_filesystem_info_finish(G_FILE(src), res, NULL);
    if (!info)
        return;
    if (the_app && !g_cancellable_is_cancelled(free_cancel)) {
        guint64 total = g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE);
        guint64 free_b = g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE);
        kf_app_status_free(the_app, free_b, total);
    }
    g_object_unref(info);
}

static void refresh_free(KfApp *app)
{
    KfPane *p = active(app);
    if (!p || !p->dir)
        return;
    if (free_cancel) {
        g_cancellable_cancel(free_cancel);
        g_object_unref(free_cancel);
    }
    free_cancel = g_cancellable_new();
    /* trash:/// and friends have no meaningful size */
    if (!g_file_is_native(p->dir)) {
        gtk_label_set_text(GTK_LABEL(app->status_free), "");
        gtk_widget_hide(app->free_bar);
        return;
    }
    g_file_query_filesystem_info_async(p->dir, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE "," G_FILE_ATTRIBUTE_FILESYSTEM_FREE,
                                       G_PRIORITY_LOW, free_cancel, on_free_info, NULL);
}

void kf_app_status_free(KfApp *app, guint64 free_b, guint64 total_b)
{
    if (total_b == 0) {
        refresh_free(app);      /* "something changed": ask again */
        return;
    }
    char *f = g_format_size_full(free_b, G_FORMAT_SIZE_IEC_UNITS);
    char *t = g_format_size_full(total_b, G_FORMAT_SIZE_IEC_UNITS);
    char *txt = g_strdup_printf(_("%s free of %s"), f, t);
    gtk_label_set_text(GTK_LABEL(app->status_free), txt);
    /* An ellipsizing label asks for almost nothing and the left one takes the
     * slack: size this one by its text so it is never cut to "6...". */
    PangoLayout *layout = gtk_widget_create_pango_layout(app->status_free, txt);
    int w;
    pango_layout_get_pixel_size(layout, &w, NULL);
    g_object_unref(layout);
    gtk_widget_set_size_request(app->status_free, w, -1);
    gtk_widget_set_tooltip_text(app->status_free, txt);
    gtk_widget_show(app->free_bar);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->free_bar),
                                  (double)(total_b - (free_b > total_b ? total_b : free_b)) / (double)total_b);
    g_free(f);
    g_free(t);
    g_free(txt);
}

/* ---- actions state ---------------------------------------------------------- */

static void sync_toggle(KfApp *app, const char *name, gboolean on)
{
    GtkToggleAction *a = GTK_TOGGLE_ACTION(act(app, name));
    if (gtk_toggle_action_get_active(a) != on)
        gtk_toggle_action_set_active(a, on);
}

static void update_actions(KfApp *app)
{
    KfPane *p = active(app);
    if (!p)
        return;
    guint sel = kf_pane_selection_count(p);
    gboolean trash = in_trash(p);
    gboolean native = p->dir && g_file_is_native(p->dir);
    gtk_action_set_sensitive(act(app, "Back"), kf_pane_can_back(p));
    gtk_action_set_sensitive(act(app, "Forward"), kf_pane_can_forward(p));
    gtk_action_set_sensitive(act(app, "Up"), kf_pane_can_up(p));
    gtk_action_set_sensitive(act(app, "Open"), sel > 0);
    gtk_action_set_sensitive(act(app, "Rename"), sel == 1 && !trash);
    gtk_action_set_sensitive(act(app, "Trash"), sel > 0 && !trash);
    gtk_action_set_sensitive(act(app, "Delete"), sel > 0);
    gtk_action_set_sensitive(act(app, "Cut"), sel > 0 && !trash);
    gtk_action_set_sensitive(act(app, "Copy"), sel > 0);
    gtk_action_set_sensitive(act(app, "CopyPath"), sel > 0);
    gtk_action_set_sensitive(act(app, "Paste"), !trash);
    gtk_action_set_sensitive(act(app, "NewFolder"), !trash);
    gtk_action_set_sensitive(act(app, "NewFile"), !trash);
    gtk_action_set_sensitive(act(app, "OpenTerminal"), native);
    gtk_action_set_sensitive(act(app, "Undo"), kf_ops_can_undo(app));
    gtk_action_set_sensitive(act(app, "Restore"), trash && sel > 0);
    gtk_action_set_sensitive(act(app, "EmptyTrash"), trash);
    gtk_action_set_sensitive(act(app, "Properties"), TRUE);
    gboolean tabs = kf_tabs_count(app) > 1;
    gtk_action_set_sensitive(act(app, "NextTab"), tabs);
    gtk_action_set_sensitive(act(app, "PrevTab"), tabs);
    gtk_action_set_sensitive(act(app, "DetachTab"), tabs);
}

char *kf_app_dir_name(GFile *dir)
{
    GFileInfo *info = g_file_query_info(dir, "standard::display-name", G_FILE_QUERY_INFO_NONE, NULL, NULL);
    char *name = NULL;
    if (info) {
        name = g_strdup(g_file_info_get_display_name(info));
        g_object_unref(info);
    }
    if (!name || !*name) {
        g_free(name);
        name = g_file_get_parse_name(dir);
    }
    return name;
}

static void update_title(KfApp *app)
{
    KfPane *p = active(app);
    if (!p || !p->dir)
        return;
    char *name = kf_app_dir_name(p->dir);
    char *title = g_strdup_printf("%s - kifiles", name);
    gtk_window_set_title(GTK_WINDOW(app->win), title);
    g_free(name);
    g_free(title);
}

static void set_active(KfApp *app, KfPane *p)
{
    if (app->active == p)
        return;
    KfTab *t = app->tab;
    app->active = t->active = p;
    gboolean split = t->panes[1] != NULL;
    for (int i = 0; i < 2; i++)
        if (t->panes[i])
            kf_pane_set_active(t->panes[i], t->panes[i] == p, split);
    kf_tabs_update_label(t);
    update_title(app);
    update_actions(app);
    update_status(app);
    refresh_free(app);
    if (p->dir)
        kf_places_set_current(app->places, p->dir);
}

void kf_app_pane_changed(KfApp *app, KfPane *p, KfChange what)
{
    if (!app->win)
        return;
    if (what == KF_CHANGE_DIR && p == p->tab->active)
        kf_tabs_update_label(p->tab);
    if (what == KF_CHANGE_FOCUS) {
        if (p->tab == app->tab)
            set_active(app, p);
        return;
    }
    if (p != active(app))
        return;
    switch (what) {
    case KF_CHANGE_DIR:
        update_title(app);
        if (p->dir)
            kf_places_set_current(app->places, p->dir);
        refresh_free(app);
        /* fall through */
    case KF_CHANGE_LOADED:
    case KF_CHANGE_CONTENT:
    case KF_CHANGE_SELECTION:
        update_actions(app);
        update_status(app);
        break;
    default:
        break;
    }
}

GFile *kf_app_dir(KfApp *app) { return active(app)->dir; }

void kf_app_navigate(KfApp *app, GFile *dir)
{
    kf_pane_navigate(active(app), dir, TRUE, NULL);
    kf_pane_focus_view(active(app));
}

void kf_app_message(KfApp *app, GtkMessageType type, const char *primary, const char *secondary)
{
    GtkWidget *d = gtk_message_dialog_new(kf_app_parent(app), GTK_DIALOG_MODAL, type, GTK_BUTTONS_CLOSE, "%s", primary);
    if (secondary && *secondary)
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "%s", secondary);
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
}

/* ---- opening ---------------------------------------------------------------- */

static void launch_uri(KfApp *app, GFile *f)
{
    char *uri = g_file_get_uri(f);
    GError *err = NULL;
    GdkAppLaunchContext *ctx = gdk_app_launch_context_new();
    gdk_app_launch_context_set_screen(ctx, gtk_widget_get_screen(app->win));
    gdk_app_launch_context_set_timestamp(ctx, gtk_get_current_event_time());
    if (!g_app_info_launch_default_for_uri(uri, G_APP_LAUNCH_CONTEXT(ctx), &err)) {
        kf_app_message(app, GTK_MESSAGE_ERROR, _("Could not open the file"), err ? err->message : NULL);
        g_clear_error(&err);
    }
    g_object_unref(ctx);
    g_free(uri);
}

void kf_app_open(KfApp *app, KfPane *p, GFile *f)
{
    gboolean is_dir = FALSE;
    if (!kf_pane_file_info(p, f, &is_dir, NULL))
        is_dir = g_file_query_file_type(f, G_FILE_QUERY_INFO_NONE, NULL) == G_FILE_TYPE_DIRECTORY;
    if (is_dir)
        kf_pane_navigate(p, f, TRUE, NULL);
    else
        launch_uri(app, f);
}

static void spawn_detached(const char *cwd, char **argv)
{
    g_spawn_async(cwd, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                  NULL, NULL, NULL, NULL);
}

static void open_terminal_in(GFile *dir)
{
    char *path = dir && g_file_is_native(dir) ? g_file_get_path(dir) : NULL;
    if (!path)
        return;
    const char *prog = g_find_program_in_path("kiterm") ? "kiterm" : "x-terminal-emulator";
    char *argv[] = { (char *)prog, NULL };
    spawn_detached(path, argv);
    g_free(path);
}

void kf_app_open_window(GFile *dir)
{
    char self[4096];
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n <= 0)
        return;
    self[n] = 0;
    char *uri = g_file_get_uri(dir);
    char *argv[] = { self, uri, NULL };
    spawn_detached(NULL, argv);
    g_free(uri);
}

/* ---- action callbacks ---------------------------------------------------------- */

static void a_back(GtkAction *a, gpointer d) { (void)a; kf_pane_go_back(active(d)); }
static void a_forward(GtkAction *a, gpointer d) { (void)a; kf_pane_go_forward(active(d)); }
static void a_up(GtkAction *a, gpointer d) { (void)a; kf_pane_go_up(active(d)); }
static void a_reload(GtkAction *a, gpointer d) { (void)a; kf_pane_reload(active(d)); kf_app_status_free(d, 0, 0); }

static void a_home(GtkAction *a, gpointer d)
{
    (void)a;
    GFile *h = g_file_new_for_path(g_get_home_dir());
    kf_app_navigate(d, h);
    g_object_unref(h);
}

static void a_trash_go(GtkAction *a, gpointer d)
{
    (void)a;
    GFile *t = g_file_new_for_uri("trash:///");
    kf_app_navigate(d, t);
    g_object_unref(t);
}

static void a_location(GtkAction *a, gpointer d) { (void)a; kf_crumbs_edit(active(d)->crumbs); }

static void a_open(GtkAction *a, gpointer d)
{
    (void)a;
    KfApp *app = d;
    GList *sel = kf_pane_selected(active(app));
    for (GList *l = sel; l; l = l->next)
        kf_app_open(app, active(app), l->data);
    kf_files_free(sel);
}

static void a_open_terminal(GtkAction *a, gpointer d)
{
    (void)a;
    KfApp *app = d;
    GList *sel = kf_pane_selected(active(app));
    gboolean is_dir = FALSE;
    if (sel && !sel->next && kf_pane_file_info(active(app), sel->data, &is_dir, NULL) && is_dir)
        open_terminal_in(sel->data);
    else
        open_terminal_in(active(app)->dir);
    kf_files_free(sel);
}

static void a_new_window(GtkAction *a, gpointer d)
{
    (void)a;
    kf_app_open_window(active(d)->dir);
}

static void a_new_tab(GtkAction *a, gpointer d)
{
    (void)a;
    kf_tabs_open(d, active(d)->dir, TRUE);
}

static void a_close_tab(GtkAction *a, gpointer d) { (void)a; kf_tabs_close(((KfApp *)d)->tab); }
static void a_detach_tab(GtkAction *a, gpointer d) { (void)a; kf_tabs_detach(((KfApp *)d)->tab); }
static void a_next_tab(GtkAction *a, gpointer d) { (void)a; kf_tabs_step(d, +1); }
static void a_prev_tab(GtkAction *a, gpointer d) { (void)a; kf_tabs_step(d, -1); }

static void a_new_folder(GtkAction *a, gpointer d) { (void)a; kf_ops_new_folder(d, active(d)->dir); }
static void a_new_file(GtkAction *a, gpointer d) { (void)a; kf_ops_new_file(d, active(d)->dir); }

static void a_rename(GtkAction *a, gpointer d)
{
    (void)a;
    GList *sel = kf_pane_selected(active(d));
    if (sel && !sel->next)
        kf_ops_rename(d, sel->data);
    kf_files_free(sel);
}

static void a_trash(GtkAction *a, gpointer d)
{
    (void)a;
    GList *sel = kf_pane_selected(active(d));
    kf_ops_trash(d, sel);
    kf_files_free(sel);
}

static void a_delete(GtkAction *a, gpointer d)
{
    (void)a;
    GList *sel = kf_pane_selected(active(d));
    kf_ops_delete(d, sel);
    kf_files_free(sel);
}

static void a_restore(GtkAction *a, gpointer d)
{
    (void)a;
    GList *sel = kf_pane_selected(active(d));
    kf_ops_restore(d, sel);
    kf_files_free(sel);
}

static void a_empty_trash(GtkAction *a, gpointer d) { (void)a; kf_ops_empty_trash(d); }
static void a_undo(GtkAction *a, gpointer d) { (void)a; kf_ops_undo(d); }

static void a_cut(GtkAction *a, gpointer d)
{
    (void)a;
    GList *sel = kf_pane_selected(active(d));
    kf_clip_set(d, sel, TRUE);
    kf_files_free(sel);
}

static void a_copy(GtkAction *a, gpointer d)
{
    (void)a;
    GList *sel = kf_pane_selected(active(d));
    kf_clip_set(d, sel, FALSE);
    kf_files_free(sel);
}

static void a_paste(GtkAction *a, gpointer d) { (void)a; kf_clip_paste(d, active(d)->dir); }

static void a_paste_into(GtkAction *a, gpointer d)
{
    (void)a;
    GList *sel = kf_pane_selected(active(d));
    if (sel && !sel->next)
        kf_clip_paste(d, sel->data);
    kf_files_free(sel);
}

static void a_copy_path(GtkAction *a, gpointer d)
{
    (void)a;
    GList *sel = kf_pane_selected(active(d));
    GString *s = g_string_new(NULL);
    for (GList *l = sel; l; l = l->next) {
        char *p = g_file_is_native(l->data) ? g_file_get_path(l->data) : g_file_get_uri(l->data);
        if (s->len)
            g_string_append_c(s, '\n');
        g_string_append(s, p);
        g_free(p);
    }
    KfApp *app = d;
    gtk_clipboard_set_text(gtk_widget_get_clipboard(app->win, GDK_SELECTION_CLIPBOARD), s->str, -1);
    g_string_free(s, TRUE);
    kf_files_free(sel);
}

static void a_select_all(GtkAction *a, gpointer d) { (void)a; kf_pane_select_all(active(d)); }
static void a_invert(GtkAction *a, gpointer d) { (void)a; kf_pane_select_invert(active(d)); }
static void a_filter(GtkAction *a, gpointer d) { (void)a; kf_pane_show_filter(active(d), TRUE); }

static void a_add_bookmark(GtkAction *a, gpointer d)
{
    (void)a;
    KfApp *app = d;
    GList *sel = kf_pane_selected(active(app));
    gboolean is_dir = FALSE;
    if (sel && !sel->next && kf_pane_file_info(active(app), sel->data, &is_dir, NULL) && is_dir)
        kf_places_add_bookmark(app->places, sel->data);
    else
        kf_places_add_bookmark(app->places, active(app)->dir);
    kf_files_free(sel);
}

GList *kf_app_panes(KfApp *app)
{
    GList *out = NULL, *tabs = kf_tabs_all(app);
    for (GList *l = tabs; l; l = l->next)
        for (int i = 0; i < 2; i++)
            if (((KfTab *)l->data)->panes[i])
                out = g_list_append(out, ((KfTab *)l->data)->panes[i]);
    g_list_free(tabs);
    return out;
}

static void each_pane(KfApp *app, void (*fn)(KfPane *))
{
    GList *panes = kf_app_panes(app);
    for (GList *l = panes; l; l = l->next)
        fn(l->data);
    g_list_free(panes);
}

static void sync_sort_actions(KfApp *app)
{
    app->updating_ui = TRUE;
    gtk_radio_action_set_current_value(GTK_RADIO_ACTION(act(app, "SortName")), app->cfg.sort);
    sync_toggle(app, "SortDesc", app->cfg.sort_desc);
    app->updating_ui = FALSE;
}

static void a_sort_changed(GtkRadioAction *a, GtkRadioAction *cur, gpointer d)
{
    (void)a;
    KfApp *app = d;
    if (app->updating_ui)
        return;
    app->cfg.sort = gtk_radio_action_get_current_value(cur);
    each_pane(app, kf_pane_apply_sort);
}

void kf_app_sort_by(KfApp *app, KfSortKey key)
{
    if (app->cfg.sort == (int)key)
        app->cfg.sort_desc = !app->cfg.sort_desc;
    else {
        app->cfg.sort = key;
        app->cfg.sort_desc = FALSE;
    }
    sync_sort_actions(app);
    each_pane(app, kf_pane_apply_sort);
}

static void a_sort_desc(GtkToggleAction *a, gpointer d)
{
    KfApp *app = d;
    if (app->updating_ui)
        return;
    app->cfg.sort_desc = gtk_toggle_action_get_active(a);
    each_pane(app, kf_pane_apply_sort);
}

static void a_folders_first(GtkToggleAction *a, gpointer d)
{
    KfApp *app = d;
    app->cfg.folders_first = gtk_toggle_action_get_active(a);
    each_pane(app, kf_pane_apply_sort);
}

static void a_hidden(GtkToggleAction *a, gpointer d)
{
    KfApp *app = d;
    app->cfg.show_hidden = gtk_toggle_action_get_active(a);
    each_pane(app, kf_pane_apply_filter);
}

static void a_hover(GtkToggleAction *a, gpointer d)
{
    KfApp *app = d;
    app->cfg.hover = gtk_toggle_action_get_active(a);
    each_pane(app, kf_hover_apply);
}

static void a_thumbs(GtkToggleAction *a, gpointer d)
{
    KfApp *app = d;
    app->cfg.thumbnails = gtk_toggle_action_get_active(a);
    each_pane(app, kf_pane_reload);
}

static void pane_view(KfPane *p) { kf_pane_set_view(p, p->app->cfg.view); }

void kf_app_setup_pane(KfApp *app, KfPane *p)
{
    kf_pane_set_view(p, app->cfg.view);
    kf_pane_apply_sort(p);
}

static void apply_view(KfApp *app)
{
    each_pane(app, pane_view);
    if (active(app))
        kf_pane_focus_view(active(app));
}

static void a_view_changed(GtkRadioAction *a, GtkRadioAction *cur, gpointer d)
{
    (void)a;
    KfApp *app = d;
    if (app->updating_ui)
        return;
    app->cfg.view = gtk_radio_action_get_current_value(cur);
    apply_view(app);
}

static void a_view_icons(GtkAction *a, gpointer d)
{
    (void)a;
    gtk_radio_action_set_current_value(GTK_RADIO_ACTION(act(d, "ViewIcons")), KF_VIEW_ICONS);
}

static void a_view_list(GtkAction *a, gpointer d)
{
    (void)a;
    gtk_radio_action_set_current_value(GTK_RADIO_ACTION(act(d, "ViewIcons")), KF_VIEW_LIST);
}

static void a_places_toggle(GtkToggleAction *a, gpointer d)
{
    KfApp *app = d;
    app->cfg.show_places = gtk_toggle_action_get_active(a);
    gtk_widget_set_visible(app->places, app->cfg.show_places);
}

static void set_split(KfApp *app, gboolean on)
{
    KfTab *t = app->tab;
    if (on && !t->panes[1]) {
        KfPane *p = kf_pane_new(app);
        p->tab = t;
        t->panes[1] = p;
        gtk_paned_pack2(GTK_PANED(t->panes_paned), p->box, TRUE, FALSE);
        kf_app_setup_pane(app, p);
        GFile *dir = t->panes[0]->dir ? g_object_ref(t->panes[0]->dir) : g_file_new_for_path(g_get_home_dir());
        kf_pane_navigate(p, dir, FALSE, NULL);
        g_object_unref(dir);
        GtkAllocation al;
        gtk_widget_get_allocation(t->panes_paned, &al);
        gtk_paned_set_position(GTK_PANED(t->panes_paned), al.width > 1 ? al.width / 2 : 400);
        for (int i = 0; i < 2; i++)
            kf_pane_set_active(t->panes[i], t->panes[i] == app->active, TRUE);
    } else if (!on && t->panes[1]) {
        KfPane *gone = t->panes[1];
        t->panes[1] = NULL;
        if (app->active == gone) {
            app->active = NULL;
            set_active(app, t->panes[0]);
        } else {
            kf_pane_set_active(t->panes[0], TRUE, FALSE);
        }
        kf_pane_destroy(gone);      /* also takes its box out of the paned */
        kf_pane_focus_view(t->panes[0]);
        update_actions(app);
        update_status(app);
    }
    app->cfg.split = t->panes[1] != NULL;
}

void kf_app_tab_switched(KfApp *app)
{
    KfTab *t = app->tab;
    app->active = NULL;
    set_active(app, t->active);
    app->updating_ui = TRUE;
    sync_toggle(app, "Split", t->panes[1] != NULL);
    app->updating_ui = FALSE;
    app->cfg.split = t->panes[1] != NULL;
    kf_pane_focus_view(t->active);
}

static void a_split(GtkToggleAction *a, gpointer d)
{
    if (((KfApp *)d)->updating_ui)
        return;
    set_split(d, gtk_toggle_action_get_active(a));
}

static void zoom_tooltip(KfApp *app)
{
    char *tip = g_strdup_printf(_("Icon size: %d px (Ctrl+Plus / Ctrl+Minus)"), app->cfg.zoom);
    gtk_widget_set_tooltip_text(app->zoom_scale, tip);
    g_free(tip);
}

static const int kZoomSteps[] = { 16, 24, 32, 48, 64, 80, 96, 128, 160, 192, 256 };

static void set_zoom(KfApp *app, int z)
{
    z = CLAMP(z, KF_ZOOM_MIN, KF_ZOOM_MAX);
    if (z == app->cfg.zoom)
        return;
    app->cfg.zoom = z;
    zoom_tooltip(app);
    gtk_range_set_value(GTK_RANGE(app->zoom_scale), z);
    each_pane(app, kf_pane_apply_zoom);
}

void kf_app_zoom_step(KfApp *app, int dir)
{
    if (dir > 0) {
        for (guint i = 0; i < G_N_ELEMENTS(kZoomSteps); i++)
            if (kZoomSteps[i] > app->cfg.zoom) {
                set_zoom(app, kZoomSteps[i]);
                return;
            }
    } else {
        for (int i = G_N_ELEMENTS(kZoomSteps) - 1; i >= 0; i--)
            if (kZoomSteps[i] < app->cfg.zoom) {
                set_zoom(app, kZoomSteps[i]);
                return;
            }
    }
}

static void a_zoom_in(GtkAction *a, gpointer d) { (void)a; kf_app_zoom_step(d, +1); }
static void a_zoom_out(GtkAction *a, gpointer d) { (void)a; kf_app_zoom_step(d, -1); }

static void a_zoom_reset(GtkAction *a, gpointer d) { (void)a; set_zoom(d, 64); }

static guint zoom_timer;
static void on_zoom_scale(GtkRange *r, gpointer d);

static gboolean zoom_apply(gpointer d)
{
    KfApp *app = d;
    zoom_timer = 0;
    each_pane(app, kf_pane_apply_zoom);
    return FALSE;
}

static void on_zoom_scale(GtkRange *r, gpointer d)
{
    KfApp *app = d;
    int v = (int)gtk_range_get_value(r);
    if (v == app->cfg.zoom)
        return;
    app->cfg.zoom = v;
    zoom_tooltip(app);
    if (zoom_timer)
        g_source_remove(zoom_timer);
    zoom_timer = g_timeout_add(90, zoom_apply, app);   /* coalesce while dragging */
}

static void a_about(GtkAction *a, gpointer d)
{
    (void)a;
    KfApp *app = d;
    gtk_show_about_dialog(GTK_WINDOW(app->win), "program-name", "kifiles", "version", KIFILES_VERSION,
                          "comments", _("File manager for KiDesktop"), "logo-icon-name", "system-file-manager",
                          "website", "https://github.com/kiyoshispreclerg/kidesktop", NULL);
}

/* ---- session: open tabs and split panes ---------------------------------------
 * Only a window started without a folder argument restores the saved tabs
 * and records its own; one opened on a specific folder (from another
 * program, "New Window", a detached tab) leaves the saved session alone. */

#define KF_MAX_TABS 64

static gboolean owns_session;
static gboolean state_saved;

static void session_collect(KfApp *app)
{
    GPtrArray *out = g_ptr_array_new();
    GList *all = kf_tabs_all(app);
    for (GList *l = all; l; l = l->next) {
        KfTab *t = l->data;
        GString *entry = g_string_new(NULL);
        for (int i = 0; i < 2; i++) {
            if (!t->panes[i] || !t->panes[i]->dir)
                continue;
            char *uri = g_file_get_uri(t->panes[i]->dir);
            g_string_append_printf(entry, "%s%s", entry->len ? " " : "", uri);
            g_free(uri);
        }
        if (entry->len)
            g_ptr_array_add(out, g_string_free(entry, FALSE));
        else
            g_string_free(entry, TRUE);
    }
    g_list_free(all);
    g_ptr_array_add(out, NULL);
    g_strfreev(app->cfg.tabs);
    app->cfg.tabs = (char **)g_ptr_array_free(out, FALSE);
    app->cfg.cur_tab = gtk_notebook_get_current_page(GTK_NOTEBOOK(app->notebook));
}

/* Idempotent and free of X round trips, so it is also safe from a signal
 * handler and from atexit() after the X connection is gone. */
static void save_state(KfApp *app)
{
    if (state_saved)
        return;
    state_saved = TRUE;
    int w, h;
    gtk_window_get_size(GTK_WINDOW(app->win), &w, &h);
    app->cfg.win_w = w;
    app->cfg.win_h = h;
    if (app->cfg.show_places)
        app->cfg.places_w = gtk_paned_get_position(GTK_PANED(app->main_paned));
    if (owns_session)
        session_collect(app);
    kf_config_save(&app->cfg);
}

/* A saved directory is usable when it is remote (not worth blocking to
 * check) or a local folder that still exists. */
static GFile *session_dir(const char *uri)
{
    GFile *f = g_file_new_for_uri(uri);
    if (g_file_is_native(f) && g_file_query_file_type(f, G_FILE_QUERY_INFO_NONE, NULL) != G_FILE_TYPE_DIRECTORY) {
        g_object_unref(f);
        return NULL;
    }
    return f;
}

/* Directories of saved tab `n`: [0] left/only pane, [1] right pane or NULL. */
static void session_entry(KfApp *app, int n, GFile *dirs[2])
{
    dirs[0] = dirs[1] = NULL;
    char **uris = g_strsplit(app->cfg.tabs[n], " ", 2);
    for (int i = 0; i < 2 && uris[i]; i++)
        dirs[i] = session_dir(uris[i]);
    g_strfreev(uris);
}

static gboolean have_session(KfApp *app)
{
    return app->cfg.tabs && app->cfg.tabs[0];
}

/* The first tab is already open (on entry 0's left folder); this opens the
 * rest, re-splits the tabs that were split, and goes back to the one that
 * was showing. */
static void session_restore(KfApp *app)
{
    int shown = app->cfg.cur_tab;
    for (int i = 0; app->cfg.tabs[i] && i < KF_MAX_TABS; i++) {
        GFile *dirs[2];
        session_entry(app, i, dirs);
        if (i > 0) {
            if (!dirs[0]) {
                if (dirs[1])
                    g_object_unref(dirs[1]);
                if (i < shown)
                    shown--;
                continue;
            }
            kf_tabs_open(app, dirs[0], TRUE);
        }
        if (dirs[1]) {
            app->updating_ui = TRUE;
            sync_toggle(app, "Split", TRUE);
            app->updating_ui = FALSE;
            set_split(app, TRUE);
            kf_pane_navigate(app->tab->panes[1], dirs[1], FALSE, NULL);
        }
        for (int k = 0; k < 2; k++)
            if (dirs[k])
                g_object_unref(dirs[k]);
    }
    int count = kf_tabs_count(app);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(app->notebook), CLAMP(shown, 0, count - 1));
}

void kf_app_quit(KfApp *app)
{
    save_state(app);
    /* Running copies/moves finish on their own, with their progress
     * dialogs standing alone; the last one to end quits (kf_ops.c). */
    if (app->jobs) {
        app->quitting = TRUE;
        gtk_widget_hide(app->win);
        kf_ops_detach(app);
        return;
    }
    gtk_main_quit();
}

/* Logging out or killing the program still records the session. */
static gboolean on_term_signal(gpointer d)
{
    save_state(d);
    exit(0);
    return G_SOURCE_REMOVE;
}

static void save_at_exit(void)
{
    if (the_app)
        save_state(the_app);
}

GtkWindow *kf_app_parent(KfApp *app)
{
    return app->quitting ? NULL : GTK_WINDOW(app->win);
}

static void a_quit(GtkAction *a, gpointer d) { (void)a; kf_app_quit(d); }

static gboolean on_delete(GtkWidget *w, GdkEvent *e, gpointer d)
{
    (void)w; (void)e;
    kf_app_quit(d);
    return TRUE;
}

static void a_properties(GtkAction *a, gpointer d);

/* The places panel may take up to half of the window: GtkPaned has no maximum
 * of its own, so a position past that is pulled back. */
static void clamp_places(KfApp *app)
{
    static gboolean busy;
    GtkAllocation a;
    gtk_widget_get_allocation(app->main_paned, &a);
    if (busy || a.width <= 1)
        return;
    int max = a.width / 2;
    if (gtk_paned_get_position(GTK_PANED(app->main_paned)) > max) {
        busy = TRUE;
        gtk_paned_set_position(GTK_PANED(app->main_paned), max);
        busy = FALSE;
    }
}

static void on_places_position(GObject *o, GParamSpec *ps, gpointer d)
{
    (void)o; (void)ps;
    clamp_places(d);
}

static void on_main_allocate(GtkWidget *w, GtkAllocation *a, gpointer d)
{
    (void)w; (void)a;
    clamp_places(d);       /* the window got narrower under the panel */
}

/* ---- context menu -------------------------------------------------------------- */

static void on_open_with(GtkMenuItem *mi, gpointer d)
{
    KfApp *app = d;
    GAppInfo *ai = g_object_get_data(G_OBJECT(mi), "kf-appinfo");
    GList *sel = kf_pane_selected(active(app));
    GError *err = NULL;
    GdkAppLaunchContext *ctx = gdk_app_launch_context_new();
    gdk_app_launch_context_set_screen(ctx, gtk_widget_get_screen(app->win));
    if (!g_app_info_launch(ai, sel, G_APP_LAUNCH_CONTEXT(ctx), &err)) {
        kf_app_message(app, GTK_MESSAGE_ERROR, _("Could not open the file"), err->message);
        g_clear_error(&err);
    }
    g_object_unref(ctx);
    kf_files_free(sel);
}

static void a_open_new_window_sel(GtkAction *a, gpointer d)
{
    (void)a;
    GList *sel = kf_pane_selected(active(d));
    for (GList *l = sel; l; l = l->next)
        kf_app_open_window(l->data);
    kf_files_free(sel);
}

static void a_open_new_tab_sel(GtkAction *a, gpointer d)
{
    (void)a;
    KfApp *app = d;
    GList *sel = kf_pane_selected(active(app));
    for (GList *l = sel; l; l = l->next) {
        gboolean is_dir = FALSE;
        if (kf_pane_file_info(active(app), l->data, &is_dir, NULL) && is_dir)
            kf_tabs_open(app, l->data, FALSE);
    }
    kf_files_free(sel);
}

static void add_action_item(KfApp *app, GtkWidget *menu, const char *name)
{
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_action_create_menu_item(act(app, name)));
}

static void add_sep(GtkWidget *menu)
{
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
}

void kf_app_popup_context(KfApp *app, KfPane *p, GdkEventButton *ev, gboolean on_item)
{
    if (p != active(app))
        set_active(app, p);
    update_actions(app);
    GtkWidget *menu = gtk_menu_new();
    gboolean trash = in_trash(p);
    if (on_item) {
        GList *sel = kf_pane_selected(p);
        gboolean one_dir = FALSE;
        char *ctype = NULL;
        if (sel && !sel->next) {
            kf_pane_file_info(p, sel->data, &one_dir, &ctype);
        } else if (sel) {
            kf_pane_file_info(p, sel->data, NULL, &ctype);
        }
        add_action_item(app, menu, "Open");
        if (sel && !sel->next && one_dir) {
            add_action_item(app, menu, "OpenNewTabSel");
            add_action_item(app, menu, "OpenNewWindowSel");
        }
        if (ctype && !one_dir) {
            GList *apps = g_app_info_get_all_for_type(ctype);
            if (apps) {
                GtkWidget *sub = gtk_menu_new();
                for (GList *l = apps; l; l = l->next) {
                    if (!g_app_info_should_show(l->data))
                        continue;
                    GtkWidget *mi = gtk_menu_item_new_with_label(g_app_info_get_display_name(l->data));
                    g_object_set_data_full(G_OBJECT(mi), "kf-appinfo", g_object_ref(l->data), g_object_unref);
                    g_signal_connect(mi, "activate", G_CALLBACK(on_open_with), app);
                    gtk_menu_shell_append(GTK_MENU_SHELL(sub), mi);
                }
                GtkWidget *top = gtk_menu_item_new_with_mnemonic(_("Open _With"));
                gtk_menu_item_set_submenu(GTK_MENU_ITEM(top), sub);
                gtk_menu_shell_append(GTK_MENU_SHELL(menu), top);
                g_list_free_full(apps, g_object_unref);
            }
        }
        if (one_dir)
            add_action_item(app, menu, "OpenTerminal");
        add_sep(menu);
        if (trash) {
            add_action_item(app, menu, "Restore");
            add_action_item(app, menu, "Delete");
        } else {
            add_action_item(app, menu, "Cut");
            add_action_item(app, menu, "Copy");
            if (one_dir)
                gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_action_create_menu_item(act(app, "PasteInto")));
            add_sep(menu);
            add_action_item(app, menu, "Rename");
            add_action_item(app, menu, "Trash");
            add_action_item(app, menu, "Delete");
        }
        add_sep(menu);
        add_action_item(app, menu, "CopyPath");
        add_action_item(app, menu, "Properties");
        g_free(ctype);
        kf_files_free(sel);
    } else if (trash) {
        add_action_item(app, menu, "EmptyTrash");
        add_sep(menu);
        add_action_item(app, menu, "Reload");
    } else {
        add_action_item(app, menu, "NewFolder");
        add_action_item(app, menu, "NewFile");
        add_sep(menu);
        add_action_item(app, menu, "Paste");
        add_sep(menu);
        add_action_item(app, menu, "OpenTerminal");
        add_action_item(app, menu, "ShowHidden");
        add_action_item(app, menu, "Reload");
        add_sep(menu);
        add_action_item(app, menu, "Properties");
    }
    gtk_widget_show_all(menu);
    g_signal_connect(menu, "selection-done", G_CALLBACK(gtk_widget_destroy), NULL);
    gtk_menu_popup(GTK_MENU(menu), NULL, NULL, NULL, NULL, ev ? ev->button : 0,
                   ev ? ev->time : gtk_get_current_event_time());
}

/* ---- properties (compact) -------------------------------------------------------- */

static void a_properties(GtkAction *a, gpointer d)
{
    (void)a;
    KfApp *app = d;
    KfPane *p = active(app);
    GList *sel = kf_pane_selected(p);
    GFile *f = sel ? sel->data : p->dir;
    GString *s = g_string_new(NULL);
    if (sel && sel->next) {
        guint fo, fi;
        guint64 b;
        kf_pane_stats(p, TRUE, &fo, &fi, &b);
        char *counts = format_counts(fo, fi, b);
        g_string_append(s, counts);
        g_free(counts);
    } else {
        GFileInfo *info = g_file_query_info(f, "standard::*,time::*,access::*,owner::*,unix::mode", G_FILE_QUERY_INFO_NONE, NULL, NULL);
        if (info) {
            char *path = g_file_is_native(f) ? g_file_get_path(f) : g_file_get_uri(f);
            const char *ct = g_file_info_get_content_type(info);
            char *desc = ct ? g_content_type_get_description(ct) : NULL;
            char *size = g_format_size_full(g_file_info_get_size(info), G_FORMAT_SIZE_IEC_UNITS);
            g_string_append_printf(s, _("Name: %s\n"), g_file_info_get_display_name(info));
            g_string_append_printf(s, _("Location: %s\n"), path);
            g_string_append_printf(s, _("Type: %s\n"), desc ? desc : (ct ? ct : "?"));
            if (g_file_info_get_file_type(info) != G_FILE_TYPE_DIRECTORY)
                g_string_append_printf(s, _("Size: %s\n"), size);
            guint64 mt = g_file_info_get_attribute_uint64(info, "time::modified");
            if (mt) {
                GDateTime *dt = g_date_time_new_from_unix_local(mt);
                char *when = g_date_time_format(dt, "%Y-%m-%d %H:%M:%S");
                g_string_append_printf(s, _("Modified: %s\n"), when);
                g_free(when);
                g_date_time_unref(dt);
            }
            const char *owner = g_file_info_get_attribute_string(info, "owner::user");
            if (owner)
                g_string_append_printf(s, _("Owner: %s\n"), owner);
            if (g_file_info_has_attribute(info, "unix::mode"))
                g_string_append_printf(s, _("Permissions: %04o\n"), g_file_info_get_attribute_uint32(info, "unix::mode") & 07777);
            g_free(path);
            g_free(desc);
            g_free(size);
            g_object_unref(info);
        }
    }
    kf_app_message(app, GTK_MESSAGE_INFO, _("Properties"), s->str);
    g_string_free(s, TRUE);
    kf_files_free(sel);
}

/* ---- window / keyboard ---------------------------------------------------------- */

/* GTK2 runs accelerators before the focus widget sees a key, so Delete or
 * BackSpace would act on files while typing in an entry. Give editable
 * widgets first refusal. */
static gboolean on_key_press(GtkWidget *win, GdkEventKey *ev, gpointer d)
{
    /* Ctrl+Tab cycles the tabs; GTK won't take Tab as an accelerator. */
    if ((ev->state & GDK_CONTROL_MASK) &&
        (ev->keyval == GDK_KEY_Tab || ev->keyval == GDK_KEY_ISO_Left_Tab || ev->keyval == GDK_KEY_KP_Tab)) {
        kf_tabs_step(d, (ev->state & GDK_SHIFT_MASK) ? -1 : +1);
        return TRUE;
    }
    GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(win));
    if (focus && GTK_IS_EDITABLE(focus) && gtk_window_propagate_key_event(GTK_WINDOW(win), ev))
        return TRUE;
    /* The tree view's type-ahead search entry is not an editable we can see. */
    return FALSE;
}

static void on_sort_button(GtkToolButton *b, gpointer d)
{
    (void)b;
    KfApp *app = d;
    GtkWidget *m = gtk_ui_manager_get_widget(app->ui, "/SortPopup");
    gtk_menu_popup(GTK_MENU(m), NULL, NULL, NULL, NULL, 1, gtk_get_current_event_time());
}

static const GtkActionEntry kEntries[] = {
    { "FileMenu", NULL, N_("_File"), NULL, NULL, NULL }, { "EditMenu", NULL, N_("_Edit"), NULL, NULL, NULL },
    { "ViewMenu", NULL, N_("_View"), NULL, NULL, NULL }, { "GoMenu", NULL, N_("_Go"), NULL, NULL, NULL },
    { "HelpMenu", NULL, N_("_Help"), NULL, NULL, NULL }, { "SortMenu", NULL, N_("_Sort By"), NULL, NULL, NULL },

    { "NewWindow", GTK_STOCK_NEW, N_("New _Window"), "<control>N", NULL, G_CALLBACK(a_new_window) },
    { "NewTab", NULL, N_("New _Tab"), "<control>T", NULL, G_CALLBACK(a_new_tab) },
    { "CloseTab", GTK_STOCK_CLOSE, N_("_Close Tab"), "<control>W", NULL, G_CALLBACK(a_close_tab) },
    { "DetachTab", NULL, N_("_Detach Tab"), NULL, N_("Move the tab to a new window"), G_CALLBACK(a_detach_tab) },
    { "NextTab", NULL, N_("_Next Tab"), "<control>Page_Down", NULL, G_CALLBACK(a_next_tab) },
    { "PrevTab", NULL, N_("_Previous Tab"), "<control>Page_Up", NULL, G_CALLBACK(a_prev_tab) },
    { "NewFolder", "folder-new", N_("New _Folder..."), "<control><shift>N", N_("Create a new folder"), G_CALLBACK(a_new_folder) },
    { "NewFile", GTK_STOCK_FILE, N_("New _File..."), "<control><alt>N", NULL, G_CALLBACK(a_new_file) },
    { "Open", GTK_STOCK_OPEN, N_("_Open"), "<control>O", NULL, G_CALLBACK(a_open) },
    { "OpenNewWindowSel", NULL, N_("Open in New _Window"), NULL, NULL, G_CALLBACK(a_open_new_window_sel) },
    { "OpenNewTabSel", NULL, N_("Open in New _Tab"), NULL, NULL, G_CALLBACK(a_open_new_tab_sel) },
    { "OpenTerminal", "utilities-terminal", N_("Open _Terminal Here"), "F4", NULL, G_CALLBACK(a_open_terminal) },
    { "Rename", NULL, N_("_Rename..."), "F2", NULL, G_CALLBACK(a_rename) },
    { "Trash", "user-trash", N_("Move to _Trash"), "Delete", NULL, G_CALLBACK(a_trash) },
    { "Delete", GTK_STOCK_DELETE, N_("_Delete"), "<shift>Delete", N_("Delete permanently"), G_CALLBACK(a_delete) },
    { "Restore", NULL, N_("_Restore"), NULL, NULL, G_CALLBACK(a_restore) },
    { "EmptyTrash", NULL, N_("E_mpty Trash"), NULL, NULL, G_CALLBACK(a_empty_trash) },
    { "Properties", GTK_STOCK_PROPERTIES, N_("_Properties"), "<alt>Return", NULL, G_CALLBACK(a_properties) },
    { "Quit", GTK_STOCK_QUIT, NULL, "<control>Q", NULL, G_CALLBACK(a_quit) },

    { "Undo", GTK_STOCK_UNDO, NULL, "<control>Z", NULL, G_CALLBACK(a_undo) },
    { "Cut", GTK_STOCK_CUT, NULL, "<control>X", NULL, G_CALLBACK(a_cut) },
    { "Copy", GTK_STOCK_COPY, NULL, "<control>C", NULL, G_CALLBACK(a_copy) },
    { "Paste", GTK_STOCK_PASTE, NULL, "<control>V", NULL, G_CALLBACK(a_paste) },
    { "PasteInto", GTK_STOCK_PASTE, N_("Paste Into _Folder"), NULL, NULL, G_CALLBACK(a_paste_into) },
    { "CopyPath", NULL, N_("Copy _Path"), "<control><shift>C", NULL, G_CALLBACK(a_copy_path) },
    { "SelectAll", GTK_STOCK_SELECT_ALL, NULL, "<control>A", NULL, G_CALLBACK(a_select_all) },
    { "InvertSel", NULL, N_("_Invert Selection"), "<control><shift>A", NULL, G_CALLBACK(a_invert) },
    { "Filter", GTK_STOCK_FIND, N_("_Filter..."), "<control>F", N_("Filter the files by name"), G_CALLBACK(a_filter) },

    { "ViewIconsBtn", NULL, N_("Icons"), "<control>1", NULL, G_CALLBACK(a_view_icons) },
    { "ViewListBtn", NULL, N_("List"), "<control>2", NULL, G_CALLBACK(a_view_list) },
    { "ZoomIn", GTK_STOCK_ZOOM_IN, N_("Zoom _In"), "<control>equal", NULL, G_CALLBACK(a_zoom_in) },
    { "ZoomOut", GTK_STOCK_ZOOM_OUT, N_("Zoom _Out"), "<control>minus", NULL, G_CALLBACK(a_zoom_out) },
    { "ZoomReset", GTK_STOCK_ZOOM_100, N_("_Reset Zoom"), "<control>0", NULL, G_CALLBACK(a_zoom_reset) },
    { "Reload", GTK_STOCK_REFRESH, N_("_Reload"), "F5", NULL, G_CALLBACK(a_reload) },

    { "Back", GTK_STOCK_GO_BACK, N_("_Back"), "<alt>Left", NULL, G_CALLBACK(a_back) },
    { "Forward", GTK_STOCK_GO_FORWARD, N_("_Forward"), "<alt>Right", NULL, G_CALLBACK(a_forward) },
    { "Up", GTK_STOCK_GO_UP, N_("_Up"), "<alt>Up", NULL, G_CALLBACK(a_up) },
    { "UpBackspace", NULL, NULL, "BackSpace", NULL, G_CALLBACK(a_up) },
    { "Home", GTK_STOCK_HOME, N_("_Home"), "<alt>Home", NULL, G_CALLBACK(a_home) },
    { "GoTrash", "user-trash", N_("_Trash"), NULL, NULL, G_CALLBACK(a_trash_go) },
    { "Location", NULL, N_("_Location..."), "<control>L", NULL, G_CALLBACK(a_location) },
    { "AddBookmark", GTK_STOCK_ADD, N_("Add to _Places"), "<control>D", NULL, G_CALLBACK(a_add_bookmark) },
    { "About", GTK_STOCK_ABOUT, NULL, NULL, NULL, G_CALLBACK(a_about) },
};

static const GtkToggleActionEntry kToggles[] = {
    { "SortDesc", NULL, N_("_Descending"), NULL, NULL, G_CALLBACK(a_sort_desc), FALSE },
    { "FoldersFirst", NULL, N_("_Folders First"), NULL, NULL, G_CALLBACK(a_folders_first), TRUE },
    { "ShowHidden", NULL, N_("Show _Hidden Files"), "<control>H", NULL, G_CALLBACK(a_hidden), FALSE },
    { "Thumbnails", NULL, N_("_Thumbnails"), NULL, NULL, G_CALLBACK(a_thumbs), TRUE },
    { "Split", GTK_STOCK_DND_MULTIPLE, N_("S_plit View"), "F3", N_("Show two folders side by side"), G_CALLBACK(a_split), FALSE },
    { "PlacesPanel", NULL, N_("_Places Panel"), "F9", NULL, G_CALLBACK(a_places_toggle), TRUE },
    { "HoverHighlight", NULL, N_("Highlight Item Under _Pointer"), NULL, NULL, G_CALLBACK(a_hover), TRUE },
};

static const GtkRadioActionEntry kViewRadio[] = {
    { "ViewIcons", NULL, N_("_Icons"), NULL, NULL, KF_VIEW_ICONS },
    { "ViewList", NULL, N_("_List"), NULL, NULL, KF_VIEW_LIST },
};

static const GtkRadioActionEntry kSortRadio[] = {
    { "SortName", NULL, N_("By _Name"), NULL, NULL, KF_SORT_NAME },
    { "SortSize", NULL, N_("By _Size"), NULL, NULL, KF_SORT_SIZE },
    { "SortMtime", NULL, N_("By _Modified"), NULL, NULL, KF_SORT_MTIME },
    { "SortType", NULL, N_("By _Type"), NULL, NULL, KF_SORT_TYPE },
    { "SortExt", NULL, N_("By _Extension"), NULL, NULL, KF_SORT_EXT },
};

static const char *kUI =
"<ui>"
" <menubar name='MenuBar'>"
"  <menu action='FileMenu'>"
"   <menuitem action='NewWindow'/><menuitem action='NewTab'/><menuitem action='NewFolder'/><menuitem action='NewFile'/><separator/>"
"   <menuitem action='Open'/><menuitem action='OpenTerminal'/><separator/>"
"   <menuitem action='Rename'/><menuitem action='Trash'/><menuitem action='Delete'/><separator/>"
"   <menuitem action='Restore'/><menuitem action='EmptyTrash'/><separator/>"
"   <menuitem action='Properties'/><separator/>"
"   <menuitem action='DetachTab'/><menuitem action='CloseTab'/><menuitem action='Quit'/>"
"  </menu>"
"  <menu action='EditMenu'>"
"   <menuitem action='Undo'/><separator/>"
"   <menuitem action='Cut'/><menuitem action='Copy'/><menuitem action='Paste'/><menuitem action='CopyPath'/><separator/>"
"   <menuitem action='SelectAll'/><menuitem action='InvertSel'/><menuitem action='Filter'/>"
"  </menu>"
"  <menu action='ViewMenu'>"
"   <menuitem action='ViewIcons'/><menuitem action='ViewList'/><separator/>"
"   <menu action='SortMenu'>"
"    <menuitem action='SortName'/><menuitem action='SortSize'/><menuitem action='SortMtime'/>"
"    <menuitem action='SortType'/><menuitem action='SortExt'/><separator/>"
"    <menuitem action='SortDesc'/><menuitem action='FoldersFirst'/>"
"   </menu>"
"   <menuitem action='ShowHidden'/><menuitem action='Thumbnails'/><menuitem action='HoverHighlight'/><separator/>"
"   <menuitem action='Split'/><menuitem action='PlacesPanel'/><separator/>"
"   <menuitem action='ZoomIn'/><menuitem action='ZoomOut'/><menuitem action='ZoomReset'/><separator/>"
"   <menuitem action='Reload'/>"
"  </menu>"
"  <menu action='GoMenu'>"
"   <menuitem action='Back'/><menuitem action='Forward'/><menuitem action='Up'/><menuitem action='Home'/>"
"   <menuitem action='GoTrash'/><separator/><menuitem action='Location'/><menuitem action='AddBookmark'/><separator/>"
"   <menuitem action='NextTab'/><menuitem action='PrevTab'/>"
"  </menu>"
"  <menu action='HelpMenu'><menuitem action='About'/></menu>"
" </menubar>"
" <toolbar name='ToolBar'>"
"  <toolitem action='Back'/><toolitem action='Forward'/><toolitem action='Up'/><toolitem action='Reload'/>"
"  <separator/><toolitem action='NewFolder'/><separator/>"
"  <toolitem action='Split'/>"
" </toolbar>"
" <popup name='SortPopup'>"
"  <menuitem action='SortName'/><menuitem action='SortSize'/><menuitem action='SortMtime'/>"
"  <menuitem action='SortType'/><menuitem action='SortExt'/><separator/>"
"  <menuitem action='SortDesc'/><menuitem action='FoldersFirst'/>"
" </popup>"
"</ui>";

static gchar *translate_label(const gchar *s, gpointer data)
{
    (void)data;
    return (gchar *)gettext(s);
}

static void translate_entries(GtkActionGroup *g)
{
    gtk_action_group_set_translate_func(g, translate_label, NULL, NULL);
}

static void build_ui(KfApp *app, GtkWidget *vbox, GtkWidget *body)
{
    app->actions = gtk_action_group_new("kifiles");
    translate_entries(app->actions);
    gtk_action_group_add_actions(app->actions, kEntries, G_N_ELEMENTS(kEntries), app);
    gtk_action_group_add_toggle_actions(app->actions, kToggles, G_N_ELEMENTS(kToggles), app);
    gtk_action_group_add_radio_actions(app->actions, kViewRadio, G_N_ELEMENTS(kViewRadio), app->cfg.view,
                                       G_CALLBACK(a_view_changed), app);
    gtk_action_group_add_radio_actions(app->actions, kSortRadio, G_N_ELEMENTS(kSortRadio), app->cfg.sort,
                                       G_CALLBACK(a_sort_changed), app);
    app->ui = gtk_ui_manager_new();
    gtk_ui_manager_insert_action_group(app->ui, app->actions, 0);
    GError *err = NULL;
    if (!gtk_ui_manager_add_ui_from_string(app->ui, kUI, -1, &err)) {
        g_printerr("kifiles: UI: %s\n", err->message);
        g_clear_error(&err);
    }
    gtk_window_add_accel_group(GTK_WINDOW(app->win), gtk_ui_manager_get_accel_group(app->ui));
    gtk_box_pack_start(GTK_BOX(vbox), gtk_ui_manager_get_widget(app->ui, "/MenuBar"), FALSE, FALSE, 0);

    /* The extra accelerators nothing displays: Ctrl+1/2 for the view buttons,
     * Backspace for "up". Registered on the same group. */
    GtkAccelGroup *ag = gtk_ui_manager_get_accel_group(app->ui);
    static const char *hidden[] = { "ViewIconsBtn", "ViewListBtn", "UpBackspace" };
    for (guint i = 0; i < G_N_ELEMENTS(hidden); i++) {
        gtk_action_set_accel_group(act(app, hidden[i]), ag);
        gtk_action_connect_accelerator(act(app, hidden[i]));
    }

    GtkWidget *tb = gtk_ui_manager_get_widget(app->ui, "/ToolBar");
    gtk_toolbar_set_style(GTK_TOOLBAR(tb), GTK_TOOLBAR_BOTH_HORIZ);   /* only "important" items keep a label */
    gtk_toolbar_set_icon_size(GTK_TOOLBAR(tb), GTK_ICON_SIZE_SMALL_TOOLBAR);

    /* view mode + sort buttons next to the stock ones */
    GtkToolItem *sep = gtk_separator_tool_item_new();
    gtk_toolbar_insert(GTK_TOOLBAR(tb), sep, 5);
    GtkToolItem *vi = gtk_radio_tool_button_new(NULL);
    GtkIconTheme *th = gtk_icon_theme_get_default();
    /* A theme without these icons gets a text button instead of a broken image. */
    if (gtk_icon_theme_has_icon(th, "view-list-icons"))
        gtk_tool_button_set_icon_name(GTK_TOOL_BUTTON(vi), "view-list-icons");
    else {
        gtk_tool_button_set_label(GTK_TOOL_BUTTON(vi), _("Icons"));
        gtk_tool_item_set_is_important(vi, TRUE);
    }
    gtk_widget_set_tooltip_text(GTK_WIDGET(vi), _("Icons (Ctrl+1)"));
    GSList *grp = gtk_radio_tool_button_get_group(GTK_RADIO_TOOL_BUTTON(vi));
    GtkToolItem *vl = gtk_radio_tool_button_new(grp);
    if (gtk_icon_theme_has_icon(th, "view-list-details"))
        gtk_tool_button_set_icon_name(GTK_TOOL_BUTTON(vl), "view-list-details");
    else {
        gtk_tool_button_set_label(GTK_TOOL_BUTTON(vl), _("List"));
        gtk_tool_item_set_is_important(vl, TRUE);
    }
    gtk_widget_set_tooltip_text(GTK_WIDGET(vl), _("List (Ctrl+2)"));
    gtk_toolbar_insert(GTK_TOOLBAR(tb), vi, 6);
    gtk_toolbar_insert(GTK_TOOLBAR(tb), vl, 7);
    app->view_toggle_icons = GTK_WIDGET(vi);
    app->view_toggle_list = GTK_WIDGET(vl);
    GtkToolItem *sortb = gtk_tool_button_new(NULL, _("Sort"));
    if (gtk_icon_theme_has_icon(th, "view-sort-ascending"))
        gtk_tool_button_set_icon_name(GTK_TOOL_BUTTON(sortb), "view-sort-ascending");
    else
        gtk_tool_item_set_is_important(sortb, TRUE);
    gtk_widget_set_tooltip_text(GTK_WIDGET(sortb), _("Sort by..."));
    g_signal_connect(sortb, "clicked", G_CALLBACK(on_sort_button), app);
    gtk_toolbar_insert(GTK_TOOLBAR(tb), sortb, 8);
    gtk_box_pack_start(GTK_BOX(body), tb, FALSE, FALSE, 0);
}

static void on_toolbar_view(GtkToggleToolButton *b, gpointer d)
{
    KfApp *app = d;
    if (!gtk_toggle_tool_button_get_active(b) || app->updating_ui)
        return;
    int v = GTK_WIDGET(b) == app->view_toggle_list ? KF_VIEW_LIST : KF_VIEW_ICONS;
    gtk_radio_action_set_current_value(GTK_RADIO_ACTION(act(app, "ViewIcons")), v);
}

static void on_view_action_changed(GtkRadioAction *a, GtkRadioAction *cur, gpointer d)
{
    (void)a;
    KfApp *app = d;
    int v = gtk_radio_action_get_current_value(cur);
    app->updating_ui = TRUE;
    gtk_toggle_tool_button_set_active(GTK_TOGGLE_TOOL_BUTTON(v == KF_VIEW_LIST ? app->view_toggle_list : app->view_toggle_icons), TRUE);
    app->updating_ui = FALSE;
}

static GFile *file_from_arg(const char *arg)
{
    if (strstr(arg, "://"))
        return g_file_new_for_uri(arg);
    char *cwd = g_get_current_dir();
    GFile *f = g_file_new_for_commandline_arg_and_cwd(arg, cwd);
    g_free(cwd);
    return f;
}

int main(int argc, char **argv)
{
    xis_i18n_init("kifiles");
    gtk_init(&argc, &argv);
    setlocale(LC_NUMERIC, "C");     /* gtk_init() resets LC_ALL */

    GFile *start = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            printf("kifiles %s - file manager\n\nUsage: kifiles [FOLDER|URI]\n", KIFILES_VERSION);
            return 0;
        }
        if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--version")) {
            printf("kifiles %s\n", KIFILES_VERSION);
            return 0;
        }
        if (!start && argv[i][0] != '-')
            start = file_from_arg(argv[i]);
    }

    KfApp *app = g_new0(KfApp, 1);
    the_app = app;
    kf_config_load(&app->cfg);
    kf_thumbs_init();
    kf_ops_init(app);

    app->win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_icon_name(GTK_WINDOW(app->win), "system-file-manager");
    gtk_window_set_default_size(GTK_WINDOW(app->win), app->cfg.win_w, app->cfg.win_h);
    gtk_window_set_role(GTK_WINDOW(app->win), "kifiles");
    GtkWidget *vbox = gtk_vbox_new(FALSE, 0);
    gtk_container_add(GTK_CONTAINER(app->win), vbox);
    /* Its own window: moving it between tab pages then moves just that one,
     * instead of every view window of every tab inside it. */
    app->body = gtk_event_box_new();
    GtkWidget *body = gtk_vbox_new(FALSE, 0);
    gtk_container_add(GTK_CONTAINER(app->body), body);
    build_ui(app, vbox, body);
    kf_tabs_setup(app);
    gtk_box_pack_start(GTK_BOX(vbox), app->notebook, TRUE, TRUE, 0);

    app->main_paned = gtk_hpaned_new();
    gtk_box_pack_start(GTK_BOX(body), app->main_paned, TRUE, TRUE, 0);
    app->places = kf_places_new(app);
    gtk_paned_pack1(GTK_PANED(app->main_paned), app->places, FALSE, FALSE);
    gtk_paned_set_position(GTK_PANED(app->main_paned), app->cfg.places_w);
    g_signal_connect(app->main_paned, "notify::position", G_CALLBACK(on_places_position), app);
    g_signal_connect(app->main_paned, "size-allocate", G_CALLBACK(on_main_allocate), app);
    app->panes_box = gtk_hbox_new(FALSE, 0);
    gtk_paned_pack2(GTK_PANED(app->main_paned), app->panes_box, TRUE, TRUE);

    /* status bar */
    GtkWidget *sb = gtk_hbox_new(FALSE, 8);
    gtk_container_set_border_width(GTK_CONTAINER(sb), 3);
    app->status_left = gtk_label_new("");
    gtk_misc_set_alignment(GTK_MISC(app->status_left), 0, 0.5);
    gtk_label_set_ellipsize(GTK_LABEL(app->status_left), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(sb), app->status_left, TRUE, TRUE, 4);
    app->status_free = gtk_label_new("");
    /* Everything in the status bar has to be able to shrink, or its natural
     * width becomes the window's minimum: the free-space text ellipsizes, and
     * the bar and zoom slider are small. */
    gtk_label_set_ellipsize(GTK_LABEL(app->status_free), PANGO_ELLIPSIZE_END);
    gtk_misc_set_alignment(GTK_MISC(app->status_free), 1, 0.5);
    gtk_box_pack_start(GTK_BOX(sb), app->status_free, FALSE, TRUE, 0);
    app->free_bar = gtk_progress_bar_new();
    gtk_widget_set_size_request(app->free_bar, 48, 10);
    gtk_widget_set_no_show_all(app->free_bar, TRUE);
    GtkWidget *bar_box = gtk_vbox_new(FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar_box), app->free_bar, TRUE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(sb), bar_box, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(sb), gtk_vseparator_new(), FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(sb), gtk_image_new_from_stock(GTK_STOCK_ZOOM_OUT, GTK_ICON_SIZE_MENU), FALSE, FALSE, 0);
    app->zoom_scale = gtk_hscale_new_with_range(KF_ZOOM_MIN, KF_ZOOM_MAX, 8);
    gtk_scale_set_draw_value(GTK_SCALE(app->zoom_scale), FALSE);
    gtk_widget_set_size_request(app->zoom_scale, 90, -1);
    gtk_range_set_value(GTK_RANGE(app->zoom_scale), app->cfg.zoom);
    zoom_tooltip(app);
    g_signal_connect(app->zoom_scale, "value-changed", G_CALLBACK(on_zoom_scale), app);
    gtk_box_pack_start(GTK_BOX(sb), app->zoom_scale, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(sb), gtk_image_new_from_stock(GTK_STOCK_ZOOM_IN, GTK_ICON_SIZE_MENU), FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(vbox), sb, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(vbox), gtk_hseparator_new(), FALSE, FALSE, 0);

    g_unix_signal_add(SIGTERM, on_term_signal, app);
    g_unix_signal_add(SIGHUP, on_term_signal, app);
    atexit(save_at_exit);
    g_signal_connect(app->win, "delete-event", G_CALLBACK(on_delete), app);
    g_signal_connect(app->win, "key-press-event", G_CALLBACK(on_key_press), app);
    g_signal_connect(act(app, "ViewIcons"), "changed", G_CALLBACK(on_view_action_changed), app);
    g_signal_connect(app->view_toggle_icons, "toggled", G_CALLBACK(on_toolbar_view), app);
    g_signal_connect(app->view_toggle_list, "toggled", G_CALLBACK(on_toolbar_view), app);

    app->updating_ui = TRUE;
    sync_toggle(app, "ShowHidden", app->cfg.show_hidden);
    sync_toggle(app, "Thumbnails", app->cfg.thumbnails);
    sync_toggle(app, "FoldersFirst", app->cfg.folders_first);
    sync_toggle(app, "PlacesPanel", app->cfg.show_places);
    sync_toggle(app, "HoverHighlight", app->cfg.hover);
    sync_sort_actions(app);
    gtk_toggle_tool_button_set_active(GTK_TOGGLE_TOOL_BUTTON(app->cfg.view == KF_VIEW_LIST ? app->view_toggle_list : app->view_toggle_icons), TRUE);
    app->updating_ui = FALSE;

    owns_session = !start;
    gboolean restoring = owns_session && have_session(app);
    if (restoring) {
        GFile *dirs[2];
        session_entry(app, 0, dirs);
        start = dirs[0];
        if (dirs[1])
            g_object_unref(dirs[1]);
    }
    if (!start)
        start = g_file_new_for_path(g_get_home_dir());
    kf_tabs_open(app, start, TRUE);
    g_object_unref(start);
    gtk_widget_show_all(app->win);
    gtk_widget_set_visible(app->places, app->cfg.show_places);
    if (restoring) {
        session_restore(app);
    } else if (app->cfg.split) {
        app->updating_ui = TRUE;
        sync_toggle(app, "Split", TRUE);
        app->updating_ui = FALSE;
        set_split(app, TRUE);
    }
    kf_pane_focus_view(app->active);
    update_actions(app);
    gtk_main();
    return 0;
}
