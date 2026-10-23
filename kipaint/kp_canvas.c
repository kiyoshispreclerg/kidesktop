/* kp_canvas.c - the image view.
 *
 * A plain GtkDrawingArea with its own scrollbars: the adjustments count
 * screen pixels of the zoomed image, so a big image at 32x never becomes a
 * huge X window (a GtkViewport child would). Only the exposed area is
 * drawn: checkerboard, the visible layers scaled (NEAREST from 100% up),
 * the pixel grid from 800% up, and the brush outline under the pointer.
 * Ctrl+wheel zooms around the pointer, the wheel scrolls (Shift for
 * sideways), the middle button drags the view. */
#include "kp.h"

#include <math.h>

static const double ZOOMS[] = { 0.125, 0.25, 1.0 / 3, 0.5, 2.0 / 3, 1, 1.5, 2, 3, 4, 6, 8, 12, 16, 24, 32 };
#define N_ZOOMS (int)(sizeof(ZOOMS) / sizeof(ZOOMS[0]))
#define GRID_MIN_ZOOM 8

static GtkWidget *g_area;
static GtkAdjustment *g_hadj, *g_vadj;
static double g_zoom = 1;
static gboolean g_need_fit;
static cairo_pattern_t *g_checker;

static const KpTool *g_stroke_tool;
static int g_stroke_button;

static gboolean g_panning;
static double g_pan_x, g_pan_y, g_pan_h, g_pan_v;

static gboolean g_ptr_in;
static double g_ptr_x, g_ptr_y;   /* screen */

/* The image point a run of zoom steps keeps under the pointer. Recomputing
 * it every step would drift while the image is still centered (smaller
 * than the view), where the point under the pointer can't be kept. */
static gboolean g_anchor_valid;
static double g_anchor_sx, g_anchor_sy, g_anchor_ix, g_anchor_iy;

/* ---- geometry -------------------------------------------------------- */

static void view_size(int *w, int *h)
{
    *w = g_area->allocation.width;
    *h = g_area->allocation.height;
}

/* Screen position of image pixel (0,0). */
static void origin(double *ox, double *oy)
{
    int vw, vh;
    view_size(&vw, &vh);
    double iw = kp.doc->w * g_zoom, ih = kp.doc->h * g_zoom;
    *ox = iw <= vw ? floor((vw - iw) / 2) : -floor(gtk_adjustment_get_value(g_hadj));
    *oy = ih <= vh ? floor((vh - ih) / 2) : -floor(gtk_adjustment_get_value(g_vadj));
}

static void to_image(double sx, double sy, double *ix, double *iy)
{
    double ox, oy;
    origin(&ox, &oy);
    *ix = (sx - ox) / g_zoom;
    *iy = (sy - oy) / g_zoom;
}

static void adj_setup(GtkAdjustment *a, double upper, double page, double value)
{
    a->lower = 0;
    a->upper = MAX(upper, page);
    a->page_size = page;
    a->page_increment = page * 0.9;
    a->step_increment = MAX(page / 10, 1);
    a->value = CLAMP(value, 0, a->upper - page);
    gtk_adjustment_changed(a);
    gtk_adjustment_value_changed(a);
}

/* Re-ranges the scrollbars after a zoom or resize; (vx, vy) is the wanted
 * scroll position. */
static void adjustments_update(double vx, double vy)
{
    int vw, vh;
    view_size(&vw, &vh);
    adj_setup(g_hadj, kp.doc->w * g_zoom, vw, vx);
    adj_setup(g_vadj, kp.doc->h * g_zoom, vh, vy);
}

/* ---- zoom ------------------------------------------------------------ */

static void zoom_to(double z, double sx, double sy)
{
    z = CLAMP(z, ZOOMS[0], ZOOMS[N_ZOOMS - 1]);
    if (!g_anchor_valid || sx != g_anchor_sx || sy != g_anchor_sy) {
        to_image(sx, sy, &g_anchor_ix, &g_anchor_iy);
        g_anchor_sx = sx;
        g_anchor_sy = sy;
        g_anchor_valid = TRUE;
    }
    double ix = g_anchor_ix, iy = g_anchor_iy;
    g_zoom = z;
    /* keep image point (ix,iy) under (sx,sy) */
    adjustments_update(ix * z - sx, iy * z - sy);
    gtk_widget_queue_draw(g_area);
    kp_status_pointer(-1, -1, FALSE);
}

