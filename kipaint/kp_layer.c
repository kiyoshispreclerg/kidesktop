/* kp_layer.c - a layer: one ARGB32 surface plus its properties. */
#include "kp.h"

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
    return l;
}

void kp_layer_free(KpLayer *l)
{
    if (!l)
        return;
    cairo_surface_destroy(l->surf);
    g_free(l->name);
    g_free(l);
}
