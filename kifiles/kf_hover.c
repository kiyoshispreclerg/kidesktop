/* kf_hover.c - highlight of the item under the pointer.
 *
 * GTK2's icon and tree views only paint selection; they don't mark the item
 * the pointer is over (that arrived with GTK3). The xisserve launcher gets a
 * similar effect by *selecting* what is under the pointer; here nothing is
 * selected, as in KDE4's Dolphin or Explorer in Windows 7: a translucent box in
 * the theme's selection color is drawn at the item before the view paints its items (behind them).
 * Colors come from the GTK theme, so it follows it.
 *
 * Finding the item's rectangle: the tree view reports row areas, the icon view
 * does not (get_cell_rect is GTK3), so the box is found by hit-testing around
 * the pointer -- a binary search per side, done only when the item under the
 * pointer changes. */
#include "kf.h"

/* The scrolling window the items are drawn in. GTK2 exposes it for the tree
 * view only; for the icon view it is the single child of the widget's window. */
static GdkWindow *bin_of(KfPane *p, GtkWidget *w)
{
    if (w == p->treeview)
        return gtk_tree_view_get_bin_window(GTK_TREE_VIEW(w));
    GdkWindow *top = gtk_widget_get_window(w);
    GList *kids = top ? gdk_window_get_children(top) : NULL;
    GdkWindow *bin = kids ? kids->data : NULL;
    g_list_free(kids);
    return bin;
}

static GtkTreePath *path_at(KfPane *p, GtkWidget *w, int x, int y)
{
    if (w == p->iconview)
        return gtk_icon_view_get_path_at_pos(GTK_ICON_VIEW(w), x, y);
    GtkTreePath *path = NULL;
    gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(w), x, y, &path, NULL, NULL, NULL);
    return path;
}

static gboolean same_item(KfPane *p, GtkWidget *w, GtkTreePath *want, int x, int y)
{
    GtkTreePath *at = path_at(p, w, x, y);
    gboolean same = at && gtk_tree_path_compare(at, want) == 0;
    if (at)
        gtk_tree_path_free(at);
    return same;
}

/* Last coordinate along one axis (from `inside`, known to be in the item) that
 * still belongs to the item; dir is +1 or -1, `limit` how far to look. */
static int edge(KfPane *p, GtkWidget *w, GtkTreePath *path, int px, int py, gboolean horizontal, int dir, int limit)
{
    int base = horizontal ? px : py;
    int lo = 0, hi = limit;             /* offsets from the pointer: lo inside, hi outside */
    gboolean far_in = horizontal ? same_item(p, w, path, px + dir * hi, py) : same_item(p, w, path, px, py + dir * hi);
    if (far_in)
        return base + dir * hi;         /* bigger than we looked for: clamp */
    while (hi - lo > 1) {
        int mid = (lo + hi) / 2;
        gboolean in = horizontal ? same_item(p, w, path, px + dir * mid, py)
                                 : same_item(p, w, path, px, py + dir * mid);
        if (in)
            lo = mid;
        else
            hi = mid;
    }
    return base + dir * lo;
}

static void invalidate(KfPane *p, GtkWidget *w, const GdkRectangle *r)
{
    GdkWindow *bin = bin_of(p, w);
    if (bin) {
        GdkRectangle grown = { r->x - 2, r->y - 2, r->width + 4, r->height + 4 };
        gdk_window_invalidate_rect(bin, &grown, FALSE);
    }
}

static void set_rect(KfPane *p, GtkWidget *w, gboolean has, const GdkRectangle *r)
{
    if (p->hover_has == has && (!has || !r || (p->hover_rect.x == r->x && p->hover_rect.y == r->y &&
                                         p->hover_rect.width == r->width && p->hover_rect.height == r->height)))
        return;
    if (p->hover_has)
        invalidate(p, w, &p->hover_rect);
    p->hover_has = has;
    if (has && r) {
        p->hover_rect = *r;
        invalidate(p, w, r);
    }
}