static void zoom_step_at(int dir, double sx, double sy)
{
    double z = g_zoom;
    if (dir > 0) {
        for (int i = 0; i < N_ZOOMS; i++)
            if (ZOOMS[i] > g_zoom + 1e-6) {
                z = ZOOMS[i];
                break;
            }
    } else {
        for (int i = N_ZOOMS - 1; i >= 0; i--)
            if (ZOOMS[i] < g_zoom - 1e-6) {
                z = ZOOMS[i];
                break;
            }
    }
    zoom_to(z, sx, sy);
}

void kp_canvas_zoom_step(int dir)
{
    int vw, vh;
    view_size(&vw, &vh);
    zoom_step_at(dir, vw / 2.0, vh / 2.0);
}

void kp_canvas_zoom_100(void)
{
    int vw, vh;
    view_size(&vw, &vh);
    zoom_to(1, vw / 2.0, vh / 2.0);
}

static double fit_zoom(void)
{
    int vw, vh;
    view_size(&vw, &vh);
    return MIN((vw - 16.0) / kp.doc->w, (vh - 16.0) / kp.doc->h);
}

void kp_canvas_zoom_fit(void)
{
    g_anchor_valid = FALSE;
    g_zoom = CLAMP(fit_zoom(), ZOOMS[0], ZOOMS[N_ZOOMS - 1]);
    adjustments_update(0, 0);
    gtk_widget_queue_draw(g_area);
    kp_status_pointer(-1, -1, FALSE);
}

double kp_canvas_zoom(void)
{
    return g_zoom;
}

/* New document: 100%, or smaller when it doesn't fit the window. */
void kp_canvas_doc_changed(void)
{
    g_stroke_tool = NULL;
    g_anchor_valid = FALSE;
    if (!GTK_WIDGET_REALIZED(g_area)) {
        g_need_fit = TRUE;
        return;
    }
    g_zoom = CLAMP(MIN(fit_zoom(), 1), ZOOMS[0], 1);
    adjustments_update(0, 0);
    gtk_widget_queue_draw(g_area);
    kp_status_pointer(-1, -1, FALSE);
}

/* ---- drawing --------------------------------------------------------- */

static cairo_pattern_t *checker(void)
{
    if (g_checker)
        return g_checker;
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, 16, 16);
    cairo_t *cr = cairo_create(s);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);
    cairo_set_source_rgb(cr, 0.8, 0.8, 0.8);
    cairo_rectangle(cr, 0, 0, 8, 8);
    cairo_rectangle(cr, 8, 8, 8, 8);
    cairo_fill(cr);
    cairo_destroy(cr);
    g_checker = cairo_pattern_create_for_surface(s);
    cairo_pattern_set_extend(g_checker, CAIRO_EXTEND_REPEAT);
    cairo_surface_destroy(s);
    return g_checker;
}

/* Screen box of the brush outline at the pointer; FALSE when none. */
static gboolean outline_box(double *x, double *y, double *w, double *h)
{
    const KpTool *t = kp.tool;
    if (!g_ptr_in || g_panning || !t->outline)
        return FALSE;
    double ox, oy, ix, iy, size = kp_tool_size();
    origin(&ox, &oy);
    to_image(g_ptr_x, g_ptr_y, &ix, &iy);
    if (t->square) {
        double x0 = floor(ix) - floor((size - 1) / 2), y0 = floor(iy) - floor((size - 1) / 2);
        *x = ox + x0 * g_zoom;
        *y = oy + y0 * g_zoom;
    } else {
        *x = g_ptr_x - size * g_zoom / 2;
        *y = g_ptr_y - size * g_zoom / 2;
    }
    *w = *h = size * g_zoom;
    return *w >= 3;   /* a 1 px pencil at low zoom: the cursor is enough */
}

static void outline_invalidate(void)
{
    double x, y, w, h;
    if (outline_box(&x, &y, &w, &h))
        gtk_widget_queue_draw_area(g_area, (int)floor(x) - 3, (int)floor(y) - 3, (int)ceil(w) + 7, (int)ceil(h) + 7);
}

