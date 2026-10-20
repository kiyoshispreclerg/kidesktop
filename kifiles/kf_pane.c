/* kf_pane.c - one folder view: model, loading, icon/list views, sorting,
 * history. Two of these make the split view.
 *
 *   GtkListStore (all entries, KF_COL_*)
 *     -> GtkTreeModelFilter (hidden files, quick filter)
 *       -> GtkTreeModelSort (kifiles' sort key; see sort_cb)
 *         -> GtkIconView / GtkTreeView (only one visible at a time)
 */
#include "kf.h"
#include "../shared/xis_i18n.h"
#include <string.h>

#define SORT_ID 1
#define ATTRS "standard::name,standard::display-name,standard::icon,standard::content-type," \
              "standard::size,standard::type,standard::is-hidden,standard::is-backup," \
              "time::modified"

typedef struct {
    KfPane *pane;
    GCancellable *cancel;       /* identifies the load this belongs to */
    GFileEnumerator *en;
} LoadCtx;

static void load_start(KfPane *p);

void kf_files_free(GList *files)
{
    g_list_free_full(files, g_object_unref);
}

/* The list view keeps rows compact: small icons that still grow with zoom. */
static int list_icon_px(KfPane *p)
{
    int z = p->app->cfg.zoom;
    if (z <= 32)
        return z;
    int px = z / 2;
    return px < 24 ? 24 : px > 64 ? 64 : px;
}

static int cur_icon_px(KfPane *p)
{
    return p->view == KF_VIEW_LIST ? list_icon_px(p) : p->app->cfg.zoom;
}

/* ---- sorting / filtering ------------------------------------------------ */

static gint sort_cb(GtkTreeModel *m, GtkTreeIter *a, GtkTreeIter *b, gpointer data)
{
    KfPane *p = data;
    const KfConfig *c = &p->app->cfg;
    gboolean da = FALSE, db = FALSE;
    gtk_tree_model_get(m, a, KF_COL_ISDIR, &da, -1);
    gtk_tree_model_get(m, b, KF_COL_ISDIR, &db, -1);
    /* Folders-first is applied before the direction flip below so that
     * "descending" never pushes folders under the files. */
    if (c->folders_first && da != db)
        return da ? -1 : 1;

    int r = 0;
    switch (c->sort) {
    case KF_SORT_SIZE:
        if (!(da && db)) {
            guint64 sa = 0, sb = 0;
            gtk_tree_model_get(m, a, KF_COL_SIZE, &sa, -1);
            gtk_tree_model_get(m, b, KF_COL_SIZE, &sb, -1);
            r = sa < sb ? -1 : sa > sb ? 1 : 0;
        }
        break;
    case KF_SORT_MTIME: {
        gint64 ta = 0, tb = 0;
        gtk_tree_model_get(m, a, KF_COL_MTIME, &ta, -1);
        gtk_tree_model_get(m, b, KF_COL_MTIME, &tb, -1);
        r = ta < tb ? -1 : ta > tb ? 1 : 0;
        break;
    }
    case KF_SORT_TYPE: {
        char *sa = NULL, *sb = NULL;
        gtk_tree_model_get(m, a, KF_COL_TYPEDESC, &sa, -1);
        gtk_tree_model_get(m, b, KF_COL_TYPEDESC, &sb, -1);
        r = g_utf8_collate(sa ? sa : "", sb ? sb : "");
        g_free(sa);
        g_free(sb);
        break;
    }
    case KF_SORT_EXT: {
        char *sa = NULL, *sb = NULL;
        gtk_tree_model_get(m, a, KF_COL_EXT, &sa, -1);
        gtk_tree_model_get(m, b, KF_COL_EXT, &sb, -1);
        r = g_strcmp0(sa, sb);
        g_free(sa);
        g_free(sb);
        break;
    }
    default:
        break;
    }
    if (r == 0) {
        char *ka = NULL, *kb = NULL;
        gtk_tree_model_get(m, a, KF_COL_COLLATE, &ka, -1);
        gtk_tree_model_get(m, b, KF_COL_COLLATE, &kb, -1);
        r = g_strcmp0(ka, kb);
        g_free(ka);
        g_free(kb);
    }
    return c->sort_desc ? -r : r;
}

static void update_sort_indicators(KfPane *p)
{
    const KfConfig *c = &p->app->cfg;
    GList *cols = gtk_tree_view_get_columns(GTK_TREE_VIEW(p->treeview));
    for (GList *l = cols; l; l = l->next) {
        int key = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(l->data), "kf-sort"));
        gboolean on = key == c->sort;
        gtk_tree_view_column_set_sort_indicator(l->data, on);
        gtk_tree_view_column_set_sort_order(l->data, c->sort_desc ? GTK_SORT_DESCENDING : GTK_SORT_ASCENDING);
    }
    g_list_free(cols);
}

void kf_pane_apply_sort(KfPane *p)
{
    GtkTreeSortable *s = GTK_TREE_SORTABLE(p->sort);
    gtk_tree_sortable_set_sort_func(s, SORT_ID, sort_cb, p, NULL);
    /* "Unsorted" is the default column with a default function that calls
     * everything equal: GTK2's explicit UNSORTED id trips assertions in
     * GtkTreeModelSort when rows are inserted. */
    if (p->loading) {
        /* Sorting once at the end is much cheaper than on every batch. */
        gtk_tree_sortable_set_sort_column_id(s, GTK_TREE_SORTABLE_DEFAULT_SORT_COLUMN_ID, GTK_SORT_ASCENDING);
    } else {
        /* set_sort_column_id() is a no-op when nothing changed, so go through
         * the default column to force a re-sort with the new key/direction. */
        gtk_tree_sortable_set_sort_column_id(s, GTK_TREE_SORTABLE_DEFAULT_SORT_COLUMN_ID, GTK_SORT_ASCENDING);
        gtk_tree_sortable_set_sort_column_id(s, SORT_ID, GTK_SORT_ASCENDING);
    }
    update_sort_indicators(p);
}

static gint unsorted_cb(GtkTreeModel *m, GtkTreeIter *a, GtkTreeIter *b, gpointer data)
{
    (void)m; (void)a; (void)b; (void)data;
    return 0;
}

static gboolean visible_cb(GtkTreeModel *m, GtkTreeIter *it, gpointer data)
{
    KfPane *p = data;
    gboolean hidden = FALSE;
    gtk_tree_model_get(m, it, KF_COL_HIDDEN, &hidden, -1);
    if (hidden && !p->app->cfg.show_hidden)
        return FALSE;
    if (p->filter_text && *p->filter_text) {
        char *name = NULL;
        gtk_tree_model_get(m, it, KF_COL_NAME, &name, -1);
        gboolean ok = FALSE;
        if (name) {
            char *a = g_utf8_casefold(name, -1), *b = g_utf8_casefold(p->filter_text, -1);
            ok = strstr(a, b) != NULL;
            g_free(a);
            g_free(b);
            g_free(name);
        }
        return ok;
    }
    return TRUE;
}

void kf_pane_apply_filter(KfPane *p)
{
    gtk_tree_model_filter_refilter(GTK_TREE_MODEL_FILTER(p->filter));
    kf_app_pane_changed(p->app, p, KF_CHANGE_CONTENT);
    kf_pane_request_thumbs(p);
}

/* ---- rows --------------------------------------------------------------- */

