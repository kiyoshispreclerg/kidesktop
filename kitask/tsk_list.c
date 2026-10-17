/* tsk_list.c - the process list: column catalog, model, filter, sorting.
 *
 * One GtkListStore row per process, kept across refreshes (pid -> iter in
 * app->rows) so selection and scroll survive; a filter model on top for the
 * search entry and the scope combo, a sort model on top of that. Numbers are
 * stored raw (doubles, -1 = unknown) and only formatted in the cell data
 * functions. Columns use fixed sizing and fixed-height mode: GTK then never
 * measures every row, which is what makes a few hundred rows refreshing every
 * second cheap. */
#include "tsk.h"
#include "../shared/xis_i18n.h"
#include <gdk/gdkkeysyms.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { COL_PID, COL_ICON, COL_FIRST };

/* flags */
#define BLANK_UNKNOWN 1     /* -1 means "doesn't use it" here: show nothing, not a dash */
#define BLANK_ZERO 2        /* idle is the common case: 0 shows nothing */

typedef struct {
    TskColumnDef d;
    int flags;
    int width;              /* default width */
} ColumnInfo;

static const ColumnInfo columns[] = {
    {{"name", N_("Process"), N_("Program name"), TSK_FMT_TEXT, 0}, 0, 200},
    {{"pid", N_("PID"), N_("Process ID"), TSK_FMT_INT, 0}, 0, 70},
    {{"ppid", N_("Parent"), N_("Parent process ID"), TSK_FMT_INT, 0}, 0, 70},
    {{"user", N_("User"), N_("Owner of the process"), TSK_FMT_TEXT, 0}, 0, 90},
    {{"state", N_("State"), N_("Running, sleeping, waiting for the disk, stopped or zombie"), TSK_FMT_TEXT, 0},
     0, 90},
    {{"nice", N_("Nice"), N_("Scheduling priority: -20 (highest) to 19 (lowest)"), TSK_FMT_INT, 0}, 0, 50},
    {{"threads", N_("Threads"), N_("Number of threads"), TSK_FMT_INT, 0}, 0, 60},
    {{"cpu", N_("CPU"),
      /* xgettext:no-c-format */
      N_("CPU use as a share of all cores: 100% means every core is busy"), TSK_FMT_PCT, 0},
     BLANK_ZERO, 65},
    {{"cpu1", N_("CPU (core)"),
      /* xgettext:no-c-format */
      N_("CPU use relative to one core: 100% is one core fully busy, multi-threaded programs go past it"),
      TSK_FMT_PCT, 0},
     BLANK_ZERO, 80},
    {{"cputime", N_("CPU Time"), N_("Processor time used since the process started"), TSK_FMT_TIME, 0}, 0, 80},
    {{"start", N_("Started"), N_("When the process started"), TSK_FMT_DATE, 0}, 0, 110},
    {{"rss", N_("Resident"), N_("Resident memory (RSS): pages in RAM, including shared ones"), TSK_FMT_BYTES, 0},
     0, 90},
    {{"shared", N_("Shared"), N_("Resident pages that may be shared with other processes: files, libraries, shared memory"),
      TSK_FMT_BYTES, 0},
     0, 90},
    {{"uss", N_("Exclusive"),
      N_("Memory only this process uses (private pages): roughly what ending it gives back"), TSK_FMT_BYTES,
      TSK_NEED_SMAPS},
     BLANK_UNKNOWN, 95},
    {{"pss", N_("Proportional"), N_("Proportional set size: private pages plus a fair share of shared ones"),
      TSK_FMT_BYTES, TSK_NEED_SMAPS},
     BLANK_UNKNOWN, 95},
    {{"swap", N_("Swap"), N_("Pages of this process moved to swap"), TSK_FMT_BYTES, TSK_NEED_SMAPS},
     BLANK_UNKNOWN | BLANK_ZERO, 80},
    {{"vsize", N_("Virtual"), N_("Virtual memory: everything mapped, most of it never used"), TSK_FMT_BYTES, 0},
     0, 90},
    {{"x11", N_("X11 Memory"), N_("Pixmaps this process's X connections hold in the X server"), TSK_FMT_BYTES,
      TSK_NEED_X11},
     BLANK_UNKNOWN | BLANK_ZERO, 90},
    {{"gpu", N_("GPU"), N_("GPU use of the busiest engine; NVIDIA through NVML, others through DRM fdinfo"),
      TSK_FMT_PCT, TSK_NEED_GPU},
     BLANK_UNKNOWN, 60},
    {{"gpumem", N_("GPU Memory"), N_("Video memory (VRAM) the process holds"), TSK_FMT_BYTES, TSK_NEED_GPU},
     BLANK_UNKNOWN, 90},
    {{"netrx", N_("Net In"), N_("Data received per second over TCP (UDP isn't counted); only your own processes"),
      TSK_FMT_RATE, TSK_NEED_NET},
     BLANK_ZERO, 85},
    {{"nettx", N_("Net Out"), N_("Data sent per second over TCP (UDP isn't counted); only your own processes"),
      TSK_FMT_RATE, TSK_NEED_NET},
     BLANK_ZERO, 85},
    {{"ioread", N_("Disk Read"), N_("Data read from storage per second; only your own processes"), TSK_FMT_RATE,
      TSK_NEED_IO},
     BLANK_ZERO, 85},
    {{"iowrite", N_("Disk Write"), N_("Data written to storage per second; only your own processes"),
      TSK_FMT_RATE, TSK_NEED_IO},
     BLANK_ZERO, 85},
    {{"ioreadsum", N_("Total Read"), N_("Data read from storage since the process started"), TSK_FMT_BYTES,
      TSK_NEED_IO},
     0, 90},
    {{"iowritesum", N_("Total Written"), N_("Data written to storage since the process started"), TSK_FMT_BYTES,
      TSK_NEED_IO},
     0, 90},
    {{"title", N_("Window Title"), N_("Title of the process's first window"), TSK_FMT_TEXT, TSK_NEED_TITLE}, 0,
     250},
    {{"cmdline", N_("Command Line"), N_("The full command line"), TSK_FMT_TEXT, 0}, 0, 400},
};

