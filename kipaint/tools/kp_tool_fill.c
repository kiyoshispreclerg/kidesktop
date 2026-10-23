/* kp_tool_fill.c - flood fill of the area with exactly the clicked color.
 *
 * Two passes: a scanline flood marks a byte mask and its bounding box,
 * then only that box goes to undo and gets the new color. */
#include "../kp.h"

#include <gdk/gdkkeysyms.h>
#include <math.h>
#include <string.h>

static void icon(cairo_t *cr)
{
    cairo_set_line_width(cr, 1);
    /* tilted bucket */
    cairo_move_to(cr, 2.5, 7.5);
    cairo_line_to(cr, 8.5, 1.5);
    cairo_line_to(cr, 14.5, 7.5);
    cairo_line_to(cr, 8.5, 13.5);
    cairo_close_path(cr);
    cairo_set_source_rgb(cr, 0.85, 0.87, 0.9);
    cairo_fill_preserve(cr);
    cairo_set_source_rgb(cr, 0.2, 0.2, 0.25);
    cairo_stroke(cr);
    /* paint pouring out */
    cairo_move_to(cr, 2.5, 7.5);
    cairo_line_to(cr, 8.5, 7.5);
    cairo_line_to(cr, 8.5, 13.5);
    cairo_close_path(cr);
    cairo_set_source_rgb(cr, 0.85, 0.15, 0.15);
    cairo_fill(cr);
    cairo_move_to(cr, 2.5, 7.5);
    cairo_curve_to(cr, 0, 10, 0.5, 13, 1.5, 14.5);
    cairo_curve_to(cr, 2.5, 13, 3, 10, 2.5, 7.5);
    cairo_fill(cr);
}

typedef struct {
    int x, y;
} Seed;

static void press(double fx, double fy, int button)
{
    KpLayer *l = kp_doc_layer(kp.doc);
    int sx = (int)floor(fx), sy = (int)floor(fy);
    if (sx < 0 || sy < 0 || sx >= l->w || sy >= l->h)
        return;
    cairo_surface_flush(l->surf);
    unsigned char *data = cairo_image_surface_get_data(l->surf);
    int stride = cairo_image_surface_get_stride(l->surf), w = l->w, h = l->h;
#define PIX(x, y) (((guint32 *)(data + (size_t)(y) * stride))[x])
    guint32 target = PIX(sx, sy), repl = kp_color_premul(&kp.color[button == 3]);
    if (target == repl)
        return;

    guint8 *mask = g_malloc0((size_t)w * h);
#define FREE(x, y) (!mask[(size_t)(y) * w + (x)] && PIX(x, y) == target)
    GArray *stack = g_array_new(FALSE, FALSE, sizeof(Seed));
    Seed s = { sx, sy };
    g_array_append_val(stack, s);
    int box[4] = { 0 };
    while (stack->len) {
        s = g_array_index(stack, Seed, stack->len - 1);
        g_array_set_size(stack, stack->len - 1);
        if (!FREE(s.x, s.y))
            continue;
        int xl = s.x;
        int xr = xl;
        while (xl > 0 && FREE(xl - 1, s.y))
            xl--;
        while (xr < w - 1 && FREE(xr + 1, s.y))
            xr++;
        memset(mask + (size_t)s.y * w + xl, 1, xr - xl + 1);
        kp_rect_union(box, xl, s.y, xr - xl + 1, 1);
        for (int ny = s.y - 1; ny <= s.y + 1; ny += 2) {
            if (ny < 0 || ny >= h)
                continue;
            gboolean in_run = FALSE;
            for (int x = xl; x <= xr; x++) {
                gboolean f = FREE(x, ny);
                if (f && !in_run) {
                    Seed n = { x, ny };
                    g_array_append_val(stack, n);
                }
                in_run = f;
            }
        }
    }
    g_array_free(stack, TRUE);

    kp_undo_begin(l);
    kp_undo_touch(box[0], box[1], box[2] - box[0], box[3] - box[1]);
    for (int y = box[1]; y < box[3]; y++)
        for (int x = box[0]; x < box[2]; x++)
            if (mask[(size_t)y * w + x])
                PIX(x, y) = repl;
    cairo_surface_mark_dirty(l->surf);
    kp_undo_end();
    g_free(mask);
#undef FREE
#undef PIX
    kp_canvas_damage(box[0], box[1], box[2] - box[0], box[3] - box[1]);
}

const KpTool kp_tool_fill = {
    .id = "fill",
    .name = N_("Fill with color"),
    .key = GDK_f,
    .draw_icon = icon,
    .press = press,
};