static GtkTreeIter *row_iter(KfPane *p, const char *name, GtkTreeIter *out)
{
    GtkTreeRowReference *rr = g_hash_table_lookup(p->rows, name);
    if (!rr)
        return NULL;
    GtkTreePath *path = gtk_tree_row_reference_get_path(rr);
    if (!path)
        return NULL;
    gboolean ok = gtk_tree_model_get_iter(GTK_TREE_MODEL(p->store), out, path);
    gtk_tree_path_free(path);
    return ok ? out : NULL;
}

static const char *type_desc(KfPane *p, const char *ctype)
{
    if (!ctype)
        return "";
    char *d = g_hash_table_lookup(p->typedescs, ctype);
    if (!d) {
        d = g_content_type_get_description(ctype);
        g_hash_table_insert(p->typedescs, g_strdup(ctype), d ? d : g_strdup(ctype));
    }
    return d ? d : ctype;
}

static char *lower_ext(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot || dot == name || !dot[1])
        return g_strdup("");
    return g_utf8_strdown(dot + 1, -1);
}

static void fill_row(KfPane *p, GtkTreeIter *it, GFile *child, GFileInfo *info)
{
    const char *name = g_file_info_get_name(info);
    const char *disp = g_file_info_get_display_name(info);
    const char *ctype = g_file_info_get_content_type(info);
    gboolean isdir = g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY;
    gboolean hidden = g_file_info_get_is_hidden(info) || g_file_info_get_is_backup(info);
    GIcon *icon = g_file_info_get_icon(info);
    char *iconstr = icon ? g_icon_to_string(icon) : g_strdup("text-x-generic");
    char *collate = g_utf8_collate_key_for_filename(disp ? disp : name, -1);
    char *ext = isdir ? g_strdup("") : lower_ext(name);
    GdkPixbuf *pb = kf_icon_lookup(iconstr, cur_icon_px(p));
    gtk_list_store_set(p->store, it,
        KF_COL_FILE, child, KF_COL_NAME, disp ? disp : name, KF_COL_ICON, pb,
        KF_COL_SIZE, isdir ? 0 : (guint64)g_file_info_get_size(info),
        KF_COL_MTIME, (gint64)g_file_info_get_attribute_uint64(info, "time::modified"),
        KF_COL_TYPEDESC, type_desc(p, ctype), KF_COL_ISDIR, isdir, KF_COL_HIDDEN, hidden,
        KF_COL_COLLATE, collate, KF_COL_CTYPE, ctype ? ctype : "", KF_COL_ICONSTR, iconstr,
        KF_COL_EXT, ext, -1);
    g_free(iconstr);
    g_free(collate);
    g_free(ext);
}

static void queue_stats(KfPane *p);

static void apply_pending_select(KfPane *p, const char *name)
{
    for (GList *l = p->pending_select; l; l = l->next) {
        if (strcmp(l->data, name) == 0) {
            GList *one = g_list_append(NULL, l->data);
            kf_pane_select_names(p, one);
            g_list_free(one);
            g_free(l->data);
            p->pending_select = g_list_delete_link(p->pending_select, l);
            return;
        }
    }
}

static void add_info(KfPane *p, GFileInfo *info)
{
    const char *name = g_file_info_get_name(info);
    if (!name || g_hash_table_contains(p->rows, name))
        return;
    GFile *child = g_file_get_child(p->dir, name);
    GtkTreeIter it;
    gtk_list_store_append(p->store, &it);
    fill_row(p, &it, child, info);
    g_object_unref(child);
    GtkTreePath *path = gtk_tree_model_get_path(GTK_TREE_MODEL(p->store), &it);
    g_hash_table_insert(p->rows, g_strdup(name), gtk_tree_row_reference_new(GTK_TREE_MODEL(p->store), path));
    gtk_tree_path_free(path);
}

static void remove_row(KfPane *p, const char *name)
{
    GtkTreeIter it;
    if (row_iter(p, name, &it)) {
        gtk_list_store_remove(p->store, &it);
        g_hash_table_remove(p->rows, name);
    }
}

/* ---- loading ------------------------------------------------------------ */

static void show_message(KfPane *p, GtkMessageType type, const char *text)
{
    if (!text) {
        gtk_widget_hide(p->msg_bar);
        return;
    }
    gtk_info_bar_set_message_type(GTK_INFO_BAR(p->msg_bar), type);
    gtk_label_set_text(GTK_LABEL(p->msg_label), text);
    gtk_widget_show(p->msg_bar);
}

static void load_ctx_free(LoadCtx *c)
{
    if (c->en)
        g_object_unref(c->en);
    g_object_unref(c->cancel);
    g_free(c);
}

static void load_finished(KfPane *p)
{
    p->loading = FALSE;
    kf_pane_apply_sort(p);
    if (gtk_tree_model_iter_n_children(p->sort, NULL) == 0 && !gtk_widget_get_visible(p->msg_bar))
        show_message(p, GTK_MESSAGE_INFO, _("This folder is empty."));
    kf_app_pane_changed(p->app, p, KF_CHANGE_LOADED);
    kf_pane_request_thumbs(p);
    kf_hover_refresh(p);
}

static void on_next_files(GObject *src, GAsyncResult *res, gpointer data)
{
    LoadCtx *c = data;
    GError *err = NULL;
    GList *infos = g_file_enumerator_next_files_finish(G_FILE_ENUMERATOR(src), res, &err);
    if (g_cancellable_is_cancelled(c->cancel)) {
        g_list_free_full(infos, g_object_unref);
        g_clear_error(&err);
        load_ctx_free(c);
        return;
    }
    KfPane *p = c->pane;
    if (err) {
        show_message(p, GTK_MESSAGE_WARNING, err->message);
        g_clear_error(&err);
    }
    for (GList *l = infos; l; l = l->next) {
        add_info(p, l->data);
        apply_pending_select(p, g_file_info_get_name(l->data));
    }
    gboolean more = infos != NULL;
    g_list_free_full(infos, g_object_unref);
    if (more) {
        g_file_enumerator_next_files_async(c->en, 200, G_PRIORITY_DEFAULT, c->cancel, on_next_files, c);
        queue_stats(p);
        return;
    }
    g_file_enumerator_close_async(c->en, G_PRIORITY_DEFAULT, NULL, NULL, NULL);
    load_ctx_free(c);
    load_finished(p);
}

static void on_mounted(GObject *src, GAsyncResult *res, gpointer data)
{
    LoadCtx *c = data;
    GError *err = NULL;
    gboolean ok = g_file_mount_enclosing_volume_finish(G_FILE(src), res, &err);
    if (g_cancellable_is_cancelled(c->cancel)) {
        g_clear_error(&err);
        load_ctx_free(c);
        return;
    }
    KfPane *p = c->pane;
    load_ctx_free(c);
    if (ok || (err && g_error_matches(err, G_IO_ERROR, G_IO_ERROR_ALREADY_MOUNTED))) {
        g_clear_error(&err);
        load_start(p);
        return;
    }
    p->loading = FALSE;
    show_message(p, GTK_MESSAGE_ERROR, err ? err->message : _("Could not mount this location."));
    g_clear_error(&err);
    kf_app_pane_changed(p->app, p, KF_CHANGE_LOADED);
}

