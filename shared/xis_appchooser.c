/* xis_appchooser.c - see xis_appchooser.h. */
#include "xis_appchooser.h"
#include "xis_i18n.h"

#include <string.h>

enum { COL_ICON, COL_NAME, COL_APP, COL_KEY, N_COLS };

typedef struct {
    GtkWidget *dlg;
    GtkWidget *entry;
    GtkWidget *view;
    GtkTreeModelFilter *filter;
} Chooser;

static gboolean row_visible(GtkTreeModel *m, GtkTreeIter *it, gpointer d)
{
    Chooser *c = d;
    const char *q = gtk_entry_get_text(GTK_ENTRY(c->entry));
    if (!*q)
        return TRUE;
    char *key = NULL;
    gtk_tree_model_get(m, it, COL_KEY, &key, -1);
    char *needle = g_utf8_casefold(q, -1);
    gboolean ok = key && strstr(key, needle);
    g_free(needle);
    g_free(key);
    return ok;
}

static void select_first(Chooser *c)
{
    GtkTreeModel *m = GTK_TREE_MODEL(c->filter);
    GtkTreeIter it;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(c->view));
    if (gtk_tree_model_get_iter_first(m, &it)) {
        gtk_tree_selection_select_iter(sel, &it);
        GtkTreePath *p = gtk_tree_model_get_path(m, &it);
        gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(c->view), p, NULL, FALSE, 0, 0);
        gtk_tree_path_free(p);
    } else {
        gtk_tree_selection_unselect_all(sel);
    }
    gtk_dialog_set_response_sensitive(GTK_DIALOG(c->dlg), GTK_RESPONSE_OK,
                                      gtk_tree_selection_count_selected_rows(sel) > 0 ||
                                          *gtk_entry_get_text(GTK_ENTRY(c->entry)));
}

static void on_changed(GtkEditable *e, gpointer d)
{
    (void)e;
    Chooser *c = d;
    gtk_tree_model_filter_refilter(c->filter);
    select_first(c);
}

static void on_activate(GtkWidget *w, gpointer d)
{
    (void)w;
    gtk_dialog_response(GTK_DIALOG(((Chooser *)d)->dlg), GTK_RESPONSE_OK);
}

static void on_row_activated(GtkTreeView *v, GtkTreePath *p, GtkTreeViewColumn *col, gpointer d)
{
    (void)v; (void)p; (void)col;
    gtk_dialog_response(GTK_DIALOG(((Chooser *)d)->dlg), GTK_RESPONSE_OK);
}

static void on_sel_changed(GtkTreeSelection *s, gpointer d)
{
    Chooser *c = d;
    gtk_dialog_set_response_sensitive(GTK_DIALOG(c->dlg), GTK_RESPONSE_OK,
                                      gtk_tree_selection_count_selected_rows(s) > 0 ||
                                          *gtk_entry_get_text(GTK_ENTRY(c->entry)));
}

static gint compare_apps(gconstpointer a, gconstpointer b)
{
    return g_utf8_collate(g_app_info_get_display_name((GAppInfo *)a), g_app_info_get_display_name((GAppInfo *)b));
}

static void add_app(GtkListStore *st, GAppInfo *ai)
{
    const char *name = g_app_info_get_display_name(ai);
    const char *cmd = g_app_info_get_commandline(ai);
    char *hay = g_strdup_printf("%s %s", name, cmd ? cmd : "");
    char *key = g_utf8_casefold(hay, -1);
    GtkTreeIter it;
    gtk_list_store_append(st, &it);
    gtk_list_store_set(st, &it, COL_ICON, g_app_info_get_icon(ai), COL_NAME, name, COL_APP, ai, COL_KEY, key, -1);
    g_free(key);
    g_free(hay);
}

static void fill(GtkListStore *st, const char *content_type)
{
    GList *rec = content_type ? g_app_info_get_all_for_type(content_type) : NULL;
    GList *all = g_app_info_get_all();
    rec = g_list_sort(rec, compare_apps);
    all = g_list_sort(all, compare_apps);
    for (GList *l = rec; l; l = l->next)
        if (g_app_info_should_show(l->data))
            add_app(st, l->data);
    for (GList *l = all; l; l = l->next) {
        gboolean dup = FALSE;
        for (GList *r = rec; r && !dup; r = r->next)
            dup = g_app_info_equal(r->data, l->data);
        if (!dup && g_app_info_should_show(l->data))
            add_app(st, l->data);
    }
    g_list_free_full(rec, g_object_unref);
    g_list_free_full(all, g_object_unref);
}

