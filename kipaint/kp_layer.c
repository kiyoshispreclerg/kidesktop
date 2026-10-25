/* kp_layer.c - a layer: one ARGB32 surface plus its properties. */
#include "kp.h"

#include <string.h>

KpLayer *kp_layer_new(int w, int h, const char *name)
{
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(s);
        return NULL;
    }
    KpLayer *l = g_new0(KpLayer, 1);
    l->surf = s;
    l->w = w;
    l->h = h;
    l->name = g_strdup(name);
    l->visible = TRUE;
    l->opacity = 1;
    return l;
}

KpLayer *kp_layer_duplicate(const KpLayer *src)
{
    KpLayer *l = kp_layer_new(src->w, src->h, src->name);
    if (!l)
        return NULL;
    cairo_surface_flush(src->surf);
    unsigned char *dst = cairo_image_surface_get_data(l->surf);
    const unsigned char *from = cairo_image_surface_get_data(src->surf);
    int ds = cairo_image_surface_get_stride(l->surf), ss = cairo_image_surface_get_stride(src->surf);
    for (int y = 0; y < src->h; y++)
        memcpy(dst + (size_t)y * ds, from + (size_t)y * ss, (size_t)src->w * 4);
    cairo_surface_mark_dirty(l->surf);
    l->visible = src->visible;
    l->opacity = src->opacity;
    return l;
}

/* Swaps the layer's properties with `p` (undo and redo are the same move). */
void kp_layer_props_swap(KpLayer *l, KpLayerProps *p)
{
    KpLayerProps t = { l->name, l->visible, l->opacity };
    l->name = p->name;
    l->visible = p->visible;
    l->opacity = p->opacity;
    *p = t;
}

void kp_layer_free(KpLayer *l)
{
    if (!l)
        return;
    cairo_surface_destroy(l->surf);
    g_free(l->name);
    g_free(l);
}
