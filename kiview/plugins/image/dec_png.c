/* SPDX-License-Identifier: GPL-3.0-or-later */
/* PNG through libpng, decoded straight into the cairo surface: 16-bit,
 * palette, gray, tRNS and interlacing all become BGRA/BGRX rows, and alpha
 * is premultiplied in place. */
#include "image.h"

#ifdef HAVE_PNG
#include <png.h>
#include <string.h>

typedef struct {
    const guchar *p;
    gsize len, pos;
} Mem;

static void read_fn(png_structp png, png_bytep out, png_size_t n)
{
    Mem *m = png_get_io_ptr(png);
    if (m->pos + n > m->len)
        png_error(png, "unexpected end of file");
    memcpy(out, m->p + m->pos, n);
    m->pos += n;
}

KvpDecoded *kvi_png(GBytes *data, const KvpTarget *t, GError **err)
{
    (void)t;
    Mem mem = {0};
    mem.p = g_bytes_get_data(data, &mem.len);
    cairo_surface_t *volatile s = NULL;
    png_bytep *volatile rows = NULL;
    GError *volatile serr = NULL;
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    png_infop info = png ? png_create_info_struct(png) : NULL;
    if (!png || !info) {
        png_destroy_read_struct(&png, &info, NULL);
        g_set_error(err, KVP_ERROR, KVP_ERR_IO, "out of memory");
        return NULL;
    }
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, NULL);
        if (s)
            cairo_surface_destroy(s);
        g_free(rows);
        if (serr)
            g_propagate_error(err, serr);
        else
            g_set_error(err, KVP_ERROR, KVP_ERR_CORRUPT, "the PNG image is damaged");
        return NULL;
    }
    png_set_read_fn(png, &mem, read_fn);
    png_read_info(png, info);
    png_uint_32 w, h;
    int depth, color;
    png_get_IHDR(png, info, &w, &h, &depth, &color, NULL, NULL, NULL);
    if ((guint64)w * h > KVI_MAX_PIXELS || w > 0x7fffff || h > 0x7fffff) {
        serr = g_error_new(KVP_ERROR, KVP_ERR_TOOLARGE, "%u×%u is too large", w, h);
        png_error(png, "too large");
    }
    gboolean alpha = (color & PNG_COLOR_MASK_ALPHA) || png_get_valid(png, info, PNG_INFO_tRNS);
    if (depth == 16)
        png_set_strip_16(png);
    if (color == PNG_COLOR_TYPE_PALETTE)
        png_set_palette_to_rgb(png);
    if ((color == PNG_COLOR_TYPE_GRAY || color == PNG_COLOR_TYPE_GRAY_ALPHA) && depth < 8)
        png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS))
        png_set_tRNS_to_alpha(png);
    if (color == PNG_COLOR_TYPE_GRAY || color == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png);
    png_set_bgr(png);
    if (!alpha)
        png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
    png_set_interlace_handling(png);
    png_read_update_info(png, info);

    GError *e = NULL;
    s = kvi_surface((int)w, (int)h, alpha, &e);
    if (!s) {
        serr = e;
        png_error(png, "no memory");
    }
    cairo_surface_flush(s);
    guchar *px = cairo_image_surface_get_data(s);
    int stride = cairo_image_surface_get_stride(s);
    rows = g_new(png_bytep, h);
    for (png_uint_32 y = 0; y < h; y++)
        rows[y] = px + (gsize)stride * y;
    png_read_image(png, rows);
    g_free(rows);
    rows = NULL;
    if (alpha) {
        for (png_uint_32 y = 0; y < h; y++) {
            guchar *q = px + (gsize)stride * y;
            for (png_uint_32 x = 0; x < w; x++, q += 4) {
                guint a = q[3];
                if (a != 255) {
                    q[0] = (guchar)((q[0] * a + 127) / 255);
                    q[1] = (guchar)((q[1] * a + 127) / 255);
                    q[2] = (guchar)((q[2] * a + 127) / 255);
                }
            }
        }
    }
    cairo_surface_mark_dirty(s);
    png_destroy_read_struct(&png, &info, NULL);

    KvpDecoded *r = g_new0(KvpDecoded, 1);
    r->img = s;
    r->denom = 1;
    r->full_w = (int)w;
    r->full_h = (int)h;
    r->format = g_strdup_printf("PNG %u×%u", w, h);
    return r;
}
#else
KvpDecoded *kvi_png(GBytes *data, const KvpTarget *t, GError **err)
{
    (void)data;
    (void)t;
    g_set_error(err, KVP_ERROR, KVP_ERR_UNSUPPORTED, "built without libpng");
    return NULL;
}
#endif