const int tsk_ncolumns = G_N_ELEMENTS(columns);

int tsk_column_index(const char *key)
{
    for (int i = 0; i < tsk_ncolumns; i++)
        if (!strcmp(columns[i].d.key, key))
            return i;
    return -1;
}

const TskColumnDef *tsk_column(int i)
{
    return &columns[i].d;
}

static int col_of(const char *key)
{
    return COL_FIRST + tsk_column_index(key);
}

/* ---- formatting ---------------------------------------------------------------- */

char *tsk_fmt_bytes(double b)
{
    if (b < 1024)
        return g_strdup_printf(_("%.0f B"), b);
    if (b < 1024.0 * 1024)
        return g_strdup_printf(_("%.0f KiB"), b / 1024);
    if (b < 1024.0 * 1024 * 1024)
        return g_strdup_printf(_("%.1f MiB"), b / (1024.0 * 1024));
    return g_strdup_printf(_("%.2f GiB"), b / (1024.0 * 1024 * 1024));
}

char *tsk_fmt_rate(double b)
{
    char *s = tsk_fmt_bytes(b), *r = g_strdup_printf(_("%s/s"), s);
    g_free(s);
    return r;
}

static const char *state_name(char s)
{
    switch (s) {
    case 'R': return _("Running");
    case 'S': return _("Sleeping");
    case 'D': return _("Disk wait");
    case 'Z': return _("Zombie");
    case 'T': return _("Stopped");
    case 't': return _("Traced");
    case 'I': return _("Idle");
    case 'X': return _("Dead");
    default: return "?";
    }
}

static void cell_data(GtkTreeViewColumn *col, GtkCellRenderer *cell, GtkTreeModel *m, GtkTreeIter *it, gpointer data)
{
    (void)col;
    const ColumnInfo *ci = &columns[GPOINTER_TO_INT(data)];
    int mc = COL_FIRST + GPOINTER_TO_INT(data);
    if (ci->d.fmt == TSK_FMT_TEXT)
        return;   /* bound directly as "text" */
    if (ci->d.fmt == TSK_FMT_INT) {
        int v;
        gtk_tree_model_get(m, it, mc, &v, -1);
        char buf[16];
        g_snprintf(buf, sizeof(buf), "%d", v);
        g_object_set(cell, "text", buf, NULL);
        return;
    }
    double v;
    gtk_tree_model_get(m, it, mc, &v, -1);
    if (v < 0) {
        g_object_set(cell, "text", (ci->flags & BLANK_UNKNOWN) ? "" : "—", NULL);
        return;
    }
    if (v == 0 && (ci->flags & BLANK_ZERO)) {
        g_object_set(cell, "text", "", NULL);
        return;
    }
    char *s = NULL;
    switch (ci->d.fmt) {
    case TSK_FMT_PCT:
        s = g_strdup_printf("%.1f", v);
        break;
    case TSK_FMT_BYTES:
        s = tsk_fmt_bytes(v);
        break;
    case TSK_FMT_RATE:
        s = tsk_fmt_rate(v);
        break;
    case TSK_FMT_TIME: {
        long t = (long)v;
        s = t >= 3600 ? g_strdup_printf("%ld:%02ld:%02ld", t / 3600, t / 60 % 60, t % 60)
                      : g_strdup_printf("%ld:%02ld", t / 60, t % 60);
        break;
    }
    case TSK_FMT_DATE: {
        GDateTime *dt = g_date_time_new_from_unix_local((gint64)v), *now = g_date_time_new_now_local();
        gboolean today = g_date_time_get_day_of_year(dt) == g_date_time_get_day_of_year(now) &&
                         g_date_time_get_year(dt) == g_date_time_get_year(now);
        s = g_date_time_format(dt, today ? "%X" : "%x %H:%M");
        g_date_time_unref(dt);
        g_date_time_unref(now);
        break;
    }
    default:
        break;
    }
    g_object_set(cell, "text", s ? s : "", NULL);
    g_free(s);
}

