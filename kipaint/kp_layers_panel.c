/* kp_layers_panel.c - the layers panel on the right of the canvas.
 *
 * A list (top layer first) with a visibility checkbox and an editable name,
 * buttons to add, duplicate, delete and reorder, and an opacity slider for
 * the active layer. Every change goes through the undo history. An opacity
 * drag is applied live and committed as one history step shortly after the
 * last movement (or before any undo, see kp_layers_flush). */
#include "kp.h"

#include <string.h>

enum { COL_VISIBLE, COL_NAME, COL_INDEX, N_COLS };

#define OPACITY_COMMIT_MS 400

static GtkWidget *g_box, *g_view, *g_scale;
static GtkListStore *g_store;
static GtkWidget *g_btn_del, *g_btn_up, *g_btn_down;
static gboolean g_refreshing;
static gboolean g_shown = TRUE;

/* An opacity edit in progress: the layer and what it was before. */
static KpLayer *g_op_layer;
static double g_op_old;
static guint g_op_timer;

/* ---- list ------------------------------------------------------------ */

static int row_to_index(int row)
{
    return (int)kp.doc->layers->len - 1 - row;
}

static void update_buttons(void)
{
    int n = kp.doc ? (int)kp.doc->layers->len : 0;
    gtk_widget_set_sensitive(g_btn_del, n > 1);
    gtk_widget_set_sensitive(g_btn_up, n > 0 && kp.doc->active < n - 1);
    gtk_widget_set_sensitive(g_btn_down, n > 0 && kp.doc->active > 0);
}

void kp_layers_refresh(void)
{
    if (!g_store)
        return;
    kp_layers_flush();
    if (!kp.doc) {
        gtk_list_store_clear(g_store);
        return;
    }
    g_refreshing = TRUE;
    gtk_list_store_clear(g_store);
    int n = kp.doc->layers->len;
    for (int row = 0; row < n; row++) {
        int idx = row_to_index(row);
        KpLayer *l = g_ptr_array_index(kp.doc->layers, idx);
        GtkTreeIter it;
        gtk_list_store_append(g_store, &it);
        gtk_list_store_set(g_store, &it, COL_VISIBLE, l->visible, COL_NAME, l->name, COL_INDEX, idx, -1);
        if (idx == kp.doc->active) {
            GtkTreePath *p = gtk_tree_path_new_from_indices(row, -1);
            gtk_tree_selection_select_path(gtk_tree_view_get_selection(GTK_TREE_VIEW(g_view)), p);
            gtk_tree_path_free(p);
        }
    }
    KpLayer *act = kp_doc_layer(kp.doc);
    gtk_range_set_value(GTK_RANGE(g_scale), act->opacity * 100);
    g_refreshing = FALSE;
    update_buttons();
    kp_canvas_redraw();
}

static void on_selection_changed(GtkTreeSelection *sel)
{
    if (g_refreshing || !kp.doc)
        return;
    GtkTreeModel *m;
    GtkTreeIter it;
    if (!gtk_tree_selection_get_selected(sel, &m, &it))
        return;
    int idx;
    gtk_tree_model_get(m, &it, COL_INDEX, &idx, -1);
    if (idx == kp.doc->active)
        return;
    kp_layers_flush();
    kp.doc->active = idx;
    g_refreshing = TRUE;
    gtk_range_set_value(GTK_RANGE(g_scale), kp_doc_layer(kp.doc)->opacity * 100);
    g_refreshing = FALSE;
    update_buttons();
}

/* Changes name/visibility as one undoable step. */
static void props_edit(KpLayer *l, const char *name, int visible)
{
    KpLayerProps old = { g_strdup(l->name), l->visible, l->opacity };
    if (name) {
        g_free(l->name);
        l->name = g_strdup(name);
    }
    if (visible >= 0)
        l->visible = visible;
    kp_undo_layer_props(l, &old);
    g_free(old.name);
}

static void on_visible_toggled(GtkCellRendererToggle *r, gchar *path_str)
{
    (void)r;
    GtkTreeIter it;
    int idx;
    if (!gtk_tree_model_get_iter_from_string(GTK_TREE_MODEL(g_store), &it, path_str))
        return;
    gtk_tree_model_get(GTK_TREE_MODEL(g_store), &it, COL_INDEX, &idx, -1);
    kp_layers_flush();
    KpLayer *l = g_ptr_array_index(kp.doc->layers, idx);
    props_edit(l, NULL, !l->visible);
    gtk_list_store_set(g_store, &it, COL_VISIBLE, l->visible, -1);
    kp_canvas_redraw();
}

