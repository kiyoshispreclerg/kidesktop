/* kp_io.c - reading and writing flat images through gdk-pixbuf. */
#include "kp.h"

#include <string.h>

/* cairo image surfaces are limited to 32767 px a side. */
#define MAX_SIDE 32767

typedef struct {
    const char *ext, *type;
    gboolean opaque;   /* no alpha in the format: flatten over white */
} Format;

static const Format FORMATS[] = {
    { "png", "png", FALSE },  { "jpg", "jpeg", TRUE },  { "jpeg", "jpeg", TRUE },
    { "bmp", "bmp", TRUE },   { "tif", "tiff", FALSE }, { "tiff", "tiff", FALSE },
};

static const Format *format_of(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot || strchr(dot, '/'))
        return NULL;
    for (guint i = 0; i < G_N_ELEMENTS(FORMATS); i++)
        if (!g_ascii_strcasecmp(dot + 1, FORMATS[i].ext))
            return &FORMATS[i];
    return NULL;
}

gboolean kp_io_writable(const char *path)
{
    return format_of(path) != NULL;
}

cairo_surface_t *kp_io_load(const char *path, GError **err)
{
    GdkPixbuf *raw = gdk_pixbuf_new_from_file(path, err);
    if (!raw)
        return NULL;
    /* photos: honour the EXIF orientation */
    GdkPixbuf *pb = gdk_pixbuf_apply_embedded_orientation(raw);
    g_object_unref(raw);
    int w = gdk_pixbuf_get_width(pb), h = gdk_pixbuf_get_height(pb);
    if (w > MAX_SIDE || h > MAX_SIDE) {
        g_set_error(err, GDK_PIXBUF_ERROR, GDK_PIXBUF_ERROR_FAILED, _("The image is too large (%d x %d)."), w, h);
        g_object_unref(pb);
        return NULL;
    }
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
        g_set_error(err, GDK_PIXBUF_ERROR, GDK_PIXBUF_ERROR_INSUFFICIENT_MEMORY, _("Not enough memory."));
        cairo_surface_destroy(s);
        g_object_unref(pb);
        return NULL;
    }
    cairo_t *cr = cairo_create(s);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    gdk_cairo_set_source_pixbuf(cr, pb, 0, 0);
    cairo_paint(cr);
    cairo_destroy(cr);
    g_object_unref(pb);
    return s;
}

/* ARGB32 premultiplied -> RGBA straight, or RGB composited over white. */
GdkPixbuf *kp_surface_to_pixbuf(cairo_surface_t *s, gboolean over_white)
{
    cairo_surface_flush(s);
    int w = cairo_image_surface_get_width(s), h = cairo_image_surface_get_height(s);
    int sstride = cairo_image_surface_get_stride(s);
    const unsigned char *src = cairo_image_surface_get_data(s);
    GdkPixbuf *pb = gdk_pixbuf_new(GDK_COLORSPACE_RGB, !over_white, 8, w, h);
    int dstride = gdk_pixbuf_get_rowstride(pb), n = over_white ? 3 : 4;
    guchar *dst = gdk_pixbuf_get_pixels(pb);
    for (int y = 0; y < h; y++) {
        const guint32 *row = (const guint32 *)(src + y * sstride);
        guchar *d = dst + y * dstride;
        for (int x = 0; x < w; x++, d += n) {
            guint32 px = row[x];
            guint a = px >> 24, r = (px >> 16) & 0xff, g = (px >> 8) & 0xff, b = px & 0xff;
            if (over_white) {
                /* premultiplied over white: c + (255 - a) */
                d[0] = r + 255 - a;
                d[1] = g + 255 - a;
                d[2] = b + 255 - a;
            } else if (a == 0) {
                d[0] = d[1] = d[2] = d[3] = 0;
            } else {
                d[0] = (r * 255 + a / 2) / a;
                d[1] = (g * 255 + a / 2) / a;
                d[2] = (b * 255 + a / 2) / a;
                d[3] = a;
            }
        }
    }
    return pb;
}

gboolean kp_io_save(KpDoc *d, const char *path, GError **err)
{
    const Format *f = format_of(path);
    if (!f) {
        g_set_error(err, GDK_PIXBUF_ERROR, GDK_PIXBUF_ERROR_UNKNOWN_TYPE,
                    _("Unknown file type. Use .png, .jpg, .bmp or .tiff."));
        return FALSE;
    }
    cairo_surface_t *flat = kp_doc_flatten(d);
    GdkPixbuf *pb = kp_surface_to_pixbuf(flat, f->opaque);
    cairo_surface_destroy(flat);
    gboolean ok;
    if (!strcmp(f->type, "jpeg"))
        ok = gdk_pixbuf_save(pb, path, f->type, err, "quality", "92", NULL);
    else if (!strcmp(f->type, "png"))
        ok = gdk_pixbuf_save(pb, path, f->type, err, "compression", "6", NULL);
    else
        ok = gdk_pixbuf_save(pb, path, f->type, err, NULL);
    g_object_unref(pb);
    return ok;
}

/* The upper-case extension ("PNG"), for messages. */
char *kp_io_format_name(const char *path)
{
    const char *dot = strrchr(path, '.');
    return g_ascii_strup(dot && !strchr(dot, '/') ? dot + 1 : "PNG", -1);
}