/* ---- icons ----------------------------------------------------------------------- */

static GdkPixbuf *icon_for(const char *name)
{
    static GHashTable *cache;
    if (!cache)
        cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    gpointer v;
    if (g_hash_table_lookup_extended(cache, name, NULL, &v))
        return v;
    char *lower = g_ascii_strdown(name, -1);
    GdkPixbuf *pb = NULL;
    GtkIconTheme *t = gtk_icon_theme_get_default();
    if (gtk_icon_theme_has_icon(t, lower))
        pb = gtk_icon_theme_load_icon(t, lower, 16, GTK_ICON_LOOKUP_FORCE_SIZE, NULL);
    g_free(lower);
    g_hash_table_insert(cache, g_strdup(name), pb);
    return pb;
}

/* ---- filter ------------------------------------------------------------------------ */

/* The filter is a list of terms separated by '|' or ',' ("kiplay|kitask"):
 * a process shows when any term matches. Plain substrings, not regexps: no
 * syntax to get wrong while typing, and `.` or `+` in a command line just
 * match themselves. Empty terms are dropped, so "kitask|" doesn't match all. */
static char **filter_terms;     /* casefolded, NULL-terminated; NULL: no filter */

static gboolean contains(const char *hay, const char *needle)
{
    if (!hay)
        return FALSE;
    char *f = g_utf8_casefold(hay, -1);
    gboolean r = strstr(f, needle) != NULL;
    g_free(f);
    return r;
}

static gboolean matches_term(TskProc *p, const char *pidbuf, const char *term)
{
    return !strcmp(pidbuf, term) || contains(p->name, term) || contains(p->cmdline, term) ||
           contains(tsk_user_name(p->uid), term) || contains(p->title, term);
}

static gboolean visible_func(GtkTreeModel *m, GtkTreeIter *it, gpointer data)
{
    TskApp *app = data;
    int pid;
    gtk_tree_model_get(m, it, COL_PID, &pid, -1);
    TskProc *p = g_hash_table_lookup(app->procs.procs, GINT_TO_POINTER(pid));
    if (!p)
        return FALSE;
    if (app->cfg.hide_kthreads && p->kthread)
        return FALSE;
    if (app->cfg.scope == TSK_SCOPE_MINE && p->uid != getuid())
        return FALSE;
    if (app->cfg.scope == TSK_SCOPE_WINDOWS && !p->window)
        return FALSE;
    if (!filter_terms)
        return TRUE;
    char pidbuf[16];
    g_snprintf(pidbuf, sizeof(pidbuf), "%d", pid);
    for (char **t = filter_terms; *t; t++)
        if (matches_term(p, pidbuf, *t))
            return TRUE;
    return FALSE;
}

void tsk_list_refilter(TskApp *app)
{
    g_strfreev(filter_terms);
    filter_terms = NULL;
    char *folded = g_utf8_casefold(gtk_entry_get_text(GTK_ENTRY(app->filter)), -1);
    char **parts = g_strsplit_set(folded, "|,", -1);
    GPtrArray *terms = g_ptr_array_new();
    for (char **p = parts; *p; p++) {
        g_strstrip(*p);
        if (**p)
            g_ptr_array_add(terms, g_strdup(*p));
    }
    if (terms->len) {
        g_ptr_array_add(terms, NULL);
        filter_terms = (char **)g_ptr_array_free(terms, FALSE);
    } else {
        g_ptr_array_free(terms, TRUE);
    }
    g_strfreev(parts);
    g_free(folded);
    gtk_tree_model_filter_refilter(GTK_TREE_MODEL_FILTER(app->filter_model));
}