static void on_name_edited(GtkCellRendererText *r, gchar *path_str, gchar *text)
{
    (void)r;
    GtkTreeIter it;
    int idx;
    if (!*text || !gtk_tree_model_get_iter_from_string(GTK_TREE_MODEL(g_store), &it, path_str))
        return;
    gtk_tree_model_get(GTK_TREE_MODEL(g_store), &it, COL_INDEX, &idx, -1);
    KpLayer *l = g_ptr_array_index(kp.doc->layers, idx);
    if (!strcmp(l->name, text))
        return;
    kp_layers_flush();
    props_edit(l, text, -1);
    gtk_list_store_set(g_store, &it, COL_NAME, l->name, -1);
}

/* ---- opacity --------------------------------------------------------- */

void kp_layers_flush(void)
{
    if (!g_op_layer)
        return;
    if (g_op_timer) {
        g_source_remove(g_op_timer);
        g_op_timer = 0;
    }
    KpLayer *l = g_op_layer;
    g_op_layer = NULL;
    if (l->opacity == g_op_old)
        return;
    KpLayerProps old = { g_strdup(l->name), l->visible, g_op_old };
    kp_undo_layer_props(l, &old);
    g_free(old.name);
}

static gboolean opacity_commit(gpointer data)
{
    (void)data;
    g_op_timer = 0;
    kp_layers_flush();
    return FALSE;
}

static void on_opacity_changed(GtkRange *r)
{
    if (g_refreshing || !kp.doc)
        return;
    KpLayer *l = kp_doc_layer(kp.doc);
    if (g_op_layer && g_op_layer != l)
        kp_layers_flush();
    if (!g_op_layer) {
        g_op_layer = l;
        g_op_old = l->opacity;
    }
    l->opacity = gtk_range_get_value(r) / 100;
    kp_canvas_redraw();
    if (g_op_timer)
        g_source_remove(g_op_timer);
    g_op_timer = g_timeout_add(OPACITY_COMMIT_MS, opacity_commit, NULL);
}

/* ---- actions --------------------------------------------------------- */

void kp_layers_add(void)
{
    kp_layers_flush();
    char name[64];
    g_snprintf(name, sizeof(name), _("Layer %u"), kp.doc->layers->len + 1);
    KpLayer *l = kp_layer_new(kp.doc->w, kp.doc->h, name);
    if (!l) {
        GtkWidget *dlg = gtk_message_dialog_new(GTK_WINDOW(kp.win), GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
                                                "%s", _("Not enough memory for another layer."));
        gtk_dialog_run(GTK_DIALOG(dlg));
        gtk_widget_destroy(dlg);
        return;
    }
    int at = kp.doc->active + 1;
    kp_doc_layer_insert(kp.doc, l, at);
    kp_undo_layer_added(at);
    kp_layers_refresh();
}

void kp_layers_duplicate(void)
{
    kp_layers_flush();
    KpLayer *src = kp_doc_layer(kp.doc);
    KpLayer *l = kp_layer_duplicate(src);
    if (!l)
        return;
    char *name = g_strdup_printf(_("%s copy"), src->name);
    g_free(l->name);
    l->name = name;
    int at = kp.doc->active + 1;
    kp_doc_layer_insert(kp.doc, l, at);
    kp_undo_layer_added(at);
    kp_layers_refresh();
}

void kp_layers_delete(void)
{
    if (kp.doc->layers->len < 2)
        return;
    kp_layers_flush();
    int at = kp.doc->active;
    KpLayer *l = kp_doc_layer_take(kp.doc, at);
    kp_undo_layer_removed(l, at);
    kp_layers_refresh();
}

static void move_active(int dir)
{
    int from = kp.doc->active, to = from + dir;
    if (to < 0 || to >= (int)kp.doc->layers->len)
        return;
    kp_layers_flush();
    kp_doc_layer_move(kp.doc, from, to);
    kp_undo_layer_moved(from, to);
    kp_layers_refresh();
}

void kp_layers_raise(void)
{
    move_active(1);
}

void kp_layers_lower(void)
{
    move_active(-1);
}

/* ---- widget ---------------------------------------------------------- */

