/* kifind - find files under a folder (GTK2).
 *
 * Name pattern ('*' = any run of characters) plus, under "Advanced", type,
 * creation/modification date, size, owner and group. The walk runs in a
 * thread (kfd_search.c); results are added to the list on a timer. kifiles
 * starts it with Ctrl+F in the folder it shows. */
#include "kifind.h"
#include "../shared/xis_i18n.h"

#include <gdk/gdkkeysyms.h>
#include <gio/gio.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { COL_ICON, COL_NAME, COL_FOLDER, COL_SIZE, COL_MTIME, COL_PATH, COL_ISDIR, N_COLS };

typedef struct {
    GtkWidget *win, *name, *folder, *subfolders, *case_sens, *hidden;
    GtkWidget *types, *btime[2], *mtime[2], *size[2], *size_unit[2], *users, *groups;
    GtkWidget *find, *stop, *status, *view;
    GtkListStore *store;
    GHashTable *icons;          /* content type -> GdkPixbuf */
    KfdSearch *search;
    guint poll;
    guint found;
} Find;

static void unref_or_null(gpointer o)
{
    if (o)
        g_object_unref(o);
}

/* ---- results ---------------------------------------------------------------- */

static GdkPixbuf *icon_for(Find *f, const char *name, gboolean is_dir)
{
    char *ct = is_dir ? g_strdup("inode/directory") : g_content_type_guess(name, NULL, 0, NULL);
    GdkPixbuf *pb = g_hash_table_lookup(f->icons, ct);
    if (!pb && !g_hash_table_contains(f->icons, ct)) {
        GIcon *gi = g_content_type_get_icon(ct);
        GtkIconInfo *info = gtk_icon_theme_lookup_by_gicon(gtk_icon_theme_get_default(), gi, 16,
                                                           GTK_ICON_LOOKUP_USE_BUILTIN | GTK_ICON_LOOKUP_FORCE_SIZE);
        if (!info)
            info = gtk_icon_theme_lookup_icon(gtk_icon_theme_get_default(), is_dir ? "folder" : "text-x-generic",
                                              16, GTK_ICON_LOOKUP_USE_BUILTIN | GTK_ICON_LOOKUP_FORCE_SIZE);
        if (info) {
            pb = gtk_icon_info_load_icon(info, NULL);
            gtk_icon_info_free(info);
        }
        g_object_unref(gi);
        g_hash_table_insert(f->icons, g_strdup(ct), pb);
    }
    g_free(ct);
    return pb;
}

static void add_results(Find *f, GPtrArray *res)
{
    for (guint i = 0; i < res->len; i++) {
        KfdResult *r = res->pdata[i];
        char *name = g_filename_display_name(r->name);
        char *folder = g_filename_display_name(r->dir);
        char *path = g_build_filename(r->dir, r->name, NULL);
        GtkTreeIter it;
        gtk_list_store_insert_with_values(f->store, &it, -1, COL_ICON, icon_for(f, r->name, r->is_dir),
                                          COL_NAME, name, COL_FOLDER, folder, COL_SIZE, r->is_dir ? (gint64)-1 : r->size,
                                          COL_MTIME, r->mtime, COL_PATH, path, COL_ISDIR, r->is_dir, -1);
        g_free(path);
        g_free(folder);
        g_free(name);
        kfd_result_free(r);
    }
    f->found += res->len;
    g_ptr_array_free(res, TRUE);
}

static void set_found_status(Find *f, gboolean stopped)
{
    char *s;
    if (!f->found)
        s = g_strdup(stopped ? _("Search stopped. Nothing found.") : _("Nothing found."));
    else if (stopped)
        s = g_strdup_printf(ngettext("Search stopped. %u item found.", "Search stopped. %u items found.", f->found),
                            f->found);
    else
        s = g_strdup_printf(ngettext("%u item found.", "%u items found.", f->found), f->found);
    gtk_label_set_text(GTK_LABEL(f->status), s);
    g_free(s);
}

static void set_searching(Find *f, gboolean on)
{
    gtk_widget_set_visible(f->find, !on);
    gtk_widget_set_visible(f->stop, on);
    if (on)
        gtk_widget_grab_default(f->stop);
    else
        gtk_widget_grab_default(f->find);
}