static GAppInfo *from_command(Chooser *c, const char *text)
{
    char *cmd = g_strstrip(g_strdup(text));
    if (!strstr(cmd, "%f") && !strstr(cmd, "%F") && !strstr(cmd, "%u") && !strstr(cmd, "%U")) {
        char *tmp = g_strconcat(cmd, " %F", NULL);
        g_free(cmd);
        cmd = tmp;
    }
    GError *err = NULL;
    GAppInfo *ai = g_app_info_create_from_commandline(cmd, NULL, G_APP_INFO_CREATE_NONE, &err);
    if (!ai) {
        GtkWidget *m = gtk_message_dialog_new(GTK_WINDOW(c->dlg), GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
                                              "%s", err ? err->message : _("Invalid command"));
        gtk_dialog_run(GTK_DIALOG(m));
        gtk_widget_destroy(m);
        g_clear_error(&err);
    }
    g_free(cmd);
    return ai;
}

GAppInfo *xis_app_chooser_run(GtkWindow *parent, const char *content_type)
{
    Chooser c = { 0 };
    c.dlg = gtk_dialog_new_with_buttons(_("Open With"), parent, GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                        GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL, GTK_STOCK_OPEN, GTK_RESPONSE_OK, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(c.dlg), GTK_RESPONSE_OK);
    gtk_window_set_default_size(GTK_WINDOW(c.dlg), 420, 460);

    GtkListStore *st = gtk_list_store_new(N_COLS, G_TYPE_ICON, G_TYPE_STRING, G_TYPE_APP_INFO, G_TYPE_STRING);
    fill(st, content_type);
    c.filter = GTK_TREE_MODEL_FILTER(gtk_tree_model_filter_new(GTK_TREE_MODEL(st), NULL));
    gtk_tree_model_filter_set_visible_func(c.filter, row_visible, &c, NULL);

    c.entry = gtk_entry_new();
    gtk_entry_set_activates_default(GTK_ENTRY(c.entry), TRUE);
    c.view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(c.filter));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(c.view), FALSE);
    gtk_tree_view_set_enable_search(GTK_TREE_VIEW(c.view), FALSE);
    GtkTreeViewColumn *col = gtk_tree_view_column_new();
    GtkCellRenderer *ir = gtk_cell_renderer_pixbuf_new();
    g_object_set(ir, "stock-size", GTK_ICON_SIZE_LARGE_TOOLBAR, NULL);
    gtk_tree_view_column_pack_start(col, ir, FALSE);
    gtk_tree_view_column_add_attribute(col, ir, "gicon", COL_ICON);
    GtkCellRenderer *tr = gtk_cell_renderer_text_new();
    gtk_tree_view_column_pack_start(col, tr, TRUE);
    gtk_tree_view_column_add_attribute(col, tr, "text", COL_NAME);
    gtk_tree_view_append_column(GTK_TREE_VIEW(c.view), col);

    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sw), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(sw), c.view);

    GtkWidget *hint = gtk_label_new(_("Filter the list, or type a command to run instead:"));
    gtk_misc_set_alignment(GTK_MISC(hint), 0, 0.5);
    GtkWidget *box = gtk_vbox_new(FALSE, 6);
    gtk_container_set_border_width(GTK_CONTAINER(box), 8);
    gtk_box_pack_start(GTK_BOX(box), hint, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), c.entry, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), sw, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(c.dlg)->vbox), box, TRUE, TRUE, 0);

    g_signal_connect(c.entry, "changed", G_CALLBACK(on_changed), &c);
    g_signal_connect(c.entry, "activate", G_CALLBACK(on_activate), &c);
    g_signal_connect(c.view, "row-activated", G_CALLBACK(on_row_activated), &c);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(c.view)), "changed", G_CALLBACK(on_sel_changed), &c);
    gtk_widget_show_all(c.dlg);
    select_first(&c);
    gtk_widget_grab_focus(c.entry);

    GAppInfo *result = NULL;
    while (!result && gtk_dialog_run(GTK_DIALOG(c.dlg)) == GTK_RESPONSE_OK) {
        GtkTreeModel *m;
        GtkTreeIter it;
        const char *text = gtk_entry_get_text(GTK_ENTRY(c.entry));
        if (gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(c.view)), &m, &it)) {
            gtk_tree_model_get(m, &it, COL_APP, &result, -1); /* takes a ref */
        } else if (*text) {
            result = from_command(&c, text);
        }
    }
    gtk_widget_destroy(c.dlg);
    g_object_unref(c.filter);
    g_object_unref(st);
    return result;
}
