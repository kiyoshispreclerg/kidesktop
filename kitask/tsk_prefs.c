/* tsk_prefs.c - the Settings window: refresh rates, which graphs (with
 * their limits and colors) and which columns. Every change applies at once
 * and is saved; there is no OK/Cancel. */
#include "tsk.h"
#include "../shared/xis_i18n.h"
#include <string.h>

enum { G_COL_LABEL, G_COL_GRAPH, G_NCOLS };
enum { C_COL_ON, C_COL_TITLE, C_COL_TIP, C_COL_KEY, C_NCOLS };

typedef struct {
    TskApp *app;
    GtkWidget *win, *nb;
    /* graphs page */
    GtkListStore *gstore;
    GtkWidget *gview, *g_editor, *g_min, *g_max, *g_auto, *g_color, *g_unit_min, *g_unit_max, *add_menu;
    gboolean loading;
    /* columns page */
    GtkListStore *cstore;
    GtkWidget *cview;
} Prefs;

static Prefs *prefs;

/* ---- general ---------------------------------------------------------------------- */

static void on_list_interval(GtkSpinButton *s, Prefs *p)
{
    p->app->cfg.list_interval = gtk_spin_button_get_value(s);
    tsk_app_restart_timers(p->app);
    tsk_app_save_soon(p->app);
}

static void on_graph_interval(GtkSpinButton *s, Prefs *p)
{
    p->app->cfg.graph_interval = gtk_spin_button_get_value(s);
    tsk_app_restart_timers(p->app);
    tsk_app_save_soon(p->app);
}

static void on_show_graphs(GtkToggleButton *b, Prefs *p)
{
    p->app->cfg.show_graphs = gtk_toggle_button_get_active(b);
    tsk_app_apply_graphs_visible(p->app);
    tsk_app_save_soon(p->app);
}

static void on_graph_height(GtkSpinButton *s, Prefs *p)
{
    p->app->cfg.graph_height = gtk_spin_button_get_value_as_int(s);
    tsk_graphs_rebuild(p->app);
    tsk_app_save_soon(p->app);
}

static void on_per_row(GtkSpinButton *s, Prefs *p)
{
    p->app->cfg.graphs_per_row = gtk_spin_button_get_value_as_int(s);
    tsk_graphs_rebuild(p->app);
    tsk_app_save_soon(p->app);
}

static void on_kthreads(GtkToggleButton *b, Prefs *p)
{
    p->app->cfg.hide_kthreads = gtk_toggle_button_get_active(b);
    tsk_list_refilter(p->app);
    tsk_app_save_soon(p->app);
}

static void on_confirm(GtkToggleButton *b, Prefs *p)
{
    p->app->cfg.confirm_signals = gtk_toggle_button_get_active(b);
    tsk_app_save_soon(p->app);
}

static GtkWidget *row(GtkWidget *tbl, int r, const char *label, GtkWidget *w, const char *unit)
{
    GtkWidget *l = gtk_label_new_with_mnemonic(label);
    gtk_misc_set_alignment(GTK_MISC(l), 0, 0.5);
    gtk_label_set_mnemonic_widget(GTK_LABEL(l), w);
    gtk_table_attach(GTK_TABLE(tbl), l, 0, 1, (guint)r, (guint)r + 1, GTK_FILL, 0, 0, 0);
    GtkWidget *box = gtk_hbox_new(FALSE, 6);
    gtk_box_pack_start(GTK_BOX(box), w, FALSE, FALSE, 0);
    if (unit) {
        GtkWidget *u = gtk_label_new(unit);
        gtk_box_pack_start(GTK_BOX(box), u, FALSE, FALSE, 0);
    }
    gtk_table_attach(GTK_TABLE(tbl), box, 1, 2, (guint)r, (guint)r + 1, GTK_FILL | GTK_EXPAND, 0, 0, 0);
    return w;
}

