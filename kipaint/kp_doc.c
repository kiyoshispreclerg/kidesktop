/* kp_doc.c - the document: size, layers, flattening, color picking. */
#include "kp.h"

#include <malloc.h>

static KpDoc *doc_alloc(int w, int h)
{
    KpDoc *d = g_new0(KpDoc, 1);
    d->w = w;
    d->h = h;
    d->layers = g_ptr_array_new_with_free_func((GDestroyNotify)kp_layer_free);
    return d;
}

KpDoc *kp_doc_new(int w, int h, const KpColor *fill)
{
    KpLayer *l = kp_layer_new(w, h, _("Background"));
    if (!l)
        return NULL;
    if (fill) {
        cairo_t *cr = cairo_create(l->surf);
        cairo_set_source_rgba(cr, fill->r, fill->g, fill->b, fill->a);
        cairo_paint(cr);
        cairo_destroy(cr);
    }
    KpDoc *d = doc_alloc(w, h);
    g_ptr_array_add(d->layers, l);
    return d;
}

/* Takes over `surf`, which must be an ARGB32 image surface. */
KpDoc *kp_doc_new_from_surface(cairo_surface_t *surf)
{
    KpLayer *l = g_new0(KpLayer, 1);
    l->surf = surf;
    l->w = cairo_image_surface_get_width(surf);
    l->h = cairo_image_surface_get_height(surf);
    l->name = g_strdup(_("Background"));
    l->visible = TRUE;
    KpDoc *d = doc_alloc(l->w, l->h);
    g_ptr_array_add(d->layers, l);
    return d;
}

void kp_doc_free(KpDoc *d)
{
    if (!d)
        return;
    g_ptr_array_free(d->layers, TRUE);
    g_free(d->path);
    g_free(d);
    /* A closed image is many megabytes in one go; hand them back. */
    malloc_trim(0);
}

KpLayer *kp_doc_layer(KpDoc *d)
{
    return g_ptr_array_index(d->layers, d->active);
}

cairo_surface_t *kp_doc_flatten(KpDoc *d)
{
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, d->w, d->h);
    cairo_t *cr = cairo_create(s);
    for (guint i = 0; i < d->layers->len; i++) {
        KpLayer *l = g_ptr_array_index(d->layers, i);
        if (!l->visible)
            continue;
        cairo_set_source_surface(cr, l->surf, 0, 0);
        cairo_paint(cr);
    }
    cairo_destroy(cr);
    return s;
}

/* The color of the visible layers composited at one pixel. */
gboolean kp_doc_pick(KpDoc *d, int x, int y, KpColor *out)
{
    if (x < 0 || y < 0 || x >= d->w || y >= d->h)
        return FALSE;
    double r = 0, g = 0, b = 0, a = 0;   /* premultiplied */
    for (guint i = 0; i < d->layers->len; i++) {
        KpLayer *l = g_ptr_array_index(d->layers, i);
        if (!l->visible || x >= l->w || y >= l->h)
            continue;
        cairo_surface_flush(l->surf);
        const unsigned char *data = cairo_image_surface_get_data(l->surf);
        guint32 px = *(const guint32 *)(data + y * cairo_image_surface_get_stride(l->surf) + x * 4);
        double pa = (px >> 24) / 255.0, pr = ((px >> 16) & 0xff) / 255.0, pg = ((px >> 8) & 0xff) / 255.0,
               pb = (px & 0xff) / 255.0;
        r = pr + r * (1 - pa);
        g = pg + g * (1 - pa);
        b = pb + b * (1 - pa);
        a = pa + a * (1 - pa);
    }
    if (a > 0) {
        out->r = r / a;
        out->g = g / a;
        out->b = b / a;
    } else {
        out->r = out->g = out->b = 1;
    }
    out->a = a;
    return TRUE;
}