/* ---- rows -------------------------------------------------------------------------- */

unsigned tsk_list_need(TskApp *app)
{
    unsigned need = 0;
    for (guint i = 0; i < app->cfg.columns->len; i++) {
        int c = tsk_column_index(g_array_index(app->cfg.columns, TskColumnCfg, i).key);
        if (c >= 0)
            need |= columns[c].d.need;
    }
    /* who has a window: the "with windows" scope and Switch to Window need it,
     * and it's a handful of round trips */
    return need | TSK_NEED_TITLE;
}

typedef struct {
    GtkTreeIter it;
    guint64 hash;       /* of the values last set: an unchanged row isn't set again */
} Row;

static guint64 fnv(guint64 h, const void *data, size_t len)
{
    const unsigned char *b = data;
    for (size_t i = 0; i < len; i++)
        h = (h ^ b[i]) * 0x100000001b3ULL;
    return h;
}

/* Setting a row costs a row-changed through the filter and sort models and
 * the view, for every row on every refresh; most processes sleep and show
 * the same numbers, so their rows are skipped. */
static void set_row(TskApp *app, Row *row, TskProc *p, const gboolean *shown, gboolean is_new)
{
    int cols[COL_FIRST + G_N_ELEMENTS(columns)];
    GValue vals[COL_FIRST + G_N_ELEMENTS(columns)];
    int n = 0;
    memset(vals, 0, sizeof(vals));
#define SET_INT(c, v) do { cols[n] = (c); g_value_init(&vals[n], G_TYPE_INT); g_value_set_int(&vals[n], (v)); n++; } while (0)
#define SET_DBL(c, v) do { cols[n] = (c); g_value_init(&vals[n], G_TYPE_DOUBLE); g_value_set_double(&vals[n], (v)); n++; } while (0)
#define SET_STR(c, v) do { cols[n] = (c); g_value_init(&vals[n], G_TYPE_STRING); g_value_set_static_string(&vals[n], (v)); n++; } while (0)
    if (is_new) {
        SET_INT(COL_PID, p->pid);
        cols[n] = COL_ICON;
        g_value_init(&vals[n], GDK_TYPE_PIXBUF);
        g_value_set_object(&vals[n], icon_for(p->name));
        n++;
    }
    /* what the filter reads is always there */
    SET_STR(col_of("name"), p->name);
    SET_STR(col_of("cmdline"), p->cmdline);
    SET_STR(col_of("user"), tsk_user_name(p->uid));
    SET_STR(col_of("title"), p->title);
    SET_INT(col_of("pid"), p->pid);
    for (int i = 0; i < tsk_ncolumns; i++) {
        if (!shown[i])
            continue;
        const char *k = columns[i].d.key;
        int c = COL_FIRST + i;
        if (!strcmp(k, "ppid")) SET_INT(c, p->ppid);
        else if (!strcmp(k, "state")) SET_STR(c, state_name(p->state));
        else if (!strcmp(k, "nice")) SET_INT(c, p->nice);
        else if (!strcmp(k, "threads")) SET_INT(c, p->threads);
        else if (!strcmp(k, "cpu")) SET_DBL(c, p->cpu / app->procs.ncpu);
        else if (!strcmp(k, "cpu1")) SET_DBL(c, p->cpu);
        else if (!strcmp(k, "cputime")) SET_DBL(c, (double)p->ticks / app->procs.hz);
        else if (!strcmp(k, "start")) SET_DBL(c, (double)app->procs.boot_time + (double)p->start / app->procs.hz);
        else if (!strcmp(k, "rss")) SET_DBL(c, p->rss);
        else if (!strcmp(k, "shared")) SET_DBL(c, p->shared);
        else if (!strcmp(k, "uss")) SET_DBL(c, p->uss);
        else if (!strcmp(k, "pss")) SET_DBL(c, p->pss);
        else if (!strcmp(k, "swap")) SET_DBL(c, p->swap);
        else if (!strcmp(k, "vsize")) SET_DBL(c, p->vsize);
        else if (!strcmp(k, "x11")) SET_DBL(c, p->x11_mem);
        else if (!strcmp(k, "gpu")) SET_DBL(c, p->gpu);
        else if (!strcmp(k, "gpumem")) SET_DBL(c, p->gpu_mem);
        else if (!strcmp(k, "netrx")) SET_DBL(c, p->net_rx_rate);
        else if (!strcmp(k, "nettx")) SET_DBL(c, p->net_tx_rate);
        else if (!strcmp(k, "ioread")) SET_DBL(c, p->io_read_rate);
        else if (!strcmp(k, "iowrite")) SET_DBL(c, p->io_write_rate);
        else if (!strcmp(k, "ioreadsum")) SET_DBL(c, p->io_read);
        else if (!strcmp(k, "iowritesum")) SET_DBL(c, p->io_write);
    }
#undef SET_INT
#undef SET_DBL
#undef SET_STR
    guint64 h = 0xcbf29ce484222325ULL;
    for (int i = 0; i < n; i++) {
        GType t = G_VALUE_TYPE(&vals[i]);
        if (t == G_TYPE_INT) {
            int v = g_value_get_int(&vals[i]);
            h = fnv(h, &v, sizeof(v));
        } else if (t == G_TYPE_DOUBLE) {
            double v = g_value_get_double(&vals[i]);
            h = fnv(h, &v, sizeof(v));
        } else if (t == G_TYPE_STRING) {
            const char *v = g_value_get_string(&vals[i]);
            h = v ? fnv(h, v, strlen(v) + 1) : fnv(h, "", 1);   /* NULL and "" differ in length */
            if (!v)
                h = fnv(h, &i, sizeof(i));
        }
    }
    if (is_new || h != row->hash)
        gtk_list_store_set_valuesv(app->store, &row->it, cols, vals, n);
    row->hash = h;
    for (int i = 0; i < n; i++)
        g_value_unset(&vals[i]);
}