static void on_enumerate(GObject *src, GAsyncResult *res, gpointer data)
{
    LoadCtx *c = data;
    GError *err = NULL;
    GFileEnumerator *en = g_file_enumerate_children_finish(G_FILE(src), res, &err);
    if (g_cancellable_is_cancelled(c->cancel)) {
        if (en)
            g_object_unref(en);
        g_clear_error(&err);
        load_ctx_free(c);
        return;
    }
    KfPane *p = c->pane;
    if (!en) {
        if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_NOT_MOUNTED)) {
            GMountOperation *mo = gtk_mount_operation_new(GTK_WINDOW(p->app->win));
            g_file_mount_enclosing_volume(p->dir, G_MOUNT_MOUNT_NONE, mo, c->cancel, on_mounted, c);
            g_object_unref(mo);
            g_clear_error(&err);
            return;
        }
        show_message(p, GTK_MESSAGE_ERROR, err->message);
        g_clear_error(&err);
        p->loading = FALSE;
        load_ctx_free(c);
        kf_app_pane_changed(p->app, p, KF_CHANGE_LOADED);
        return;
    }
    c->en = en;
    g_file_enumerator_next_files_async(en, 200, G_PRIORITY_DEFAULT, c->cancel, on_next_files, c);
}

static void on_monitor(GFileMonitor *m, GFile *file, GFile *other, GFileMonitorEvent ev, gpointer data);

static void on_info_added(GObject *src, GAsyncResult *res, gpointer data)
{
    LoadCtx *c = data;
    GFileInfo *info = g_file_query_info_finish(G_FILE(src), res, NULL);
    if (g_cancellable_is_cancelled(c->cancel)) {
        if (info)
            g_object_unref(info);
        load_ctx_free(c);
        return;
    }
    KfPane *p = c->pane;
    if (info) {
        add_info(p, info);
        apply_pending_select(p, g_file_info_get_name(info));
        g_object_unref(info);
        queue_stats(p);
        kf_pane_request_thumbs(p);
    }
    load_ctx_free(c);
}

static void on_info_updated(GObject *src, GAsyncResult *res, gpointer data)
{
    LoadCtx *c = data;
    GFileInfo *info = g_file_query_info_finish(G_FILE(src), res, NULL);
    if (g_cancellable_is_cancelled(c->cancel)) {
        if (info)
            g_object_unref(info);
        load_ctx_free(c);
        return;
    }
    KfPane *p = c->pane;
    if (info) {
        GtkTreeIter it;
        const char *name = g_file_info_get_name(info);
        if (name && row_iter(p, name, &it)) {
            GFile *child = g_file_get_child(p->dir, name);
            fill_row(p, &it, child, info);
            /* The content changed: any thumbnail is stale. */
            gtk_list_store_set(p->store, &it, KF_COL_THSTATE, KF_TH_NONE, -1);
            g_object_unref(child);
            queue_stats(p);
            kf_pane_request_thumbs(p);
        }
        g_object_unref(info);
    }
    load_ctx_free(c);
}

static void query_async(KfPane *p, GFile *file, GAsyncReadyCallback cb)
{
    LoadCtx *c = g_new0(LoadCtx, 1);
    c->pane = p;
    c->cancel = g_object_ref(p->cancel);
    g_file_query_info_async(file, ATTRS, G_FILE_QUERY_INFO_NONE, G_PRIORITY_DEFAULT, c->cancel, cb, c);
}

static void on_monitor(GFileMonitor *m, GFile *file, GFile *other, GFileMonitorEvent ev, gpointer data)
{
    (void)m;
    KfPane *p = data;
    if (p->loading)
        return;     /* the enumeration will see it */
    gboolean in_dir = g_file_has_parent(file, p->dir);
    char *name = g_file_get_basename(file);
    switch (ev) {
    case G_FILE_MONITOR_EVENT_CREATED:
    case G_FILE_MONITOR_EVENT_MOVED_IN:
        if (in_dir)
            query_async(p, file, on_info_added);
        break;
    case G_FILE_MONITOR_EVENT_DELETED:
    case G_FILE_MONITOR_EVENT_MOVED_OUT:
        if (in_dir) {
            remove_row(p, name);
            queue_stats(p);
        }
        break;
    case G_FILE_MONITOR_EVENT_RENAMED:
        if (in_dir)
            remove_row(p, name);
        if (other && g_file_has_parent(other, p->dir))
            query_async(p, other, on_info_added);
        queue_stats(p);
        break;
    case G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT:
    case G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED:
        if (in_dir)
            query_async(p, file, on_info_updated);
        break;
    default:
        break;
    }
    g_free(name);
}

static void load_start(KfPane *p)
{
    if (p->cancel) {
        g_cancellable_cancel(p->cancel);
        g_object_unref(p->cancel);
    }
    p->cancel = g_cancellable_new();
    if (p->monitor) {
        g_file_monitor_cancel(p->monitor);
        g_object_unref(p->monitor);
        p->monitor = NULL;
    }
    show_message(p, GTK_MESSAGE_INFO, NULL);
    p->loading = TRUE;
    kf_pane_apply_sort(p);          /* parks the sort while loading */
    gtk_list_store_clear(p->store);
    g_hash_table_remove_all(p->rows);

    LoadCtx *c = g_new0(LoadCtx, 1);
    c->pane = p;
    c->cancel = g_object_ref(p->cancel);
    g_file_enumerate_children_async(p->dir, ATTRS, G_FILE_QUERY_INFO_NONE, G_PRIORITY_DEFAULT,
                                    c->cancel, on_enumerate, c);

    p->monitor = g_file_monitor_directory(p->dir, G_FILE_MONITOR_WATCH_MOVES, NULL, NULL);
    if (p->monitor)
        g_signal_connect(p->monitor, "changed", G_CALLBACK(on_monitor), p);
    queue_stats(p);
}

/* ---- navigation --------------------------------------------------------- */

static void history_push(GList **list, GFile *f)
{
    if (*list && g_file_equal((*list)->data, f))
        return;
    *list = g_list_prepend(*list, g_object_ref(f));
    if (g_list_length(*list) > 100) {
        GList *last = g_list_last(*list);
        g_object_unref(last->data);
        *list = g_list_delete_link(*list, last);
    }
}

/* Event for the next kf_pane_navigate(): back/forward/up say which one they
 * are, a plain user navigation (crumb, sidebar, double-click) is "folder-open",
 * and startup/split-view navigations (push_history FALSE) are silent. */
static const char *nav_sound;

void kf_pane_navigate(KfPane *p, GFile *dir, gboolean push_history, GFile *select_after)
{
    const char *snd = nav_sound ? nav_sound : (push_history ? "folder-open" : NULL);
    nav_sound = NULL;
    if (snd && !(p->dir && g_file_equal(p->dir, dir)))
        xis_sound_play(snd);
    if (p->dir && g_file_equal(p->dir, dir)) {
        kf_pane_reload(p);
        return;
    }
    if (push_history && p->dir) {
        history_push(&p->back, p->dir);
        kf_files_free(p->fwd);
        p->fwd = NULL;
    }
    g_list_free_full(p->pending_select, g_free);
    p->pending_select = NULL;
    if (select_after)
        p->pending_select = g_list_append(NULL, g_file_get_basename(select_after));
    if (p->dir)
        g_object_unref(p->dir);
    p->dir = g_object_ref(dir);
    kf_pane_show_filter(p, FALSE);
    kf_crumbs_set_location(p->crumbs, dir);
    load_start(p);
    kf_app_pane_changed(p->app, p, KF_CHANGE_DIR);
}

void kf_pane_go_back(KfPane *p)
{
    if (!p->back)
        return;
    GFile *to = p->back->data;
    p->back = g_list_delete_link(p->back, p->back);
    GFile *from = g_object_ref(p->dir);
    history_push(&p->fwd, from);
    nav_sound = "folder-back";
    kf_pane_navigate(p, to, FALSE, from);
    g_object_unref(from);
    g_object_unref(to);
}

void kf_pane_go_forward(KfPane *p)
{
    if (!p->fwd)
        return;
    GFile *to = p->fwd->data;
    p->fwd = g_list_delete_link(p->fwd, p->fwd);
    history_push(&p->back, p->dir);
    nav_sound = "folder-forward";
    kf_pane_navigate(p, to, FALSE, NULL);
    g_object_unref(to);
}

