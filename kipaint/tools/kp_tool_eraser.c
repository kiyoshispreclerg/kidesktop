/* kp_tool_eraser.c - square eraser. Like Paint's, it paints color 2;
 * with "Transparent" on it clears to alpha instead. */
#include "../kp.h"

#include <gdk/gdkkeysyms.h>

static void icon(cairo_t *cr)
{
    cairo_set_line_width(cr, 1);
    cairo_move_to(cr, 9.5, 2.5);
    cairo_line_to(cr, 14.5, 7.5);
    cairo_line_to(cr, 7.5, 14.5);
    cairo_line_to(cr, 2.5, 9.5);
    cairo_close_path(cr);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_fill_preserve(cr);
    cairo_set_source_rgb(cr, 0.25, 0.25, 0.3);
    cairo_stroke(cr);
    cairo_move_to(cr, 9.5, 2.5);
    cairo_line_to(cr, 14.5, 7.5);
    cairo_line_to(cr, 11.5, 10.5);
    cairo_line_to(cr, 6.5, 5.5);
    cairo_close_path(cr);
    cairo_set_source_rgb(cr, 0.93, 0.45, 0.55);
    cairo_fill_preserve(cr);
    cairo_set_source_rgb(cr, 0.25, 0.25, 0.3);
    cairo_stroke(cr);
}

static void press(double x, double y, int button)
{
    (void)button;
    guint32 px = kp.eraser_transparent ? 0 : kp_color_premul(&kp.color[1]);
    kp_square_stroke_begin(px, kp_tool_size(), x, y);
}

const KpTool kp_tool_eraser = {
    .id = "eraser",
    .name = N_("Eraser"),
    .key = GDK_e,
    .draw_icon = icon,
    .default_size = 10,
    .max_size = 200,
    .outline = TRUE,
    .square = TRUE,
    .press = press,
    .motion = kp_square_stroke_to,
    .release = kp_square_stroke_end,
};
