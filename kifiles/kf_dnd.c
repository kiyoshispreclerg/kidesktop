/* kf_dnd.c - drag and drop for the file views.
 *
 * Sources: the selected files are offered as text/uri-list (and plain text),
 * which is what other file managers, the desktop and most apps accept.
 * Destinations: dropping on a folder item puts the files in that folder,
 * dropping on empty space puts them in the pane's folder. The same drops work
 * from other applications. Our own drags remember where they came from so the
 * default action can be "move on the same file system, copy across". */
#include "kf.h"
#include "../shared/xis_i18n.h"
#include <string.h>

enum { T_URIS, T_TEXT };

static const GtkTargetEntry kSrcTargets[] = {
    { (gchar *)"text/uri-list", 0, T_URIS },
    { (gchar *)"UTF8_STRING", 0, T_TEXT },
};
static const GtkTargetEntry kDstTargets[] = {
    { (gchar *)"text/uri-list", 0, T_URIS },
};
#define ACTIONS (GDK_ACTION_COPY | GDK_ACTION_MOVE | GDK_ACTION_LINK)

/* One drag at a time: where our own drag started. */
static GList *drag_files;                   /* GFile*, NULL when the drag is foreign */

/* ---- helpers ------------------------------------------------------------- */

static gboolean same_filesystem(GFile *a, GFile *b)
{
    GFileInfo *ia = g_file_query_info(a, "id::filesystem", G_FILE_QUERY_INFO_NONE, NULL, NULL);
    GFileInfo *ib = g_file_query_info(b, "id::filesystem", G_FILE_QUERY_INFO_NONE, NULL, NULL);
    gboolean same = FALSE;
    if (ia && ib) {
        const char *x = g_file_info_get_attribute_string(ia, "id::filesystem");
        const char *y = g_file_info_get_attribute_string(ib, "id::filesystem");
        same = x && y && !strcmp(x, y);
    }
    if (ia) g_object_unref(ia);
    if (ib) g_object_unref(ib);
    return same;
}

GdkDragAction kf_dnd_default_action(KfApp *app, GFile *dest, GdkDragContext *ctx)
{
    (void)app;
    GdkDragAction sug = gdk_drag_context_get_suggested_action(ctx);
    GdkDragAction avail = gdk_drag_context_get_actions(ctx);
    /* Modifiers made an explicit choice: keep it. */
    GdkModifierType mods = 0;
    gdk_window_get_pointer(NULL, NULL, NULL, &mods);
    if (mods & (GDK_CONTROL_MASK | GDK_SHIFT_MASK | GDK_MOD1_MASK))
        return sug;
    if (drag_files && (avail & GDK_ACTION_MOVE) && (avail & GDK_ACTION_COPY)) {
        /* Cross-device drags copy; anything else moves. */
        return same_filesystem(drag_files->data, dest) ? GDK_ACTION_MOVE : GDK_ACTION_COPY;
    }
    return sug;
}

gboolean kf_dnd_drop_uris(KfApp *app, GtkSelectionData *sel, GdkDragContext *ctx, GFile *dest)
{
    char **uris = gtk_selection_data_get_uris(sel);
    if (!uris)
        return FALSE;
    GList *files = NULL;
    for (char **u = uris; *u; u++)
        files = g_list_append(files, g_file_new_for_uri(*u));
    g_strfreev(uris);
    gboolean move = gdk_drag_context_get_selected_action(ctx) == GDK_ACTION_MOVE;
    kf_ops_transfer(app, files, dest, move);
    kf_files_free(files);
    return TRUE;
}

static gboolean path_selected(KfPane *p, GtkTreePath *path)
{
    if (p->view == KF_VIEW_ICONS)
        return gtk_icon_view_path_is_selected(GTK_ICON_VIEW(p->iconview), path);
    return gtk_tree_selection_path_is_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(p->treeview)), path);
}

/* The folder a drop at widget coordinates (wx, wy) would land in. *hover is
 * the item to highlight, or NULL when it is the pane's own folder. */
