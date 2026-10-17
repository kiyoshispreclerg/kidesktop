/* tsk_graph.c - the graphs above the process list.
 *
 * A source is a string key ("cpu", "cpu.3", "ram.apps", "vram@0000:08:00.0",
 * ...); a graph is a source plus its limits and color, drawn as a history of
 * thin vertical bars, newest at the right, like qps. Values are kept in base
 * units (%, bytes, bytes/s); the limits are in the source's config unit. */
#include "tsk.h"
#include "../shared/xis_i18n.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BAR_W 2
#define MIB (1024.0 * 1024.0)

/* ---- sources ------------------------------------------------------------------ */

static const char *const ram_sources[] = {"ram.apps", "ram.used", "ram.total", "ram.cache", "swap"};
static const char *const io_sources[] = {"net.rx", "net.tx", "disk.read", "disk.write"};

GPtrArray *tsk_graph_sources(void)
{
    GPtrArray *a = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(a, g_strdup("cpu"));
    int ncpu = (int)sysconf(_SC_NPROCESSORS_CONF);
    for (int i = 0; i < ncpu; i++)
        g_ptr_array_add(a, g_strdup_printf("cpu.%d", i));
    for (size_t i = 0; i < G_N_ELEMENTS(ram_sources); i++)
        g_ptr_array_add(a, g_strdup(ram_sources[i]));
    for (int i = 0; i < tsk_gpu_count(); i++) {
        TskGpu *g = tsk_gpu_get(i);
        if (g->has_busy)
            g_ptr_array_add(a, g_strdup_printf("gpu@%s", g->pci));
        if (g->has_vram)
            g_ptr_array_add(a, g_strdup_printf("vram@%s", g->pci));
        if (g->has_gtt)
            g_ptr_array_add(a, g_strdup_printf("gtt@%s", g->pci));
    }
    for (size_t i = 0; i < G_N_ELEMENTS(io_sources); i++)
        g_ptr_array_add(a, g_strdup(io_sources[i]));
    return a;
}

static TskGpu *source_gpu(const char *source)
{
    const char *at = strchr(source, '@');
    return at ? tsk_gpu_find(at + 1) : NULL;
}

gboolean tsk_graph_source_valid(const char *source)
{
    GPtrArray *all = tsk_graph_sources();
    gboolean ok = FALSE;
    for (guint i = 0; i < all->len && !ok; i++)
        ok = !strcmp(all->pdata[i], source);
    g_ptr_array_free(all, TRUE);
    return ok;
}

char *tsk_graph_source_label(const char *source)
{
    TskGpu *gpu = source_gpu(source);
    if (!strcmp(source, "cpu"))
        return g_strdup(_("CPU"));
    if (!strncmp(source, "cpu.", 4))
        return g_strdup_printf(_("CPU %d"), atoi(source + 4) + 1);
    if (!strcmp(source, "ram.apps"))
        return g_strdup(_("RAM: applications"));
    if (!strcmp(source, "ram.used"))
        return g_strdup(_("RAM: in use"));
    if (!strcmp(source, "ram.total"))
        return g_strdup(_("RAM: including caches"));
    if (!strcmp(source, "ram.cache"))
        return g_strdup(_("RAM: caches and buffers"));
    if (!strcmp(source, "swap"))
        return g_strdup(_("Swap"));
    if (!strcmp(source, "net.rx"))
        return g_strdup(_("Network: receiving"));
    if (!strcmp(source, "net.tx"))
        return g_strdup(_("Network: sending"));
    if (!strcmp(source, "disk.read"))
        return g_strdup(_("Disk: reading"));
    if (!strcmp(source, "disk.write"))
        return g_strdup(_("Disk: writing"));
    const char *name = gpu ? gpu->name : _("(not present)");
    if (!strncmp(source, "gpu@", 4))
        return g_strdup_printf(_("GPU: %s"), name);
    if (!strncmp(source, "vram@", 5))
        return g_strdup_printf(_("VRAM: %s"), name);
    if (!strncmp(source, "gtt@", 4))
        /* TRANSLATORS: GTT is system RAM the GPU has mapped (amdgpu) */
        return g_strdup_printf(_("GPU system memory (GTT): %s"), name);
    return g_strdup(source);
}

