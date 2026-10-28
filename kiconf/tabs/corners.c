/* kiconf - hot corners (cantos ativos) section of the Efeitos tab.
 *
 * Reads and writes kicomp.conf's [hotcorners] / [hotcorners:MONITOR]
 * sections. Not a tab of its own: kicomp.conf is rewritten as a whole by
 * the Efeitos tab's "Aplicar", so this contributes a frame to that tab
 * (corners_build) and the text of its sections to the same write
 * (corners_write).
 *
 * One row per monitor plus a "Todas as telas" row that is the default.
 * A monitor's cell left on "(padrao)" is not written at all, so it keeps
 * following the default row. Monitors named in the file but not connected
 * right now keep their row, so unplugging a screen doesn't lose its
 * settings. */
#include "../common.h"
#include "../tabs.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KICOMP_CONF "kicomp.conf"
#define N_CORNERS 4
#define ROW_DEFAULT_NAME ""   /* the "Todas as telas" row */

static const char *const CORNER_KEYS[N_CORNERS] = {
    "top-left", "top-right", "bottom-left", "bottom-right",
};
static const char *const CORNER_TITLES[N_CORNERS] = {
    "Superior esq.", "Superior dir.", "Inferior esq.", "Inferior dir.",
};

/* What a cell can say. Index 0 is "follow the default"; for the default
 * row itself it means the same as "nenhum". */
static const char *const ACTION_VALUES[] = {
    "", "none", "expo", "show-windows", "cover-switch", NULL,
};
static const char *const ACTION_LABELS[] = {
    "(padrao)", "nenhum", "expo", "show-windows", "cover-switch", NULL,
};

enum { COL_NAME = 0, COL_LABEL, COL_C0, N_COLS = COL_C0 + N_CORNERS };

static GtkListStore *g_store;
static GtkWidget *g_delay_spin, *g_size_spin;

static const char *label_of(const char *value)
{
    for (int i = 0; ACTION_VALUES[i]; i++)
        if (!strcmp(ACTION_VALUES[i], value))
            return ACTION_LABELS[i];
    return value;   /* an effect name this build doesn't list: keep it */
}

static const char *value_of(const char *label)
{
    for (int i = 0; ACTION_LABELS[i]; i++)
        if (!strcmp(ACTION_LABELS[i], label))
            return ACTION_VALUES[i];
    return label;
}

static gboolean find_row(const char *name, GtkTreeIter *it)
{
    GtkTreeModel *m = GTK_TREE_MODEL(g_store);
    gboolean ok = gtk_tree_model_get_iter_first(m, it);
    while (ok) {
        gchar *n;
        gtk_tree_model_get(m, it, COL_NAME, &n, -1);
        gboolean hit = n && !strcmp(n, name);
        g_free(n);
        if (hit)
            return TRUE;
        ok = gtk_tree_model_iter_next(m, it);
    }
    return FALSE;
}

static void add_row(const char *name, const char *label)
{
    GtkTreeIter it;
    if (find_row(name, &it))
        return;
    gtk_list_store_append(g_store, &it);
    gtk_list_store_set(g_store, &it, COL_NAME, name, COL_LABEL, label, -1);
    for (int c = 0; c < N_CORNERS; c++)
        gtk_list_store_set(g_store, &it, COL_C0 + c, ACTION_LABELS[0], -1);
}

static void set_cell(const char *section, const char *key, const char *val)
{
    GtkTreeIter it;
    if (section[0])
        add_row(section, section);
    if (!find_row(section, &it))
        return;
    for (int c = 0; c < N_CORNERS; c++)
        if (!strcmp(key, CORNER_KEYS[c])) {
            /* "off" is accepted by kicomp as a spelling of "none". */
            const char *v = !strcmp(val, "off") ? "none" : val;
            gtk_list_store_set(g_store, &it, COL_C0 + c, label_of(v), -1);
        }
}