void kf_pane_go_up(KfPane *p)
{
    GFile *par = g_file_get_parent(p->dir);
    if (!par)
        return;
    GFile *from = g_object_ref(p->dir);
    nav_sound = "folder-up";
    kf_pane_navigate(p, par, TRUE, from);
    g_object_unref(from);
    g_object_unref(par);
}

gboolean kf_pane_can_back(KfPane *p) { return p->back != NULL; }
gboolean kf_pane_can_forward(KfPane *p) { return p->fwd != NULL; }

gboolean kf_pane_can_up(KfPane *p)
{
    GFile *par = p->dir ? g_file_get_parent(p->dir) : NULL;
    if (!par)
        return FALSE;
    g_object_unref(par);
    return TRUE;
}

void kf_pane_reload(KfPane *p)
{
    /* Keep the selection across a reload. */
    GList *sel = kf_pane_selected(p);
    g_list_free_full(p->pending_select, g_free);
    p->pending_select = NULL;
    for (GList *l = sel; l; l = l->next)
        p->pending_select = g_list_append(p->pending_select, g_file_get_basename(l->data));
    kf_files_free(sel);
    load_start(p);
}

/* ---- selection ---------------------------------------------------------- */

static GList *selected_paths(KfPane *p)
{
    if (p->view == KF_VIEW_ICONS)
        return gtk_icon_view_get_selected_items(GTK_ICON_VIEW(p->iconview));
    GtkTreeModel *m;
    return gtk_tree_selection_get_selected_rows(gtk_tree_view_get_selection(GTK_TREE_VIEW(p->treeview)), &m);
}

GList *kf_pane_selected(KfPane *p)
{
    GList *paths = selected_paths(p), *out = NULL;
    for (GList *l = paths; l; l = l->next) {
        GtkTreeIter it;
        GFile *f = NULL;
        if (gtk_tree_model_get_iter(p->sort, &it, l->data)) {
            gtk_tree_model_get(p->sort, &it, KF_COL_FILE, &f, -1);
            if (f)
                out = g_list_append(out, f);
        }
    }
    g_list_free_full(paths, (GDestroyNotify)gtk_tree_path_free);
    return out;
}

guint kf_pane_selection_count(KfPane *p)
{
    GList *paths = selected_paths(p);
    guint n = g_list_length(paths);
    g_list_free_full(paths, (GDestroyNotify)gtk_tree_path_free);
    return n;
}

void kf_pane_select_all(KfPane *p)
{
    if (p->view == KF_VIEW_ICONS)
        gtk_icon_view_select_all(GTK_ICON_VIEW(p->iconview));
    else
        gtk_tree_selection_select_all(gtk_tree_view_get_selection(GTK_TREE_VIEW(p->treeview)));
}

void kf_pane_select_none(KfPane *p)
{
    if (p->view == KF_VIEW_ICONS)
        gtk_icon_view_unselect_all(GTK_ICON_VIEW(p->iconview));
    else
        gtk_tree_selection_unselect_all(gtk_tree_view_get_selection(GTK_TREE_VIEW(p->treeview)));
}

void kf_pane_select_invert(KfPane *p)
{
    GtkTreeIter it;
    gboolean ok = gtk_tree_model_get_iter_first(p->sort, &it);
    while (ok) {
        GtkTreePath *path = gtk_tree_model_get_path(p->sort, &it);
        if (p->view == KF_VIEW_ICONS) {
            GtkIconView *iv = GTK_ICON_VIEW(p->iconview);
            if (gtk_icon_view_path_is_selected(iv, path))
                gtk_icon_view_unselect_path(iv, path);
            else
                gtk_icon_view_select_path(iv, path);
        } else {
            GtkTreeSelection *s = gtk_tree_view_get_selection(GTK_TREE_VIEW(p->treeview));
            if (gtk_tree_selection_path_is_selected(s, path))
                gtk_tree_selection_unselect_path(s, path);
            else
                gtk_tree_selection_select_path(s, path);
        }
        gtk_tree_path_free(path);
        ok = gtk_tree_model_iter_next(p->sort, &it);
    }
}

/* Store path -> the path the views use (NULL while it is filtered out). */
static GtkTreePath *view_path_for(KfPane *p, GtkTreePath *store_path)
{
    GtkTreePath *fp = gtk_tree_model_filter_convert_child_path_to_path(GTK_TREE_MODEL_FILTER(p->filter), store_path);
    if (!fp)
        return NULL;
    GtkTreePath *vp = gtk_tree_model_sort_convert_child_path_to_path(GTK_TREE_MODEL_SORT(p->sort), fp);
    gtk_tree_path_free(fp);
    return vp;
}

void kf_pane_select_names(KfPane *p, GList *names)
{
    gboolean first = TRUE;
    for (GList *l = names; l; l = l->next) {
        GtkTreeRowReference *rr = g_hash_table_lookup(p->rows, l->data);
        if (!rr)
            continue;
        GtkTreePath *sp = gtk_tree_row_reference_get_path(rr);
        if (!sp)
            continue;
        GtkTreePath *vp = view_path_for(p, sp);
        gtk_tree_path_free(sp);
        if (!vp)
            continue;
        if (p->view == KF_VIEW_ICONS) {
            GtkIconView *iv = GTK_ICON_VIEW(p->iconview);
            gtk_icon_view_select_path(iv, vp);
            if (first)
                gtk_icon_view_scroll_to_path(iv, vp, FALSE, 0, 0);
        } else {
            GtkTreeView *tv = GTK_TREE_VIEW(p->treeview);
            gtk_tree_selection_select_path(gtk_tree_view_get_selection(tv), vp);
            if (first)
                gtk_tree_view_scroll_to_cell(tv, vp, NULL, FALSE, 0, 0);
        }
        first = FALSE;
        gtk_tree_path_free(vp);
    }
}

void kf_pane_select_when_added(KfPane *p, const char *name)
{
    GList *one = g_list_append(NULL, (gpointer)name);
    if (g_hash_table_contains(p->rows, name))
        kf_pane_select_names(p, one);
    else
        p->pending_select = g_list_append(p->pending_select, g_strdup(name));
    g_list_free(one);
}

/* In split view the active pane gets a strip in the selection color. */
void kf_pane_set_active(KfPane *p, gboolean active, gboolean show_marker)
{
    if (!show_marker) {
        gtk_widget_hide(p->active_bar);
        return;
    }
    gtk_widget_show(p->active_bar);
    GtkStyle *st = gtk_widget_get_style(p->box);
    gtk_widget_modify_bg(p->active_bar, GTK_STATE_NORMAL, active ? &st->bg[GTK_STATE_SELECTED] : NULL);
}

void kf_pane_focus_view(KfPane *p)
{
    gtk_widget_grab_focus(p->view == KF_VIEW_ICONS ? p->iconview : p->treeview);
}

gboolean kf_pane_file_info(KfPane *p, GFile *f, gboolean *is_dir, char **ctype)
{
    char *name = g_file_get_basename(f);
    GtkTreeIter it;
    gboolean ok = row_iter(p, name, &it) != NULL;
    g_free(name);
    if (!ok)
        return FALSE;
    gboolean d = FALSE;
    char *ct = NULL;
    gtk_tree_model_get(GTK_TREE_MODEL(p->store), &it, KF_COL_ISDIR, &d, KF_COL_CTYPE, &ct, -1);
    if (is_dir)
        *is_dir = d;
    if (ctype)
        *ctype = ct;
    else
        g_free(ct);
    return TRUE;
}