static const char *source_tip(const char *source)
{
    if (!strcmp(source, "ram.apps"))
        return _("Memory held by programs, without any cache: total - free - buffers - page cache - reclaimable slab");
    if (!strcmp(source, "ram.used"))
        return _("Memory that can't be freed quickly, as the kernel estimates it: total - available");
    if (!strcmp(source, "ram.total"))
        return _("Everything that isn't free, caches included: total - free");
    if (!strcmp(source, "ram.cache"))
        return _("Page cache, buffers and reclaimable slab: freed when programs need the memory");
    if (!strncmp(source, "net.", 4))
        return _("All network interfaces except loopback");
    if (!strncmp(source, "disk.", 5))
        return _("Whole physical disks");
    return NULL;
}

TskUnit tsk_graph_source_unit(const char *source)
{
    if (!strncmp(source, "cpu", 3) || !strncmp(source, "gpu@", 4))
        return TSK_UNIT_PCT;
    if (!strncmp(source, "net.", 4) || !strncmp(source, "disk.", 5))
        return TSK_UNIT_RATE;
    return TSK_UNIT_BYTES;
}

static void set_color(GdkColor *c, guint rgb)
{
    c->pixel = 0;
    c->red = (guint16)(((rgb >> 16) & 0xff) * 257);
    c->green = (guint16)(((rgb >> 8) & 0xff) * 257);
    c->blue = (guint16)((rgb & 0xff) * 257);
}

void tsk_graph_source_defaults(const char *source, GdkColor *color, double *max, gboolean *auto_max)
{
    guint rgb = 0x3c7dd9;
    if (!strncmp(source, "cpu.", 4)) rgb = 0x5a8fd8;
    else if (!strcmp(source, "ram.cache")) rgb = 0x8fae3a;
    else if (!strcmp(source, "swap")) rgb = 0xc0803a;
    else if (!strncmp(source, "ram.", 4)) rgb = 0x3faa5a;
    else if (!strncmp(source, "vram@", 5)) rgb = 0x9b59b6;
    else if (!strncmp(source, "gtt@", 4)) rgb = 0xb07cc6;
    else if (!strncmp(source, "gpu@", 4)) rgb = 0xe0752d;
    else if (!strcmp(source, "net.rx")) rgb = 0x2aa198;
    else if (!strcmp(source, "net.tx")) rgb = 0x1f7f8a;
    else if (!strcmp(source, "disk.read")) rgb = 0xb58900;
    else if (!strcmp(source, "disk.write")) rgb = 0xa0522d;
    set_color(color, rgb);
    *max = tsk_graph_source_unit(source) == TSK_UNIT_PCT ? 100 : 0;
    *auto_max = tsk_graph_source_unit(source) != TSK_UNIT_PCT;
}

/* current value in base units; `natural` gets the source's own maximum, or -1 */
static double source_value(TskApp *app, const char *source, double *natural)
{
    const TskSys *s = &app->sys;
    TskGpu *gpu = source_gpu(source);
    *natural = -1;
    if (!strcmp(source, "cpu")) {
        *natural = 100;
        return s->cpu;
    }
    if (!strncmp(source, "cpu.", 4)) {
        int i = atoi(source + 4);
        *natural = 100;
        return i >= 0 && i < s->ncpu ? s->core[i] : 0;
    }
    if (!strncmp(source, "ram.", 4))
        *natural = s->mem_total;
    if (!strcmp(source, "ram.apps"))
        return MAX(0, s->mem_total - s->mem_free - s->buffers - s->cached - s->sreclaimable);
    if (!strcmp(source, "ram.used"))
        return MAX(0, s->mem_total - s->mem_available);
    if (!strcmp(source, "ram.total"))
        return MAX(0, s->mem_total - s->mem_free);
    if (!strcmp(source, "ram.cache"))
        return s->buffers + s->cached + s->sreclaimable;
    if (!strcmp(source, "swap")) {
        *natural = s->swap_total;
        return s->swap_total - s->swap_free;
    }
    if (!strcmp(source, "net.rx"))
        return s->net_rx;
    if (!strcmp(source, "net.tx"))
        return s->net_tx;
    if (!strcmp(source, "disk.read"))
        return s->disk_read;
    if (!strcmp(source, "disk.write"))
        return s->disk_write;
    if (!gpu)
        return 0;
    if (!strncmp(source, "gpu@", 4)) {
        *natural = 100;
        return gpu->busy;
    }
    if (!strncmp(source, "vram@", 5)) {
        *natural = gpu->vram_total > 0 ? gpu->vram_total : -1;
        return gpu->vram_used;
    }
    if (!strncmp(source, "gtt@", 4)) {
        *natural = gpu->gtt_total > 0 ? gpu->gtt_total : -1;
        return gpu->gtt_used;
    }
    return 0;
}