static void load_conf(int *delay, int *size)
{
    char path[PATH_MAX];
    resolve_path(KICOMP_CONF, path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f)
        return;

    char section[64] = "";
    int in_hc = 0;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *l = trim(line);
        if (!*l || *l == '#')
            continue;
        if (*l == '[') {
            char *close = strchr(l, ']');
            if (!close)
                continue;
            *close = '\0';
            const char *name = l + 1;
            in_hc = !strcmp(name, "hotcorners") || !strncmp(name, "hotcorners:", 11);
            snprintf(section, sizeof(section), "%s", in_hc && name[10] == ':' ? name + 11 : "");
            continue;
        }
        if (!in_hc)
            continue;
        char *eq = strchr(l, '=');
        if (!eq)
            continue;
        *eq = '\0';
        char *key = trim(l);
        char *val = trim(eq + 1);
        /* A trailing comment ends the value, as in kicomp. */
        for (char *c = val; *c; c++)
            if (*c == '#' && c > val && (c[-1] == ' ' || c[-1] == '\t')) {
                *c = '\0';
                break;
            }
        val = trim(val);

        if (!section[0] && !strcmp(key, "delay"))
            *delay = atoi(val);
        else if (!section[0] && !strcmp(key, "size"))
            *size = atoi(val);
        else
            set_cell(section, key, val);
    }
    fclose(f);
}

static void cell_changed(GtkCellRendererCombo *r, gchar *path, GtkTreeIter *new_iter,
                         gpointer data)
{
    (void)r;
    int corner = GPOINTER_TO_INT(data);
    GtkTreeModel *combo_model;
    g_object_get(r, "model", &combo_model, NULL);
    gchar *text = NULL;
    gtk_tree_model_get(combo_model, new_iter, 0, &text, -1);
    GtkTreeIter it;
    if (text && gtk_tree_model_get_iter_from_string(GTK_TREE_MODEL(g_store), &it, path))
        gtk_list_store_set(g_store, &it, COL_C0 + corner, text, -1);
    g_free(text);
    g_object_unref(combo_model);
}

GtkWidget *corners_build(void)
{
    g_store = gtk_list_store_new(N_COLS, G_TYPE_STRING, G_TYPE_STRING,
                                 G_TYPE_STRING, G_TYPE_STRING,
                                 G_TYPE_STRING, G_TYPE_STRING);
    add_row(ROW_DEFAULT_NAME, _("Todas as telas"));

    /* The monitors as kicomp names them: RandR output names. */
    GdkScreen *screen = gdk_screen_get_default();
    for (int i = 0; screen && i < gdk_screen_get_n_monitors(screen); i++) {
        gchar *plug = gdk_screen_get_monitor_plug_name(screen, i);
        if (plug && *plug)
            add_row(plug, plug);
        g_free(plug);
    }

    int delay = 150, size = 1;
    load_conf(&delay, &size);

    GtkListStore *opts = gtk_list_store_new(1, G_TYPE_STRING);
    for (int i = 0; ACTION_LABELS[i]; i++) {
        GtkTreeIter it;
        gtk_list_store_append(opts, &it);
        gtk_list_store_set(opts, &it, 0, ACTION_LABELS[i], -1);
    }

    GtkWidget *view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(g_store));
    gtk_tree_view_append_column(GTK_TREE_VIEW(view),
        gtk_tree_view_column_new_with_attributes(_("Tela"), gtk_cell_renderer_text_new(),
                                                 "text", COL_LABEL, NULL));
    for (int c = 0; c < N_CORNERS; c++) {
        GtkCellRenderer *r = gtk_cell_renderer_combo_new();
        g_object_set(r, "model", opts, "text-column", 0, "editable", TRUE,
                     "has-entry", FALSE, NULL);
        g_signal_connect(r, "changed", G_CALLBACK(cell_changed), GINT_TO_POINTER(c));
        gtk_tree_view_append_column(GTK_TREE_VIEW(view),
            gtk_tree_view_column_new_with_attributes(_(CORNER_TITLES[c]), r,
                                                     "text", COL_C0 + c, NULL));
    }
    a11y_name(view, _("Cantos ativos por tela"));

    GtkWidget *box = gtk_vbox_new(FALSE, 6);
    GtkWidget *hint = gtk_label_new(_("Encostar o mouse no canto da tela dispara o efeito. "
        "\"(padrao)\" segue a linha \"Todas as telas\"."));
    gtk_misc_set_alignment(GTK_MISC(hint), 0.0, 0.5);
    gtk_label_set_line_wrap(GTK_LABEL(hint), TRUE);
    gtk_box_pack_start(GTK_BOX(box), hint, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), view, FALSE, FALSE, 0);

    GtkWidget *table = gtk_table_new(2, 2, FALSE);
    g_delay_spin = gtk_spin_button_new_with_range(0, 2000, 10);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_delay_spin), delay);
    labeled_row(table, 0, _("Atraso (ms):"), g_delay_spin);
    gtk_widget_set_tooltip_text(g_delay_spin,
        _("Tempo que o mouse precisa ficar no canto. Passar de raspao nao dispara."));
    g_size_spin = gtk_spin_button_new_with_range(1, 32, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_size_spin), size);
    labeled_row(table, 1, _("Tamanho do canto (px):"), g_size_spin);
    gtk_widget_set_tooltip_text(g_size_spin,
        _("O pixel do canto deixa de chegar a janela embaixo (botao fechar de janela maximizada)."));
    gtk_box_pack_start(GTK_BOX(box), table, FALSE, FALSE, 0);

    return frame_with(_("Cantos ativos"), box);
}