static void end_search(Find *f, gboolean stopped)
{
    if (f->poll) {
        g_source_remove(f->poll);
        f->poll = 0;
    }
    if (f->search) {
        kfd_search_cancel(f->search);
        add_results(f, kfd_search_take(f->search, NULL, NULL));
        kfd_search_unref(f->search);
        f->search = NULL;
    }
    set_searching(f, FALSE);
    set_found_status(f, stopped);
}

static gboolean on_poll(gpointer d)
{
    Find *f = d;
    char *cur = NULL;
    gboolean done = FALSE;
    add_results(f, kfd_search_take(f->search, &cur, &done));
    if (done) {
        g_free(cur);
        f->poll = 0;
        end_search(f, FALSE);
        return FALSE;
    }
    if (cur) {
        char *disp = g_filename_display_name(cur);
        char *s = g_strdup_printf(ngettext("%u found -- searching %s", "%u found -- searching %s", f->found),
                                  f->found, disp);
        gtk_label_set_text(GTK_LABEL(f->status), s);
        g_free(s);
        g_free(disp);
        g_free(cur);
    }
    return TRUE;
}

/* ---- starting a search -------------------------------------------------------- */

static void error_status(Find *f, GtkWidget *focus, const char *msg)
{
    gtk_label_set_text(GTK_LABEL(f->status), msg);
    if (focus)
        gtk_widget_grab_focus(focus);
}

/* "1,5" or "1.5" in the chosen unit; empty = no limit. */
static gboolean size_get(Find *f, int i, gint64 *out)
{
    static const gint64 mult[] = { 1, 1024, 1024 * 1024, 1024LL * 1024 * 1024 };
    char *t = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(f->size[i]))));
    *out = KFD_NO_LIMIT;
    gboolean ok = TRUE;
    if (*t) {
        g_strdelimit(t, ",", '.');
        char *end = NULL;
        double v = g_ascii_strtod(t, &end);
        int u = gtk_combo_box_get_active(GTK_COMBO_BOX(f->size_unit[i]));
        if (end == t || *end || v < 0 || u < 0)
            ok = FALSE;
        else
            *out = (gint64)(v * mult[u] + 0.5);
    }
    g_free(t);
    return ok;
}

static gboolean build_query(Find *f, KfdQuery *q)
{
    memset(q, 0, sizeof(*q));
    q->root = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(f->folder));
    if (!q->root) {
        error_status(f, f->folder, _("Choose a folder to search."));
        return FALSE;
    }
    q->pattern = g_strdup(gtk_entry_get_text(GTK_ENTRY(f->name)));
    q->subfolders = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(f->subfolders));
    q->case_sensitive = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(f->case_sens));
    q->hidden = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(f->hidden));
    GArray *types = kfd_multi_selected(f->types);
    for (guint i = 0; types && i < types->len; i++)
        q->types |= g_array_index(types, int, i);
    if (types)
        g_array_free(types, TRUE);
    gint64 *dates[] = { &q->btime_from, &q->btime_to, &q->mtime_from, &q->mtime_to };
    GtkWidget *dw[] = { f->btime[0], f->btime[1], f->mtime[0], f->mtime[1] };
    for (int i = 0; i < 4; i++)
        if (!kfd_date_get(dw[i], i % 2, dates[i])) {
            error_status(f, dw[i], _("That is not a date."));
            kfd_query_clear(q);
            return FALSE;
        }
    if (!size_get(f, 0, &q->size_from) || !size_get(f, 1, &q->size_to)) {
        error_status(f, f->size[0], _("The size must be a number."));
        kfd_query_clear(q);
        return FALSE;
    }
    q->uids = kfd_multi_selected(f->users);
    q->gids = kfd_multi_selected(f->groups);
    return TRUE;
}

static void on_find(GtkWidget *w, gpointer d)
{
    (void)w;
    Find *f = d;
    KfdQuery q;
    if (!build_query(f, &q))
        return;
    if (f->search)
        end_search(f, TRUE);
    gtk_list_store_clear(f->store);
    f->found = 0;
    gtk_label_set_text(GTK_LABEL(f->status), _("Searching..."));
    f->search = kfd_search_start(&q);
    f->poll = g_timeout_add(150, on_poll, f);
    set_searching(f, TRUE);
}

static void on_stop(GtkWidget *w, gpointer d)
{
    (void)w;
    end_search(d, TRUE);
}

/* ---- opening results -------------------------------------------------------- */

