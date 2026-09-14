/* kiconf - Paineis tab: xispanel.conf editor + xispanel-ctl RELOAD.
 * See kiconf.c's top doc comment for the overall design. */
#include "../common.h"
#include "../tabs.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

/* Paineis tab (xispanel.conf) widgets + state */
static GtkListStore *g_panels_store;
static GtkListStore *g_widgets_store;
static GtkWidget *g_theme_options_entry;
static char g_selected_panel[NAME_LEN] = "";

enum { COL_PANEL_NAME = 0, COL_PANEL_OUTPUT, COL_PANEL_OPTIONS, N_PANEL_COLS };
enum { COL_WIDGET_PANEL = 0, COL_WIDGET_TYPE, COL_WIDGET_OPTIONS, N_WIDGET_COLS };

/* ---- Paineis tab: xispanel.conf editor + xispanel-ctl RELOAD ---------- */

#define MAX_PANELS 32
#define MAX_WIDGETS 256
typedef struct {
    char name[NAME_LEN], output[NAME_LEN], options[512];
} PanelRec;
typedef struct {
    char panel[NAME_LEN], type[64], options[512];
} WidgetRec;
typedef struct {
    char panel[NAME_LEN], options[512];
} ThemeRec;

static void load_xispanel_conf(PanelRec *panels, int *n_panels, WidgetRec *widgets, int *n_widgets,
                                 ThemeRec *themes, int *n_themes)
{
    *n_panels = *n_widgets = *n_themes = 0;
    char path[PATH_MAX];
    resolve_path("xispanel.conf", path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) {
        return;
    }
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        char *l = line;
        size_t len = strlen(l);
        while (len > 0 && (l[len - 1] == '\n' || l[len - 1] == '\r')) {
            l[--len] = '\0';
        }
        char *t = skip_ws(l);
        if (!*t || *t == '#') {
            continue;
        }
        char *cursor = t;
        char *tag = next_field(&cursor);
        if (!tag) {
            continue;
        }
        if (!strcmp(tag, "PANEL") && *n_panels < MAX_PANELS) {
            char *name = next_field(&cursor);
            char *output = next_field(&cursor);
            if (!name || !output) {
                continue;
            }
            PanelRec *r = &panels[(*n_panels)++];
            snprintf(r->name, sizeof(r->name), "%s", name);
            snprintf(r->output, sizeof(r->output), "%s", output);
            snprintf(r->options, sizeof(r->options), "%s", skip_ws(cursor));
        } else if (!strcmp(tag, "WIDGET") && *n_widgets < MAX_WIDGETS) {
            char *panel = next_field(&cursor);
            char *order = next_field(&cursor); /* positional only, see PROTOCOL.md */
            char *type = next_field(&cursor);
            if (!panel || !order || !type) {
                continue;
            }
            WidgetRec *r = &widgets[(*n_widgets)++];
            snprintf(r->panel, sizeof(r->panel), "%s", panel);
            snprintf(r->type, sizeof(r->type), "%s", type);
            snprintf(r->options, sizeof(r->options), "%s", skip_ws(cursor));
        } else if (!strcmp(tag, "THEME") && *n_themes < MAX_PANELS) {
            char *panel = next_field(&cursor);
            if (!panel) {
                continue;
            }
            ThemeRec *r = &themes[(*n_themes)++];
            snprintf(r->panel, sizeof(r->panel), "%s", panel);
            snprintf(r->options, sizeof(r->options), "%s", skip_ws(cursor));
        }
    }
    fclose(f);
}

