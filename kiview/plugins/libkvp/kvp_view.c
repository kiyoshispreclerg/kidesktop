/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "kvp_view.h"

#include <math.h>

static double clampd(double x, double lo, double hi)
{
    return x < lo ? lo : x > hi ? hi : x;
}

void kvp_view_rotated_size(const KvpView *v, int *w, int *h)
{
    bool odd = v->rot & 1;
    *w = odd ? v->ih : v->iw;
    *h = odd ? v->iw : v->ih;
}

void kvp_view_scaled_size(const KvpView *v, double *w, double *h)
{
    int dw, dh;
    kvp_view_rotated_size(v, &dw, &dh);
    *w = dw * v->zoom;
    *h = dh * v->zoom;
}

/* The scroll position clamped to what the document leaves to scroll. */
static void clamp_scroll(KvpView *v)
{
    double sw, sh;
    kvp_view_scaled_size(v, &sw, &sh);
    v->px = clampd(v->px, 0, fmax(sw - v->aw, 0));
    v->py = clampd(v->py, 0, fmax(sh - v->ah, 0));
}

static void apply_mode(KvpView *v)
{
    int dw, dh;
    kvp_view_rotated_size(v, &dw, &dh);
    if (v->mode == KVP_FIT && dw > 0 && dh > 0 && v->aw > 0 && v->ah > 0)
        v->zoom = fmin(1.0, fmin((double)v->aw / dw, (double)v->ah / dh));
    else if (v->mode == KVP_FIT_WIDTH && dw > 0 && v->aw > 0)
        v->zoom = (double)v->aw / dw;
    else if (v->mode == KVP_ACTUAL)
        v->zoom = 1.0;
    v->zoom = clampd(v->zoom, KVP_ZOOM_MIN, KVP_ZOOM_MAX);
    if (v->mode != KVP_CUSTOM) {
        v->px = v->py = 0;
        if (v->mode == KVP_ACTUAL) {
            /* from the middle of the document, like a zoom on its centre */
            double sw, sh;
            kvp_view_scaled_size(v, &sw, &sh);
            v->px = (sw - v->aw) / 2;
            v->py = (sh - v->ah) / 2;
        }
    }
    clamp_scroll(v);
}

void kvp_view_init(KvpView *v, int iw, int ih)
{
    int aw = v->aw, ah = v->ah;
    *v = (KvpView){.iw = iw, .ih = ih, .aw = aw, .ah = ah, .mode = KVP_FIT, .zoom = 1};
    apply_mode(v);
}

void kvp_view_set_area(KvpView *v, int aw, int ah)
{
    if (aw == v->aw && ah == v->ah)
        return;
    v->aw = aw;
    v->ah = ah;
    apply_mode(v);
}

void kvp_view_set_mode(KvpView *v, KvpMode mode)
{
    v->mode = mode;
    apply_mode(v);
}

void kvp_view_origin(const KvpView *v, double *ox, double *oy)
{
    double sw, sh;
    kvp_view_scaled_size(v, &sw, &sh);
    *ox = sw <= v->aw ? -(v->aw - sw) / 2 : v->px;
    *oy = sh <= v->ah ? -(v->ah - sh) / 2 : v->py;
}

void kvp_view_zoom_set(KvpView *v, double zoom, double ax, double ay)
{
    zoom = clampd(zoom, KVP_ZOOM_MIN, KVP_ZOOM_MAX);
    double ox, oy;
    kvp_view_origin(v, &ox, &oy);
    double f = zoom / v->zoom;
    /* the point under the anchor stays under it */
    double nx = (ox + ax) * f - ax, ny = (oy + ay) * f - ay;
    v->zoom = zoom;
    v->mode = KVP_CUSTOM;
    v->px = nx;
    v->py = ny;
    clamp_scroll(v);
}

void kvp_view_zoom_by(KvpView *v, double factor, double ax, double ay)
{
    kvp_view_zoom_set(v, v->zoom * factor, ax, ay);
}

void kvp_view_pan(KvpView *v, double dx, double dy)
{
    double ox, oy;
    kvp_view_origin(v, &ox, &oy);
    v->px = ox + dx;
    v->py = oy + dy;
    clamp_scroll(v);
}

void kvp_view_rotate(KvpView *v, int quarter_turns)
{
    v->rot = ((v->rot + quarter_turns) % 4 + 4) % 4;
    if (v->mode == KVP_CUSTOM) {
        /* keep the middle of what is shown, in the new orientation, simply:
         * centre it */
        double sw, sh;
        kvp_view_scaled_size(v, &sw, &sh);
        v->px = (sw - v->aw) / 2;
        v->py = (sh - v->ah) / 2;
    }
    apply_mode(v);
}

bool kvp_view_scrolls_x(const KvpView *v)
{
    double sw, sh;
    kvp_view_scaled_size(v, &sw, &sh);
    return sw > v->aw + 0.5;
}

bool kvp_view_scrolls_y(const KvpView *v)
{
    double sw, sh;
    kvp_view_scaled_size(v, &sw, &sh);
    return sh > v->ah + 0.5;
}