void tsk_list_update(TskApp *app)
{
    gboolean shown[G_N_ELEMENTS(columns)] = {FALSE};
    for (guint i = 0; i < app->cfg.columns->len; i++) {
        int c = tsk_column_index(g_array_index(app->cfg.columns, TskColumnCfg, i).key);
        if (c >= 0)
            shown[c] = TRUE;
    }
    GHashTableIter hi;
    gpointer k, v;
    g_hash_table_iter_init(&hi, app->procs.procs);
    while (g_hash_table_iter_next(&hi, &k, &v)) {
        Row *row = g_hash_table_lookup(app->rows, k);
        gboolean is_new = !row;
        if (is_new) {
            row = g_new0(Row, 1);
            gtk_list_store_append(app->store, &row->it);
            g_hash_table_insert(app->rows, k, row);
        }
        set_row(app, row, v, shown, is_new);
    }
    g_hash_table_iter_init(&hi, app->rows);
    while (g_hash_table_iter_next(&hi, &k, &v))
        if (!g_hash_table_contains(app->procs.procs, k)) {
            gtk_list_store_remove(app->store, &((Row *)v)->it);
            g_hash_table_iter_remove(&hi);
        }
}

/* ---- columns ----------------------------------------------------------------------- */

/* The view removes its columns one by one when the window is destroyed, and
 * each removal fires columns-changed and sort-column-changed: acting on
 * those would save a shrinking list, ending with no columns at all. */
static gboolean view_closing;

static void on_view_destroy(GtkWidget *w, gpointer data)
{
    (void)w;
    (void)data;
    view_closing = TRUE;
}

static void save_columns(TskApp *app)
{
    if (view_closing)
        return;
    GList *cols = gtk_tree_view_get_columns(GTK_TREE_VIEW(app->list));
    if (!cols)
        return;     /* never save an empty list: it would load back as only "Process" */
    g_array_set_size(app->cfg.columns, 0);
    for (GList *l = cols; l; l = l->next) {
        const char *key = g_object_get_data(G_OBJECT(l->data), "key");
        TskColumnCfg c = {{0}, gtk_tree_view_column_get_width(l->data)};
        g_strlcpy(c.key, key, sizeof(c.key));
        g_array_append_val(app->cfg.columns, c);
    }
    g_list_free(cols);
    tsk_app_save_soon(app);
}

static gboolean rebuilding;

static void on_columns_changed(GtkTreeView *tv, TskApp *app)
{
    (void)tv;
    if (!rebuilding)
        save_columns(app);
}

static void on_width(GObject *col, GParamSpec *ps, TskApp *app)
{
    (void)col;
    (void)ps;
    if (!rebuilding && gtk_widget_get_realized(app->list))
        save_columns(app);
}

static void on_toggle_column(GtkCheckMenuItem *mi, TskApp *app)
{
    const char *key = g_object_get_data(G_OBJECT(mi), "key");
    GArray *a = app->cfg.columns;
    if (gtk_check_menu_item_get_active(mi)) {
        TskColumnCfg c = {{0}, 0};
        g_strlcpy(c.key, key, sizeof(c.key));
        g_array_append_val(a, c);
    } else {
        if (a->len <= 1)
            return;   /* the list needs one column */
        for (guint i = 0; i < a->len; i++)
            if (!strcmp(g_array_index(a, TskColumnCfg, i).key, key)) {
                g_array_remove_index(a, i);
                break;
            }
    }
    tsk_list_rebuild_columns(app);
    tsk_app_save_soon(app);
    tsk_app_refresh_now(app);
}

