/* kf_hover.c - highlight of the item under the pointer.
 *
 * GTK2's icon and tree views only paint selection; they don't mark the item
 * the pointer is over (that arrived with GTK3). The xisserve launcher gets a
 * similar effect by *selecting* what is under the pointer; here nothing is
 * selected, as in KDE4's Dolphin or Explorer in Windows 7: a translucent box in
 * the theme's selection color is drawn at the item before the view paints its items (behind them).
 * Colors come from the GTK theme, so it follows it.
 *
 * Finding the item's rectangle: the tree view reports row areas. The icon view
 * does not, and its hit-testing only covers what each cell drew (an icon
 * smaller than its cell, a short name), so anything derived from it changes
 * from file to file. The box GTK paints for a selected item is the item's
 * layout rectangle instead, so that layout is redone here with the same
 * inputs -- gtk_icon_view_layout_single_row() and calculate_item_size2() in
 * gtkiconview.c, vertical orientation, left-to-right, no colspan (our cells
 * have fixed widths) -- and both the box and what is "under the pointer" come from it. */
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

/* Size of one cell for the item at `it`, asked the way GTK asks it: the cell's
 * attributes set from the model, then get_size() with no area. GTK sets the
 * attributes again before it next uses the renderers, so borrowing them is safe. */
static void cell_size(GtkCellRenderer *cell, GtkWidget *w, const char *prop, GtkTreeModel *m, GtkTreeIter *it,
                      int col, int *cw, int *ch)
{
    GValue v = G_VALUE_INIT;
    gtk_tree_model_get_value(m, it, col, &v);
    g_object_set_property(G_OBJECT(cell), prop, &v);
    g_value_unset(&v);
    gtk_cell_renderer_get_size(cell, w, NULL, NULL, NULL, cw, ch);
}

/* Redo the icon view's layout: item width, columns and each row's top and height. */
static void layout(KfPane *p, GtkWidget *w)
{
    GtkIconView *iv = GTK_ICON_VIEW(w);
    GtkTreeModel *m = gtk_icon_view_get_model(iv);
    if (!p->hl_rows)
        p->hl_rows = g_array_new(FALSE, FALSE, sizeof(GdkRectangle));
    g_array_set_size(p->hl_rows, 0);
    p->hl_valid = TRUE;
    p->hl_cols = 0;
    if (!m)
        return;
    int fw = 1;
    gtk_widget_style_get(w, "focus-line-width", &fw, NULL);
    int pad = gtk_icon_view_get_item_padding(iv), margin = gtk_icon_view_get_margin(iv);
    int cs = gtk_icon_view_get_column_spacing(iv), rs = gtk_icon_view_get_row_spacing(iv);
    int spacing = gtk_icon_view_get_spacing(iv);
    gboolean pix_on = gtk_cell_renderer_get_visible(p->icon_pix_r), txt_on = gtk_cell_renderer_get_visible(p->icon_text_r);

    int n = gtk_tree_model_iter_n_children(m, NULL);
    int *ph = g_new0(int, n ? n : 1), *th = g_new0(int, n ? n : 1);
    int item_w = 0;
    GtkTreeIter it;
    gboolean ok = gtk_tree_model_get_iter_first(m, &it);
    for (int i = 0; ok && i < n; i++, ok = gtk_tree_model_iter_next(m, &it)) {
        int pw = 0, tw = 0;
        if (pix_on)
            cell_size(p->icon_pix_r, w, "pixbuf", m, &it, KF_COL_ICON, &pw, &ph[i]);
        if (txt_on)
            cell_size(p->icon_text_r, w, "text", m, &it, KF_COL_NAME, &tw, &th[i]);
        item_w = MAX(item_w, MAX(pw, tw) + 2 * pad);
    }

    /* As many columns as fit the allocation, at least one. */
    GtkAllocation a;
    gtk_widget_get_allocation(w, &a);
    int cur = 2 * (margin + fw), cols = 0;
    for (;;) {
        cur += item_w;
        if (cols > 0 && cur > a.width)
            break;
        cur += cs + 2 * fw;
        cols++;
        if (cols >= n)
            break;
    }
    p->hl_cols = MAX(cols, 1);
    p->hl_item_w = item_w;
    p->hl_x0 = margin + fw;
    p->hl_pitch = item_w + cs + 2 * fw;

    /* Every item of a row gets the row's tallest icon and name cells. */
    int y = margin;
    for (int first = 0; first < n; first += p->hl_cols) {
        int mp = 0, mt = 0;
        for (int i = first; i < n && i < first + p->hl_cols; i++) {
            mp = MAX(mp, ph[i]);
            mt = MAX(mt, th[i]);
        }
        GdkRectangle r = { 0, y + fw, 0, 2 * pad };
        if (pix_on)
            r.height += mp;
        if (txt_on)
            r.height += mt + (pix_on ? spacing : 0);
        g_array_append_val(p->hl_rows, r);
        y = r.y + r.height + fw + rs;
    }
    g_free(ph);
    g_free(th);
}

