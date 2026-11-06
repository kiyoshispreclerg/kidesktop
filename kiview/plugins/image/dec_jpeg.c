/* SPDX-License-Identifier: GPL-3.0-or-later */
/* JPEG through libjpeg(-turbo). The DCT does the reduction: an image shown
 * far smaller than it is decodes at 1/2, 1/4 or 1/8 for a fraction of the
 * time and memory, straight into the cairo surface (BGRX, no conversion). */
#include "image.h"

#ifdef HAVE_JPEG
#include <setjmp.h>
#include <stdio.h>
#include <jpeglib.h>

struct jerr {
    struct jpeg_error_mgr pub;
    jmp_buf jb;
};

static void error_exit(j_common_ptr c)
{
    longjmp(((struct jerr *)c->err)->jb, 1);
}

static void quiet(j_common_ptr c)
{
    (void)c;
}

KvpDecoded *kvi_jpeg(GBytes *data, const KvpTarget *t, GError **err)
{
    gsize len;
    const guchar *p = g_bytes_get_data(data, &len);
    struct jpeg_decompress_struct c;
    struct jerr je;
    cairo_surface_t *volatile s = NULL;
    GError *volatile serr = NULL;

    c.err = jpeg_std_error(&je.pub);
    je.pub.error_exit = error_exit;
    je.pub.output_message = quiet;
    if (setjmp(je.jb)) {
        jpeg_destroy_decompress(&c);
        if (s)
            cairo_surface_destroy(s);
        if (serr)
            g_propagate_error(err, serr);
        else
            g_set_error(err, KVP_ERROR, KVP_ERR_CORRUPT, "the JPEG image is damaged");
        return NULL;
    }
    jpeg_create_decompress(&c);
    jpeg_mem_src(&c, p, len);
    jpeg_save_markers(&c, JPEG_APP0 + 1, 0xFFFF);
    jpeg_read_header(&c, TRUE);

    if (c.num_components == 4) { /* CMYK: not ours */
        jpeg_destroy_decompress(&c);
        g_set_error(err, KVP_ERROR, KVP_ERR_UNSUPPORTED, "CMYK JPEG");
        return NULL;
    }
    int iw = (int)c.image_width, ih = (int)c.image_height;
    if ((guint64)iw * ih > KVI_MAX_PIXELS) {
        jpeg_destroy_decompress(&c);
        g_set_error(err, KVP_ERROR, KVP_ERR_TOOLARGE, "%d×%d is too large", iw, ih);
        return NULL;
    }
    int orientation = 1;
    for (jpeg_saved_marker_ptr m = c.marker_list; m; m = m->next)
        if (m->marker == JPEG_APP0 + 1 && orientation == 1)
            orientation = kvi_exif_orientation(m->data, m->data_length);
    bool flip;
    int rot;
    kvp_orientation(orientation, &flip, &rot);

    int denom = kvp_pick_denom(t->min_zoom, t->area_w, t->area_h, iw, ih, rot);
    c.scale_num = 1;
    c.scale_denom = (unsigned)denom;
    c.out_color_space = JCS_EXT_BGRX;
    jpeg_start_decompress(&c);

    GError *e = NULL;
    s = kvi_surface((int)c.output_width, (int)c.output_height, FALSE, &e);
    if (!s) {
        serr = e;
        longjmp(je.jb, 1);
    }
    cairo_surface_flush(s);
    guchar *px = cairo_image_surface_get_data(s);
    int stride = cairo_image_surface_get_stride(s);
    while (c.output_scanline < c.output_height) {
        JSAMPROW row = px + (gsize)stride * c.output_scanline;
        jpeg_read_scanlines(&c, &row, 1);
    }
    cairo_surface_mark_dirty(s);
    jpeg_finish_decompress(&c);
    jpeg_destroy_decompress(&c);

    KvpDecoded *r = g_new0(KvpDecoded, 1);
    r->img = s;
    r->denom = denom;
    r->full_w = iw;
    r->full_h = ih;
    r->flip = flip;
    r->rot = rot;
    r->keep = g_bytes_ref(data);
    r->format = g_strdup_printf("JPEG %d×%d", iw, ih);
    return r;
}
#else
KvpDecoded *kvi_jpeg(GBytes *data, const KvpTarget *t, GError **err)
{
    (void)data;
    (void)t;
    g_set_error(err, KVP_ERROR, KVP_ERR_UNSUPPORTED, "built without libjpeg");
    return NULL;
}
#endif
