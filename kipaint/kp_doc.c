/* kp_doc.c - the document: size, layers, flattening, color picking. */
#include "kp.h"

#include <malloc.h>

static KpDoc *doc_alloc(int w, int h)
{
    KpDoc *d = g_new0(KpDoc, 1);
    d->w = w;
    d->h = h;
    /* No free func: the undo history can own a layer taken out of the doc. */
    d->layers = g_ptr_array_new();
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
    l->opacity = 1;
    KpDoc *d = doc_alloc(l->w, l->h);
    g_ptr_array_add(d->layers, l);
    return d;
}

void kp_doc_free(KpDoc *d)
{
    if (!d)
        return;
    for (guint i = 0; i < d->layers->len; i++)
        kp_layer_free(g_ptr_array_index(d->layers, i));
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
        cairo_paint_with_alpha(cr, l->opacity);
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
        double o = l->opacity / 255.0;
        double pa = (px >> 24) * o, pr = ((px >> 16) & 0xff) * o, pg = ((px >> 8) & 0xff) * o, pb = (px & 0xff) * o;
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

void kp_doc_layer_insert(KpDoc *d, KpLayer *l, int index)
{
    index = CLAMP(index, 0, (int)d->layers->len);
    g_ptr_array_insert(d->layers, index, l);
    d->active = index;
}

KpLayer *kp_doc_layer_take(KpDoc *d, int index)
{
    KpLayer *l = g_ptr_array_index(d->layers, index);
    g_ptr_array_remove_index(d->layers, index);
    d->active = MIN(d->active, (int)d->layers->len - 1);
    if (d->active < 0)
        d->active = 0;
    return l;
}

void kp_doc_layer_move(KpDoc *d, int from, int to)
{
    KpLayer *l = g_ptr_array_index(d->layers, from);
    g_ptr_array_remove_index(d->layers, from);
    g_ptr_array_insert(d->layers, to, l);
    d->active = to;
}