static GtkWidget *spin(double min, double max, double step, int digits, double val, GCallback cb, Prefs *p)
{
    GtkWidget *s = gtk_spin_button_new_with_range(min, max, step);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(s), (guint)digits);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(s), val);
    g_signal_connect(s, "value-changed", cb, p);
    return s;
}

static GtkWidget *check(const char *label, gboolean on, GCallback cb, Prefs *p)
{
    GtkWidget *c = gtk_check_button_new_with_mnemonic(label);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(c), on);
    g_signal_connect(c, "toggled", cb, p);
    return c;
}

static GtkWidget *general_page(Prefs *p)
{
    TskConfig *c = &p->app->cfg;
    GtkWidget *t = gtk_table_new(8, 2, FALSE);
    gtk_container_set_border_width(GTK_CONTAINER(t), 12);
    gtk_table_set_row_spacings(GTK_TABLE(t), 6);
    gtk_table_set_col_spacings(GTK_TABLE(t), 12);
    row(t, 0, _("_Process list refresh:"), spin(0.25, 60, 0.25, 2, c->list_interval, G_CALLBACK(on_list_interval), p),
        _("seconds"));
    row(t, 1, _("_Graphs refresh:"), spin(0.1, 60, 0.1, 1, c->graph_interval, G_CALLBACK(on_graph_interval), p),
        _("seconds"));
    row(t, 2, _("Graph _height:"), spin(30, 400, 5, 0, c->graph_height, G_CALLBACK(on_graph_height), p),
        _("pixels"));
    row(t, 3, _("Graphs per _row:"), spin(1, 16, 1, 0, c->graphs_per_row, G_CALLBACK(on_per_row), p), NULL);
    gtk_table_attach(GTK_TABLE(t), check(_("_Show the graphs above the list"), c->show_graphs,
                                         G_CALLBACK(on_show_graphs), p),
                     0, 2, 4, 5, GTK_FILL, 0, 0, 0);
    gtk_table_attach(GTK_TABLE(t), check(_("Hide _kernel threads"), c->hide_kthreads, G_CALLBACK(on_kthreads), p),
                     0, 2, 5, 6, GTK_FILL, 0, 0, 0);
    gtk_table_attach(GTK_TABLE(t), check(_("_Ask before ending, killing or pausing a process"),
                                         c->confirm_signals, G_CALLBACK(on_confirm), p),
                     0, 2, 6, 7, GTK_FILL, 0, 0, 0);
    GtkWidget *note = gtk_label_new(_("Columns that cost more to read (exclusive memory, GPU, network, disk, X11) "
                                      "are only read while they are shown, and only for processes that ran since "
                                      "the last refresh."));
    gtk_label_set_line_wrap(GTK_LABEL(note), TRUE);
    gtk_misc_set_alignment(GTK_MISC(note), 0, 0);
    gtk_widget_set_size_request(note, 380, -1);
    gtk_table_attach(GTK_TABLE(t), note, 0, 2, 7, 8, GTK_FILL, 0, 0, 8);
    return t;
}

/* ---- graphs --------------------------------------------------------------------- */

static TskGraph *selected_graph(Prefs *p, GtkTreeIter *out)
{
    GtkTreeIter it;
    TskGraph *g = NULL;
    if (gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(p->gview)), NULL, &it)) {
        gtk_tree_model_get(GTK_TREE_MODEL(p->gstore), &it, G_COL_GRAPH, &g, -1);
        if (out)
            *out = it;
    }
    return g;
}

static void fill_graphs(Prefs *p, TskGraph *select)
{
    gtk_list_store_clear(p->gstore);
    for (guint i = 0; i < p->app->cfg.graphs->len; i++) {
        TskGraph *g = p->app->cfg.graphs->pdata[i];
        char *label = tsk_graph_source_label(g->source);
        if (!tsk_graph_source_valid(g->source)) {
            char *l2 = g_strdup_printf(_("%s (not available here)"), label);
            g_free(label);
            label = l2;
        }
        GtkTreeIter it;
        gtk_list_store_insert_with_values(p->gstore, &it, -1, G_COL_LABEL, label, G_COL_GRAPH, g, -1);
        if (g == select)
            gtk_tree_selection_select_iter(gtk_tree_view_get_selection(GTK_TREE_VIEW(p->gview)), &it);
        g_free(label);
    }
}

