/* kp_tool_brush.c - antialiased round brush, one cairo segment per motion. */
#include "../kp.h"

#include <gdk/gdkkeysyms.h>
#include <math.h>

static KpLayer *g_layer;
static KpColor g_color;
static double g_x, g_y;

static void icon(cairo_t *cr)
{
    cairo_set_line_width(cr, 1);
    /* handle */
    cairo_move_to(cr, 13.5, 0.5);
    cairo_line_to(cr, 15.5, 2.5);
    cairo_line_to(cr, 8.5, 9.5);
    cairo_line_to(cr, 6.5, 7.5);
    cairo_close_path(cr);
    cairo_set_source_rgb(cr, 0.6, 0.4, 0.2);
    cairo_fill_preserve(cr);
    cairo_set_source_rgb(cr, 0.25, 0.15, 0.05);
    cairo_stroke(cr);
    /* bristles */
    cairo_move_to(cr, 6.5, 7.5);
    cairo_line_to(cr, 8.5, 9.5);
    cairo_curve_to(cr, 7, 13, 4, 15.5, 0.5, 15.5);
    cairo_curve_to(cr, 1.5, 12, 3, 9, 6.5, 7.5);
    cairo_set_source_rgb(cr, 0.15, 0.35, 0.8);
    cairo_fill_preserve(cr);
    cairo_set_source_rgb(cr, 0.1, 0.15, 0.35);
    cairo_stroke(cr);
}

static void segment(double x, double y)
{
    double size = kp_tool_size();
    double m = size / 2 + 2;
    int bx = (int)floor(MIN(g_x, x) - m), by = (int)floor(MIN(g_y, y) - m);
    int bw = (int)ceil(fabs(x - g_x) + 2 * m) + 1, bh = (int)ceil(fabs(y - g_y) + 2 * m) + 1;
    kp_undo_touch(bx, by, bw, bh);

    cairo_t *cr = cairo_create(g_layer->surf);
    cairo_set_source_rgba(cr, g_color.r, g_color.g, g_color.b, g_color.a);
    cairo_set_line_width(cr, size);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_move_to(cr, g_x, g_y);
    cairo_line_to(cr, x, y);
    cairo_stroke(cr);
    cairo_destroy(cr);

    kp_canvas_damage(bx, by, bw, bh);
    g_x = x;
    g_y = y;
}

static void press(double x, double y, int button)
{
    g_layer = kp_doc_layer(kp.doc);
    g_color = kp.color[button == 3];
    kp_undo_begin(g_layer);
    g_x = x;
    g_y = y;
    segment(x, y);   /* a click alone leaves a dot */
}

static void motion(double x, double y)
{
    if (x != g_x || y != g_y)
        segment(x, y);
}

static void release(void)
{
    kp_undo_end();
}

const KpTool kp_tool_brush = {
    .id = "brush",
    .name = N_("Brush"),
    .key = GDK_b,
    .draw_icon = icon,
    .default_size = 5,
    .max_size = 200,
    .outline = TRUE,
    .press = press,
    .motion = motion,
    .release = release,
};