static GFile *drop_target(KfPane *p, gint wx, gint wy, GtkTreePath **hover)
{
    GtkTreePath *path = NULL;
    gint bx, by;
    if (p->view == KF_VIEW_ICONS) {
        GtkIconView *iv = GTK_ICON_VIEW(p->iconview);
        gtk_icon_view_convert_widget_to_bin_window_coords(iv, wx, wy, &bx, &by);
        path = gtk_icon_view_get_path_at_pos(iv, bx, by);
    } else {
        GtkTreeView *tv = GTK_TREE_VIEW(p->treeview);
        gtk_tree_view_convert_widget_to_bin_window_coords(tv, wx, wy, &bx, &by);
        gtk_tree_view_get_path_at_pos(tv, bx, by, &path, NULL, NULL, NULL);
    }
    *hover = NULL;
    if (path) {
        GtkTreeIter it;
        gboolean isdir = FALSE;
        GFile *f = NULL;
        if (gtk_tree_model_get_iter(p->sort, &it, path)) {
            gtk_tree_model_get(p->sort, &it, KF_COL_ISDIR, &isdir, KF_COL_FILE, &f, -1);
            /* Not onto an item that is itself being dragged. */
            if (isdir && f && !path_selected(p, path)) {
                *hover = path;
                return f;
            }
        }
        if (f)
            g_object_unref(f);
        gtk_tree_path_free(path);
    }
    return p->dir ? g_object_ref(p->dir) : NULL;
}

static void set_highlight(KfPane *p, GtkTreePath *path)
{
    if (p->view == KF_VIEW_ICONS)
        gtk_icon_view_set_drag_dest_item(GTK_ICON_VIEW(p->iconview), path, GTK_ICON_VIEW_DROP_INTO);
    else
        gtk_tree_view_set_drag_dest_row(GTK_TREE_VIEW(p->treeview), path, GTK_TREE_VIEW_DROP_INTO_OR_BEFORE);
}

/* ---- autoscroll ------------------------------------------------------------ */

static KfPane *scroll_pane;
static int scroll_dir;                  /* -1 up, 0 none, +1 down */
static guint scroll_timer;

static gboolean scroll_tick(gpointer data)
{
    (void)data;
    if (!scroll_pane || !scroll_dir) {
        scroll_timer = 0;
        return FALSE;
    }
    GtkWidget *sw = scroll_pane->view == KF_VIEW_ICONS ? scroll_pane->sw_icons : scroll_pane->sw_list;
    GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(sw));
    double v = gtk_adjustment_get_value(adj) + scroll_dir * 24;
    double max = gtk_adjustment_get_upper(adj) - gtk_adjustment_get_page_size(adj);
    gtk_adjustment_set_value(adj, CLAMP(v, gtk_adjustment_get_lower(adj), max));
    return TRUE;
}

static void autoscroll(KfPane *p, gint wy)
{
    GtkWidget *w = p->view == KF_VIEW_ICONS ? p->iconview : p->treeview;
    GtkAllocation a;
    gtk_widget_get_allocation(w, &a);
    /* The widget is as tall as its content; what matters is the viewport. */
    GtkWidget *sw = p->view == KF_VIEW_ICONS ? p->sw_icons : p->sw_list;
    GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(sw));
    int rel = wy - (int)gtk_adjustment_get_value(adj);
    int page = (int)gtk_adjustment_get_page_size(adj);
    scroll_pane = p;
    scroll_dir = rel < 28 ? -1 : rel > page - 28 ? 1 : 0;
    if (scroll_dir && !scroll_timer)
        scroll_timer = g_timeout_add(50, scroll_tick, NULL);
}

static void stop_autoscroll(void)
{
    scroll_dir = 0;
    if (scroll_timer) {
        g_source_remove(scroll_timer);
        scroll_timer = 0;
    }
}

/* ---- source ---------------------------------------------------------------- */

static void on_drag_begin(GtkWidget *w, GdkDragContext *ctx, gpointer data)
{
    (void)w; (void)ctx;
    KfPane *p = data;
    kf_files_free(drag_files);
    drag_files = kf_pane_selected(p);
    kf_app_pane_changed(p->app, p, KF_CHANGE_FOCUS);
}

static void on_drag_end(GtkWidget *w, GdkDragContext *ctx, gpointer data)
{
    (void)w; (void)ctx; (void)data;
    kf_files_free(drag_files);
    drag_files = NULL;
    stop_autoscroll();
    kf_hover_clear(data);
}

static void on_drag_data_get(GtkWidget *w, GdkDragContext *ctx, GtkSelectionData *sel, guint info, guint time, gpointer data)
{
    (void)w; (void)ctx; (void)time;
    KfPane *p = data;
    GList *files = drag_files ? drag_files : kf_pane_selected(p);
    GString *out = g_string_new(NULL);
    for (GList *l = files; l; l = l->next) {
        if (info == T_URIS) {
            char *u = g_file_get_uri(l->data);
            g_string_append_printf(out, "%s\r\n", u);
            g_free(u);
        } else {
            char *path = g_file_is_native(l->data) ? g_file_get_path(l->data) : g_file_get_uri(l->data);
            if (out->len)
                g_string_append_c(out, '\n');
            g_string_append(out, path);
            g_free(path);
        }
    }
    gtk_selection_data_set(sel, gtk_selection_data_get_target(sel), 8, (const guchar *)out->str, out->len);
    g_string_free(out, TRUE);
    if (files != drag_files)
        kf_files_free(files);
}