static const char *unit_name(TskUnit u)
{
    return u == TSK_UNIT_PCT ? "%" : u == TSK_UNIT_BYTES ? _("MiB") : _("KiB/s");
}

static void on_graph_selected(GtkTreeSelection *sel, Prefs *p)
{
    (void)sel;
    TskGraph *g = selected_graph(p, NULL);
    gtk_widget_set_sensitive(p->g_editor, g != NULL);
    if (!g)
        return;
    p->loading = TRUE;
    TskUnit u = tsk_graph_source_unit(g->source);
    double top = u == TSK_UNIT_PCT ? 100 : 1e9;
    gtk_spin_button_set_range(GTK_SPIN_BUTTON(p->g_min), 0, top);
    gtk_spin_button_set_range(GTK_SPIN_BUTTON(p->g_max), 0, top);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(p->g_min), g->min);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(p->g_max), g->max);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(p->g_auto), g->auto_max);
    gtk_widget_set_sensitive(p->g_max, !g->auto_max);
    gtk_color_button_set_color(GTK_COLOR_BUTTON(p->g_color), &g->color);
    gtk_label_set_text(GTK_LABEL(p->g_unit_min), unit_name(u));
    gtk_label_set_text(GTK_LABEL(p->g_unit_max), unit_name(u));
    p->loading = FALSE;
}

static void graph_changed(Prefs *p)
{
    TskGraph *g = selected_graph(p, NULL);
    if (p->loading || !g)
        return;
    g->min = gtk_spin_button_get_value(GTK_SPIN_BUTTON(p->g_min));
    g->auto_max = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(p->g_auto));
    g->max = gtk_spin_button_get_value(GTK_SPIN_BUTTON(p->g_max));
    gtk_color_button_get_color(GTK_COLOR_BUTTON(p->g_color), &g->color);
    gtk_widget_set_sensitive(p->g_max, !g->auto_max);
    if (g->area)
        gtk_widget_queue_draw(g->area);
    tsk_app_save_soon(p->app);
}

static void on_graph_edit(GtkWidget *w, Prefs *p)
{
    (void)w;
    graph_changed(p);
}

static void graphs_changed(Prefs *p, TskGraph *select)
{
    tsk_graphs_rebuild(p->app);
    fill_graphs(p, select);
    tsk_app_save_soon(p->app);
}

static void on_add_source(GtkMenuItem *mi, Prefs *p)
{
    TskGraph *g = tsk_graph_new(g_object_get_data(G_OBJECT(mi), "source"));
    g_ptr_array_add(p->app->cfg.graphs, g);
    if (!p->app->cfg.show_graphs) {
        p->app->cfg.show_graphs = TRUE;
        tsk_app_apply_graphs_visible(p->app);
    }
    graphs_changed(p, g);
}

static void on_add(GtkButton *b, Prefs *p)
{
    if (p->add_menu)
        gtk_widget_destroy(p->add_menu);
    p->add_menu = gtk_menu_new();
    GPtrArray *src = tsk_graph_sources();
    GtkWidget *cores = NULL;
    for (guint i = 0; i < src->len; i++) {
        const char *s = src->pdata[i];
        char *label = tsk_graph_source_label(s);
        GtkWidget *mi = gtk_menu_item_new_with_label(label);
        g_free(label);
        g_object_set_data_full(G_OBJECT(mi), "source", g_strdup(s), g_free);
        g_signal_connect(mi, "activate", G_CALLBACK(on_add_source), p);
        if (!strncmp(s, "cpu.", 4)) {
            /* one entry per core goes in its own submenu */
            if (!cores) {
                cores = gtk_menu_new();
                GtkWidget *sub = gtk_menu_item_new_with_label(_("One CPU core"));
                gtk_menu_item_set_submenu(GTK_MENU_ITEM(sub), cores);
                gtk_menu_shell_append(GTK_MENU_SHELL(p->add_menu), sub);
            }
            gtk_menu_shell_append(GTK_MENU_SHELL(cores), mi);
        } else {
            gtk_menu_shell_append(GTK_MENU_SHELL(p->add_menu), mi);
        }
    }
    g_ptr_array_free(src, TRUE);
    gtk_widget_show_all(p->add_menu);
    gtk_menu_attach_to_widget(GTK_MENU(p->add_menu), GTK_WIDGET(b), NULL);
    gtk_menu_popup(GTK_MENU(p->add_menu), NULL, NULL, NULL, NULL, 0, gtk_get_current_event_time());
}

