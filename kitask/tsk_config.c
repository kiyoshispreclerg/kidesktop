/* tsk_config.c - ~/.config/kitask.conf, "key = value" lines.
 *
 *   columns = name:220,pid,user,cpu:70      visible columns in order, :width optional
 *   graph = cpu min=0 max=100 color=#3c7dd9 one line per graph, in order
 *   graph = ram.apps min=0 max=auto
 *
 * Graph limits are in the source's unit: % for CPU/GPU, MiB for memory and
 * KiB/s for traffic. Written whenever a setting changes (debounced). */
#include "tsk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HIST_LEN 2048

static const char *const default_columns[] = {"name:220", "pid:70", "user:90", "cpu:70", "uss:100", "rss:100",
                                              "title:0"};

static char *conf_path(void)
{
    const char *xdg = g_getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg)
        return g_build_filename(xdg, "kitask.conf", NULL);
    return g_build_filename(g_get_home_dir(), ".config", "kitask.conf", NULL);
}

TskGraph *tsk_graph_new(const char *source)
{
    TskGraph *g = g_new0(TskGraph, 1);
    g->source = g_strdup(source);
    g->hist_len = HIST_LEN;
    g->hist = g_new0(double, HIST_LEN);
    tsk_graph_source_defaults(source, &g->color, &g->max, &g->auto_max);
    return g;
}

void tsk_graph_free(TskGraph *g)
{
    g_free(g->source);
    g_free(g->hist);
    g_free(g);
}

static void parse_columns(TskConfig *c, const char *val)
{
    g_array_set_size(c->columns, 0);
    char **items = g_strsplit(val, ",", -1);
    for (char **i = items; *i; i++) {
        TskColumnCfg col = {{0}, 0};
        char *colon = strchr(*i, ':');
        if (colon) {
            col.width = atoi(colon + 1);
            *colon = '\0';
        }
        g_strstrip(*i);
        if (tsk_column_index(*i) < 0)
            continue;
        g_strlcpy(col.key, *i, sizeof(col.key));
        g_array_append_val(c->columns, col);
    }
    g_strfreev(items);
}

static void parse_graph(TskConfig *c, const char *val)
{
    char **parts = g_strsplit_set(val, " \t", -1);
    TskGraph *g = NULL;
    for (char **p = parts; *p; p++) {
        if (!**p)
            continue;
        if (!g) {
            g = tsk_graph_new(*p);
            continue;
        }
        if (!strncmp(*p, "min=", 4)) {
            g->min = g_ascii_strtod(*p + 4, NULL);
        } else if (!strncmp(*p, "max=", 4)) {
            g->auto_max = !strcmp(*p + 4, "auto");
            if (!g->auto_max)
                g->max = g_ascii_strtod(*p + 4, NULL);
        } else if (!strncmp(*p, "color=", 6)) {
            gdk_color_parse(*p + 6, &g->color);
        }
    }
    g_strfreev(parts);
    if (g)
        g_ptr_array_add(c->graphs, g);
}

static void default_graphs(TskConfig *c)
{
    g_ptr_array_add(c->graphs, tsk_graph_new("cpu"));
    g_ptr_array_add(c->graphs, tsk_graph_new("ram.apps"));
    for (int i = 0; i < tsk_gpu_count(); i++) {
        TskGpu *gpu = tsk_gpu_get(i);
        if (!gpu->has_busy)
            continue;
        char *s = g_strdup_printf("gpu@%s", gpu->pci);
        g_ptr_array_add(c->graphs, tsk_graph_new(s));
        g_free(s);
        if (gpu->has_vram) {
            s = g_strdup_printf("vram@%s", gpu->pci);
            g_ptr_array_add(c->graphs, tsk_graph_new(s));
            g_free(s);
        }
        break;
    }
}