static void draw_grid(cairo_t *cr, double ox, double oy)
{
    double cx0, cy0, cx1, cy1;
    cairo_clip_extents(cr, &cx0, &cy0, &cx1, &cy1);
    int x0 = MAX(0, (int)floor((cx0 - ox) / g_zoom)), x1 = MIN(kp.doc->w, (int)ceil((cx1 - ox) / g_zoom));
    int y0 = MAX(0, (int)floor((cy0 - oy) / g_zoom)), y1 = MIN(kp.doc->h, (int)ceil((cy1 - oy) / g_zoom));
    cairo_set_line_width(cr, 1);
    cairo_set_source_rgba(cr, 0.5, 0.5, 0.5, 0.45);
    for (int x = x0; x <= x1; x++) {
        double sx = floor(ox + x * g_zoom) + 0.5;
        cairo_move_to(cr, sx, oy + y0 * g_zoom);
        cairo_line_to(cr, sx, oy + y1 * g_zoom);
    }
    for (int y = y0; y <= y1; y++) {
        double sy = floor(oy + y * g_zoom) + 0.5;
        cairo_move_to(cr, ox + x0 * g_zoom, sy);
        cairo_line_to(cr, ox + x1 * g_zoom, sy);
    }
    cairo_stroke(cr);
}

static gboolean on_expose(GtkWidget *w, GdkEventExpose *ev, gpointer data)
{
    (void)data;
    cairo_t *cr = gdk_cairo_create(w->window);
    gdk_cairo_region(cr, ev->region);
    cairo_clip(cr);

    gdk_cairo_set_source_color(cr, &w->style->dark[GTK_STATE_NORMAL]);
    cairo_paint(cr);

    double ox, oy;
    origin(&ox, &oy);
    double iw = kp.doc->w * g_zoom, ih = kp.doc->h * g_zoom;

    cairo_matrix_t m;
    cairo_matrix_init_translate(&m, -ox, -oy);
    cairo_pattern_set_matrix(checker(), &m);
    cairo_set_source(cr, checker());
    cairo_rectangle(cr, ox, oy, iw, ih);
    cairo_fill(cr);

    cairo_save(cr);
    cairo_rectangle(cr, ox, oy, iw, ih);
    cairo_clip(cr);
    cairo_translate(cr, ox, oy);
    cairo_scale(cr, g_zoom, g_zoom);
    for (guint i = 0; i < kp.doc->layers->len; i++) {
        KpLayer *l = g_ptr_array_index(kp.doc->layers, i);
        if (!l->visible)
            continue;
        cairo_set_source_surface(cr, l->surf, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), g_zoom >= 1 ? CAIRO_FILTER_NEAREST : CAIRO_FILTER_GOOD);
        cairo_paint(cr);
    }
    cairo_restore(cr);

    if (kp.grid && g_zoom >= GRID_MIN_ZOOM)
        draw_grid(cr, ox, oy);

    double bx, by, bw, bh;
    if (outline_box(&bx, &by, &bw, &bh)) {
        if (kp.tool->square)
            cairo_rectangle(cr, floor(bx) + 0.5, floor(by) + 0.5, floor(bw) - 1, floor(bh) - 1);
        else
            cairo_arc(cr, bx + bw / 2, by + bh / 2, bw / 2, 0, 2 * G_PI);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.7);
        cairo_set_line_width(cr, 3);
        cairo_stroke_preserve(cr);
        cairo_set_source_rgb(cr, 0, 0, 0);
        cairo_set_line_width(cr, 1);
        cairo_stroke(cr);
    }
    cairo_destroy(cr);
    return TRUE;
}

void kp_canvas_damage(int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0 || !GTK_WIDGET_REALIZED(g_area))
        return;
    double ox, oy;
    origin(&ox, &oy);
    int sx = (int)floor(ox + x * g_zoom) - 1, sy = (int)floor(oy + y * g_zoom) - 1;
    int sw = (int)ceil(w * g_zoom) + 3, sh = (int)ceil(h * g_zoom) + 3;
    gtk_widget_queue_draw_area(g_area, sx, sy, sw, sh);
}

void kp_canvas_redraw(void)
{
    gtk_widget_queue_draw(g_area);
}

