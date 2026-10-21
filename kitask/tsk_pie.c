/* tsk_pie.c - kitask: pie chart of what is using the CPU or the RAM.
 *
 * A donut whose full circle is 100% of the resource, one slice per process
 * (or per program name, which folds a browser's many children into one), like
 * a disk-usage sunburst. Nothing here exists until the window is opened: the
 * window, its timer and its data are all freed when it closes. While it is
 * open its timer drives the sampler, and the list's own timer stands aside so
 * the two never cut each other's intervals short. */
#include "tsk.h"
#include "../shared/xis_i18n.h"
#include <malloc.h>
#include <math.h>
#include <string.h>

#define MIN_SHARE 0.01      /* below this a process is folded into "Others" */
#define HOLE 0.40           /* inner radius, as a fraction of the outer one */

typedef struct {
    char *label;
    double value;           /* % of all cores, or bytes of RAM */
    int count;              /* processes behind the slice, 0 when it isn't made of any */
    gboolean rest;          /* idle / available / kernel: not a process */
    double a0, a1;          /* radians, clockwise from 12 o'clock minus pi/2 */
    double r, g, b;
} Slice;

typedef struct {
    char *name;
    double v;
    int n;
} Acc;

typedef struct {
    TskApp *app;
    GtkWidget *win, *mode, *interval, *group, *area;
    guint timer;
    int kind;
    GArray *slices;         /* Slice */
    double denom;           /* what the whole circle stands for: 100 (%) or bytes */
    double used;            /* what the processes add up to, same unit */
} Pie;

static Pie *pie;

static void acc_free(gpointer p)
{
    Acc *a = p;
    g_free(a->name);
    g_free(a);
}

static gint acc_cmp(gconstpointer a, gconstpointer b)
{
    double x = (*(Acc *const *)a)->v, y = (*(Acc *const *)b)->v;
    return x < y ? 1 : x > y ? -1 : 0;
}

static void hsl(double h, double s, double l, double *r, double *g, double *b)
{
    double c = (1 - fabs(2 * l - 1)) * s, hp = h * 6, x = c * (1 - fabs(fmod(hp, 2) - 1)), m = l - c / 2;
    double rr = 0, gg = 0, bb = 0;
    switch ((int)hp % 6) {
    case 0: rr = c; gg = x; break;
    case 1: rr = x; gg = c; break;
    case 2: gg = c; bb = x; break;
    case 3: gg = x; bb = c; break;
    case 4: rr = x; bb = c; break;
    default: rr = c; bb = x; break;
    }
    *r = rr + m;
    *g = gg + m;
    *b = bb + m;
}

/* the same program keeps its color from one refresh to the next */
static void color_for(Slice *s, const char *name)
{
    guint h = g_str_hash(name);
    hsl((h % 360) / 360.0, 0.55, 0.68, &s->r, &s->g, &s->b);
}

static void add_slice(Slice *s, char *label, double value, int count, gboolean rest)
{
    s->label = label;
    s->value = value;
    s->count = count;
    s->rest = rest;
    if (rest) {
        s->r = s->g = s->b = 0.82;
    } else {
        color_for(s, label);
    }
}

static void slices_clear(Pie *p)
{
    for (guint i = 0; i < p->slices->len; i++)
        g_free(g_array_index(p->slices, Slice, i).label);
    g_array_set_size(p->slices, 0);
}