void tsk_config_load(TskConfig *c)
{
    memset(c, 0, sizeof(*c));
    c->list_interval = 2.0;
    c->graph_interval = 1.0;
    c->show_graphs = TRUE;
    c->graph_height = 80;
    c->graphs_per_row = 4;
    c->confirm_signals = TRUE;
    c->hide_kthreads = TRUE;
    c->scope = TSK_SCOPE_ALL;
    g_strlcpy(c->sort_key, "cpu", sizeof(c->sort_key));
    c->sort_desc = TRUE;
    c->columns = g_array_new(FALSE, FALSE, sizeof(TskColumnCfg));
    c->graphs = g_ptr_array_new_with_free_func((GDestroyNotify)tsk_graph_free);
    c->win_w = 900;
    c->win_h = 650;
    c->details_w = 560;
    c->details_h = 520;

    gboolean have_columns = FALSE, have_graphs = FALSE;
    char *path = conf_path(), *data = NULL;
    if (g_file_get_contents(path, &data, NULL, NULL)) {
        char **lines = g_strsplit(data, "\n", -1);
        for (char **l = lines; *l; l++) {
            char *eq = strchr(*l, '=');
            if (!eq || **l == '#')
                continue;
            *eq = '\0';
            char *key = g_strstrip(*l), *val = g_strstrip(eq + 1);
            if (!strcmp(key, "list_interval"))
                c->list_interval = CLAMP(g_ascii_strtod(val, NULL), 0.25, 60);
            else if (!strcmp(key, "graph_interval"))
                c->graph_interval = CLAMP(g_ascii_strtod(val, NULL), 0.1, 60);
            else if (!strcmp(key, "show_graphs"))
                c->show_graphs = atoi(val) != 0;
            else if (!strcmp(key, "graph_height"))
                c->graph_height = CLAMP(atoi(val), 30, 400);
            else if (!strcmp(key, "graphs_per_row"))
                c->graphs_per_row = CLAMP(atoi(val), 1, 16);
            else if (!strcmp(key, "confirm_signals"))
                c->confirm_signals = atoi(val) != 0;
            else if (!strcmp(key, "hide_kernel_threads"))
                c->hide_kthreads = atoi(val) != 0;
            else if (!strcmp(key, "scope"))
                c->scope = !strcmp(val, "mine") ? TSK_SCOPE_MINE : !strcmp(val, "windows") ? TSK_SCOPE_WINDOWS
                                                                                            : TSK_SCOPE_ALL;
            else if (!strcmp(key, "sort")) {
                char k[16] = "", dir[8] = "";
                if (sscanf(val, "%15s %7s", k, dir) >= 1 && tsk_column_index(k) >= 0) {
                    g_strlcpy(c->sort_key, k, sizeof(c->sort_key));
                    c->sort_desc = strcmp(dir, "asc") != 0;
                }
            } else if (!strcmp(key, "columns")) {
                parse_columns(c, val);
                have_columns = TRUE;
            } else if (!strcmp(key, "graphs")) {
                have_graphs = TRUE;   /* "graphs = none": the user removed them all */
            } else if (!strcmp(key, "graph")) {
                parse_graph(c, val);
                have_graphs = TRUE;
            } else if (!strcmp(key, "window"))
                sscanf(val, "%dx%d", &c->win_w, &c->win_h);
            else if (!strcmp(key, "details_window"))
                sscanf(val, "%dx%d", &c->details_w, &c->details_h);
        }
        g_strfreev(lines);
        g_free(data);
    }
    g_free(path);
    if (!have_columns || !c->columns->len)
        parse_columns(c, "name");
    if (!have_columns) {
        char *joined = g_strjoinv(",", (char **)default_columns);
        parse_columns(c, joined);
        g_free(joined);
    }
    if (!have_graphs)
        default_graphs(c);
}

static void put_num(GString *s, double v)
{
    char buf[G_ASCII_DTOSTR_BUF_SIZE];
    g_string_append(s, g_ascii_formatd(buf, sizeof(buf), "%.6g", v));
}

void tsk_config_save(const TskConfig *c)
{
    GString *s = g_string_new("# kitask settings, written by kitask\n");
    g_string_append(s, "list_interval = ");
    put_num(s, c->list_interval);
    g_string_append(s, "\ngraph_interval = ");
    put_num(s, c->graph_interval);
    g_string_append_printf(s, "\nshow_graphs = %d\ngraph_height = %d\ngraphs_per_row = %d\n", c->show_graphs,
                           c->graph_height, c->graphs_per_row);
    g_string_append_printf(s, "confirm_signals = %d\nhide_kernel_threads = %d\n", c->confirm_signals,
                           c->hide_kthreads);
    g_string_append_printf(s, "scope = %s\n",
                           c->scope == TSK_SCOPE_MINE ? "mine" : c->scope == TSK_SCOPE_WINDOWS ? "windows" : "all");
    g_string_append_printf(s, "sort = %s %s\n", c->sort_key, c->sort_desc ? "desc" : "asc");
    g_string_append(s, "columns = ");
    for (guint i = 0; i < c->columns->len; i++) {
        const TskColumnCfg *col = &g_array_index(c->columns, TskColumnCfg, i);
        g_string_append_printf(s, "%s%s:%d", i ? "," : "", col->key, col->width);
    }
    g_string_append_c(s, '\n');
    if (!c->graphs->len)
        g_string_append(s, "graphs = none\n");
    for (guint i = 0; i < c->graphs->len; i++) {
        const TskGraph *g = c->graphs->pdata[i];
        g_string_append_printf(s, "graph = %s min=", g->source);
        put_num(s, g->min);
        g_string_append(s, " max=");
        if (g->auto_max)
            g_string_append(s, "auto");
        else
            put_num(s, g->max);
        g_string_append_printf(s, " color=#%02x%02x%02x\n", g->color.red >> 8, g->color.green >> 8,
                               g->color.blue >> 8);
    }
    g_string_append_printf(s, "window = %dx%d\ndetails_window = %dx%d\n", c->win_w, c->win_h, c->details_w,
                           c->details_h);

    char *path = conf_path(), *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0755);
    g_file_set_contents(path, s->str, -1, NULL);
    g_free(dir);
    g_free(path);
    g_string_free(s, TRUE);
}
