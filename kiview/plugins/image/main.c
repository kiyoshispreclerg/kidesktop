/* SPDX-License-Identifier: GPL-3.0-or-later */
/* kiview-image: still images. WebP by libwebp when built with it; everything
 * else through gdk-pixbuf's loaders (no GTK). The whole file is read into
 * memory first: both decoders want it, and the descriptor is then done. */
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef HAVE_WEBP
#include <webp/decode.h>
#endif

#include "../libkvp/kvp.h"

#define MAX_FILE_BYTES (256u * 1024 * 1024)
#define MAX_PIXELS (256u * 1024 * 1024)

static gboolean read_all(int fd, guchar **data, gsize *len, GError **err)
{
    GByteArray *a = g_byte_array_new();
    guchar buf[65536];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n < 0) {
            g_set_error(err, KVP_ERROR, KVP_ERR_IO, "%s", g_strerror(errno));
            g_byte_array_free(a, TRUE);
            return FALSE;
        }
        if (n == 0)
            break;
        if (a->len + (gsize)n > MAX_FILE_BYTES) {
            g_set_error(err, KVP_ERROR, KVP_ERR_TOOLARGE, "file too large");
            g_byte_array_free(a, TRUE);
            return FALSE;
        }
        g_byte_array_append(a, buf, (guint)n);
    }
    *len = a->len;
    *data = g_byte_array_free(a, FALSE);
    return TRUE;
}

/* ARGB32, premultiplied: what cairo draws. Takes ownership of nothing. */
static cairo_surface_t *surface_from_rgba(const guchar *px, int w, int h, int stride, int channels,
                                          gboolean bgra)
{
    cairo_format_t fmt = channels == 4 ? CAIRO_FORMAT_ARGB32 : CAIRO_FORMAT_RGB24;
    cairo_surface_t *s = cairo_image_surface_create(fmt, w, h);
    if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS)
        return s;
    cairo_surface_flush(s);
    guchar *dst = cairo_image_surface_get_data(s);
    int dstride = cairo_image_surface_get_stride(s);
    for (int y = 0; y < h; y++) {
        const guchar *in = px + (gsize)y * stride;
        guint32 *out = (guint32 *)(dst + (gsize)y * dstride);
        for (int x = 0; x < w; x++, in += channels) {
            guint r = bgra ? in[2] : in[0], g = in[1], b = bgra ? in[0] : in[2];
            if (channels == 4) {
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
    return s;
}

#ifdef HAVE_WEBP
static cairo_surface_t *load_webp(const guchar *data, gsize len, char **format, GError **err)
{
    int w, h;
    if (!WebPGetInfo(data, len, &w, &h)) {
        g_set_error(err, KVP_ERROR, KVP_ERR_CORRUPT, "not a valid WebP image");
        return NULL;
    }
    if ((guint64)w * h > MAX_PIXELS) {
        g_set_error(err, KVP_ERROR, KVP_ERR_TOOLARGE, "%d×%d is too large", w, h);
        return NULL;
    }
    guint8 *bgra = WebPDecodeBGRA(data, len, &w, &h);
    if (!bgra) {
        g_set_error(err, KVP_ERROR, KVP_ERR_CORRUPT, "can't decode the WebP image");
        return NULL;
    }
    cairo_surface_t *s = surface_from_rgba(bgra, w, h, w * 4, 4, TRUE);
    WebPFree(bgra);
    *format = g_strdup_printf("WebP %d×%d", w, h);
    return s;
}
#endif

static void size_prepared(GdkPixbufLoader *l, gint w, gint h, gpointer data)
{
    (void)l;
    if ((guint64)w * h > MAX_PIXELS)
        *(gboolean *)data = TRUE;
}

static cairo_surface_t *load_pixbuf(const guchar *data, gsize len, char **format, GError **err)
{
    GdkPixbufLoader *l = gdk_pixbuf_loader_new();
    gboolean toolarge = FALSE;
    g_signal_connect(l, "size-prepared", G_CALLBACK(size_prepared), &toolarge);
    GError *e = NULL;
    /* Feed in chunks so an oversized image is refused before it is decoded. */
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
    g_object_ref(pb);
    GdkPixbuf *rot = gdk_pixbuf_apply_embedded_orientation(pb);
    g_object_unref(pb);
    pb = rot ? rot : NULL;
    g_object_unref(l);
    if (!pb) {
        g_free(name);
        g_set_error(err, KVP_ERROR, KVP_ERR_CORRUPT, "can't orient the image");
        return NULL;
    }
    int w = gdk_pixbuf_get_width(pb), h = gdk_pixbuf_get_height(pb);
    cairo_surface_t *s = surface_from_rgba(gdk_pixbuf_read_pixels(pb), w, h, gdk_pixbuf_get_rowstride(pb),
                                           gdk_pixbuf_get_has_alpha(pb) ? 4 : 3, FALSE);
    g_object_unref(pb);
    *format = g_strdup_printf("%s %d×%d", name, w, h);
    g_free(name);
    return s;
}

static cairo_surface_t *load(int fd, const char *name, const char *mime, char **format, GError **err)
{
    (void)name;
    (void)mime;
    guchar *data = NULL;
    gsize len = 0;
    gboolean ok = read_all(fd, &data, &len, err);
    close(fd);
    if (!ok)
        return NULL;
    cairo_surface_t *s = NULL;
#ifdef HAVE_WEBP
    if (len >= 12 && !memcmp(data, "RIFF", 4) && !memcmp(data + 8, "WEBP", 4))
        s = load_webp(data, len, format, err);
    else
#endif
        s = load_pixbuf(data, len, format, err);
    g_free(data);
    if (s && cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
        g_set_error(err, KVP_ERROR, KVP_ERR_TOOLARGE, "not enough memory for the image");
        cairo_surface_destroy(s);
        g_free(*format);
        *format = NULL;
        return NULL;
    }
    return s;
}

int main(void)
{
    static const KvpBitmapOps ops = {.caps = "rotate", .load = load};
    return kvp_bitmap_main(&ops);
}