static GtkWidget *icon_button(const char *stock, const char *tip, GCallback cb)
{
    GtkWidget *b = gtk_button_new();
    gtk_container_add(GTK_CONTAINER(b), gtk_image_new_from_stock(stock, GTK_ICON_SIZE_MENU));
    gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text(b, tip);
    g_signal_connect(b, "clicked", cb, NULL);
    GTK_WIDGET_UNSET_FLAGS(b, GTK_CAN_FOCUS);
    return b;
}

GtkWidget *kp_layers_panel_new(void)
{
    g_store = gtk_list_store_new(N_COLS, G_TYPE_BOOLEAN, G_TYPE_STRING, G_TYPE_INT);
    g_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(g_store));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(g_view), FALSE);
    GtkCellRenderer *tr = gtk_cell_renderer_toggle_new();
    g_signal_connect(tr, "toggled", G_CALLBACK(on_visible_toggled), NULL);
    gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(g_view), -1, NULL, tr, "active", COL_VISIBLE, NULL);
    GtkCellRenderer *nr = gtk_cell_renderer_text_new();
    g_object_set(nr, "editable", TRUE, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
    g_signal_connect(nr, "edited", G_CALLBACK(on_name_edited), NULL);
    GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes(NULL, nr, "text", COL_NAME, NULL);
    gtk_tree_view_column_set_expand(col, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(g_view), col);
    gtk_tree_view_set_search_column(GTK_TREE_VIEW(g_view), COL_NAME);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(g_view)), "changed", G_CALLBACK(on_selection_changed),
                     NULL);

    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sw), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(sw), g_view);

    GtkWidget *buttons = gtk_hbox_new(FALSE, 0);
    gtk_box_pack_start(GTK_BOX(buttons), icon_button(GTK_STOCK_ADD, _("New layer"), G_CALLBACK(kp_layers_add)), FALSE,
                       FALSE, 0);
    g_btn_del = icon_button(GTK_STOCK_REMOVE, _("Delete layer"), G_CALLBACK(kp_layers_delete));
    gtk_box_pack_start(GTK_BOX(buttons), g_btn_del, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(buttons),
                       icon_button(GTK_STOCK_COPY, _("Duplicate layer"), G_CALLBACK(kp_layers_duplicate)), FALSE, FALSE,
                       0);
    g_btn_up = icon_button(GTK_STOCK_GO_UP, _("Raise layer"), G_CALLBACK(kp_layers_raise));
    gtk_box_pack_start(GTK_BOX(buttons), g_btn_up, FALSE, FALSE, 0);
    g_btn_down = icon_button(GTK_STOCK_GO_DOWN, _("Lower layer"), G_CALLBACK(kp_layers_lower));
    gtk_box_pack_start(GTK_BOX(buttons), g_btn_down, FALSE, FALSE, 0);

    GtkWidget *title = gtk_label_new(_("Layers"));
    gtk_misc_set_alignment(GTK_MISC(title), 0, 0.5);
    GtkWidget *opac = gtk_label_new(_("Opacity"));
    gtk_misc_set_alignment(GTK_MISC(opac), 0, 0.5);
    g_scale = gtk_hscale_new_with_range(0, 100, 1);
    gtk_scale_set_value_pos(GTK_SCALE(g_scale), GTK_POS_RIGHT);
    gtk_scale_set_digits(GTK_SCALE(g_scale), 0);
    gtk_range_set_value(GTK_RANGE(g_scale), 100);
    g_signal_connect(g_scale, "value-changed", G_CALLBACK(on_opacity_changed), NULL);

    g_box = gtk_vbox_new(FALSE, 4);
    gtk_container_set_border_width(GTK_CONTAINER(g_box), 4);
    gtk_widget_set_size_request(g_box, 170, -1);
    gtk_box_pack_start(GTK_BOX(g_box), title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(g_box), sw, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(g_box), buttons, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(g_box), opac, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(g_box), g_scale, FALSE, FALSE, 0);
    /* shown or hidden by kp_layers_set_shown, not by gtk_widget_show_all */
    gtk_widget_set_no_show_all(g_box, TRUE);
    kp_layers_set_shown(kp.layers_panel);
    return g_box;
}

void kp_layers_set_shown(gboolean shown)
{
    g_shown = shown;
    kp.layers_panel = shown;
    if (g_box) {
        if (shown) {
            /* show_all skips a no_show_all widget, itself included */
            gtk_widget_set_no_show_all(g_box, FALSE);
            gtk_widget_show_all(g_box);
            gtk_widget_set_no_show_all(g_box, TRUE);
        } else
            gtk_widget_hide(g_box);
    }
}

gboolean kp_layers_shown(void)
{
    return g_shown;
}
