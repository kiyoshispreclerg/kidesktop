/*
 * Which way slide-in and slide-out move a window: the direction setting,
 * and the displacement vector worked out from it. Shared so the two
 * effects, which are mirrors of each other, cannot drift apart.
 *
 *   auto           the edge of the window's output nearest its centre:
 *                  that edge's axis, and its side.
 *   auto-panel     the side of the screen the nearest dock (panel) sits
 *                  on, so the window looks like it comes out of that
 *                  panel. With no dock on screen it behaves as `auto`.
 *   horizontal /   pin the axis; the sign still follows the nearest edge
 *   vertical       of the output on that axis.
 */
#ifndef KICOMP_SLIDE_DIR_H
#define KICOMP_SLIDE_DIR_H

#include "../comp.h"
#include "../window.h"

#include <math.h>
#include <string.h>

typedef enum {
    SLIDE_AUTO,
    SLIDE_AUTO_PANEL,
    SLIDE_HORIZONTAL,
    SLIDE_VERTICAL,
} SlideDirection;

static bool slide_direction_parse(const char *value, SlideDirection *out)
{
    if (strcmp(value, "auto") == 0 || strcmp(value, "auto-edge") == 0)
        *out = SLIDE_AUTO;
    else if (strcmp(value, "auto-panel") == 0)
        *out = SLIDE_AUTO_PANEL;
    else if (strcmp(value, "horizontal") == 0)
        *out = SLIDE_HORIZONTAL;
    else if (strcmp(value, "vertical") == 0)
        *out = SLIDE_VERTICAL;
    else
        return false;
    return true;
}

static const CompRect *slide_output_at(float x, float y)
{
    for (int i = 0; i < comp.output_count; i++) {
        CompRect centre = { (int)x, (int)y, 1, 1 };
        CompRect hit;
        if (rect_intersect(&centre, &comp.outputs[i].rect, &hit))
            return &comp.outputs[i].rect;
    }
    return NULL;
}

/* Gap between two rectangles; 0 when they touch or overlap. */
static float slide_rect_gap(const CompRect *a, const CompRect *b)
{
    float dx = fmaxf(0.0f, fmaxf((float)(b->x - (a->x + a->w)), (float)(a->x - (b->x + b->w))));
    float dy = fmaxf(0.0f, fmaxf((float)(b->y - (a->y + a->h)), (float)(a->y - (b->y + b->h))));
    return sqrtf(dx * dx + dy * dy);
}

/* The side of its output the nearest dock to `r` -- on the window's own
 * output only -- is attached to, as the unit direction from the screen's
 * middle towards it (-1/+1 on one axis). False when that output has no
 * mapped dock. */
static bool slide_panel_side(const CompWindow *self, const CompRect *r, float *sx, float *sy)
{
    const CompRect *wout = slide_output_at(r->x + r->w / 2.0f, r->y + r->h / 2.0f);
    if (!wout)
        return false;

    const CompWindow *best = NULL;
    float best_gap = 0.0f;
    for (const CompWindow *d = comp.stack; d; d = d->next) {
        if (d == self || d->type != COMP_WINDOW_DOCK || !d->mapped || d->input_only)
            continue;
        CompRect dr = window_rect((CompWindow *)d);
        if (slide_output_at(dr.x + dr.w / 2.0f, dr.y + dr.h / 2.0f) != wout)
            continue;
        float gap = slide_rect_gap(r, &dr);
        if (!best || gap < best_gap) {
            best = d;
            best_gap = gap;
        }
    }
    if (!best)
        return false;

    CompRect dr = window_rect((CompWindow *)best);
    const CompRect *out = wout;

    /* The output edge the dock's own edge is closest to. */
    float g_left = (float)(dr.x - out->x);
    float g_right = (float)((out->x + out->w) - (dr.x + dr.w));
    float g_top = (float)(dr.y - out->y);
    float g_bottom = (float)((out->y + out->h) - (dr.y + dr.h));

    /* A panel is long along its edge: of the two edges it could be on
     * for its axis, the shorter gap wins, and a wide dock is top/bottom,
     * a tall one left/right. */
    *sx = 0.0f;
    *sy = 0.0f;
    if (dr.w >= dr.h)
        *sy = g_top <= g_bottom ? -1.0f : 1.0f;
    else
        *sx = g_left <= g_right ? -1.0f : 1.0f;
    return true;
}

/* The full displacement vector for this window, computed once when the
 * effect starts, in root pixels. A window that isn't over any known
 * output falls back to the left/top. */
static void slide_offset_for(CompWindow *w, SlideDirection dir, float offset, float *dx, float *dy)
{
    CompRect r = window_rect(w);
    float cx = r.x + r.w / 2.0f;
    float cy = r.y + r.h / 2.0f;

    *dx = 0.0f;
    *dy = 0.0f;

    if (dir == SLIDE_AUTO_PANEL) {
        float sx, sy;
        if (slide_panel_side(w, &r, &sx, &sy)) {
            *dx = sx * offset * r.w;
            *dy = sy * offset * r.h;
            return;
        }
    }

    const CompRect *out = slide_output_at(cx, cy);
    float d_left = out ? cx - out->x : 1.0f;
    float d_right = out ? (out->x + out->w) - cx : 0.0f;
    float d_top = out ? cy - out->y : 1.0f;
    float d_bottom = out ? (out->y + out->h) - cy : 0.0f;

    bool horizontal;
    if (dir == SLIDE_HORIZONTAL)
        horizontal = true;
    else if (dir == SLIDE_VERTICAL)
        horizontal = false;
    else
        horizontal = fminf(d_left, d_right) <= fminf(d_top, d_bottom);

    if (horizontal)
        *dx = (d_left <= d_right ? -1.0f : 1.0f) * offset * r.w;
    else
        *dy = (d_top <= d_bottom ? -1.0f : 1.0f) * offset * r.h;
}

#endif