static void save_xispanel_conf(void)
{
    char path[PATH_MAX];
    resolve_path("xispanel.conf", path, sizeof(path));
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) {
        g_warning("kiconf: could not write '%s': %s", tmp, strerror(errno));
        return;
    }

    GtkTreeIter it;
    gboolean valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(g_panels_store), &it);
    while (valid) {
        gchar *name, *output, *opts;
        gtk_tree_model_get(GTK_TREE_MODEL(g_panels_store), &it,
                            COL_PANEL_NAME, &name, COL_PANEL_OUTPUT, &output, COL_PANEL_OPTIONS, &opts, -1);
        if (name && *name) {
            fprintf(f, "PANEL\t%s\t%s\t%s\n", name, output && *output ? output : "*", opts ? opts : "");
        }
        g_free(name);
        g_free(output);
        g_free(opts);
        valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(g_panels_store), &it);
    }

    /* order is positional-only (see PROTOCOL.md), so it's just recomputed
     * here as each panel's 0-based count in file-write order, not edited
     * directly anywhere in the UI. */
    GHashTable *counters = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(g_widgets_store), &it);
    while (valid) {
        gchar *panel, *type, *opts;
        gtk_tree_model_get(GTK_TREE_MODEL(g_widgets_store), &it,
                            COL_WIDGET_PANEL, &panel, COL_WIDGET_TYPE, &type, COL_WIDGET_OPTIONS, &opts, -1);
        if (panel && *panel && type && *type) {
            gpointer countp = g_hash_table_lookup(counters, panel);
            int count = countp ? GPOINTER_TO_INT(countp) : 0;
            fprintf(f, "WIDGET\t%s\t%d\t%s\t%s\n", panel, count, type, opts ? opts : "");
            g_hash_table_insert(counters, g_strdup(panel), GINT_TO_POINTER(count + 1));
        }
        g_free(panel);
        g_free(type);
        g_free(opts);
        valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(g_widgets_store), &it);
    }
    g_hash_table_destroy(counters);

    char *theme_opts = gtk_editable_get_chars(GTK_EDITABLE(g_theme_options_entry), 0, -1);
    if (g_selected_panel[0] && theme_opts && theme_opts[0]) {
        fprintf(f, "THEME\t%s\t%s\n", g_selected_panel, theme_opts);
    }
    g_free(theme_opts);

    fclose(f);
    rename(tmp, path);
}

static void save_panels_cb(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    save_xispanel_conf();
    xispanel_reload();
}

static void add_panel_cb(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    GtkTreeIter it;
    gtk_list_store_append(g_panels_store, &it);
    gtk_list_store_set(g_panels_store, &it,
                        COL_PANEL_NAME, "novo-painel", COL_PANEL_OUTPUT, "*",
                        COL_PANEL_OPTIONS, "edge=top pct=100 thickness=32 mode=dock", -1);
}

static void remove_panel_cb(GtkWidget *widget, gpointer data)
{
    GtkTreeView *view = GTK_TREE_VIEW(data);
    (void)widget;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(view);
    GtkTreeIter it;
    if (gtk_tree_selection_get_selected(sel, NULL, &it)) {
        gtk_list_store_remove(g_panels_store, &it);
    }
}

static void panel_cell_edited(GtkCellRendererText *cell, gchar *path_str, gchar *new_text, gpointer data)
{
    (void)cell;
    gint col = GPOINTER_TO_INT(data);
    GtkTreePath *path = gtk_tree_path_new_from_string(path_str);
    GtkTreeIter it;
    if (gtk_tree_model_get_iter(GTK_TREE_MODEL(g_panels_store), &it, path)) {
        gtk_list_store_set(g_panels_store, &it, col, new_text, -1);
    }
    gtk_tree_path_free(path);
}

static void add_widget_cb(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    GtkTreeIter it;
    gtk_list_store_append(g_widgets_store, &it);
    gtk_list_store_set(g_widgets_store, &it,
                        COL_WIDGET_PANEL, g_selected_panel[0] ? g_selected_panel : "novo-painel",
                        COL_WIDGET_TYPE, "spacer", COL_WIDGET_OPTIONS, "", -1);
}

