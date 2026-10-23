/* kp_paint.c - pixel primitives shared by the tools. */
#include "kp.h"

#include <math.h>

guint32 kp_color_premul(const KpColor *c)
{
    guint32 a = (guint32)(c->a * 255 + 0.5);
    guint32 r = (guint32)(c->r * c->a * 255 + 0.5), g = (guint32)(c->g * c->a * 255 + 0.5),
            b = (guint32)(c->b * c->a * 255 + 0.5);
    return a << 24 | r << 16 | g << 8 | b;
}

/* Grows r = {x0, y0, x1, y1} (x1/y1 exclusive; empty when x1 <= x0). */
void kp_rect_union(int r[4], int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0)
        return;
    if (r[2] <= r[0] || r[3] <= r[1]) {
        r[0] = x;
        r[1] = y;
        r[2] = x + w;
        r[3] = y + h;
        return;
    }
    r[0] = MIN(r[0], x);
    r[1] = MIN(r[1], y);
    r[2] = MAX(r[2], x + w);
    r[3] = MAX(r[3], y + h);
}

static void fill_square(unsigned char *data, int stride, int lw, int lh, int cx, int cy, int size, guint32 px)
{
    int x0 = cx - (size - 1) / 2, y0 = cy - (size - 1) / 2;
    int x1 = MIN(x0 + size, lw), y1 = MIN(y0 + size, lh);
    x0 = MAX(x0, 0);
    y0 = MAX(y0, 0);
    for (int y = y0; y < y1; y++) {
        guint32 *row = (guint32 *)(data + y * stride);
        for (int x = x0; x < x1; x++)
            row[x] = px;
    }
}

/* Aliased line of `size`x`size` squares, writing `px` as is (no blending:
 * the pencil and the eraser replace pixels, like Paint's). Records the
 * undo tiles first and adds the changed area to `rect`. */
void kp_paint_square_line(KpLayer *l, int x0, int y0, int x1, int y1, int size, guint32 px, int rect[4])
{
    int half = (size - 1) / 2;
    int bx = MIN(x0, x1) - half, by = MIN(y0, y1) - half;
    int bw = ABS(x1 - x0) + size, bh = ABS(y1 - y0) + size;
    kp_undo_touch(bx, by, bw, bh);
    kp_rect_union(rect, bx, by, bw, bh);

    cairo_surface_flush(l->surf);
    unsigned char *data = cairo_image_surface_get_data(l->surf);
    int stride = cairo_image_surface_get_stride(l->surf);
    int dx = ABS(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -ABS(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        fill_square(data, stride, l->w, l->h, x0, y0, size, px);
        if (x0 == x1 && y0 == y1)
            break;
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
    cairo_surface_mark_dirty(l->surf);
}

/* An aliased stroke on the active layer (pencil, eraser), one undo step. */
static struct {
    KpLayer *layer;
    guint32 px;
    int size, x, y;
} g_sq;

static void square_segment(int x, int y)
{
    int r[4] = { 0 };
    kp_paint_square_line(g_sq.layer, g_sq.x, g_sq.y, x, y, g_sq.size, g_sq.px, r);
    kp_canvas_damage(r[0], r[1], r[2] - r[0], r[3] - r[1]);
    g_sq.x = x;
    g_sq.y = y;
}

void kp_square_stroke_begin(guint32 px, int size, double x, double y)
{
    g_sq.layer = kp_doc_layer(kp.doc);
    g_sq.px = px;
    g_sq.size = MAX(size, 1);
    g_sq.x = (int)floor(x);
    g_sq.y = (int)floor(y);
    kp_undo_begin(g_sq.layer);
    square_segment(g_sq.x, g_sq.y);
}

void kp_square_stroke_to(double x, double y)
{
    int nx = (int)floor(x), ny = (int)floor(y);
    if (nx != g_sq.x || ny != g_sq.y)
        square_segment(nx, ny);
}

void kp_square_stroke_end(void)
{
    kp_undo_end();
}