static char *selected_path(Find *f, gboolean *is_dir)
{
    GtkTreeModel *m;
    GtkTreeIter it;
    char *path = NULL;
    if (gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(f->view)), &m, &it))
        gtk_tree_model_get(m, &it, COL_PATH, &path, COL_ISDIR, is_dir, -1);
    return path;
}

static void launch_uri_for(Find *f, const char *path)
{
    GFile *file = g_file_new_for_path(path);
    char *uri = g_file_get_uri(file);
    GError *err = NULL;
    if (!g_app_info_launch_default_for_uri(uri, NULL, &err)) {
        gtk_label_set_text(GTK_LABEL(f->status), err->message);
        g_error_free(err);
    }
    g_free(uri);
    g_object_unref(file);
}

static void open_selected(Find *f)
{
    gboolean is_dir = FALSE;
    char *path = selected_path(f, &is_dir);
    if (path)
        launch_uri_for(f, path);
    g_free(path);
}

/* The folder holding the result, in kifiles when there is one. */
static void open_folder(Find *f)
{
    gboolean is_dir = FALSE;
    char *path = selected_path(f, &is_dir);
    if (!path)
        return;
    char *dir = g_path_get_dirname(path);
    char *argv[] = { "kifiles", dir, NULL };
    if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                       NULL, NULL, NULL, NULL))
        launch_uri_for(f, dir);
    g_free(dir);
    g_free(path);
}

static void copy_path(Find *f)
{
    gboolean is_dir = FALSE;
    char *path = selected_path(f, &is_dir);
    if (!path)
        return;
    char *disp = g_filename_display_name(path);
    gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), disp, -1);
    g_free(disp);
    g_free(path);
}

static void m_open(GtkMenuItem *i, gpointer d) { (void)i; open_selected(d); }
static void m_folder(GtkMenuItem *i, gpointer d) { (void)i; open_folder(d); }
static void m_copy(GtkMenuItem *i, gpointer d) { (void)i; copy_path(d); }

static void on_row_activated(GtkTreeView *tv, GtkTreePath *p, GtkTreeViewColumn *c, gpointer d)
{
    (void)tv; (void)p; (void)c;
    open_selected(d);
}

static void menu_item(GtkWidget *menu, const char *label, GCallback cb, gpointer d)
{
    GtkWidget *i = gtk_menu_item_new_with_mnemonic(label);
    g_signal_connect(i, "activate", cb, d);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), i);
}

static gboolean on_view_button(GtkWidget *w, GdkEventButton *ev, gpointer d)
{
    if (ev->type != GDK_BUTTON_PRESS || ev->button != 3)
        return FALSE;
    GtkTreePath *path = NULL;
    if (!gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(w), (int)ev->x, (int)ev->y, &path, NULL, NULL, NULL))
        return TRUE;
    gtk_tree_selection_select_path(gtk_tree_view_get_selection(GTK_TREE_VIEW(w)), path);
    gtk_tree_path_free(path);
    GtkWidget *menu = gtk_menu_new();
    menu_item(menu, _("_Open"), G_CALLBACK(m_open), d);
    menu_item(menu, _("Open Containing _Folder"), G_CALLBACK(m_folder), d);
    menu_item(menu, _("Copy _Path"), G_CALLBACK(m_copy), d);
    gtk_widget_show_all(menu);
    g_signal_connect(menu, "selection-done", G_CALLBACK(gtk_widget_destroy), NULL);
    gtk_menu_popup(GTK_MENU(menu), NULL, NULL, NULL, NULL, ev->button, ev->time);
    return TRUE;
}

static void size_cell(GtkTreeViewColumn *c, GtkCellRenderer *r, GtkTreeModel *m, GtkTreeIter *it, gpointer d)
{
    (void)c; (void)d;
    gint64 v;
    gtk_tree_model_get(m, it, COL_SIZE, &v, -1);
    char *s = v < 0 ? g_strdup("") : g_format_size_full((guint64)v, G_FORMAT_SIZE_IEC_UNITS);
    g_object_set(r, "text", s, NULL);
    g_free(s);
}

static void mtime_cell(GtkTreeViewColumn *c, GtkCellRenderer *r, GtkTreeModel *m, GtkTreeIter *it, gpointer d)
{
    (void)c; (void)d;
    gint64 v;
    gtk_tree_model_get(m, it, COL_MTIME, &v, -1);
    GDateTime *dt = g_date_time_new_from_unix_local(v);
    char *s = dt ? g_date_time_format(dt, "%x %H:%M") : NULL;
    g_object_set(r, "text", s ? s : "", NULL);
    g_free(s);
    if (dt)
        g_date_time_unref(dt);
}