static void build(Pie *p)
{
    TskApp *app = p->app;
    gboolean cpu = p->kind == 0, group = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(p->group));
    int ncpu = MAX(1, app->procs.ncpu);
    GHashTable *acc = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, acc_free);
    GHashTableIter it;
    gpointer v;
    double sum = 0;
    g_hash_table_iter_init(&it, app->procs.procs);
    while (g_hash_table_iter_next(&it, NULL, &v)) {
        TskProc *pr = v;
        double val = cpu ? pr->cpu / ncpu : (pr->pss >= 0 ? pr->pss : pr->rss);
        if (val <= 0)
            continue;
        const char *name = pr->name && *pr->name ? pr->name : pr->comm;
        char *key = group ? g_strdup(name) : g_strdup_printf("%s [%d]", name, (int)pr->pid);
        Acc *a = g_hash_table_lookup(acc, key);
        if (a) {
            g_free(key);
        } else {
            a = g_new0(Acc, 1);
            a->name = key;
            g_hash_table_insert(acc, a->name, a);
        }
        a->v += val;
        a->n++;
        sum += val;
    }

    double total = cpu ? 100 : app->sys.mem_total;
    p->denom = MAX(total, sum);
    p->used = sum;
    GPtrArray *sorted = g_ptr_array_new();
    g_hash_table_iter_init(&it, acc);
    while (g_hash_table_iter_next(&it, NULL, &v))
        g_ptr_array_add(sorted, v);
    g_ptr_array_sort(sorted, acc_cmp);

    slices_clear(p);
    double others = 0;
    int others_n = 0;
    for (guint i = 0; i < sorted->len; i++) {
        Acc *a = sorted->pdata[i];
        if (a->v / p->denom < MIN_SHARE) {
            others += a->v;
            others_n += a->n;
            continue;
        }
        Slice s = { 0 };
        add_slice(&s, g_strdup(a->name), a->v, a->n, FALSE);
        g_array_append_val(p->slices, s);
    }
    if (others > 0) {
        Slice s = { 0 };
        add_slice(&s, g_strdup(_("Others")), others, others_n, TRUE);
        s.r = s.g = s.b = 0.90;
        g_array_append_val(p->slices, s);
    }
    double left = p->denom - sum;
    if (cpu) {
        if (left > p->denom * 0.001) {
            Slice s = { 0 };
            add_slice(&s, g_strdup(_("Idle")), left, 0, TRUE);
            g_array_append_val(p->slices, s);
        }
    } else {
        double avail = MIN(MAX(0, app->sys.mem_available), MAX(0, left));
        if (avail > 0) {
            Slice s = { 0 };
            add_slice(&s, g_strdup(_("Available (free and cache)")), avail, 0, TRUE);
            g_array_append_val(p->slices, s);
        }
        if (left - avail > p->denom * 0.001) {
            Slice s = { 0 };
            add_slice(&s, g_strdup(_("Kernel and other")), left - avail, 0, TRUE);
            s.r = s.g = s.b = 0.68;
            g_array_append_val(p->slices, s);
        }
    }

    double a = -G_PI / 2;
    for (guint i = 0; i < p->slices->len; i++) {
        Slice *s = &g_array_index(p->slices, Slice, i);
        s->a0 = a;
        a += 2 * G_PI * s->value / p->denom;
        s->a1 = a;
    }
    g_ptr_array_free(sorted, TRUE);
    g_hash_table_destroy(acc);
}

/* ---- drawing ------------------------------------------------------------------------ */

/* shortens `text` with an ellipsis until it fits `width` */
static char *fit_text(cairo_t *cr, const char *text, double width)
{
    cairo_text_extents_t e;
    cairo_text_extents(cr, text, &e);
    if (e.width <= width)
        return g_strdup(text);
    for (glong n = g_utf8_strlen(text, -1) - 1; n > 1; n--) {
        char *cut = g_strndup(text, (gsize)(g_utf8_offset_to_pointer(text, n) - text));
        char *t = g_strconcat(cut, "…", NULL);
        g_free(cut);
        cairo_text_extents(cr, t, &e);
        if (e.width <= width)
            return t;
        g_free(t);
    }
    return g_strdup("…");
}

static void center_text(cairo_t *cr, double x, double y, const char *text)
{
    cairo_text_extents_t e;
    cairo_text_extents(cr, text, &e);
    cairo_move_to(cr, x - e.width / 2 - e.x_bearing, y);
    cairo_show_text(cr, text);
}