void tsk_list_columns_menu(TskApp *app, GtkMenuShell *menu)
{
    for (int i = 0; i < tsk_ncolumns; i++) {
        GtkWidget *mi = gtk_check_menu_item_new_with_label(_(columns[i].d.title));
        gboolean on = FALSE;
        for (guint k = 0; k < app->cfg.columns->len; k++)
            on |= !strcmp(g_array_index(app->cfg.columns, TskColumnCfg, k).key, columns[i].d.key);
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(mi), on);
        gtk_widget_set_tooltip_text(mi, _(columns[i].d.tip));
        g_object_set_data(G_OBJECT(mi), "key", (gpointer)columns[i].d.key);
        g_signal_connect(mi, "toggled", G_CALLBACK(on_toggle_column), app);
        gtk_menu_shell_append(menu, mi);
    }
}

static gboolean on_header_button(GtkWidget *w, GdkEventButton *ev, TskApp *app)
{
    (void)w;
    if (ev->type != GDK_BUTTON_PRESS || ev->button != 3)
        return FALSE;
    GtkWidget *menu = gtk_menu_new();
    tsk_list_columns_menu(app, GTK_MENU_SHELL(menu));
    gtk_widget_show_all(menu);
    g_signal_connect(menu, "selection-done", G_CALLBACK(gtk_widget_destroy), NULL);
    gtk_menu_popup(GTK_MENU(menu), NULL, NULL, NULL, NULL, ev->button, ev->time);
    return TRUE;
}

static void on_sort_changed(GtkTreeSortable *s, TskApp *app)
{
    int id;
    GtkSortType order;
    if (view_closing || !gtk_tree_sortable_get_sort_column_id(s, &id, &order) || id < COL_FIRST || rebuilding)
        return;
    g_strlcpy(app->cfg.sort_key, columns[id - COL_FIRST].d.key, sizeof(app->cfg.sort_key));
    app->cfg.sort_desc = order == GTK_SORT_DESCENDING;
    tsk_app_save_soon(app);
}

void tsk_list_rebuild_columns(TskApp *app)
{
    GtkTreeView *tv = GTK_TREE_VIEW(app->list);
    rebuilding = TRUE;
    GList *old = gtk_tree_view_get_columns(tv);
    for (GList *l = old; l; l = l->next)
        gtk_tree_view_remove_column(tv, l->data);
    g_list_free(old);
    gboolean sort_shown = FALSE;
    for (guint i = 0; i < app->cfg.columns->len; i++) {
        const TskColumnCfg *cc = &g_array_index(app->cfg.columns, TskColumnCfg, i);
        int ci = tsk_column_index(cc->key);
        if (ci < 0)
            continue;
        const ColumnInfo *info = &columns[ci];
        gboolean numeric = info->d.fmt != TSK_FMT_TEXT;
        GtkTreeViewColumn *col = gtk_tree_view_column_new();
        GtkWidget *label = gtk_label_new(_(info->d.title));
        gtk_widget_show(label);
        gtk_tree_view_column_set_widget(col, label);
        gtk_widget_set_tooltip_text(label, _(info->d.tip));
        if (!strcmp(info->d.key, "name")) {
            GtkCellRenderer *pr = gtk_cell_renderer_pixbuf_new();
            gtk_tree_view_column_pack_start(col, pr, FALSE);
            gtk_tree_view_column_add_attribute(col, pr, "pixbuf", COL_ICON);
        }
        GtkCellRenderer *r = gtk_cell_renderer_text_new();
        g_object_set(r, "ellipsize", numeric ? PANGO_ELLIPSIZE_NONE : PANGO_ELLIPSIZE_END, "xalign",
                     numeric ? 1.0 : 0.0, NULL);
        gtk_tree_view_column_pack_start(col, r, TRUE);
        if (numeric)
            gtk_tree_view_column_set_cell_data_func(col, r, cell_data, GINT_TO_POINTER(ci), NULL);
        else
            gtk_tree_view_column_add_attribute(col, r, "text", COL_FIRST + ci);
        gtk_tree_view_column_set_alignment(col, numeric ? 1.0 : 0.0);
        gtk_tree_view_column_set_sizing(col, GTK_TREE_VIEW_COLUMN_FIXED);
        gtk_tree_view_column_set_fixed_width(col, cc->width > 0 ? cc->width : info->width);
        gtk_tree_view_column_set_resizable(col, TRUE);
        gtk_tree_view_column_set_reorderable(col, TRUE);
        gtk_tree_view_column_set_sort_column_id(col, COL_FIRST + ci);
        g_object_set_data(G_OBJECT(col), "key", (gpointer)info->d.key);
        gtk_tree_view_append_column(tv, col);
        GtkWidget *button = gtk_widget_get_ancestor(label, GTK_TYPE_BUTTON);
        if (button)
            g_signal_connect(button, "button-press-event", G_CALLBACK(on_header_button), app);
        g_signal_connect(col, "notify::width", G_CALLBACK(on_width), app);
        sort_shown |= !strcmp(info->d.key, app->cfg.sort_key);
    }
    /* the last column takes what's left, so there is no dead strip at the right */
    GList *cols = gtk_tree_view_get_columns(tv);
    GList *last = g_list_last(cols);
    if (last)
        gtk_tree_view_column_set_expand(last->data, TRUE);
    g_list_free(cols);
    if (!sort_shown && app->cfg.columns->len)
        g_strlcpy(app->cfg.sort_key, g_array_index(app->cfg.columns, TskColumnCfg, 0).key,
                  sizeof(app->cfg.sort_key));
    gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(app->sort_model), col_of(app->cfg.sort_key),
                                         app->cfg.sort_desc ? GTK_SORT_DESCENDING : GTK_SORT_ASCENDING);
    rebuilding = FALSE;
}