static gint cmp_int64(GtkTreeModel *m, GtkTreeIter *a, GtkTreeIter *b, gpointer d)
{
    int col = GPOINTER_TO_INT(d);
    gint64 x, y;
    gtk_tree_model_get(m, a, col, &x, -1);
    gtk_tree_model_get(m, b, col, &y, -1);
    return x < y ? -1 : x > y;
}

static GtkTreeViewColumn *text_column(Find *f, const char *title, int sort_col, GtkTreeCellDataFunc fn, int model_col)
{
    GtkCellRenderer *r = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *c = gtk_tree_view_column_new();
    gtk_tree_view_column_set_title(c, title);
    gtk_tree_view_column_pack_start(c, r, TRUE);
    if (fn) {
        gtk_tree_view_column_set_cell_data_func(c, r, fn, NULL, NULL);
        g_object_set(r, "xalign", sort_col == COL_SIZE ? 1.0 : 0.0, NULL);
    } else {
        gtk_tree_view_column_add_attribute(c, r, "text", model_col);
        g_object_set(r, "ellipsize", PANGO_ELLIPSIZE_START, NULL);     /* the deepest part of the path matters most */
        gtk_tree_view_column_set_expand(c, TRUE);
    }
    gtk_tree_view_column_set_resizable(c, TRUE);
    gtk_tree_view_column_set_sort_column_id(c, sort_col);
    gtk_tree_view_append_column(GTK_TREE_VIEW(f->view), c);
    return c;
}

static GtkWidget *build_results(Find *f)
{
    f->store = gtk_list_store_new(N_COLS, GDK_TYPE_PIXBUF, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT64,
                                  G_TYPE_INT64, G_TYPE_STRING, G_TYPE_BOOLEAN);
    gtk_tree_sortable_set_sort_func(GTK_TREE_SORTABLE(f->store), COL_SIZE, cmp_int64, GINT_TO_POINTER(COL_SIZE), NULL);
    gtk_tree_sortable_set_sort_func(GTK_TREE_SORTABLE(f->store), COL_MTIME, cmp_int64, GINT_TO_POINTER(COL_MTIME), NULL);
    f->view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(f->store));
    g_object_unref(f->store);
    GtkTreeViewColumn *nc = gtk_tree_view_column_new();
    gtk_tree_view_column_set_title(nc, _("Name"));
    GtkCellRenderer *pr = gtk_cell_renderer_pixbuf_new();
    gtk_tree_view_column_pack_start(nc, pr, FALSE);
    gtk_tree_view_column_add_attribute(nc, pr, "pixbuf", COL_ICON);
    GtkCellRenderer *tr = gtk_cell_renderer_text_new();
    g_object_set(tr, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
    gtk_tree_view_column_pack_start(nc, tr, TRUE);
    gtk_tree_view_column_add_attribute(nc, tr, "text", COL_NAME);
    gtk_tree_view_column_set_expand(nc, TRUE);
    gtk_tree_view_column_set_resizable(nc, TRUE);
    gtk_tree_view_column_set_sort_column_id(nc, COL_NAME);
    gtk_tree_view_append_column(GTK_TREE_VIEW(f->view), nc);
    text_column(f, _("In Folder"), COL_FOLDER, NULL, COL_FOLDER);
    text_column(f, _("Size"), COL_SIZE, size_cell, 0);
    text_column(f, _("Modified"), COL_MTIME, mtime_cell, 0);
    gtk_tree_view_set_search_column(GTK_TREE_VIEW(f->view), COL_NAME);
    g_signal_connect(f->view, "row-activated", G_CALLBACK(on_row_activated), f);
    g_signal_connect(f->view, "button-press-event", G_CALLBACK(on_view_button), f);
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sw), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(sw), f->view);
    return sw;
}

/* ---- advanced options ---------------------------------------------------------- */

typedef struct { char *name; int id; gboolean primary; } Account;

static gint cmp_account(gconstpointer a, gconstpointer b)
{
    return g_utf8_collate(((const Account *)a)->name, ((const Account *)b)->name);
}