void kf_pane_stats(KfPane *p, gboolean selection_only, guint *folders, guint *files, guint64 *bytes)
{
    *folders = *files = 0;
    *bytes = 0;
    if (selection_only) {
        GList *paths = selected_paths(p);
        for (GList *l = paths; l; l = l->next) {
            GtkTreeIter it;
            if (!gtk_tree_model_get_iter(p->sort, &it, l->data))
                continue;
            gboolean d;
            guint64 s;
            gtk_tree_model_get(p->sort, &it, KF_COL_ISDIR, &d, KF_COL_SIZE, &s, -1);
            if (d)
                (*folders)++;
            else {
                (*files)++;
                *bytes += s;
            }
        }
        g_list_free_full(paths, (GDestroyNotify)gtk_tree_path_free);
        return;
    }
    GtkTreeIter it;
    gboolean ok = gtk_tree_model_get_iter_first(p->filter, &it);
    while (ok) {
        gboolean d;
        guint64 s;
        gtk_tree_model_get(p->filter, &it, KF_COL_ISDIR, &d, KF_COL_SIZE, &s, -1);
        if (d)
            (*folders)++;
        else {
            (*files)++;
            *bytes += s;
        }
        ok = gtk_tree_model_iter_next(p->filter, &it);
    }
}

static gboolean stats_idle_cb(gpointer data)
{
    KfPane *p = data;
    p->stats_idle = 0;
    kf_app_pane_changed(p->app, p, KF_CHANGE_CONTENT);
    return FALSE;
}

static void queue_stats(KfPane *p)
{
    if (!p->stats_idle)
        p->stats_idle = g_timeout_add(150, stats_idle_cb, p);
}

/* ---- thumbnails --------------------------------------------------------- */

#define THUMB_KEEP 200      /* rows past the requested range that keep their thumbnail */

static void on_thumb(GFile *file, GdkPixbuf *pb, gpointer data)
{
    KfPane *p = data;
    char *name = g_file_get_basename(file);
    GtkTreeIter it;
    if (row_iter(p, name, &it)) {
        if (pb) {
            GdkPixbuf *fit = kf_icon_fit(pb, cur_icon_px(p));
            gtk_list_store_set(p->store, &it, KF_COL_ICON, fit, KF_COL_THSTATE, KF_TH_SHOWN, -1);
            g_object_unref(fit);
        } else {
            gtk_list_store_set(p->store, &it, KF_COL_THSTATE, KF_TH_NOTHUMB, -1);
        }
    }
    g_free(name);
}

static gboolean thumbs_idle_cb(gpointer data)
{
    KfPane *p = data;
    p->thumbs_idle = 0;
    if (!p->app->cfg.thumbnails || p->loading || !p->cancel)
        return FALSE;

    /* Only rows near the visible part: scrolling requests more. */
    GtkTreePath *start = NULL, *end = NULL;
    gboolean have;
    if (p->view == KF_VIEW_ICONS)
        have = gtk_icon_view_get_visible_range(GTK_ICON_VIEW(p->iconview), &start, &end);
    else
        have = gtk_tree_view_get_visible_range(GTK_TREE_VIEW(p->treeview), &start, &end);
    if (!have || !start || !end) {
        if (start) gtk_tree_path_free(start);
        if (end) gtk_tree_path_free(end);
        return FALSE;
    }
    int i0 = gtk_tree_path_get_indices(start)[0], i1 = gtk_tree_path_get_indices(end)[0];
    int margin = 40;
    i0 = i0 > margin ? i0 - margin : 0;
    i1 += margin;
    gtk_tree_path_free(start);
    gtk_tree_path_free(end);

    int px = p->app->cfg.zoom <= 128 ? 128 : 256;

    /* Thumbnails of rows far from the view go back to the theme icon, so a
     * long folder scrolled to the end doesn't keep one image per file. */
    int keep0 = i0 > THUMB_KEEP ? i0 - THUMB_KEEP : 0, keep1 = i1 + THUMB_KEEP;

    /* Collect first, mark/request after: changing the store while iterating
     * the sort model would invalidate the iterators. */
    typedef struct { GFile *f; char *ct; gint64 mt; } Want;
    GList *wants = NULL, *evict = NULL;
    GtkTreeIter it;
    int idx = 0;
    gboolean ok = gtk_tree_model_get_iter_first(p->sort, &it);
    while (ok) {
        int st = 0;
        gboolean d = FALSE;
        gtk_tree_model_get(p->sort, &it, KF_COL_THSTATE, &st, KF_COL_ISDIR, &d, -1);
        if (idx >= i0 && idx <= i1 && st == KF_TH_NONE && !d) {
            Want *w = g_new0(Want, 1);
            gtk_tree_model_get(p->sort, &it, KF_COL_CTYPE, &w->ct, KF_COL_FILE, &w->f, KF_COL_MTIME, &w->mt, -1);
            wants = g_list_prepend(wants, w);
        } else if ((idx < keep0 || idx > keep1) && st == KF_TH_SHOWN) {
            GFile *f = NULL;
            gtk_tree_model_get(p->sort, &it, KF_COL_FILE, &f, -1);
            if (f)
                evict = g_list_prepend(evict, f);
        }
        idx++;
        ok = gtk_tree_model_iter_next(p->sort, &it);
    }
    for (GList *l = evict; l; l = l->next) {
        char *name = g_file_get_basename(l->data);
        GtkTreeIter row;
        if (row_iter(p, name, &row)) {
            char *iconstr = NULL;
            gtk_tree_model_get(GTK_TREE_MODEL(p->store), &row, KF_COL_ICONSTR, &iconstr, -1);
            gtk_list_store_set(p->store, &row, KF_COL_ICON, kf_icon_lookup(iconstr, cur_icon_px(p)),
                               KF_COL_THSTATE, KF_TH_NONE, -1);
            g_free(iconstr);
        }
        g_free(name);
    }
    g_list_free_full(evict, g_object_unref);
    for (GList *l = g_list_reverse(wants); l; l = l->next) {
        Want *w = l->data;
        char *name = w->f ? g_file_get_basename(w->f) : NULL;
        GtkTreeIter row;
        if (name && row_iter(p, name, &row)) {
            if (w->ct && kf_thumbs_supported(w->ct)) {
                gtk_list_store_set(p->store, &row, KF_COL_THSTATE, KF_TH_PENDING, -1);
                kf_thumbs_request(w->f, w->ct, w->mt, px, p->cancel, on_thumb, p);
            } else {
                gtk_list_store_set(p->store, &row, KF_COL_THSTATE, KF_TH_NOTHUMB, -1);
            }
        }
        g_free(name);
        g_free(w->ct);
        if (w->f)
            g_object_unref(w->f);
        g_free(w);
    }
    g_list_free(wants);
    return FALSE;
}

void kf_pane_request_thumbs(KfPane *p)
{
    if (!p->thumbs_idle)
        p->thumbs_idle = g_timeout_add(80, thumbs_idle_cb, p);
}

static void on_scrolled(GtkAdjustment *adj, gpointer data)
{
    (void)adj;
    kf_pane_request_thumbs(data);
    kf_hover_refresh(data);
}

/* ---- zoom / view mode ---------------------------------------------------- */