static gboolean on_expose(GtkWidget *w, GdkEventExpose *ev, Pie *p)
{
    (void)ev;
    GtkAllocation al;
    gtk_widget_get_allocation(w, &al);
    double cx = al.width / 2.0, cy = al.height / 2.0, R = MIN(al.width, al.height) / 2.0 - 8, r0 = R * HOLE;
    if (R < 20)
        return TRUE;
    cairo_t *cr = gdk_cairo_create(gtk_widget_get_window(w));
    cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);

    for (guint i = 0; i < p->slices->len; i++) {
        Slice *s = &g_array_index(p->slices, Slice, i);
        cairo_new_path(cr);
        cairo_arc(cr, cx, cy, R, s->a0, s->a1);
        cairo_arc_negative(cr, cx, cy, r0, s->a1, s->a0);
        cairo_close_path(cr);
        cairo_set_source_rgb(cr, s->r, s->g, s->b);
        cairo_fill_preserve(cr);
        gdk_cairo_set_source_color(cr, &w->style->bg[GTK_STATE_NORMAL]);
        cairo_set_line_width(cr, 1.5);
        cairo_stroke(cr);
    }

    /* names inside the slices that are big enough to hold one */
    cairo_set_font_size(cr, 11);
    cairo_set_source_rgb(cr, 0.08, 0.08, 0.08);
    for (guint i = 0; i < p->slices->len; i++) {
        Slice *s = &g_array_index(p->slices, Slice, i);
        double span = s->a1 - s->a0, mid = (s->a0 + s->a1) / 2, rm = (R + r0) / 2;
        double room = MIN(2 * rm * sin(MIN(span, G_PI) / 2) * 0.85, (R - r0) * 1.5);
        if (span < 0.22 || room < 28)
            continue;
        char *t = fit_text(cr, s->label, room);
        char *pct = g_strdup_printf("%.0f%%", 100 * s->value / p->denom);
        center_text(cr, cx + rm * cos(mid), cy + rm * sin(mid) - 1, t);
        center_text(cr, cx + rm * cos(mid), cy + rm * sin(mid) + 12, pct);
        g_free(t);
        g_free(pct);
    }

    /* the middle: how much of the whole the processes add up to */
    gdk_cairo_set_source_color(cr, &w->style->fg[GTK_STATE_NORMAL]);
    char *big, *small;
    if (p->kind == 0) {
        big = g_strdup_printf("%.0f%%", p->used);
        small = g_strdup(_("CPU"));
    } else {
        char *u = tsk_fmt_bytes(p->used), *t = tsk_fmt_bytes(p->denom);
        big = g_strdup(u);
        small = g_strdup_printf(_("of %s RAM"), t);
        g_free(u);
        g_free(t);
    }
    cairo_set_font_size(cr, MAX(12, MIN(r0 * 0.36, 30)));
    center_text(cr, cx, cy, big);
    cairo_set_font_size(cr, 11);
    center_text(cr, cx, cy + 16, small);
    g_free(big);
    g_free(small);
    cairo_destroy(cr);
    return TRUE;
}

static gboolean on_query_tooltip(GtkWidget *w, gint x, gint y, gboolean kb, GtkTooltip *tip, Pie *p)
{
    (void)kb;
    GtkAllocation al;
    gtk_widget_get_allocation(w, &al);
    double dx = x - al.width / 2.0, dy = y - al.height / 2.0, R = MIN(al.width, al.height) / 2.0 - 8;
    double r = hypot(dx, dy);
    if (r < R * HOLE || r > R)
        return FALSE;
    double a = atan2(dy, dx);
    if (a < -G_PI / 2)
        a += 2 * G_PI;
    for (guint i = 0; i < p->slices->len; i++) {
        Slice *s = &g_array_index(p->slices, Slice, i);
        if (a < s->a0 || a >= s->a1)
            continue;
        double share = 100 * s->value / p->denom;
        char *size = p->kind == 1 ? tsk_fmt_bytes(s->value) : NULL;
        char *amount = size ? g_strdup_printf(_("%s (%.1f%% of the RAM)"), size, share)
                            : g_strdup_printf(_("%.1f%% of the CPU"), share);
        char *procs = s->count > 0 ? g_strdup_printf(ngettext("%d process", "%d processes", s->count), s->count)
                                   : NULL;
        char *text = procs ? g_strdup_printf("%s\n%s\n%s", s->label, amount, procs)
                           : g_strdup_printf("%s\n%s", s->label, amount);
        gtk_tooltip_set_text(tip, text);
        g_free(text);
        g_free(procs);
        g_free(amount);
        g_free(size);
        return TRUE;
    }
    return FALSE;
}

/* ---- refresh and window ----------------------------------------------------------------- */

static void apply_kind(Pie *p)
{
    /* the proportional size is what makes the RAM slices add up to the total */
    p->app->extra_need = p->kind == 1 ? TSK_NEED_SMAPS : 0;
}

static void update(Pie *p)
{
    tsk_app_refresh_now(p->app);
    build(p);
    gtk_widget_queue_draw(p->area);
}

static gboolean on_timer(gpointer data)
{
    update(data);
    return TRUE;
}