static void on_remove(GtkButton *b, Prefs *p)
{
    (void)b;
    TskGraph *g = selected_graph(p, NULL);
    if (!g)
        return;
    guint idx = 0;
    g_ptr_array_find(p->app->cfg.graphs, g, &idx);
    g_ptr_array_remove_index(p->app->cfg.graphs, idx);
    GPtrArray *a = p->app->cfg.graphs;
    graphs_changed(p, a->len ? a->pdata[MIN(idx, a->len - 1)] : NULL);
}

static void move_graph(Prefs *p, int delta)
{
    TskGraph *g = selected_graph(p, NULL);
    GPtrArray *a = p->app->cfg.graphs;
    guint idx;
    if (!g || !g_ptr_array_find(a, g, &idx))
        return;
    int to = (int)idx + delta;
    if (to < 0 || to >= (int)a->len)
        return;
    a->pdata[idx] = a->pdata[to];
    a->pdata[to] = g;
    graphs_changed(p, g);
}

static void on_up(GtkButton *b, Prefs *p)
{
    (void)b;
    move_graph(p, -1);
}

static void on_down(GtkButton *b, Prefs *p)
{
    (void)b;
    move_graph(p, 1);
}

static GtkWidget *button(const char *stock, const char *label, GCallback cb, gpointer data)
{
    GtkWidget *b = label ? gtk_button_new_with_mnemonic(label) : gtk_button_new_from_stock(stock);
    if (label && stock)
        gtk_button_set_image(GTK_BUTTON(b), gtk_image_new_from_stock(stock, GTK_ICON_SIZE_BUTTON));
    g_signal_connect(b, "clicked", cb, data);
    return b;
}