void kf_pane_apply_zoom(KfPane *p)
{
    int z = p->app->cfg.zoom;
    int px = cur_icon_px(p);

    int item_w = z + 40;
    gtk_cell_renderer_set_fixed_size(p->icon_pix_r, item_w - 8, z);   /* as wide as the text, icon centered in it */
    gtk_cell_renderer_set_fixed_size(p->list_pix_r, list_icon_px(p), list_icon_px(p));
    /* A fixed text width makes every item the same width: without it, names that
     * wrap are wider than names that don't, and the grid loses its columns. */
    g_object_set(p->icon_text_r, "wrap-width", item_w - 8, NULL);
    gtk_cell_renderer_set_fixed_size(p->icon_text_r, item_w - 8, -1);
    gtk_icon_view_set_item_width(GTK_ICON_VIEW(p->iconview), -1);

    /* A shown thumbnail keeps its current picture (shrunk if it is now too
     * big) until the request below brings one at the new size. */
    GtkTreeIter it;
    gboolean ok = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(p->store), &it);
    while (ok) {
        GdkPixbuf *icon = NULL;
        char *iconstr = NULL;
        int st = 0;
        gtk_tree_model_get(GTK_TREE_MODEL(p->store), &it, KF_COL_ICON, &icon, KF_COL_ICONSTR, &iconstr,
                           KF_COL_THSTATE, &st, -1);
        if (st == KF_TH_SHOWN && icon) {
            GdkPixbuf *pb = kf_icon_fit(icon, px);
            gtk_list_store_set(p->store, &it, KF_COL_ICON, pb, KF_COL_THSTATE, KF_TH_NONE, -1);
            g_object_unref(pb);
        } else {
            gtk_list_store_set(p->store, &it, KF_COL_ICON, kf_icon_lookup(iconstr, px), -1);
            if (st == KF_TH_PENDING)
                gtk_list_store_set(p->store, &it, KF_COL_THSTATE, KF_TH_NONE, -1);
        }
        if (icon)
            g_object_unref(icon);
        g_free(iconstr);
        ok = gtk_tree_model_iter_next(GTK_TREE_MODEL(p->store), &it);
    }
    gtk_widget_queue_resize(p->iconview);
    gtk_widget_queue_resize(p->treeview);
    kf_pane_request_thumbs(p);
    kf_hover_clear(p);              /* item sizes changed; the next motion recomputes */
}

void kf_pane_set_view(KfPane *p, KfViewMode m)
{
    p->view = m;
    kf_hover_clear(p);
    if (m == KF_VIEW_LIST) {
        gtk_widget_hide(p->sw_icons);
        gtk_widget_show(p->sw_list);
    } else {
        gtk_widget_hide(p->sw_list);
        gtk_widget_show(p->sw_icons);
    }
    kf_pane_apply_zoom(p);
}

/* ---- view events -------------------------------------------------------- */

static gboolean sel_idle_cb(gpointer data)
{
    KfPane *p = data;
    p->sel_idle = 0;
    kf_app_pane_changed(p->app, p, KF_CHANGE_SELECTION);
    return FALSE;
}

/* Deferred: removing a selected row emits this while the filter and sort
 * models are still catching up, and reading the selection then crashes. */
static void on_selection(gpointer a, gpointer data)
{
    (void)a;
    KfPane *p = data;
    if (!p->sel_idle)
        p->sel_idle = g_idle_add(sel_idle_cb, p);
}

static void open_path(KfPane *p, GtkTreePath *path)
{
    GtkTreeIter it;
    GFile *f = NULL;
    if (gtk_tree_model_get_iter(p->sort, &it, path)) {
        gtk_tree_model_get(p->sort, &it, KF_COL_FILE, &f, -1);
        if (f) {
            kf_app_open(p->app, p, f);
            g_object_unref(f);
        }
    }
}

static void on_item_activated(GtkIconView *iv, GtkTreePath *path, gpointer data)
{
    (void)iv;
    open_path(data, path);
}

static void on_row_activated(GtkTreeView *tv, GtkTreePath *path, GtkTreeViewColumn *col, gpointer data)
{
    (void)tv; (void)col;
    open_path(data, path);
}

static GtkTreePath *path_at(KfPane *p, GdkEventButton *ev)
{
    if (p->view == KF_VIEW_ICONS)
        return gtk_icon_view_get_path_at_pos(GTK_ICON_VIEW(p->iconview), (gint)ev->x, (gint)ev->y);
    GtkTreePath *path = NULL;
    if (ev->window == gtk_tree_view_get_bin_window(GTK_TREE_VIEW(p->treeview)))
        gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(p->treeview), (gint)ev->x, (gint)ev->y, &path, NULL, NULL, NULL);
    return path;
}

static gboolean path_selected(KfPane *p, GtkTreePath *path)
{
    if (p->view == KF_VIEW_ICONS)
        return gtk_icon_view_path_is_selected(GTK_ICON_VIEW(p->iconview), path);
    return gtk_tree_selection_path_is_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(p->treeview)), path);
}

static gboolean on_view_button(GtkWidget *w, GdkEventButton *ev, gpointer data)
{
    (void)w;
    KfPane *p = data;
    kf_app_pane_changed(p->app, p, KF_CHANGE_FOCUS);
    if (ev->type != GDK_BUTTON_PRESS)
        return FALSE;
    if (ev->button == 3) {
        GtkTreePath *path = path_at(p, ev);
        if (path) {
            if (!path_selected(p, path)) {
                kf_pane_select_none(p);
                if (p->view == KF_VIEW_ICONS)
                    gtk_icon_view_select_path(GTK_ICON_VIEW(p->iconview), path);
                else
                    gtk_tree_selection_select_path(gtk_tree_view_get_selection(GTK_TREE_VIEW(p->treeview)), path);
            }
            gtk_tree_path_free(path);
        } else {
            kf_pane_select_none(p);
        }
        kf_app_popup_context(p->app, p, ev, path != NULL);
        return TRUE;
    }
    if (ev->button == 2) {      /* middle click on a folder: open it in a new tab */
        GtkTreePath *path = path_at(p, ev);
        GtkTreeIter it;
        GFile *f = NULL;
        gboolean is_dir = FALSE;
        if (path && gtk_tree_model_get_iter(p->sort, &it, path))
            gtk_tree_model_get(p->sort, &it, KF_COL_FILE, &f, KF_COL_ISDIR, &is_dir, -1);
        if (f && is_dir)
            kf_tabs_open(p->app, f, FALSE);
        if (f)
            g_object_unref(f);
        if (path)
            gtk_tree_path_free(path);
        return f != NULL;
    }
    if (ev->button == 8) { kf_pane_go_back(p); return TRUE; }
    if (ev->button == 9) { kf_pane_go_forward(p); return TRUE; }
    return FALSE;
}

/* Ctrl+wheel zooms (icon size), like most file managers and browsers; a plain
 * wheel keeps scrolling. */
static gboolean on_view_scroll(GtkWidget *w, GdkEventScroll *ev, gpointer data)
{
    (void)w;
    KfPane *p = data;
    if (!(ev->state & GDK_CONTROL_MASK))
        return FALSE;
    if (ev->direction == GDK_SCROLL_UP)
        kf_app_zoom_step(p->app, +1);
    else if (ev->direction == GDK_SCROLL_DOWN)
        kf_app_zoom_step(p->app, -1);
    return TRUE;
}

static gboolean on_view_focus(GtkWidget *w, GdkEventFocus *ev, gpointer data)
{
    (void)w; (void)ev;
    KfPane *p = data;
    kf_app_pane_changed(p->app, p, KF_CHANGE_FOCUS);
    return FALSE;
}

static void on_col_clicked(GtkTreeViewColumn *col, gpointer data)
{
    KfPane *p = data;
    kf_app_sort_by(p->app, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(col), "kf-sort")));
}

