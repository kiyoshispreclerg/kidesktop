/* kp_tool_pencil.c - aliased freehand line, 1 px by default. */
#include "../kp.h"

#include <gdk/gdkkeysyms.h>

static void icon(cairo_t *cr)
{
    cairo_set_line_width(cr, 1);
    /* body, from the eraser end (top right) to the tip (bottom left) */
    cairo_move_to(cr, 13.5, 0.5);
    cairo_line_to(cr, 15.5, 2.5);
    cairo_line_to(cr, 5.5, 12.5);
    cairo_line_to(cr, 3.5, 10.5);
    cairo_close_path(cr);
    cairo_set_source_rgb(cr, 0.98, 0.78, 0.2);
    cairo_fill_preserve(cr);
    cairo_set_source_rgb(cr, 0.3, 0.25, 0.1);
    cairo_stroke(cr);
    cairo_move_to(cr, 3.5, 10.5);
    cairo_line_to(cr, 5.5, 12.5);
    cairo_line_to(cr, 1, 15);
    cairo_close_path(cr);
    cairo_set_source_rgb(cr, 0.95, 0.85, 0.65);
    cairo_fill_preserve(cr);
    cairo_set_source_rgb(cr, 0.3, 0.25, 0.1);
    cairo_stroke(cr);
}

static void press(double x, double y, int button)
{
    kp_square_stroke_begin(kp_color_premul(&kp.color[button == 3]), kp_tool_size(), x, y);
}

const KpTool kp_tool_pencil = {
    .id = "pencil",
    .name = N_("Pencil"),
    .key = GDK_p,
    .draw_icon = icon,
    .default_size = 1,
    .max_size = 50,
    .outline = TRUE,
    .square = TRUE,
    .press = press,
    .motion = kp_square_stroke_to,
    .release = kp_square_stroke_end,
};