/* The item whose layout rectangle contains (x, y), as GTK would paint it. */
static gboolean icon_item_at(KfPane *p, GtkWidget *w, int x, int y, GdkRectangle *r)
{
    if (!p->hl_valid)
        layout(p, w);
    if (p->hl_cols <= 0 || x < p->hl_x0)
        return FALSE;
    int col = (x - p->hl_x0) / p->hl_pitch;
    int cx = p->hl_x0 + col * p->hl_pitch;
    if (col >= p->hl_cols || x >= cx + p->hl_item_w)
        return FALSE;                    /* past the last column or in the gap after one */
    for (guint row = 0; row < p->hl_rows->len; row++) {
        GdkRectangle *rr = &g_array_index(p->hl_rows, GdkRectangle, row);
        if (y < rr->y)
            return FALSE;                /* in the gap above this row */
        if (y >= rr->y + rr->height)
            continue;
        GtkTreeModel *m = gtk_icon_view_get_model(GTK_ICON_VIEW(w));
        int index = (int)row * p->hl_cols + col;
        if (!m || index >= gtk_tree_model_iter_n_children(m, NULL))
            return FALSE;                /* the short last row */
        *r = (GdkRectangle){ cx, rr->y, p->hl_item_w, rr->height };
        return TRUE;
    }
    return FALSE;
}

static void track(KfPane *p, GtkWidget *w, int x, int y)
{
    if (!p->app->cfg.hover) {
        set_rect(p, w, FALSE, NULL);
        return;
    }
    GdkRectangle r;
    if (w == p->iconview) {
        set_rect(p, w, icon_item_at(p, w, x, y, &r), &r);
        return;
    }
    GtkTreePath *path = NULL;
    gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(w), x, y, &path, NULL, NULL, NULL);
    if (!path) {
        set_rect(p, w, FALSE, NULL);
        return;
    }
    gtk_tree_view_get_background_area(GTK_TREE_VIEW(w), path, NULL, &r);
    /* With no column the area comes back 0 wide: the box spans the view. */
    GtkAllocation va;
    gtk_widget_get_allocation(w, &va);
    r.x = 0;
    r.width = va.width;
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
    /* The 1 px outline runs along the rectangle's own edge pixels, as GTK's selection box fills them. */
    double x = h->x + 0.5, y = h->y + 0.5, ww = h->width - 1, hh = h->height - 1, rad = 4;
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

/* Names, icons, the number of items or the font changed: lay out again. */
static void stale(KfPane *p)
{
    p->hl_valid = FALSE;
}

void kf_hover_clear(KfPane *p)
{
    p->hl_valid = FALSE;
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
    /* Scrolling moves the already-painted pixels, box included: the old rect no
     * longer says where the stale box is, so repaint everything visible. */
    if (p->hover_has)
        gdk_window_invalidate_rect(bin, NULL, FALSE);
    gdk_window_get_pointer(bin, &x, &y, NULL);
    GdkWindow *top = gtk_widget_get_window(w);
    gdk_window_get_pointer(top, &vx, &vy, NULL);
    /* Only when the pointer is over the view itself. */
    if (vx < 0 || vy < 0 || vx >= gdk_window_get_width(top) || vy >= gdk_window_get_height(top)) {
        set_rect(p, w, FALSE, NULL);
        return;
    }
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
    static const char *const model_sigs[] = { "row-inserted", "row-deleted", "row-changed", "rows-reordered" };
    GtkTreeModel *m = gtk_icon_view_get_model(GTK_ICON_VIEW(p->iconview));
    for (guint i = 0; m && i < G_N_ELEMENTS(model_sigs); i++)
        g_signal_connect_swapped(m, model_sigs[i], G_CALLBACK(stale), p);
    g_signal_connect_swapped(p->iconview, "style-set", G_CALLBACK(stale), p);
}
