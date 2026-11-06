/* SPDX-License-Identifier: GPL-3.0-or-later */
/* kiview-test: ignores the file and shows a 4000x3000 grid with its own
 * coordinates, a 1-pixel chessboard, and a transparent corner, to check
 * zoom anchors, panning, rotation, density and transparency by eye. Opened
 * for files named *.kvtest. */
#include <stdio.h>
#include <unistd.h>

#include "../libkvp/kvp.h"

#define W 4000
#define H 3000

static cairo_surface_t *load(int fd, const char *name, const char *mime, char **format, GError **err)
{
    (void)name;
    (void)mime;
    (void)err;
    close(fd);
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);
    cairo_t *cr = cairo_create(s);
    cairo_pattern_t *g = cairo_pattern_create_linear(0, 0, W, H);
    cairo_pattern_add_color_stop_rgb(g, 0, 0.2, 0.3, 0.7);
    cairo_pattern_add_color_stop_rgb(g, 1, 0.8, 0.5, 0.2);
    cairo_set_source(cr, g);
    cairo_paint(cr);
    cairo_pattern_destroy(g);

    cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
    cairo_set_line_width(cr, 1);
    for (int x = 0; x <= W; x += 100) {
        cairo_set_source_rgba(cr, 1, 1, 1, x % 500 ? 0.25 : 0.8);
        cairo_rectangle(cr, x, 0, 1, H);
        cairo_fill(cr);
    }
    for (int y = 0; y <= H; y += 100) {
        cairo_set_source_rgba(cr, 1, 1, 1, y % 500 ? 0.25 : 0.8);
        cairo_rectangle(cr, 0, y, W, 1);
        cairo_fill(cr);
    }
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_DEFAULT);
    cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 28);
    cairo_set_source_rgb(cr, 1, 1, 1);
    for (int x = 0; x < W; x += 500)
        for (int y = 0; y < H; y += 500) {
            char t[32];
            snprintf(t, sizeof t, "%d,%d", x, y);
            cairo_move_to(cr, x + 6, y + 30);
            cairo_show_text(cr, t);
        }
    /* 1-pixel chessboard at 200..264: nearest-neighbour shows it exactly */
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            cairo_set_source_rgb(cr, (x + y) & 1, (x + y) & 1, (x + y) & 1);
            cairo_rectangle(cr, 200 + x, 200 + y, 1, 1);
            cairo_fill(cr);
        }
    /* a transparent hole in the bottom right */
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_rectangle(cr, W - 600, H - 400, 600, 400);
    cairo_fill(cr);
    cairo_destroy(cr);
    *format = g_strdup_printf("test grid %d×%d", W, H);
    return s;
}

int main(void)
{
    static const KvpBitmapOps ops = {.caps = "rotate", .load = load};
    return kvp_bitmap_main(&ops);
}