void kp_canvas_grab_focus(void)
{
    gtk_widget_grab_focus(g_area);
}

/* ---- input ----------------------------------------------------------- */

static void update_cursor(void)
{
    if (!GTK_WIDGET_REALIZED(g_area))
        return;
    GdkCursorType type = g_panning ? GDK_FLEUR : kp.tool->outline ? GDK_CROSSHAIR : GDK_TCROSS;
    GdkCursor *c = gdk_cursor_new(type);
    gdk_window_set_cursor(g_area->window, c);
    gdk_cursor_unref(c);
}

void kp_canvas_tool_changed(void)
{
    update_cursor();
    gtk_widget_queue_draw(g_area);
}

static void pointer_moved(double sx, double sy)
{
    outline_invalidate();
    g_ptr_in = TRUE;
    g_ptr_x = sx;
    g_ptr_y = sy;
    outline_invalidate();
    double ix, iy;
    to_image(sx, sy, &ix, &iy);
    int px = (int)floor(ix), py = (int)floor(iy);
    kp_status_pointer(px, py, px >= 0 && py >= 0 && px < kp.doc->w && py < kp.doc->h);
}

static gboolean on_press(GtkWidget *w, GdkEventButton *ev, gpointer data)
{
    (void)w;
    (void)data;
    if (ev->type != GDK_BUTTON_PRESS)
        return TRUE;   /* double/triple clicks: the single presses already went */
    gtk_widget_grab_focus(g_area);
    if (ev->button == 2 && !g_stroke_tool) {
        g_panning = TRUE;
        g_pan_x = ev->x_root;
        g_pan_y = ev->y_root;
        g_pan_h = gtk_adjustment_get_value(g_hadj);
        g_pan_v = gtk_adjustment_get_value(g_vadj);
        outline_invalidate();
        update_cursor();
        return TRUE;
    }
    if ((ev->button != 1 && ev->button != 3) || g_stroke_tool || g_panning)
        return TRUE;
    double ix, iy;
    to_image(ev->x, ev->y, &ix, &iy);
    g_stroke_tool = kp.tool;
    g_stroke_button = ev->button;
    if (g_stroke_tool->press)
        g_stroke_tool->press(ix, iy, ev->button);
    return TRUE;
}

static gboolean on_motion(GtkWidget *w, GdkEventMotion *ev, gpointer data)
{
    (void)w;
    (void)data;
    if (g_panning) {
        gtk_adjustment_set_value(g_hadj, CLAMP(g_pan_h - (ev->x_root - g_pan_x), 0, g_hadj->upper - g_hadj->page_size));
        gtk_adjustment_set_value(g_vadj, CLAMP(g_pan_v - (ev->y_root - g_pan_y), 0, g_vadj->upper - g_vadj->page_size));
        return TRUE;
    }
    if (ev->x != g_anchor_sx || ev->y != g_anchor_sy)
        g_anchor_valid = FALSE;
    pointer_moved(ev->x, ev->y);
    if (g_stroke_tool && g_stroke_tool->motion) {
        double ix, iy;
        to_image(ev->x, ev->y, &ix, &iy);
        g_stroke_tool->motion(ix, iy);
    }
    return TRUE;
}

static gboolean on_release(GtkWidget *w, GdkEventButton *ev, gpointer data)
{
    (void)w;
    (void)data;
    if (ev->button == 2 && g_panning) {
        g_panning = FALSE;
        update_cursor();
        pointer_moved(ev->x, ev->y);
        return TRUE;
    }
    if (g_stroke_tool && (int)ev->button == g_stroke_button) {
        const KpTool *t = g_stroke_tool;
        g_stroke_tool = NULL;
        if (t->release)
            t->release();
    }
    return TRUE;
}

static gboolean on_leave(GtkWidget *w, GdkEventCrossing *ev, gpointer data)
{
    (void)w;
    (void)ev;
    (void)data;
    outline_invalidate();
    g_ptr_in = FALSE;
    kp_status_pointer(-1, -1, FALSE);
    return FALSE;
}