/* People (ids 1000..65533) and root up top, service accounts in a submenu. */
static void fill_accounts(GtkWidget *multi, gboolean groups)
{
    GArray *list = g_array_new(FALSE, FALSE, sizeof(Account));
    if (groups) {
        setgrent();
        for (struct group *g; (g = getgrent());) {
            Account a = { g_strdup(g->gr_name), (int)g->gr_gid, g->gr_gid == 0 || (g->gr_gid >= 1000 && g->gr_gid < 65534) };
            g_array_append_val(list, a);
        }
        endgrent();
    } else {
        setpwent();
        for (struct passwd *p; (p = getpwent());) {
            Account a = { g_strdup(p->pw_name), (int)p->pw_uid, p->pw_uid == 0 || (p->pw_uid >= 1000 && p->pw_uid < 65534) };
            g_array_append_val(list, a);
        }
        endpwent();
    }
    g_array_sort(list, cmp_account);
    kfd_multi_set_secondary_label(multi, groups ? _("System groups") : _("System accounts"));
    for (guint i = 0; i < list->len; i++) {
        Account *a = &g_array_index(list, Account, i);
        kfd_multi_add(multi, a->name, a->id, !a->primary);
        g_free(a->name);
    }
    g_array_free(list, TRUE);
}

static GtkWidget *range_row(GtkWidget *table, int row, const char *title, GtkWidget *from, GtkWidget *to,
                            GtkWidget *from_extra, GtkWidget *to_extra)
{
    GtkWidget *l = gtk_label_new(title);
    gtk_misc_set_alignment(GTK_MISC(l), 0, 0.5);
    gtk_table_attach(GTK_TABLE(table), l, 0, 1, row, row + 1, GTK_FILL, 0, 0, 0);
    GtkWidget *box = gtk_hbox_new(FALSE, 4);
    gtk_box_pack_start(GTK_BOX(box), gtk_label_new(_("From")), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), from, TRUE, TRUE, 0);
    if (from_extra)
        gtk_box_pack_start(GTK_BOX(box), from_extra, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), gtk_label_new(_("To")), FALSE, FALSE, 6);
    gtk_box_pack_start(GTK_BOX(box), to, TRUE, TRUE, 0);
    if (to_extra)
        gtk_box_pack_start(GTK_BOX(box), to_extra, FALSE, FALSE, 0);
    gtk_table_attach(GTK_TABLE(table), box, 1, 2, row, row + 1, GTK_EXPAND | GTK_FILL, 0, 0, 0);
    return box;
}

static GtkWidget *unit_combo(void)
{
    GtkWidget *c = gtk_combo_box_new_text();
    gtk_combo_box_append_text(GTK_COMBO_BOX(c), _("bytes"));
    gtk_combo_box_append_text(GTK_COMBO_BOX(c), "KiB");
    gtk_combo_box_append_text(GTK_COMBO_BOX(c), "MiB");
    gtk_combo_box_append_text(GTK_COMBO_BOX(c), "GiB");
    gtk_combo_box_set_active(GTK_COMBO_BOX(c), 2);
    return c;
}

static void labeled(GtkWidget *table, int row, const char *title, GtkWidget *w)
{
    GtkWidget *l = gtk_label_new(title);
    gtk_misc_set_alignment(GTK_MISC(l), 0, 0.5);
    gtk_table_attach(GTK_TABLE(table), l, 0, 1, row, row + 1, GTK_FILL, 0, 0, 0);
    gtk_table_attach(GTK_TABLE(table), w, 1, 2, row, row + 1, GTK_EXPAND | GTK_FILL, 0, 0, 0);
}

static void on_reset_advanced(GtkButton *b, gpointer d)
{
    (void)b;
    Find *f = d;
    kfd_multi_clear(f->types);
    kfd_multi_clear(f->users);
    kfd_multi_clear(f->groups);
    for (int i = 0; i < 2; i++) {
        gtk_entry_set_text(GTK_ENTRY(f->btime[i]), "");
        gtk_entry_set_text(GTK_ENTRY(f->mtime[i]), "");
        gtk_entry_set_text(GTK_ENTRY(f->size[i]), "");
    }
}