static void remove_widget_cb(GtkWidget *widget, gpointer data)
{
    GtkTreeView *view = GTK_TREE_VIEW(data);
    (void)widget;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(view);
    GtkTreeIter it;
    if (gtk_tree_selection_get_selected(sel, NULL, &it)) {
        gtk_list_store_remove(g_widgets_store, &it);
    }
}

static void widget_cell_edited(GtkCellRendererText *cell, gchar *path_str, gchar *new_text, gpointer data)
{
    (void)cell;
    gint col = GPOINTER_TO_INT(data);
    GtkTreePath *path = gtk_tree_path_new_from_string(path_str);
    GtkTreeIter it;
    if (gtk_tree_model_get_iter(GTK_TREE_MODEL(g_widgets_store), &it, path)) {
        gtk_list_store_set(g_widgets_store, &it, col, new_text, -1);
    }
    gtk_tree_path_free(path);
}

static GtkWidget *build_editable_list(GtkListStore *store, int ncols, const char *const *titles)
{
    GtkWidget *view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    for (int col = 0; col < ncols; col++) {
        GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
        g_object_set(renderer, "editable", TRUE, NULL);
        const char *sig = "edited";
        GCallback cb = store == g_panels_store ? G_CALLBACK(panel_cell_edited) : G_CALLBACK(widget_cell_edited);
        g_signal_connect(renderer, sig, cb, GINT_TO_POINTER(col));
        GtkTreeViewColumn *tvcol = gtk_tree_view_column_new_with_attributes(titles[col], renderer, "text", col, NULL);
        gtk_tree_view_column_set_expand(tvcol, TRUE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(view), tvcol);
    }
    return view;
}