static gboolean on_scroll(GtkWidget *w, GdkEventScroll *ev, gpointer data)
{
    (void)w;
    (void)data;
    if (ev->state & GDK_CONTROL_MASK) {
        if (ev->direction == GDK_SCROLL_UP || ev->direction == GDK_SCROLL_DOWN)
            zoom_step_at(ev->direction == GDK_SCROLL_UP ? 1 : -1, ev->x, ev->y);
        return TRUE;
    }
    gboolean horiz = ev->direction == GDK_SCROLL_LEFT || ev->direction == GDK_SCROLL_RIGHT ||
                     (ev->state & GDK_SHIFT_MASK);
    GtkAdjustment *a = horiz ? g_hadj : g_vadj;
    g_anchor_valid = FALSE;
    int sign = ev->direction == GDK_SCROLL_UP || ev->direction == GDK_SCROLL_LEFT ? -1 : 1;
    double v = gtk_adjustment_get_value(a) + sign * a->step_increment;
    gtk_adjustment_set_value(a, CLAMP(v, 0, a->upper - a->page_size));
    pointer_moved(ev->x, ev->y);
    return TRUE;
}

static void on_size_allocate(GtkWidget *w, GtkAllocation *a, gpointer data)
{
    (void)w;
    (void)a;
    (void)data;
    if (g_need_fit) {
        g_need_fit = FALSE;
        g_zoom = CLAMP(MIN(fit_zoom(), 1), ZOOMS[0], 1);
        adjustments_update(0, 0);
        kp_status_pointer(-1, -1, FALSE);
        return;
    }
    adjustments_update(gtk_adjustment_get_value(g_hadj), gtk_adjustment_get_value(g_vadj));
}

static void on_realize(GtkWidget *w, gpointer data)
{
    (void)w;
    (void)data;
    update_cursor();
}

static void on_scrolled(GtkAdjustment *a, gpointer data)
{
    (void)a;
    (void)data;
    gtk_widget_queue_draw(g_area);
}

GtkWidget *kp_canvas_new(void)
{
    g_hadj = GTK_ADJUSTMENT(gtk_adjustment_new(0, 0, 1, 1, 1, 1));
    g_vadj = GTK_ADJUSTMENT(gtk_adjustment_new(0, 0, 1, 1, 1, 1));
    g_signal_connect(g_hadj, "value-changed", G_CALLBACK(on_scrolled), NULL);
    g_signal_connect(g_vadj, "value-changed", G_CALLBACK(on_scrolled), NULL);

    g_area = gtk_drawing_area_new();
    GTK_WIDGET_SET_FLAGS(g_area, GTK_CAN_FOCUS);
    gtk_widget_add_events(g_area, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK |
                                      GDK_LEAVE_NOTIFY_MASK | GDK_SCROLL_MASK);
    /* Every pixel is drawn in on_expose; skip GTK clearing it first. */
    gtk_widget_set_app_paintable(g_area, TRUE);
    g_signal_connect(g_area, "expose-event", G_CALLBACK(on_expose), NULL);
    g_signal_connect(g_area, "button-press-event", G_CALLBACK(on_press), NULL);
    g_signal_connect(g_area, "button-release-event", G_CALLBACK(on_release), NULL);
    g_signal_connect(g_area, "motion-notify-event", G_CALLBACK(on_motion), NULL);
    g_signal_connect(g_area, "leave-notify-event", G_CALLBACK(on_leave), NULL);
    g_signal_connect(g_area, "scroll-event", G_CALLBACK(on_scroll), NULL);
    g_signal_connect(g_area, "size-allocate", G_CALLBACK(on_size_allocate), NULL);
    g_signal_connect(g_area, "realize", G_CALLBACK(on_realize), NULL);

    GtkWidget *table = gtk_table_new(2, 2, FALSE);
    gtk_table_attach(GTK_TABLE(table), g_area, 0, 1, 0, 1, GTK_EXPAND | GTK_FILL, GTK_EXPAND | GTK_FILL, 0, 0);
    gtk_table_attach(GTK_TABLE(table), gtk_vscrollbar_new(g_vadj), 1, 2, 0, 1, GTK_FILL, GTK_EXPAND | GTK_FILL, 0, 0);
    gtk_table_attach(GTK_TABLE(table), gtk_hscrollbar_new(g_hadj), 0, 1, 1, 2, GTK_EXPAND | GTK_FILL, GTK_FILL, 0, 0);
    return table;
}