static void restart_timer(Pie *p)
{
    if (p->timer)
        g_source_remove(p->timer);
    double s = gtk_spin_button_get_value(GTK_SPIN_BUTTON(p->interval));
    p->timer = g_timeout_add((guint)(s * 1000), on_timer, p);
}

static void on_mode(GtkComboBox *c, Pie *p)
{
    p->kind = gtk_combo_box_get_active(c);
    apply_kind(p);
    update(p);
}

static void on_group(GtkToggleButton *b, Pie *p)
{
    (void)b;
    build(p);
    gtk_widget_queue_draw(p->area);
}

static void on_interval(GtkSpinButton *b, Pie *p)
{
    (void)b;
    restart_timer(p);
}

static void on_destroy(GtkWidget *w, Pie *p)
{
    (void)w;
    if (p->timer)
        g_source_remove(p->timer);
    p->app->extra_need = 0;
    p->app->pie_open = FALSE;
    slices_clear(p);
    g_array_free(p->slices, TRUE);
    g_free(p);
    pie = NULL;
    malloc_trim(0);     /* the per-process tables just built were this window's */
}

void tsk_pie_show(TskApp *app, int kind)
{
    if (pie) {
        gtk_combo_box_set_active(GTK_COMBO_BOX(pie->mode), kind);
        gtk_window_present(GTK_WINDOW(pie->win));
        return;
    }
    Pie *p = pie = g_new0(Pie, 1);
    p->app = app;
    p->kind = kind;
    p->slices = g_array_new(FALSE, TRUE, sizeof(Slice));
    app->pie_open = TRUE;
    apply_kind(p);

    p->win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(p->win), _("Resource usage - kitask"));
    gtk_window_set_icon_name(GTK_WINDOW(p->win), "kitask");
    gtk_window_set_transient_for(GTK_WINDOW(p->win), GTK_WINDOW(app->win));
    gtk_window_set_destroy_with_parent(GTK_WINDOW(p->win), TRUE);
    gtk_window_set_role(GTK_WINDOW(p->win), "kitask-pie");
    gtk_window_set_default_size(GTK_WINDOW(p->win), 560, 600);

    GtkWidget *vbox = gtk_vbox_new(FALSE, 6), *bar = gtk_hbox_new(FALSE, 6);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 6);
    p->mode = gtk_combo_box_new_text();
    gtk_combo_box_append_text(GTK_COMBO_BOX(p->mode), _("Total CPU"));
    gtk_combo_box_append_text(GTK_COMBO_BOX(p->mode), _("RAM"));
    gtk_combo_box_set_active(GTK_COMBO_BOX(p->mode), kind);
    atk_object_set_name(gtk_widget_get_accessible(p->mode), _("Resource"));
    g_signal_connect(p->mode, "changed", G_CALLBACK(on_mode), p);
    gtk_box_pack_start(GTK_BOX(bar), p->mode, FALSE, FALSE, 0);

    p->group = gtk_check_button_new_with_mnemonic(_("_Group by program"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(p->group), TRUE);
    gtk_widget_set_tooltip_text(p->group, _("Add up every process with the same name into one slice"));
    g_signal_connect(p->group, "toggled", G_CALLBACK(on_group), p);
    gtk_box_pack_start(GTK_BOX(bar), p->group, FALSE, FALSE, 6);

    GtkWidget *lbl = gtk_label_new_with_mnemonic(_("_Refresh every (s):"));
    p->interval = gtk_spin_button_new_with_range(0.5, 60, 0.5);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(p->interval), CLAMP(app->cfg.list_interval, 0.5, 60));
    gtk_label_set_mnemonic_widget(GTK_LABEL(lbl), p->interval);
    g_signal_connect(p->interval, "value-changed", G_CALLBACK(on_interval), p);
    gtk_box_pack_end(GTK_BOX(bar), p->interval, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(bar), lbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), bar, FALSE, FALSE, 0);

    p->area = gtk_drawing_area_new();
    gtk_widget_set_has_tooltip(p->area, TRUE);
    g_signal_connect(p->area, "expose-event", G_CALLBACK(on_expose), p);
    g_signal_connect(p->area, "query-tooltip", G_CALLBACK(on_query_tooltip), p);
    gtk_box_pack_start(GTK_BOX(vbox), p->area, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(p->win), vbox);
    g_signal_connect(p->win, "destroy", G_CALLBACK(on_destroy), p);

    update(p);
    restart_timer(p);
    gtk_widget_show_all(p->win);
}