static void size_cell(GtkTreeViewColumn *col, GtkCellRenderer *r, GtkTreeModel *m, GtkTreeIter *it, gpointer d)
{
    (void)col; (void)d;
    gboolean isdir;
    guint64 s;
    gtk_tree_model_get(m, it, KF_COL_ISDIR, &isdir, KF_COL_SIZE, &s, -1);
    if (isdir) {
        g_object_set(r, "text", "", NULL);
        return;
    }
    char *t = g_format_size_full(s, G_FORMAT_SIZE_IEC_UNITS);
    g_object_set(r, "text", t, NULL);
    g_free(t);
}

static void mtime_cell(GtkTreeViewColumn *col, GtkCellRenderer *r, GtkTreeModel *m, GtkTreeIter *it, gpointer d)
{
    (void)col; (void)d;
    gint64 t;
    gtk_tree_model_get(m, it, KF_COL_MTIME, &t, -1);
    char *txt = NULL;
    if (t > 0) {
        GDateTime *dt = g_date_time_new_from_unix_local(t);
        if (dt) {
            txt = g_date_time_format(dt, "%Y-%m-%d %H:%M");
            g_date_time_unref(dt);
        }
    }
    g_object_set(r, "text", txt ? txt : "", NULL);
    g_free(txt);
}

static GtkTreeViewColumn *add_text_col(KfPane *p, const char *title, int sort_key, int model_col,
                                       GtkTreeCellDataFunc fn, int width, gboolean right)
{
    GtkCellRenderer *r = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *col = gtk_tree_view_column_new();
    gtk_tree_view_column_set_title(col, title);
    gtk_tree_view_column_pack_start(col, r, TRUE);
    if (fn)
        gtk_tree_view_column_set_cell_data_func(col, r, fn, NULL, NULL);
    else
        gtk_tree_view_column_add_attribute(col, r, "text", model_col);
    if (right)
        g_object_set(r, "xalign", 1.0, NULL);
    gtk_tree_view_column_set_resizable(col, TRUE);
    gtk_tree_view_column_set_sizing(col, GTK_TREE_VIEW_COLUMN_FIXED);
    gtk_tree_view_column_set_fixed_width(col, width);
    gtk_tree_view_column_set_clickable(col, TRUE);
    g_object_set_data(G_OBJECT(col), "kf-sort", GINT_TO_POINTER(sort_key));
    g_signal_connect(col, "clicked", G_CALLBACK(on_col_clicked), p);
    gtk_tree_view_append_column(GTK_TREE_VIEW(p->treeview), col);
    return col;
}

static void on_msg_response(GtkInfoBar *bar, gint resp, gpointer data)
{
    (void)resp;
    (void)data;
    gtk_widget_hide(GTK_WIDGET(bar));
}

static void on_filter_changed(GtkEditable *e, gpointer data)
{
    KfPane *p = data;
    g_free(p->filter_text);
    p->filter_text = g_strdup(gtk_entry_get_text(GTK_ENTRY(e)));
    kf_pane_apply_filter(p);
}

static gboolean on_filter_key(GtkWidget *w, GdkEventKey *ev, gpointer data)
{
    (void)w;
    if (ev->keyval == GDK_KEY_Escape) {
        kf_pane_show_filter(data, FALSE);
        return TRUE;
    }
    return FALSE;
}

static void on_filter_close(GtkButton *b, gpointer data)
{
    (void)b;
    kf_pane_show_filter(data, FALSE);
}

void kf_pane_show_filter(KfPane *p, gboolean show)
{
    if (show) {
        gtk_widget_show(p->filter_bar);
        gtk_widget_grab_focus(p->filter_entry);
        return;
    }
    gboolean had = gtk_widget_get_visible(p->filter_bar);
    gtk_widget_hide(p->filter_bar);
    if (p->filter_text && *p->filter_text) {
        g_free(p->filter_text);
        p->filter_text = NULL;
        gtk_entry_set_text(GTK_ENTRY(p->filter_entry), "");
        kf_pane_apply_filter(p);
    }
    if (had)
        kf_pane_focus_view(p);
}