static GtkWidget *graphs_page(Prefs *p)
{
    GtkWidget *box = gtk_vbox_new(FALSE, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    GtkWidget *top = gtk_hbox_new(FALSE, 8);
    p->gstore = gtk_list_store_new(G_NCOLS, G_TYPE_STRING, G_TYPE_POINTER);
    p->gview = gtk_tree_view_new_with_model(GTK_TREE_MODEL(p->gstore));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(p->gview), FALSE);
    gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(p->gview), -1, NULL, gtk_cell_renderer_text_new(),
                                                "text", G_COL_LABEL, NULL);
    atk_object_set_name(gtk_widget_get_accessible(p->gview), _("Graphs shown"));
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sw), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(sw), p->gview);
    gtk_box_pack_start(GTK_BOX(top), sw, TRUE, TRUE, 0);
    GtkWidget *btns = gtk_vbox_new(FALSE, 4);
    gtk_box_pack_start(GTK_BOX(btns), button(GTK_STOCK_ADD, _("_Add..."), G_CALLBACK(on_add), p), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(btns), button(GTK_STOCK_REMOVE, NULL, G_CALLBACK(on_remove), p), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(btns), button(GTK_STOCK_GO_UP, NULL, G_CALLBACK(on_up), p), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(btns), button(GTK_STOCK_GO_DOWN, NULL, G_CALLBACK(on_down), p), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(top), btns, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), top, TRUE, TRUE, 0);

    GtkWidget *frame = gtk_frame_new(_("Selected graph"));
    GtkWidget *t = gtk_table_new(4, 2, FALSE);
    gtk_container_set_border_width(GTK_CONTAINER(t), 8);
    gtk_table_set_row_spacings(GTK_TABLE(t), 6);
    gtk_table_set_col_spacings(GTK_TABLE(t), 12);
    p->g_min = gtk_spin_button_new_with_range(0, 1e9, 1);
    p->g_max = gtk_spin_button_new_with_range(0, 1e9, 1);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(p->g_min), 1);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(p->g_max), 1);
    p->g_auto = gtk_check_button_new_with_mnemonic(_("A_utomatic"));
    /* xgettext:no-c-format */
    gtk_widget_set_tooltip_text(p->g_auto, _("The total there is (RAM, VRAM, 100%), or the highest value on screen "
                                             "for traffic"));
    p->g_color = gtk_color_button_new();
    p->g_unit_min = gtk_label_new("");
    p->g_unit_max = gtk_label_new("");
    GtkWidget *l = gtk_label_new_with_mnemonic(_("_Minimum:"));
    gtk_misc_set_alignment(GTK_MISC(l), 0, 0.5);
    gtk_label_set_mnemonic_widget(GTK_LABEL(l), p->g_min);
    gtk_table_attach(GTK_TABLE(t), l, 0, 1, 0, 1, GTK_FILL, 0, 0, 0);
    GtkWidget *hb = gtk_hbox_new(FALSE, 6);
    gtk_box_pack_start(GTK_BOX(hb), p->g_min, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hb), p->g_unit_min, FALSE, FALSE, 0);
    gtk_table_attach(GTK_TABLE(t), hb, 1, 2, 0, 1, GTK_FILL | GTK_EXPAND, 0, 0, 0);
    l = gtk_label_new_with_mnemonic(_("Ma_ximum:"));
    gtk_misc_set_alignment(GTK_MISC(l), 0, 0.5);
    gtk_label_set_mnemonic_widget(GTK_LABEL(l), p->g_max);
    gtk_table_attach(GTK_TABLE(t), l, 0, 1, 1, 2, GTK_FILL, 0, 0, 0);
    hb = gtk_hbox_new(FALSE, 6);
    gtk_box_pack_start(GTK_BOX(hb), p->g_max, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hb), p->g_unit_max, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hb), p->g_auto, FALSE, FALSE, 6);
    gtk_table_attach(GTK_TABLE(t), hb, 1, 2, 1, 2, GTK_FILL | GTK_EXPAND, 0, 0, 0);
    l = gtk_label_new_with_mnemonic(_("C_olor:"));
    gtk_misc_set_alignment(GTK_MISC(l), 0, 0.5);
    gtk_label_set_mnemonic_widget(GTK_LABEL(l), p->g_color);
    gtk_table_attach(GTK_TABLE(t), l, 0, 1, 2, 3, GTK_FILL, 0, 0, 0);
    hb = gtk_hbox_new(FALSE, 6);
    gtk_box_pack_start(GTK_BOX(hb), p->g_color, FALSE, FALSE, 0);
    gtk_table_attach(GTK_TABLE(t), hb, 1, 2, 2, 3, GTK_FILL | GTK_EXPAND, 0, 0, 0);
    gtk_container_add(GTK_CONTAINER(frame), t);
    p->g_editor = frame;
    gtk_box_pack_start(GTK_BOX(box), frame, FALSE, FALSE, 0);
    g_signal_connect(p->g_min, "value-changed", G_CALLBACK(on_graph_edit), p);
    g_signal_connect(p->g_max, "value-changed", G_CALLBACK(on_graph_edit), p);
    g_signal_connect(p->g_auto, "toggled", G_CALLBACK(on_graph_edit), p);
    g_signal_connect(p->g_color, "color-set", G_CALLBACK(on_graph_edit), p);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(p->gview)), "changed",
                     G_CALLBACK(on_graph_selected), p);
    fill_graphs(p, p->app->cfg.graphs->len ? p->app->cfg.graphs->pdata[0] : NULL);
    on_graph_selected(NULL, p);
    return box;
}

