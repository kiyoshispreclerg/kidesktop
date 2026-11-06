/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Everything else (BMP, TIFF, ICO, TGA, PNM, XPM, ...) through gdk-pixbuf's
 * loaders, without GTK. */
#include <gdk-pixbuf/gdk-pixbuf.h>

#include "image.h"

cairo_surface_t *kvi_surface(int w, int h, gboolean alpha, GError **err)
{
    cairo_surface_t *s = cairo_image_surface_create(alpha ? CAIRO_FORMAT_ARGB32 : CAIRO_FORMAT_RGB24, w, h);
    if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(s);
        g_set_error(err, KVP_ERROR, KVP_ERR_TOOLARGE, "not enough memory for a %d×%d image", w, h);
        return NULL;
    }
    return s;
}

static void size_prepared(GdkPixbufLoader *l, gint w, gint h, gpointer data)
{
    (void)l;
    if ((guint64)w * h > KVI_MAX_PIXELS)
        *(gboolean *)data = TRUE;
}

KvpDecoded *kvi_pixbuf(GBytes *bytes, const KvpTarget *t, GError **err)
{
    (void)t;
    gsize len;
    const guchar *data = g_bytes_get_data(bytes, &len);
    GdkPixbufLoader *l = gdk_pixbuf_loader_new();
    gboolean toolarge = FALSE;
    g_signal_connect(l, "size-prepared", G_CALLBACK(size_prepared), &toolarge);
    GError *e = NULL;
    /* Chunks, so an oversized image is refused before it is decoded. */
    gboolean ok = TRUE;
    for (gsize off = 0; ok && off < len && !toolarge; off += 65536)
        ok = gdk_pixbuf_loader_write(l, data + off, MIN((gsize)65536, len - off), &e);
    if (toolarge) {
        g_clear_error(&e);
        gdk_pixbuf_loader_close(l, NULL);
        g_object_unref(l);
        g_set_error(err, KVP_ERROR, KVP_ERR_TOOLARGE, "image too large");
        return NULL;
    }
    if (ok)
        ok = gdk_pixbuf_loader_close(l, &e);
    else
        gdk_pixbuf_loader_close(l, NULL);
    GdkPixbuf *pb = ok ? gdk_pixbuf_loader_get_pixbuf(l) : NULL;
    if (!pb) {
        gboolean unsup = e && e->domain == GDK_PIXBUF_ERROR &&
                         (e->code == GDK_PIXBUF_ERROR_UNKNOWN_TYPE ||
                          e->code == GDK_PIXBUF_ERROR_UNSUPPORTED_OPERATION);
        g_set_error(err, KVP_ERROR, unsup ? KVP_ERR_UNSUPPORTED : KVP_ERR_CORRUPT, "%s",
                    e ? e->message : "can't decode the image");
        g_clear_error(&e);
        g_object_unref(l);
        return NULL;
    }
    GdkPixbufFormat *f = gdk_pixbuf_loader_get_format(l);
    char *name = f ? g_ascii_strup(gdk_pixbuf_format_get_name(f), -1) : g_strdup("Image");
    GdkPixbuf *oriented = gdk_pixbuf_apply_embedded_orientation(pb); /* a new reference */
    g_object_unref(l);
    if (!oriented) {
        g_free(name);
        g_set_error(err, KVP_ERROR, KVP_ERR_CORRUPT, "can't orient the image");
        return NULL;
    }
    int w = gdk_pixbuf_get_width(oriented), h = gdk_pixbuf_get_height(oriented);
    int stride = gdk_pixbuf_get_rowstride(oriented), ch = gdk_pixbuf_get_n_channels(oriented);
    gboolean alpha = gdk_pixbuf_get_has_alpha(oriented);
    cairo_surface_t *s = kvi_surface(w, h, alpha, err);
    if (!s) {
        g_object_unref(oriented);
        g_free(name);
        return NULL;
    }
    cairo_surface_flush(s);
    const guchar *px = gdk_pixbuf_read_pixels(oriented);
    guchar *dst = cairo_image_surface_get_data(s);
    int dstride = cairo_image_surface_get_stride(s);
    for (int y = 0; y < h; y++) {
        const guchar *in = px + (gsize)y * stride;
        guint32 *out = (guint32 *)(dst + (gsize)y * dstride);
        for (int x = 0; x < w; x++, in += ch) {
            guint r = in[0], g = in[1], b = in[2];
            if (alpha) {
                guint a = in[3];
                if (a != 255) {
                    r = (r * a + 127) / 255;
                    g = (g * a + 127) / 255;
                    b = (b * a + 127) / 255;
                }
                out[x] = (a << 24) | (r << 16) | (g << 8) | b;
            } else {
                out[x] = 0xff000000u | (r << 16) | (g << 8) | b;
            }
        }
    }
    cairo_surface_mark_dirty(s);
    g_object_unref(oriented);

    KvpDecoded *r = g_new0(KvpDecoded, 1);
    r->img = s;
    r->denom = 1;
    r->full_w = w;
    r->full_h = h;
    r->format = g_strdup_printf("%s %d×%d", name, w, h);
    g_free(name);
    return r;
}