/* The value a cell stands for, as it goes into the file. "" = write
 * nothing: a monitor on "(padrao)" follows the default row, and the
 * default row's own "(padrao)" and "nenhum" both mean no corner. */
static const char *cell_value(GtkTreeModel *m, GtkTreeIter *it, int corner, int is_default)
{
    static gchar *keep[N_CORNERS];   /* value_of() may hand the label back */
    g_free(keep[corner]);
    gtk_tree_model_get(m, it, COL_C0 + corner, &keep[corner], -1);
    const char *v = value_of(keep[corner] ? keep[corner] : "");
    if (is_default && !strcmp(v, "none"))
        return "";
    return v;
}

/* Writes the [hotcorners*] sections. Nothing at all while every cell is on
 * its default, so a config that never used the feature stays that way. */
void corners_write(FILE *out)
{
    GtkTreeModel *m = GTK_TREE_MODEL(g_store);
    GtkTreeIter it;
    int delay = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_delay_spin));
    int size = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_size_spin));

    int used = delay != 150 || size != 1;
    for (gboolean ok = gtk_tree_model_get_iter_first(m, &it); ok;
         ok = gtk_tree_model_iter_next(m, &it)) {
        gchar *name;
        gtk_tree_model_get(m, &it, COL_NAME, &name, -1);
        int is_default = !name || !*name;
        for (int c = 0; c < N_CORNERS; c++)
            if (*cell_value(m, &it, c, is_default))
                used = 1;
        g_free(name);
    }
    if (!used)
        return;

    /* The default row is always first in the store. */
    for (gboolean ok = gtk_tree_model_get_iter_first(m, &it); ok;
         ok = gtk_tree_model_iter_next(m, &it)) {
        gchar *name;
        gtk_tree_model_get(m, &it, COL_NAME, &name, -1);
        int is_default = !name || !*name;

        const char *val[N_CORNERS];
        int n = 0;
        for (int c = 0; c < N_CORNERS; c++) {
            val[c] = cell_value(m, &it, c, is_default);
            if (*val[c])
                n++;
        }
        if (is_default || n) {
            if (is_default)
                fprintf(out, "\n[hotcorners]\n");
            else
                fprintf(out, "\n[hotcorners:%s]\n", name);
            for (int c = 0; c < N_CORNERS; c++)
                if (*val[c])
                    fprintf(out, "%s=%s\n", CORNER_KEYS[c], val[c]);
            if (is_default)
                fprintf(out, "delay=%d\nsize=%d\n", delay, size);
        }
        g_free(name);
    }
}