/* ---- destination ----------------------------------------------------------- */

static gboolean on_drag_motion(GtkWidget *w, GdkDragContext *ctx, gint x, gint y, guint time, gpointer data)
{
    (void)w;
    KfPane *p = data;
    GdkAtom target = gtk_drag_dest_find_target(w, ctx, NULL);
    GtkTreePath *hover = NULL;
    GFile *dest = target != GDK_NONE ? drop_target(p, x, y, &hover) : NULL;
    if (!dest) {
        set_highlight(p, NULL);
        gdk_drag_status(ctx, 0, time);
        return TRUE;
    }
    set_highlight(p, hover);
    if (hover)
        gtk_tree_path_free(hover);
    gdk_drag_status(ctx, kf_dnd_default_action(p->app, dest, ctx), time);
    g_object_unref(dest);
    /* Coordinates here are widget coordinates; the viewport offset is
     * accounted for inside autoscroll(). */
    autoscroll(p, y);
    return TRUE;
}

static void on_drag_leave(GtkWidget *w, GdkDragContext *ctx, guint time, gpointer data)
{
    (void)w; (void)ctx; (void)time;
    set_highlight(data, NULL);
    stop_autoscroll();
}

static gboolean on_drag_drop(GtkWidget *w, GdkDragContext *ctx, gint x, gint y, guint time, gpointer data)
{
    KfPane *p = data;
    GdkAtom target = gtk_drag_dest_find_target(w, ctx, NULL);
    if (target == GDK_NONE)
        return FALSE;
    /* Remember where it landed: the data arrives in drag-data-received. */
    GtkTreePath *hover = NULL;
    GFile *dest = drop_target(p, x, y, &hover);
    if (hover)
        gtk_tree_path_free(hover);
    g_object_set_data_full(G_OBJECT(w), "kf-drop-dest", dest, dest ? g_object_unref : NULL);
    gtk_drag_get_data(w, ctx, target, time);
    return TRUE;
}

static void on_drag_data_received(GtkWidget *w, GdkDragContext *ctx, gint x, gint y, GtkSelectionData *sel,
                                  guint info, guint time, gpointer data)
{
    (void)x; (void)y; (void)info;
    KfPane *p = data;
    GFile *dest = g_object_get_data(G_OBJECT(w), "kf-drop-dest");
    gboolean ok = FALSE;
    if (dest)
        ok = kf_dnd_drop_uris(p->app, sel, ctx, dest);
    g_object_set_data(G_OBJECT(w), "kf-drop-dest", NULL);
    set_highlight(p, NULL);
    stop_autoscroll();
    /* The transfer is ours to do (a move deletes sources itself), so the
     * source must not delete anything. */
    gtk_drag_finish(ctx, ok, FALSE, time);
}

static void setup_dest(KfPane *p, GtkWidget *w)
{
    gtk_drag_dest_set(w, 0, kDstTargets, G_N_ELEMENTS(kDstTargets), ACTIONS);
    g_signal_connect(w, "drag-motion", G_CALLBACK(on_drag_motion), p);
    g_signal_connect(w, "drag-leave", G_CALLBACK(on_drag_leave), p);
    g_signal_connect(w, "drag-drop", G_CALLBACK(on_drag_drop), p);
    g_signal_connect(w, "drag-data-received", G_CALLBACK(on_drag_data_received), p);
}

void kf_dnd_setup_pane(KfPane *p)
{
    gtk_icon_view_enable_model_drag_source(GTK_ICON_VIEW(p->iconview), GDK_BUTTON1_MASK, kSrcTargets,
                                           G_N_ELEMENTS(kSrcTargets), ACTIONS);
    gtk_tree_view_enable_model_drag_source(GTK_TREE_VIEW(p->treeview), GDK_BUTTON1_MASK, kSrcTargets,
                                           G_N_ELEMENTS(kSrcTargets), ACTIONS);
    GtkWidget *views[2] = { p->iconview, p->treeview };
    for (int i = 0; i < 2; i++) {
        g_signal_connect(views[i], "drag-begin", G_CALLBACK(on_drag_begin), p);
        g_signal_connect(views[i], "drag-end", G_CALLBACK(on_drag_end), p);
        g_signal_connect(views[i], "drag-data-get", G_CALLBACK(on_drag_data_get), p);
        setup_dest(p, views[i]);
    }
}