static GtkWidget *build_advanced(Find *f)
{
    GtkWidget *t = gtk_table_new(6, 2, FALSE);
    gtk_table_set_row_spacings(GTK_TABLE(t), 4);
    gtk_table_set_col_spacings(GTK_TABLE(t), 8);
    gtk_container_set_border_width(GTK_CONTAINER(t), 4);

    f->types = kfd_multi_new(_("Any type"));
    static const struct { const char *label; int bit; } types[] = {
        { N_("Folders"), KFD_T_DIR }, { N_("Images"), KFD_T_IMAGE }, { N_("Audio"), KFD_T_AUDIO },
        { N_("Video"), KFD_T_VIDEO }, { N_("Text files"), KFD_T_TEXT }, { N_("Documents"), KFD_T_DOC },
        { N_("Archives"), KFD_T_ARCHIVE }, { N_("Executables"), KFD_T_EXEC }, { N_("Other files"), KFD_T_OTHER },
    };
    for (guint i = 0; i < G_N_ELEMENTS(types); i++)
        kfd_multi_add(f->types, _(types[i].label), types[i].bit, FALSE);
    labeled(t, 0, _("Type:"), f->types);

    for (int i = 0; i < 2; i++) {
        f->btime[i] = kfd_date_new();
        f->mtime[i] = kfd_date_new();
        f->size[i] = gtk_entry_new();
        gtk_entry_set_width_chars(GTK_ENTRY(f->size[i]), 6);
        f->size_unit[i] = unit_combo();
    }
    range_row(t, 1, _("Created:"), f->btime[0], f->btime[1], NULL, NULL);
    range_row(t, 2, _("Modified:"), f->mtime[0], f->mtime[1], NULL, NULL);
    range_row(t, 3, _("Size:"), f->size[0], f->size[1], f->size_unit[0], f->size_unit[1]);

    f->users = kfd_multi_new(_("Anyone"));
    fill_accounts(f->users, FALSE);
    labeled(t, 4, _("Owner:"), f->users);
    f->groups = kfd_multi_new(_("Any group"));
    fill_accounts(f->groups, TRUE);
    labeled(t, 5, _("Group:"), f->groups);

    GtkWidget *reset = gtk_button_new_with_mnemonic(_("_Reset"));
    gtk_widget_set_tooltip_text(reset, _("Clear the advanced options"));
    g_signal_connect(reset, "clicked", G_CALLBACK(on_reset_advanced), f);
    GtkWidget *row = gtk_hbox_new(FALSE, 0);
    gtk_box_pack_end(GTK_BOX(row), reset, FALSE, FALSE, 0);
    GtkWidget *box = gtk_vbox_new(FALSE, 2);
    gtk_box_pack_start(GTK_BOX(box), t, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
    return box;
}

/* ---- window ---------------------------------------------------------------- */

static gboolean on_key(GtkWidget *w, GdkEventKey *ev, gpointer d)
{
    Find *f = d;
    if (ev->keyval != GDK_KEY_Escape)
        return FALSE;
    if (f->search)
        end_search(f, TRUE);
    else
        gtk_widget_destroy(w);
    return TRUE;
}

/* A path, a file:// URI or nothing (the current folder). */
static char *start_folder(const char *arg)
{
    if (!arg)
        return g_get_current_dir();
    GFile *f = g_file_new_for_commandline_arg(arg);
    char *path = g_file_get_path(f);
    g_object_unref(f);
    if (path && g_file_test(path, G_FILE_TEST_IS_DIR))
        return path;
    g_free(path);
    return g_strdup(g_get_home_dir());
}

static GtkWidget *row_label(const char *text, GtkWidget *target)
{
    GtkWidget *l = gtk_label_new_with_mnemonic(text);
    gtk_misc_set_alignment(GTK_MISC(l), 0, 0.5);
    gtk_label_set_mnemonic_widget(GTK_LABEL(l), target);
    return l;
}

int main(int argc, char **argv)
{
    xis_i18n_init("kifind");
    const char *arg = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--version")) {
            printf("kifind %s\n", KIFIND_VERSION);
            return 0;
        }
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            printf(_("Usage: kifind [FOLDER]\n"));
            return 0;
        }
        if (!arg && argv[i][0] != '-')
            arg = argv[i];
    }
    gtk_init(&argc, &argv);
    gtk_window_set_default_icon_name("system-search");

    Find *f = g_new0(Find, 1);
    f->icons = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, unref_or_null);
    f->win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(f->win), _("Find Files"));
    gtk_window_set_role(GTK_WINDOW(f->win), "kifind");
    gtk_window_set_default_size(GTK_WINDOW(f->win), 640, 480);
    g_signal_connect(f->win, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    g_signal_connect(f->win, "key-press-event", G_CALLBACK(on_key), f);

    GtkWidget *vbox = gtk_vbox_new(FALSE, 6);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 8);
    gtk_container_add(GTK_CONTAINER(f->win), vbox);

    GtkWidget *grid = gtk_table_new(2, 2, FALSE);
    gtk_table_set_row_spacings(GTK_TABLE(grid), 4);
    gtk_table_set_col_spacings(GTK_TABLE(grid), 8);
    f->name = gtk_entry_new();
    gtk_entry_set_activates_default(GTK_ENTRY(f->name), TRUE);
    gtk_widget_set_tooltip_text(f->name, _("Part of the name, or a pattern where * stands for any characters "
                                           "(*.txt, report*2024*). Empty finds everything."));
    gtk_table_attach(GTK_TABLE(grid), row_label(_("_Named:"), f->name), 0, 1, 0, 1, GTK_FILL, 0, 0, 0);
    gtk_table_attach(GTK_TABLE(grid), f->name, 1, 2, 0, 1, GTK_EXPAND | GTK_FILL, 0, 0, 0);
    f->folder = gtk_file_chooser_button_new(_("Look In"), GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER);
    char *dir = start_folder(arg);
    gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(f->folder), dir);
    gtk_table_attach(GTK_TABLE(grid), row_label(_("_Look in:"), f->folder), 0, 1, 1, 2, GTK_FILL, 0, 0, 0);
    gtk_table_attach(GTK_TABLE(grid), f->folder, 1, 2, 1, 2, GTK_EXPAND | GTK_FILL, 0, 0, 0);
    gtk_box_pack_start(GTK_BOX(vbox), grid, FALSE, FALSE, 0);

    GtkWidget *checks = gtk_vbox_new(FALSE, 0);
    f->subfolders = gtk_check_button_new_with_mnemonic(_("Include _subfolders"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(f->subfolders), TRUE);
    f->case_sens = gtk_check_button_new_with_mnemonic(_("_Match case"));
    f->hidden = gtk_check_button_new_with_mnemonic(_("Include _hidden files and folders"));
    gtk_box_pack_start(GTK_BOX(checks), f->subfolders, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(checks), f->case_sens, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(checks), f->hidden, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), checks, FALSE, FALSE, 0);

    GtkWidget *exp = gtk_expander_new_with_mnemonic(_("_Advanced"));
    gtk_container_add(GTK_CONTAINER(exp), build_advanced(f));
    gtk_box_pack_start(GTK_BOX(vbox), exp, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(vbox), build_results(f), TRUE, TRUE, 0);

    GtkWidget *bottom = gtk_hbox_new(FALSE, 6);
    f->status = gtk_label_new("");
    gtk_misc_set_alignment(GTK_MISC(f->status), 0, 0.5);
    gtk_label_set_ellipsize(GTK_LABEL(f->status), PANGO_ELLIPSIZE_MIDDLE);
    gtk_box_pack_start(GTK_BOX(bottom), f->status, TRUE, TRUE, 0);
    GtkWidget *close = gtk_button_new_from_stock(GTK_STOCK_CLOSE);
    g_signal_connect_swapped(close, "clicked", G_CALLBACK(gtk_widget_destroy), f->win);
    gtk_box_pack_end(GTK_BOX(bottom), close, FALSE, FALSE, 0);
    f->stop = gtk_button_new_from_stock(GTK_STOCK_STOP);
    gtk_widget_set_can_default(f->stop, TRUE);
    gtk_widget_set_no_show_all(f->stop, TRUE);
    g_signal_connect(f->stop, "clicked", G_CALLBACK(on_stop), f);
    gtk_box_pack_end(GTK_BOX(bottom), f->stop, FALSE, FALSE, 0);
    f->find = gtk_button_new_from_stock(GTK_STOCK_FIND);
    gtk_widget_set_can_default(f->find, TRUE);
    g_signal_connect(f->find, "clicked", G_CALLBACK(on_find), f);
    gtk_box_pack_end(GTK_BOX(bottom), f->find, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), bottom, FALSE, FALSE, 0);

    gtk_widget_show_all(f->win);
    gtk_widget_grab_default(f->find);
    gtk_widget_grab_focus(f->name);
    g_free(dir);
    gtk_main();
    if (f->search) {
        kfd_search_cancel(f->search);
        kfd_search_unref(f->search);
    }
    if (f->poll)
        g_source_remove(f->poll);
    g_hash_table_destroy(f->icons);
    g_free(f);
    return 0;
}