/* The box GTK paints for a selected icon: the item's cells plus the theme's
 * item padding on every side, as wide as the cell and as tall as the *row*
 * (the tallest item in it, so a one-line name sits in a box as tall as its
 * neighbour's four-line one). Hit-testing only covers the cells, so the rest is
 * derived: the cell hull from a search around the pointer, the row's height by
 * probing the other items of the row, found one column pitch apart. The pitch
 * and the gap between rows are the spacing properties plus the 2 px GTK adds
 * (measured on screenshots of selected items). */
static void icon_box(KfPane *p, GtkWidget *w, GtkTreePath *path, int x, int y, GdkRectangle *r)
{
    GtkIconView *iv = GTK_ICON_VIEW(w);
    GtkAllocation a;
    gtk_widget_get_allocation(w, &a);
    (void)a;
    int y0 = edge(p, w, path, x, y, FALSE, -1, 600), y1 = edge(p, w, path, x, y, FALSE, +1, 600);
    int pad = gtk_icon_view_get_item_padding(iv);
    /* Hit-testing covers only what the cells draw, so the horizontal extent
     * comes from the regular grid instead: every cell is as wide as the text
     * cell we fixed, columns are one pitch apart, starting at the margin. */
    int cell_w = 0, cell_h = 0;
    gtk_cell_renderer_get_fixed_size(p->icon_text_r, &cell_w, &cell_h);
    int bw = cell_w + 2 * pad;
    int bx = gtk_icon_view_get_margin(iv) + gtk_icon_view_get_item_column(iv, path) *
             (bw + gtk_icon_view_get_column_spacing(iv) + 2);
    int row = gtk_icon_view_get_item_row(iv, path);
    if (p->hover_row != row) {
        int pitch = bw + gtk_icon_view_get_column_spacing(iv) + 2;
        int bottom = y1;
        for (int dir = -1; dir <= 1; dir += 2) {
            for (int k = 1; k < 64; k++) {
                int px = bx + bw / 2 + dir * k * pitch, py = y0 + 2;
                GtkTreePath *other = path_at(p, w, px, py);
                if (!other)
                    break;
                gboolean same_row = gtk_icon_view_get_item_row(iv, other) == row;
                if (same_row) {
                    int ob = edge(p, w, other, px, py, FALSE, +1, 600);
                    if (ob > bottom)
                        bottom = ob;
                }
                gtk_tree_path_free(other);
                if (!same_row)
                    break;
            }
        }
        p->hover_row = row;
        p->hover_row_y0 = y0;
        p->hover_row_y1 = bottom;
    }
    r->x = bx;
    r->width = bw + 2;                      /* GTK's box is 2 px wider than cells + padding... */
    r->y = p->hover_row_y0 - pad - 1;      /* hit-testing starts 1 px inside the cell */
    r->height = p->hover_row_y1 - p->hover_row_y0 + 2 + 2 * pad;   /* ...and 1 px taller */
}

static void track(KfPane *p, GtkWidget *w, int x, int y)
{
    if (!p->app->cfg.hover) {
        set_rect(p, w, FALSE, NULL);
        return;
    }
    GtkTreePath *path = path_at(p, w, x, y);
    if (!path) {
        set_rect(p, w, FALSE, NULL);
        return;
    }
    GdkRectangle r;
    if (w == p->treeview) {
        gtk_tree_view_get_background_area(GTK_TREE_VIEW(w), path, NULL, &r);
        /* With no column the area comes back 0 wide: the box spans the view. */
        GtkAllocation va;
        gtk_widget_get_allocation(w, &va);
        r.x = 0;
        r.width = va.width;
    } else if (p->hover_has && x >= p->hover_rect.x && x < p->hover_rect.x + p->hover_rect.width &&
               y >= p->hover_rect.y && y < p->hover_rect.y + p->hover_rect.height) {
        gtk_tree_path_free(path);
        return;                          /* still inside the same box: nothing to do */
    } else {
        icon_box(p, w, path, x, y, &r);
    }
    gtk_tree_path_free(path);
    set_rect(p, w, TRUE, &r);
}

static gboolean on_motion(GtkWidget *w, GdkEventMotion *ev, gpointer data)
{
    KfPane *p = data;
    if (ev->window != bin_of(p, w))
        return FALSE;
    track(p, w, (int)ev->x, (int)ev->y);
    return FALSE;
}

