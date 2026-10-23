/* kp_tool_picker.c - color picker: left button sets color 1, right
 * color 2, following the pointer while held; on release it goes back to
 * the previous tool, like Paint's. */
#include "../kp.h"

#include <gdk/gdkkeysyms.h>
#include <math.h>

static int g_slot;

static void icon(cairo_t *cr)
{
    cairo_set_line_width(cr, 1);
    /* bulb */
    cairo_arc(cr, 12.5, 3.5, 2.6, 0, 2 * G_PI);
    cairo_set_source_rgb(cr, 0.2, 0.2, 0.25);
    cairo_fill(cr);
    /* glass tube */
    cairo_move_to(cr, 9.5, 4.5);
    cairo_line_to(cr, 11.5, 6.5);
    cairo_line_to(cr, 3.5, 14.5);
    cairo_line_to(cr, 1.5, 14.5);
    cairo_line_to(cr, 1.5, 12.5);
    cairo_close_path(cr);
    cairo_set_source_rgb(cr, 0.85, 0.92, 1);
    cairo_fill_preserve(cr);
    cairo_set_source_rgb(cr, 0.2, 0.2, 0.25);
    cairo_stroke(cr);
    cairo_move_to(cr, 8.5, 5.5);
    cairo_line_to(cr, 10.5, 7.5);
    cairo_stroke(cr);
}

static void pick(double x, double y)
{
    KpColor c;
    if (!kp_doc_pick(kp.doc, (int)floor(x), (int)floor(y), &c))
        return;
    c.a = 1;   /* strokes are opaque in 0.0.1 (see PLAN.md) */
    kp.color[g_slot] = c;
    kp_palette_refresh();
}

static void press(double x, double y, int button)
{
    g_slot = button == 3;
    pick(x, y);
}

static void release(void)
{
    kp_toolbox_select(kp.prev_tool ? kp.prev_tool : &kp_tool_pencil);
}

const KpTool kp_tool_picker = {
    .id = "picker",
    .name = N_("Pick color"),
    .key = GDK_i,
    .draw_icon = icon,
    .press = press,
    .motion = pick,
    .release = release,
};