KfPane *kf_pane_new(KfApp *app)
{
    KfPane *p = g_new0(KfPane, 1);
    p->app = app;
    p->rows = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)gtk_tree_row_reference_free);
    p->typedescs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    p->store = gtk_list_store_new(KF_N_COLS, G_TYPE_OBJECT, G_TYPE_STRING, GDK_TYPE_PIXBUF,
                                  G_TYPE_UINT64, G_TYPE_INT64, G_TYPE_STRING, G_TYPE_BOOLEAN, G_TYPE_BOOLEAN,
                                  G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT);
    p->filter = gtk_tree_model_filter_new(GTK_TREE_MODEL(p->store), NULL);
    gtk_tree_model_filter_set_visible_func(GTK_TREE_MODEL_FILTER(p->filter), visible_cb, p, NULL);
    p->sort = gtk_tree_model_sort_new_with_model(p->filter);
    gtk_tree_sortable_set_default_sort_func(GTK_TREE_SORTABLE(p->sort), unsorted_cb, NULL, NULL);

    p->box = gtk_vbox_new(FALSE, 0);
    p->crumbs = kf_crumbs_new(p);
    GtkWidget *cb = gtk_hbox_new(FALSE, 0);
    gtk_container_set_border_width(GTK_CONTAINER(cb), 2);
    gtk_box_pack_start(GTK_BOX(cb), p->crumbs, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(p->box), cb, FALSE, FALSE, 0);
    p->active_bar = gtk_event_box_new();
    gtk_widget_set_size_request(p->active_bar, -1, 3);
    gtk_widget_set_no_show_all(p->active_bar, TRUE);
    gtk_box_pack_start(GTK_BOX(p->box), p->active_bar, FALSE, FALSE, 0);

    /* message bar (errors, "empty folder") */
    p->msg_bar = gtk_info_bar_new();
    gtk_info_bar_add_button(GTK_INFO_BAR(p->msg_bar), GTK_STOCK_CLOSE, GTK_RESPONSE_CLOSE);
    p->msg_label = gtk_label_new("");
    gtk_label_set_line_wrap(GTK_LABEL(p->msg_label), TRUE);
    gtk_label_set_selectable(GTK_LABEL(p->msg_label), TRUE);
    gtk_container_add(GTK_CONTAINER(gtk_info_bar_get_content_area(GTK_INFO_BAR(p->msg_bar))), p->msg_label);
    gtk_widget_show(p->msg_label);
    gtk_widget_set_no_show_all(p->msg_bar, TRUE);
    g_signal_connect(p->msg_bar, "response", G_CALLBACK(on_msg_response), NULL);
    gtk_box_pack_start(GTK_BOX(p->box), p->msg_bar, FALSE, FALSE, 0);

    /* icon view */
    p->iconview = gtk_icon_view_new_with_model(p->sort);
    GtkIconView *iv = GTK_ICON_VIEW(p->iconview);
    /* The icon view requests the width of its last layout, which would make the
     * window unable to shrink once it has been wide; it wraps to whatever
     * width it is given, so ask for next to nothing. */
    gtk_widget_set_size_request(p->iconview, 1, -1);
    gtk_icon_view_set_selection_mode(iv, GTK_SELECTION_MULTIPLE);
    gtk_icon_view_set_spacing(iv, 2);
    gtk_icon_view_set_row_spacing(iv, 4);
    gtk_icon_view_set_column_spacing(iv, 2);
    gtk_icon_view_set_margin(iv, 6);
    p->icon_pix_r = gtk_cell_renderer_pixbuf_new();
    g_object_set(p->icon_pix_r, "xalign", 0.5, "yalign", 0.5, NULL);
    gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(iv), p->icon_pix_r, FALSE);
    gtk_cell_layout_add_attribute(GTK_CELL_LAYOUT(iv), p->icon_pix_r, "pixbuf", KF_COL_ICON);
    p->icon_text_r = gtk_cell_renderer_text_new();
    /* Centered by Pango inside the fixed wrap width; xalign 0.5 on top of that
     * would shift it by the same amount again. */
    g_object_set(p->icon_text_r, "xalign", 0.0, "alignment", PANGO_ALIGN_CENTER, "wrap-mode", PANGO_WRAP_WORD_CHAR, NULL);
    gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(iv), p->icon_text_r, FALSE);
    gtk_cell_layout_add_attribute(GTK_CELL_LAYOUT(iv), p->icon_text_r, "text", KF_COL_NAME);
    g_signal_connect(iv, "item-activated", G_CALLBACK(on_item_activated), p);
    g_signal_connect(iv, "selection-changed", G_CALLBACK(on_selection), p);
    g_signal_connect(iv, "button-press-event", G_CALLBACK(on_view_button), p);
    g_signal_connect(iv, "focus-in-event", G_CALLBACK(on_view_focus), p);
    g_signal_connect(iv, "scroll-event", G_CALLBACK(on_view_scroll), p);
    p->sw_icons = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(p->sw_icons), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(p->sw_icons), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(p->sw_icons), p->iconview);
    g_signal_connect(gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(p->sw_icons)), "value-changed",
                     G_CALLBACK(on_scrolled), p);

    /* list view */
    p->treeview = gtk_tree_view_new_with_model(p->sort);
    GtkTreeView *tv = GTK_TREE_VIEW(p->treeview);
    gtk_tree_view_set_rubber_banding(tv, TRUE);
    gtk_tree_view_set_enable_search(tv, TRUE);
    gtk_tree_view_set_search_column(tv, KF_COL_NAME);
    gtk_tree_selection_set_mode(gtk_tree_view_get_selection(tv), GTK_SELECTION_MULTIPLE);
    GtkTreeViewColumn *ncol = gtk_tree_view_column_new();
    gtk_tree_view_column_set_title(ncol, _("Name"));
    p->list_pix_r = gtk_cell_renderer_pixbuf_new();
    gtk_tree_view_column_pack_start(ncol, p->list_pix_r, FALSE);
    gtk_tree_view_column_add_attribute(ncol, p->list_pix_r, "pixbuf", KF_COL_ICON);
    GtkCellRenderer *tr = gtk_cell_renderer_text_new();
    g_object_set(tr, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
    gtk_tree_view_column_pack_start(ncol, tr, TRUE);
    gtk_tree_view_column_add_attribute(ncol, tr, "text", KF_COL_NAME);
    gtk_tree_view_column_set_expand(ncol, TRUE);
    gtk_tree_view_column_set_resizable(ncol, TRUE);
    gtk_tree_view_column_set_min_width(ncol, 120);
    gtk_tree_view_column_set_clickable(ncol, TRUE);
    g_object_set_data(G_OBJECT(ncol), "kf-sort", GINT_TO_POINTER(KF_SORT_NAME));
    g_signal_connect(ncol, "clicked", G_CALLBACK(on_col_clicked), p);
    gtk_tree_view_append_column(tv, ncol);
    add_text_col(p, _("Size"), KF_SORT_SIZE, 0, size_cell, 90, TRUE);
    add_text_col(p, _("Modified"), KF_SORT_MTIME, 0, mtime_cell, 140, FALSE);
    add_text_col(p, _("Type"), KF_SORT_TYPE, KF_COL_TYPEDESC, NULL, 150, FALSE);
    g_signal_connect(tv, "row-activated", G_CALLBACK(on_row_activated), p);
    g_signal_connect(gtk_tree_view_get_selection(tv), "changed", G_CALLBACK(on_selection), p);
    g_signal_connect(tv, "button-press-event", G_CALLBACK(on_view_button), p);
    g_signal_connect(tv, "focus-in-event", G_CALLBACK(on_view_focus), p);
    g_signal_connect(tv, "scroll-event", G_CALLBACK(on_view_scroll), p);
    p->sw_list = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(p->sw_list), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(p->sw_list), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(p->sw_list), p->treeview);
    g_signal_connect(gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(p->sw_list)), "value-changed",
                     G_CALLBACK(on_scrolled), p);

    /* The scrolled windows are shown/hidden by kf_pane_set_view(); show_all
     * must not touch them, so their children are shown here by hand. */
    gtk_widget_show(p->iconview);
    gtk_widget_show(p->treeview);
    gtk_widget_set_no_show_all(p->sw_icons, TRUE);
    gtk_widget_set_no_show_all(p->sw_list, TRUE);
    gtk_box_pack_start(GTK_BOX(p->box), p->sw_icons, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(p->box), p->sw_list, TRUE, TRUE, 0);

    /* quick filter bar */
    p->filter_bar = gtk_hbox_new(FALSE, 4);
    gtk_container_set_border_width(GTK_CONTAINER(p->filter_bar), 3);
    GtkWidget *close = gtk_button_new();
    gtk_button_set_relief(GTK_BUTTON(close), GTK_RELIEF_NONE);
    gtk_container_add(GTK_CONTAINER(close), gtk_image_new_from_stock(GTK_STOCK_CLOSE, GTK_ICON_SIZE_MENU));
    g_signal_connect(close, "clicked", G_CALLBACK(on_filter_close), p);
    gtk_box_pack_start(GTK_BOX(p->filter_bar), close, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(p->filter_bar), gtk_label_new(_("Filter:")), FALSE, FALSE, 0);
    p->filter_entry = gtk_entry_new();
    g_signal_connect(p->filter_entry, "changed", G_CALLBACK(on_filter_changed), p);
    g_signal_connect(p->filter_entry, "key-press-event", G_CALLBACK(on_filter_key), p);
    gtk_box_pack_start(GTK_BOX(p->filter_bar), p->filter_entry, TRUE, TRUE, 0);
    gtk_widget_show_all(p->filter_bar);
    gtk_widget_set_no_show_all(p->filter_bar, TRUE);
    gtk_widget_hide(p->filter_bar);
    gtk_box_pack_end(GTK_BOX(p->box), p->filter_bar, FALSE, FALSE, 0);

    gtk_widget_show_all(p->box);
    gtk_widget_hide(p->msg_bar);
    kf_dnd_setup_pane(p);
    kf_hover_setup(p);
    return p;
}

void kf_pane_destroy(KfPane *p)
{
    if (p->cancel) {
        g_cancellable_cancel(p->cancel);
        g_object_unref(p->cancel);
    }
    if (p->monitor) {
        g_file_monitor_cancel(p->monitor);
        g_object_unref(p->monitor);
    }
    if (p->thumbs_idle)
        g_source_remove(p->thumbs_idle);
    if (p->stats_idle)
        g_source_remove(p->stats_idle);
    kf_files_free(p->back);
    kf_files_free(p->fwd);
    g_list_free_full(p->pending_select, g_free);
    g_free(p->filter_text);
    if (p->dir)
        g_object_unref(p->dir);
    gtk_widget_destroy(p->box);
    if (p->sel_idle)                    /* destroying the views can still queue it */
        g_source_remove(p->sel_idle);
    g_hash_table_destroy(p->rows);
    g_hash_table_destroy(p->typedescs);
    if (p->hl_rows)
        g_array_free(p->hl_rows, TRUE);
    g_object_unref(p->sort);
    g_object_unref(p->filter);
    g_object_unref(p->store);
    g_free(p);
}