/* config unit -> base unit */
static double unit_scale(TskUnit u)
{
    return u == TSK_UNIT_BYTES ? MIB : u == TSK_UNIT_RATE ? 1024.0 : 1.0;
}

static char *fmt_value(TskUnit u, double v)
{
    if (u == TSK_UNIT_PCT)
        return g_strdup_printf("%.0f%%", v);
    return u == TSK_UNIT_RATE ? tsk_fmt_rate(v) : tsk_fmt_bytes(v);
}

/* the range drawn, in base units */
static void graph_range(TskApp *app, TskGraph *g, double *lo, double *hi)
{
    TskUnit u = tsk_graph_source_unit(g->source);
    double natural;
    source_value(app, g->source, &natural);
    *lo = g->min * unit_scale(u);
    if (!g->auto_max) {
        *hi = g->max * unit_scale(u);
    } else if (natural > 0) {
        *hi = natural;
    } else {
        /* no maximum of its own (traffic): the peak of what's on screen */
        double peak = 0;
        for (int i = 0; i < g->hist_n; i++)
            peak = MAX(peak, g->hist[i]);
        *hi = MAX(peak * 1.1, u == TSK_UNIT_RATE ? 64 * 1024.0 : 1.0);
    }
    if (*hi <= *lo)
        *hi = *lo + 1;
}

/* ---- drawing ------------------------------------------------------------------ */

static TskApp *g_app;

static gboolean on_expose(GtkWidget *w, GdkEventExpose *ev, TskGraph *g)
{
    (void)ev;
    cairo_t *cr = gdk_cairo_create(w->window);
    int W = w->allocation.width, H = w->allocation.height;
    GtkStyle *st = gtk_widget_get_style(w);
    GdkColor bg = st->base[GTK_STATE_NORMAL], fg = st->text[GTK_STATE_NORMAL];

    cairo_rectangle(cr, 0.5, 0.5, W - 1, H - 1);
    gdk_cairo_set_source_color(cr, &bg);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, fg.red / 65535.0, fg.green / 65535.0, fg.blue / 65535.0, 0.25);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
    for (int q = 1; q < 4; q++) {
        double y = floor(H * q / 4.0) + 0.5;
        cairo_move_to(cr, 1, y);
        cairo_line_to(cr, W - 1, y);
    }
    cairo_set_source_rgba(cr, fg.red / 65535.0, fg.green / 65535.0, fg.blue / 65535.0, 0.08);
    cairo_stroke(cr);

    double lo, hi;
    graph_range(g_app, g, &lo, &hi);
    double r = g->color.red / 65535.0, gr = g->color.green / 65535.0, b = g->color.blue / 65535.0;
    /* one bar per sample: a translucent body with a solid 1 px top */
    int bars = MIN(g->hist_n, (W - 2) / BAR_W + 1);
    int plot_h = H - 2;
    cairo_save(cr);
    cairo_rectangle(cr, 1, 1, W - 2, H - 2);
    cairo_clip(cr);
    for (int pass = 0; pass < 2; pass++) {
        for (int k = 0; k < bars; k++) {
            int idx = (g->hist_pos - 1 - k + g->hist_len) % g->hist_len;
            double f = CLAMP((g->hist[idx] - lo) / (hi - lo), 0, 1);
            int bh = (int)lround(f * plot_h);
            if (bh > 0)
                cairo_rectangle(cr, W - 1 - (k + 1) * BAR_W, H - 1 - bh, BAR_W, pass ? 1 : bh);
        }
        if (pass)
            cairo_set_source_rgb(cr, r, gr, b);
        else
            cairo_set_source_rgba(cr, r, gr, b, 0.55);
        cairo_fill(cr);
    }
    cairo_restore(cr);

    /* label at the top left, value at the top right */
    TskUnit u = tsk_graph_source_unit(g->source);
    double cur = g->hist_n ? g->hist[(g->hist_pos - 1 + g->hist_len) % g->hist_len] : 0;
    char *val = fmt_value(u, cur), *label = tsk_graph_source_label(g->source);
    PangoLayout *pl = gtk_widget_create_pango_layout(w, val);
    PangoFontDescription *fd = pango_font_description_copy(st->font_desc);
    pango_font_description_set_size(fd, pango_font_description_get_size(fd) * 9 / 10);
    pango_layout_set_font_description(pl, fd);
    int vw, vh;
    pango_layout_get_pixel_size(pl, &vw, &vh);
    gdk_cairo_set_source_color(cr, &fg);
    cairo_move_to(cr, W - 5 - vw, 3);
    pango_cairo_show_layout(cr, pl);
    pango_font_description_set_weight(fd, PANGO_WEIGHT_BOLD);
    pango_layout_set_font_description(pl, fd);
    pango_layout_set_text(pl, label, -1);
    pango_layout_set_width(pl, MAX(1, W - vw - 14) * PANGO_SCALE);
    pango_layout_set_ellipsize(pl, PANGO_ELLIPSIZE_END);
    cairo_move_to(cr, 5, 3);
    pango_cairo_show_layout(cr, pl);
    pango_font_description_free(fd);
    g_object_unref(pl);
    g_free(val);
    g_free(label);
    cairo_destroy(cr);
    return TRUE;
}