/* Items reflow when the view changes width: what was measured is stale. */
static void on_resize(GtkWidget *w, GtkAllocation *a, gpointer data)
{
    (void)w; (void)a;
    kf_hover_clear(data);
}

static gboolean on_leave(GtkWidget *w, GdkEventCrossing *ev, gpointer data)
{
    (void)ev;
    set_rect(data, w, FALSE, NULL);
    return FALSE;
}

static gboolean on_expose(GtkWidget *w, GdkEventExpose *ev, gpointer data)
{
    KfPane *p = data;
    if (!p->hover_has || !p->app->cfg.hover || ev->window != bin_of(p, w))
        return FALSE;
    const GdkColor *c = &gtk_widget_get_style(w)->bg[GTK_STATE_SELECTED];
    double r = c->red / 65535.0, g = c->green / 65535.0, b = c->blue / 65535.0;
    cairo_t *cr = gdk_cairo_create(ev->window);
    gdk_cairo_region(cr, ev->region);
    cairo_clip(cr);
    const GdkRectangle *h = &p->hover_rect;
    double x = h->x + 1.5, y = h->y + 1.5, ww = h->width - 3, hh = h->height - 3, rad = 4;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + ww - rad, y + rad, rad, -G_PI / 2, 0);
    cairo_arc(cr, x + ww - rad, y + hh - rad, rad, 0, G_PI / 2);
    cairo_arc(cr, x + rad, y + hh - rad, rad, G_PI / 2, G_PI);
    cairo_arc(cr, x + rad, y + rad, rad, G_PI, 3 * G_PI / 2);
    cairo_close_path(cr);
    cairo_set_source_rgba(cr, r, g, b, 0.18);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, r, g, b, 0.55);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);
    cairo_destroy(cr);
    return FALSE;
}

void kf_hover_clear(KfPane *p)
{
    p->hover_row = -1;
    GtkWidget *w = p->view == KF_VIEW_ICONS ? p->iconview : p->treeview;
    set_rect(p, w, FALSE, NULL);
}

/* The content scrolled or reloaded under a pointer that didn't move. */
void kf_hover_refresh(KfPane *p)
{
    if (!p->app->cfg.hover)
        return;
    GtkWidget *w = p->view == KF_VIEW_ICONS ? p->iconview : p->treeview;
    GdkWindow *bin = bin_of(p, w);
    if (!bin || !gtk_widget_get_mapped(w))
        return;
    int x, y, vx, vy;
    gdk_window_get_pointer(bin, &x, &y, NULL);
    GdkWindow *top = gtk_widget_get_window(w);
    gdk_window_get_pointer(top, &vx, &vy, NULL);
    /* Only when the pointer is over the view itself. */
    if (vx < 0 || vy < 0 || vx >= gdk_window_get_width(top) || vy >= gdk_window_get_height(top)) {
        set_rect(p, w, FALSE, NULL);
        return;
    }
    p->hover_row = -1;
    p->hover_has = FALSE;               /* the old box is stale after a scroll/reload */
    track(p, w, x, y);
}

void kf_hover_apply(KfPane *p)
{
    if (!p->app->cfg.hover)
        kf_hover_clear(p);
}

void kf_hover_setup(KfPane *p)
{
    GtkWidget *views[2] = { p->iconview, p->treeview };
    for (int i = 0; i < 2; i++) {
        gtk_widget_add_events(views[i], GDK_POINTER_MOTION_MASK | GDK_LEAVE_NOTIFY_MASK);
        g_signal_connect(views[i], "motion-notify-event", G_CALLBACK(on_motion), p);
        g_signal_connect(views[i], "leave-notify-event", G_CALLBACK(on_leave), p);
        g_signal_connect(views[i], "size-allocate", G_CALLBACK(on_resize), p);
        /* Icon view: before it paints (its expose handler returns TRUE, which
         * would keep an "after" handler from running), so the box ends up behind
         * the icons as in KDE4. Tree view: after, because it paints opaque row
         * backgrounds that would cover a box drawn first. */
        if (views[i] == p->iconview)
            g_signal_connect(views[i], "expose-event", G_CALLBACK(on_expose), p);
        else
            g_signal_connect_after(views[i], "expose-event", G_CALLBACK(on_expose), p);
    }
}