GtkWidget *build_paineis_tab(void)
{
    PanelRec panels[MAX_PANELS];
    WidgetRec widgets[MAX_WIDGETS];
    ThemeRec themes[MAX_PANELS];
    int n_panels, n_widgets, n_themes;
    load_xispanel_conf(panels, &n_panels, widgets, &n_widgets, themes, &n_themes);

    GtkWidget *outer = gtk_vbox_new(FALSE, 8);
    gtk_container_set_border_width(GTK_CONTAINER(outer), 12);

    GtkWidget *note = gtk_label_new(
        "Editor direto de xispanel.conf (PANEL/WIDGET/THEME). Opcoes sao\n"
        "a string key=value bruta de cada linha -- ver xispanel/PROTOCOL.md.");
    gtk_misc_set_alignment(GTK_MISC(note), 0.0, 0.5);
    gtk_box_pack_start(GTK_BOX(outer), note, FALSE, FALSE, 0);

    g_panels_store = gtk_list_store_new(N_PANEL_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    for (int i = 0; i < n_panels; i++) {
        GtkTreeIter it;
        gtk_list_store_append(g_panels_store, &it);
        gtk_list_store_set(g_panels_store, &it,
                            COL_PANEL_NAME, panels[i].name, COL_PANEL_OUTPUT, panels[i].output,
                            COL_PANEL_OPTIONS, panels[i].options, -1);
    }
    const char *panel_titles[N_PANEL_COLS] = {"Nome", "Output", "Opcoes"};
    GtkWidget *panels_view = build_editable_list(g_panels_store, N_PANEL_COLS, panel_titles);
    GtkWidget *panels_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(panels_scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(panels_scroll, -1, 100);
    gtk_container_add(GTK_CONTAINER(panels_scroll), panels_view);
    GtkWidget *panels_box = gtk_vbox_new(FALSE, 4);
    gtk_box_pack_start(GTK_BOX(panels_box), panels_scroll, TRUE, TRUE, 0);
    GtkWidget *panels_btnbox = gtk_hbox_new(FALSE, 6);
    GtkWidget *padd = gtk_button_new_with_label("Adicionar painel");
    GtkWidget *prem = gtk_button_new_with_label("Remover painel");
    g_signal_connect(padd, "clicked", G_CALLBACK(add_panel_cb), NULL);
    g_signal_connect(prem, "clicked", G_CALLBACK(remove_panel_cb), panels_view);
    gtk_box_pack_start(GTK_BOX(panels_btnbox), padd, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(panels_btnbox), prem, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(panels_box), panels_btnbox, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), frame_with("Paineis", panels_box), FALSE, FALSE, 0);

    g_widgets_store = gtk_list_store_new(N_WIDGET_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    for (int i = 0; i < n_widgets; i++) {
        GtkTreeIter it;
        gtk_list_store_append(g_widgets_store, &it);
        gtk_list_store_set(g_widgets_store, &it,
                            COL_WIDGET_PANEL, widgets[i].panel, COL_WIDGET_TYPE, widgets[i].type,
                            COL_WIDGET_OPTIONS, widgets[i].options, -1);
    }
    const char *widget_titles[N_WIDGET_COLS] = {"Painel", "Tipo", "Opcoes"};
    GtkWidget *widgets_view = build_editable_list(g_widgets_store, N_WIDGET_COLS, widget_titles);
    GtkWidget *widgets_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(widgets_scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(widgets_scroll, -1, 140);
    gtk_container_add(GTK_CONTAINER(widgets_scroll), widgets_view);
    GtkWidget *widgets_box = gtk_vbox_new(FALSE, 4);
    gtk_box_pack_start(GTK_BOX(widgets_box), widgets_scroll, TRUE, TRUE, 0);
    GtkWidget *widgets_btnbox = gtk_hbox_new(FALSE, 6);
    GtkWidget *wadd = gtk_button_new_with_label("Adicionar widget");
    GtkWidget *wrem = gtk_button_new_with_label("Remover widget");
    g_signal_connect(wadd, "clicked", G_CALLBACK(add_widget_cb), NULL);
    g_signal_connect(wrem, "clicked", G_CALLBACK(remove_widget_cb), widgets_view);
    gtk_box_pack_start(GTK_BOX(widgets_btnbox), wadd, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(widgets_btnbox), wrem, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(widgets_box), widgets_btnbox, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), frame_with("Widgets (coluna Painel = nome do painel dono)", widgets_box), TRUE, TRUE, 0);

    /* Theme editing is intentionally reduced to "one raw options string
     * for the first panel found with a THEME line" rather than a proper
     * per-panel selector -- xispanel already defaults to the live system
     * theme when no THEME line exists at all (see PROTOCOL.md), so most
     * setups will simply leave this blank. */
    if (n_themes > 0) {
        snprintf(g_selected_panel, sizeof(g_selected_panel), "%s", themes[0].panel);
    } else if (n_panels > 0) {
        snprintf(g_selected_panel, sizeof(g_selected_panel), "%s", panels[0].name);
    }
    GtkWidget *theme_table = gtk_table_new(1, 2, FALSE);
    g_theme_options_entry = gtk_entry_new();
    if (n_themes > 0) {
        gtk_entry_set_text(GTK_ENTRY(g_theme_options_entry), themes[0].options);
    }
    char theme_label_text[NAME_LEN + 16];
    snprintf(theme_label_text, sizeof(theme_label_text), "THEME de '%s':", g_selected_panel[0] ? g_selected_panel : "?");
    labeled_row(theme_table, 0, theme_label_text, g_theme_options_entry);
    gtk_box_pack_start(GTK_BOX(outer), frame_with("Tema (cores/fonte do painel)", theme_table), FALSE, FALSE, 0);

    GtkWidget *save_btn = gtk_button_new_with_label("Salvar e recarregar xispanel");
    g_signal_connect(save_btn, "clicked", G_CALLBACK(save_panels_cb), NULL);
    GtkWidget *save_btnbox = gtk_hbox_new(FALSE, 0);
    gtk_box_pack_end(GTK_BOX(save_btnbox), save_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), save_btnbox, FALSE, FALSE, 0);

    return outer;
}