/* ---- columns ---------------------------------------------------------------------- */

static int shown_index(TskApp *app, const char *key)
{
    for (guint i = 0; i < app->cfg.columns->len; i++)
        if (!strcmp(g_array_index(app->cfg.columns, TskColumnCfg, i).key, key))
            return (int)i;
    return -1;
}

static void fill_columns(Prefs *p, const char *select)
{
    gtk_list_store_clear(p->cstore);
    TskApp *app = p->app;
    /* shown ones in their order, then the rest in catalog order */
    for (int pass = 0; pass < 2; pass++) {
        int n = pass ? tsk_ncolumns : (int)app->cfg.columns->len;
        for (int i = 0; i < n; i++) {
            const TskColumnDef *d =
                pass ? tsk_column(i) : tsk_column(tsk_column_index(g_array_index(app->cfg.columns, TskColumnCfg, i).key));
            if (pass && shown_index(app, d->key) >= 0)
                continue;
            GtkTreeIter it;
            gtk_list_store_insert_with_values(p->cstore, &it, -1, C_COL_ON, !pass, C_COL_TITLE, _(d->title), C_COL_TIP,
                                              _(d->tip), C_COL_KEY, d->key, -1);
            if (select && !strcmp(select, d->key))
                gtk_tree_selection_select_iter(gtk_tree_view_get_selection(GTK_TREE_VIEW(p->cview)), &it);
        }
    }
}

static void columns_changed(Prefs *p, const char *select)
{
    tsk_list_rebuild_columns(p->app);
    tsk_app_save_soon(p->app);
    tsk_app_refresh_now(p->app);
    char *keep = g_strdup(select);
    fill_columns(p, keep);
    g_free(keep);
}

static void on_column_toggled(GtkCellRendererToggle *r, char *path, Prefs *p)
{
    (void)r;
    GtkTreeIter it;
    char *key;
    gboolean on;
    if (!gtk_tree_model_get_iter_from_string(GTK_TREE_MODEL(p->cstore), &it, path))
        return;
    gtk_tree_model_get(GTK_TREE_MODEL(p->cstore), &it, C_COL_ON, &on, C_COL_KEY, &key, -1);
    GArray *a = p->app->cfg.columns;
    int idx = shown_index(p->app, key);
    if (on && idx >= 0 && a->len > 1) {
        g_array_remove_index(a, (guint)idx);
    } else if (!on && idx < 0) {
        TskColumnCfg c = {{0}, 0};
        g_strlcpy(c.key, key, sizeof(c.key));
        g_array_append_val(a, c);
    }
    columns_changed(p, key);
    g_free(key);
}

static void move_column(Prefs *p, int delta)
{
    GtkTreeIter it;
    char *key = NULL;
    if (!gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(p->cview)), NULL, &it))
        return;
    gtk_tree_model_get(GTK_TREE_MODEL(p->cstore), &it, C_COL_KEY, &key, -1);
    GArray *a = p->app->cfg.columns;
    int idx = shown_index(p->app, key), to = idx + delta;
    if (idx >= 0 && to >= 0 && to < (int)a->len) {
        TskColumnCfg tmp = g_array_index(a, TskColumnCfg, idx);
        g_array_index(a, TskColumnCfg, idx) = g_array_index(a, TskColumnCfg, to);
        g_array_index(a, TskColumnCfg, to) = tmp;
        columns_changed(p, key);
    }
    g_free(key);
}

static void on_col_up(GtkButton *b, Prefs *p)
{
    (void)b;
    move_column(p, -1);
}

static void on_col_down(GtkButton *b, Prefs *p)
{
    (void)b;
    move_column(p, 1);
}