static void update_tooltip(TskGraph *g)
{
    TskUnit u = tsk_graph_source_unit(g->source);
    double lo, hi;
    graph_range(g_app, g, &lo, &hi);
    double cur = g->hist_n ? g->hist[(g->hist_pos - 1 + g->hist_len) % g->hist_len] : 0;
    char *label = tsk_graph_source_label(g->source), *v = fmt_value(u, cur), *a = fmt_value(u, lo),
         *b = fmt_value(u, hi);
    const char *tip = source_tip(g->source);
    char *text = g_strdup_printf(_("%s: %s\nScale: %s to %s%s%s"), label, v, a, b, tip ? "\n" : "", tip ? tip : "");
    gtk_widget_set_tooltip_text(g->area, text);
    AtkObject *acc = gtk_widget_get_accessible(g->area);
    if (acc) {
        atk_object_set_name(acc, label);
        atk_object_set_description(acc, v);
    }
    g_free(text);
    g_free(label);
    g_free(v);
    g_free(a);
    g_free(b);
}

/* ---- menu ---------------------------------------------------------------------- */

static void on_configure(GtkMenuItem *mi, TskApp *app)
{
    (void)mi;
    tsk_prefs_show(app, 1);
}

static void on_remove(GtkMenuItem *mi, TskApp *app)
{
    TskGraph *g = g_object_get_data(G_OBJECT(mi), "graph");
    g_ptr_array_remove(app->cfg.graphs, g);
    tsk_graphs_rebuild(app);
    tsk_app_save_soon(app);
}

static void on_hide(GtkMenuItem *mi, TskApp *app)
{
    (void)mi;
    app->cfg.show_graphs = FALSE;
    tsk_app_apply_graphs_visible(app);
    tsk_app_save_soon(app);
}

static gboolean on_button(GtkWidget *w, GdkEventButton *ev, TskGraph *g)
{
    (void)w;
    if (ev->type != GDK_BUTTON_PRESS || ev->button != 3)
        return FALSE;
    GtkWidget *menu = gtk_menu_new();
    GtkWidget *mi = gtk_menu_item_new_with_mnemonic(_("_Configure Graphs..."));
    g_signal_connect(mi, "activate", G_CALLBACK(on_configure), g_app);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    if (g) {
        mi = gtk_menu_item_new_with_mnemonic(_("_Remove This Graph"));
        g_object_set_data(G_OBJECT(mi), "graph", g);
        g_signal_connect(mi, "activate", G_CALLBACK(on_remove), g_app);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    }
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    mi = gtk_menu_item_new_with_mnemonic(_("_Hide Graphs"));
    g_signal_connect(mi, "activate", G_CALLBACK(on_hide), g_app);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    gtk_widget_show_all(menu);
    g_signal_connect(menu, "selection-done", G_CALLBACK(gtk_widget_destroy), NULL);
    gtk_menu_popup(GTK_MENU(menu), NULL, NULL, NULL, NULL, ev->button, ev->time);
    return TRUE;
}