/* ---- events ------------------------------------------------------------------------ */

static gboolean on_list_button(GtkWidget *w, GdkEventButton *ev, TskApp *app)
{
    GtkTreeView *tv = GTK_TREE_VIEW(w);
    if (ev->window != gtk_tree_view_get_bin_window(tv))
        return FALSE;
    GtkTreePath *path = NULL;
    gtk_tree_view_get_path_at_pos(tv, (int)ev->x, (int)ev->y, &path, NULL, NULL, NULL);
    if (ev->type != GDK_BUTTON_PRESS || ev->button != 3) {
        gtk_tree_path_free(path);
        return FALSE;
    }
    GtkTreeSelection *sel = gtk_tree_view_get_selection(tv);
    if (path && !gtk_tree_selection_path_is_selected(sel, path)) {
        gtk_tree_selection_unselect_all(sel);
        gtk_tree_selection_select_path(sel, path);
        gtk_tree_view_set_cursor(tv, path, NULL, FALSE);
    }
    gtk_tree_path_free(path);
    if (gtk_tree_selection_count_selected_rows(sel) == 0)
        return TRUE;
    GtkWidget *menu = tsk_process_menu(app);
    gtk_menu_popup(GTK_MENU(menu), NULL, NULL, NULL, NULL, ev->button, ev->time);
    return TRUE;
}

static gboolean on_list_key(GtkWidget *w, GdkEventKey *ev, TskApp *app)
{
    (void)w;
    gboolean shift = (ev->state & GDK_SHIFT_MASK) != 0;
    if (ev->keyval == GDK_Delete || ev->keyval == GDK_KP_Delete) {
        tsk_send_signal(app, shift ? 9 : 15);
        return TRUE;
    }
    if (ev->keyval == GDK_Menu || (ev->keyval == GDK_F10 && shift)) {
        if (gtk_tree_selection_count_selected_rows(gtk_tree_view_get_selection(GTK_TREE_VIEW(app->list))) == 0)
            return TRUE;
        GtkWidget *menu = tsk_process_menu(app);
        gtk_menu_popup(GTK_MENU(menu), NULL, NULL, NULL, NULL, 0, ev->time);
        gtk_menu_shell_select_first(GTK_MENU_SHELL(menu), FALSE);
        return TRUE;
    }
    return FALSE;
}

static void on_row_activated(GtkTreeView *tv, GtkTreePath *path, GtkTreeViewColumn *col, TskApp *app)
{
    (void)tv;
    (void)col;
    GtkTreeIter it;
    int pid;
    if (!gtk_tree_model_get_iter(app->sort_model, &it, path))
        return;
    gtk_tree_model_get(app->sort_model, &it, COL_PID, &pid, -1);
    tsk_details(app, pid);
}