static GtkWidget *columns_page(Prefs *p)
{
    GtkWidget *box = gtk_hbox_new(FALSE, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    p->cstore = gtk_list_store_new(C_NCOLS, G_TYPE_BOOLEAN, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    p->cview = gtk_tree_view_new_with_model(GTK_TREE_MODEL(p->cstore));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(p->cview), FALSE);
    gtk_tree_view_set_tooltip_column(GTK_TREE_VIEW(p->cview), C_COL_TIP);
    GtkCellRenderer *tr = gtk_cell_renderer_toggle_new();
    g_signal_connect(tr, "toggled", G_CALLBACK(on_column_toggled), p);
    gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(p->cview), -1, NULL, tr, "active", C_COL_ON, NULL);
    gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(p->cview), -1, NULL, gtk_cell_renderer_text_new(),
                                                "text", C_COL_TITLE, NULL);
    atk_object_set_name(gtk_widget_get_accessible(p->cview), _("Columns"));
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sw), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(sw), p->cview);
    gtk_box_pack_start(GTK_BOX(box), sw, TRUE, TRUE, 0);
    GtkWidget *btns = gtk_vbox_new(FALSE, 4);
    gtk_box_pack_start(GTK_BOX(btns), button(GTK_STOCK_GO_UP, NULL, G_CALLBACK(on_col_up), p), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(btns), button(GTK_STOCK_GO_DOWN, NULL, G_CALLBACK(on_col_down), p), FALSE, FALSE, 0);
    GtkWidget *hint = gtk_label_new(_("Columns can also be dragged by their header in the list, and right-clicking "
                                      "a header shows or hides them."));
    gtk_label_set_line_wrap(GTK_LABEL(hint), TRUE);
    gtk_widget_set_size_request(hint, 160, -1);
    gtk_box_pack_start(GTK_BOX(btns), hint, FALSE, FALSE, 8);
    gtk_box_pack_start(GTK_BOX(box), btns, FALSE, FALSE, 0);
    fill_columns(p, NULL);
    return box;
}

/* ---- window ----------------------------------------------------------------------- */

static void on_destroy(GtkWidget *w, Prefs *p)
{
    (void)w;
    if (p->add_menu)
        gtk_widget_destroy(p->add_menu);
    g_object_unref(p->gstore);
    g_object_unref(p->cstore);
    g_free(p);
    prefs = NULL;
}

void tsk_prefs_show(TskApp *app, int page)
{
    if (prefs) {
        gtk_notebook_set_current_page(GTK_NOTEBOOK(prefs->nb), page);
        gtk_window_present(GTK_WINDOW(prefs->win));
        return;
    }
    Prefs *p = prefs = g_new0(Prefs, 1);
    p->app = app;
    p->win = gtk_dialog_new_with_buttons(_("kitask Settings"), GTK_WINDOW(app->win), GTK_DIALOG_DESTROY_WITH_PARENT,
                                         GTK_STOCK_CLOSE, GTK_RESPONSE_CLOSE, NULL);
    gtk_window_set_default_size(GTK_WINDOW(p->win), 520, 480);
    p->nb = gtk_notebook_new();
    gtk_container_set_border_width(GTK_CONTAINER(p->nb), 6);
    gtk_notebook_append_page(GTK_NOTEBOOK(p->nb), general_page(p), gtk_label_new_with_mnemonic(_("_General")));
    gtk_notebook_append_page(GTK_NOTEBOOK(p->nb), graphs_page(p), gtk_label_new_with_mnemonic(_("G_raphs")));
    gtk_notebook_append_page(GTK_NOTEBOOK(p->nb), columns_page(p), gtk_label_new_with_mnemonic(_("_Columns")));
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(p->win)->vbox), p->nb, TRUE, TRUE, 0);
    g_signal_connect(p->win, "response", G_CALLBACK(gtk_widget_destroy), NULL);
    g_signal_connect(p->win, "destroy", G_CALLBACK(on_destroy), p);
    gtk_widget_show_all(p->win);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(p->nb), page);
}
