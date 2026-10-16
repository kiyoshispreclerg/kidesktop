/* kf_usagecell.c - the places panel's name cell, with the device's usage bar
 * under the name.
 *
 * A tree view column only packs renderers side by side, so the bar can't be
 * a renderer of its own below the text: this is a GtkCellRendererText that,
 * when "usage" is 0..100, asks for a thin strip more and draws the bar there,
 * across the whole width of the cell. */
#include "kf.h"

#define BAR_H 3
#define BAR_GAP 2           /* between the text and the bar */

typedef struct {
    GtkCellRendererText parent;
    int usage;              /* percent, -1 = no bar */
} KfUsageCell;

typedef struct {
    GtkCellRendererTextClass parent_class;
} KfUsageCellClass;

G_DEFINE_TYPE(KfUsageCell, kf_usage_cell, GTK_TYPE_CELL_RENDERER_TEXT)

enum { PROP_0, PROP_USAGE };

static void set_property(GObject *o, guint id, const GValue *v, GParamSpec *ps)
{
    if (id == PROP_USAGE)
        ((KfUsageCell *)o)->usage = g_value_get_int(v);
    else
        G_OBJECT_WARN_INVALID_PROPERTY_ID(o, id, ps);
}

static void get_property(GObject *o, guint id, GValue *v, GParamSpec *ps)
{
    if (id == PROP_USAGE)
        g_value_set_int(v, ((KfUsageCell *)o)->usage);
    else
        G_OBJECT_WARN_INVALID_PROPERTY_ID(o, id, ps);
}

static GtkCellRendererClass *parent_class(void) { return GTK_CELL_RENDERER_CLASS(kf_usage_cell_parent_class); }

static void get_size(GtkCellRenderer *cell, GtkWidget *w, GdkRectangle *area, gint *x, gint *y, gint *width, gint *height)
{
    parent_class()->get_size(cell, w, area, x, y, width, height);
    if (((KfUsageCell *)cell)->usage >= 0 && height)
        *height += BAR_GAP + BAR_H;
}

static void set_source(cairo_t *cr, const GdkColor *c, double alpha)
{
    cairo_set_source_rgba(cr, c->red / 65535.0, c->green / 65535.0, c->blue / 65535.0, alpha);
}

static void render(GtkCellRenderer *cell, GdkDrawable *win, GtkWidget *w, GdkRectangle *bg, GdkRectangle *area,
                   GdkRectangle *expose, GtkCellRendererState flags)
{
    int usage = ((KfUsageCell *)cell)->usage;
    if (usage < 0) {
        parent_class()->render(cell, win, w, bg, area, expose, flags);
        return;
    }
    /* Text and bar as one block, centered in the row like a plain cell. */
    int th, xpad, ypad;
    parent_class()->get_size(cell, w, NULL, NULL, NULL, NULL, &th);
    gtk_cell_renderer_get_padding(cell, &xpad, &ypad);
    int block = th - ypad + BAR_GAP + BAR_H;
    GdkRectangle text = *area;
    text.y = area->y + MAX(0, (area->height - block) / 2);
    text.height = th;
    parent_class()->render(cell, win, w, bg, &text, expose, flags);

    int bx = area->x + xpad, bw = area->width - 2 * xpad;
    int by = MIN(text.y + th - ypad + BAR_GAP, area->y + area->height - BAR_H);
    if (bw <= 0)
        return;
    GtkStyle *st = gtk_widget_get_style(w);
    gboolean sel = (flags & GTK_CELL_RENDERER_SELECTED) != 0;
    cairo_t *cr = gdk_cairo_create(win);
    gdk_cairo_rectangle(cr, expose);
    cairo_clip(cr);
    set_source(cr, &st->text[sel ? GTK_STATE_SELECTED : GTK_STATE_NORMAL], 0.2);
    cairo_rectangle(cr, bx, by, bw, BAR_H);
    cairo_fill(cr);
    if (usage >= 90)        /* nearly full */
        cairo_set_source_rgb(cr, 0xe0 / 255.0, 0x1b / 255.0, 0x24 / 255.0);
    else
        set_source(cr, sel ? &st->text[GTK_STATE_SELECTED] : &st->bg[GTK_STATE_SELECTED], 1.0);
    cairo_rectangle(cr, bx, by, bw * CLAMP(usage, 0, 100) / 100.0, BAR_H);
    cairo_fill(cr);
    cairo_destroy(cr);
}

static void kf_usage_cell_class_init(KfUsageCellClass *k)
{
    GObjectClass *oc = G_OBJECT_CLASS(k);
    oc->set_property = set_property;
    oc->get_property = get_property;
    GTK_CELL_RENDERER_CLASS(k)->get_size = get_size;
    GTK_CELL_RENDERER_CLASS(k)->render = render;
    g_object_class_install_property(oc, PROP_USAGE,
        g_param_spec_int("usage", "Usage", "Percent used, -1 for no bar", -1, 100, -1, G_PARAM_READWRITE));
}

static void kf_usage_cell_init(KfUsageCell *u)
{
    u->usage = -1;
}

GtkCellRenderer *kf_usage_cell_new(void)
{
    return g_object_new(kf_usage_cell_get_type(), NULL);
}