GList *tsk_list_selected_pids(TskApp *app)
{
    GList *rows = gtk_tree_selection_get_selected_rows(gtk_tree_view_get_selection(GTK_TREE_VIEW(app->list)), NULL);
    GList *pids = NULL;
    for (GList *l = rows; l; l = l->next) {
        GtkTreeIter it;
        int pid;
        if (gtk_tree_model_get_iter(app->sort_model, &it, l->data)) {
            gtk_tree_model_get(app->sort_model, &it, COL_PID, &pid, -1);
            pids = g_list_append(pids, GINT_TO_POINTER(pid));
        }
    }
    g_list_free_full(rows, (GDestroyNotify)gtk_tree_path_free);
    return pids;
}

/* the cell's own text is the tooltip: long command lines and titles get cut */
static gboolean on_query_tooltip(GtkWidget *w, int x, int y, gboolean kb, GtkTooltip *tip, TskApp *app)
{
    GtkTreeView *tv = GTK_TREE_VIEW(w);
    GtkTreeModel *m;
    GtkTreePath *path;
    GtkTreeIter it;
    if (!gtk_tree_view_get_tooltip_context(tv, &x, &y, kb, &m, &path, &it))
        return FALSE;
    int bx, by, pid;
    gtk_tree_view_convert_widget_to_bin_window_coords(tv, x, y, &bx, &by);
    GtkTreeViewColumn *col = NULL;
    gtk_tree_view_get_path_at_pos(tv, bx, by, NULL, &col, NULL, NULL);
    const char *key = col ? g_object_get_data(G_OBJECT(col), "key") : NULL;
    gboolean ok = FALSE;
    if (key && (!strcmp(key, "cmdline") || !strcmp(key, "title") || !strcmp(key, "name"))) {
        gtk_tree_model_get(m, &it, COL_PID, &pid, -1);
        TskProc *p = g_hash_table_lookup(app->procs.procs, GINT_TO_POINTER(pid));
        const char *text = !p ? NULL : !strcmp(key, "title") ? p->title : p->cmdline;
        if (text && *text) {
            gtk_tooltip_set_text(tip, text);
            gtk_tree_view_set_tooltip_cell(tv, tip, path, col, NULL);
            ok = TRUE;
        }
    }
    gtk_tree_path_free(path);
    return ok;
}

GtkWidget *tsk_list_new(TskApp *app)
{
    GType types[COL_FIRST + G_N_ELEMENTS(columns)];
    types[COL_PID] = G_TYPE_INT;
    types[COL_ICON] = GDK_TYPE_PIXBUF;
    for (int i = 0; i < tsk_ncolumns; i++)
        types[COL_FIRST + i] = columns[i].d.fmt == TSK_FMT_TEXT ? G_TYPE_STRING
                               : columns[i].d.fmt == TSK_FMT_INT ? G_TYPE_INT
                                                                 : G_TYPE_DOUBLE;
    app->store = gtk_list_store_newv(COL_FIRST + tsk_ncolumns, types);
    app->rows = g_hash_table_new_full(NULL, NULL, NULL, g_free);
    app->filter_model = gtk_tree_model_filter_new(GTK_TREE_MODEL(app->store), NULL);
    gtk_tree_model_filter_set_visible_func(GTK_TREE_MODEL_FILTER(app->filter_model), visible_func, app, NULL);
    app->sort_model = gtk_tree_model_sort_new_with_model(app->filter_model);
    g_signal_connect(app->sort_model, "sort-column-changed", G_CALLBACK(on_sort_changed), app);

    app->list = gtk_tree_view_new_with_model(app->sort_model);
    GtkTreeView *tv = GTK_TREE_VIEW(app->list);
    gtk_tree_view_set_rules_hint(tv, TRUE);
    gtk_tree_view_set_fixed_height_mode(tv, TRUE);
    gtk_tree_view_set_enable_search(tv, FALSE);
    gtk_tree_selection_set_mode(gtk_tree_view_get_selection(tv), GTK_SELECTION_MULTIPLE);
    gtk_widget_set_has_tooltip(app->list, TRUE);
    atk_object_set_name(gtk_widget_get_accessible(app->list), _("Processes"));
    tsk_list_rebuild_columns(app);
    g_signal_connect(tv, "destroy", G_CALLBACK(on_view_destroy), NULL);
    g_signal_connect(tv, "columns-changed", G_CALLBACK(on_columns_changed), app);
    g_signal_connect(tv, "button-press-event", G_CALLBACK(on_list_button), app);
    g_signal_connect(tv, "key-press-event", G_CALLBACK(on_list_key), app);
    g_signal_connect(tv, "row-activated", G_CALLBACK(on_row_activated), app);
    g_signal_connect(tv, "query-tooltip", G_CALLBACK(on_query_tooltip), app);

    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sw), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(sw), app->list);
    return sw;
}