/* ---- panel ----------------------------------------------------------------------- */

GtkWidget *tsk_graphs_new(TskApp *app)
{
    g_app = app;
    app->graphs_box = gtk_vbox_new(FALSE, 0);
    gtk_container_set_border_width(GTK_CONTAINER(app->graphs_box), 4);
    tsk_graphs_rebuild(app);
    return app->graphs_box;
}

void tsk_graphs_rebuild(TskApp *app)
{
    if (app->graphs_table)
        gtk_widget_destroy(app->graphs_table);
    GPtrArray *shown = g_ptr_array_new();
    for (int i = 0; i < tsk_gpu_count(); i++)
        tsk_gpu_get(i)->wanted = FALSE;
    for (guint i = 0; i < app->cfg.graphs->len; i++) {
        TskGraph *g = app->cfg.graphs->pdata[i];
        g->area = NULL;
        if (!tsk_graph_source_valid(g->source))
            continue;
        g_ptr_array_add(shown, g);
        TskGpu *gpu = source_gpu(g->source);
        if (gpu)
            gpu->wanted = TRUE;   /* tsk_gpu_sample() skips the others (NVML costs ~1 ms a call) */
    }
    int per_row = MAX(1, MIN(app->cfg.graphs_per_row, (int)shown->len));
    int rows = shown->len ? ((int)shown->len + per_row - 1) / per_row : 1;
    app->graphs_table = gtk_table_new((guint)rows, (guint)per_row, TRUE);
    gtk_table_set_row_spacings(GTK_TABLE(app->graphs_table), 4);
    gtk_table_set_col_spacings(GTK_TABLE(app->graphs_table), 4);
    for (guint i = 0; i < shown->len; i++) {
        TskGraph *g = shown->pdata[i];
        g->area = gtk_drawing_area_new();
        gtk_widget_set_size_request(g->area, 60, app->cfg.graph_height);
        gtk_widget_add_events(g->area, GDK_BUTTON_PRESS_MASK);
        g_signal_connect(g->area, "expose-event", G_CALLBACK(on_expose), g);
        g_signal_connect(g->area, "button-press-event", G_CALLBACK(on_button), g);
        guint r = i / (guint)per_row, c = i % (guint)per_row;
        gtk_table_attach(GTK_TABLE(app->graphs_table), g->area, c, c + 1, r, r + 1, GTK_FILL | GTK_EXPAND,
                         GTK_FILL, 0, 0);
        update_tooltip(g);
    }
    if (!shown->len) {
        GtkWidget *l = gtk_label_new(_("No graphs selected. Right-click here or use View > Settings to add some."));
        gtk_misc_set_padding(GTK_MISC(l), 6, 12);
        GtkWidget *eb = gtk_event_box_new();
        gtk_container_add(GTK_CONTAINER(eb), l);
        g_signal_connect(eb, "button-press-event", G_CALLBACK(on_button), NULL);
        gtk_table_attach_defaults(GTK_TABLE(app->graphs_table), eb, 0, 1, 0, 1);
    }
    g_ptr_array_free(shown, TRUE);
    gtk_box_pack_start(GTK_BOX(app->graphs_box), app->graphs_table, TRUE, TRUE, 0);
    gtk_widget_show_all(app->graphs_table);
}

void tsk_graphs_push(TskApp *app)
{
    for (guint i = 0; i < app->cfg.graphs->len; i++) {
        TskGraph *g = app->cfg.graphs->pdata[i];
        double natural;
        g->hist[g->hist_pos] = source_value(app, g->source, &natural);
        g->hist_pos = (g->hist_pos + 1) % g->hist_len;
        if (g->hist_n < g->hist_len)
            g->hist_n++;
        if (g->area && gtk_widget_is_drawable(g->area)) {
            gtk_widget_queue_draw(g->area);
            update_tooltip(g);
        }
    }
}
