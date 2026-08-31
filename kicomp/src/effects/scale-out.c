/*
 * Scale out (section 24.3): the closing half of scale-in, and its exact
 * mirror -- the window starts at its real size and shrinks toward the
 * configured origin, where scale-in grew out of it.
 *
 * Like fade-out, it draws a window that is already gone, held by
 * window_retain() until the animation ends (see window.h and
 * effects/fade-out.c).
 *
 * kicomp.conf:
 *
 *   [effect:scale-out]
 *   enabled  = 0
 *   duration = 1.0     # multiple of the global animation unit
 *   to       = 0.8     # final size as a fraction of the real one; above
 *                      # 1 swells instead of shrinking (0.05 .. 4.0)
 *   origin   = window  # window | pointer | output
 *   windows  = 1
 *   menus    = 1
 *   docks    = 0
 *
 * Pairing it with fade-out is the usual "zoom + fade on close" of section
 * 24.3: the two are separate effects and compose without either knowing
 * about the other -- one writes the node's transform, the other its
 * opacity.
 */
#include "../effect.h"
#include "../animation.h"
#include "../output.h"
#include "../window.h"
#include "../renderer.h"
#include "../transform.h"

#include <stdlib.h>
#include <string.h>

typedef enum {
    ORIGIN_WINDOW,
    ORIGIN_POINTER,
    ORIGIN_OUTPUT,
} ScaleOrigin;

static struct {
    bool windows;
    bool menus;
    bool docks;
    float to;
    ScaleOrigin origin;
} cfg = { true, true, false, 0.8f, ORIGIN_WINDOW };

typedef struct {
    float ox, oy;
    CompRect geometry;   /* the window's rect, frozen: it has no live one any more */
    CompRect covered;
} ScaleData;

static bool kind_enabled(CompWindowKind kind)
{
    switch (kind) {
    case COMP_WINDOW_MENU:    return cfg.menus;
    case COMP_WINDOW_DOCK:    return cfg.docks;
    case COMP_WINDOW_DESKTOP: return false;
    default:                  return cfg.windows;
    }
}

static void origin_for(CompWindow *w, float *ox, float *oy)
{
    CompRect r = window_rect(w);

    if (cfg.origin == ORIGIN_POINTER) {
        xcb_query_pointer_reply_t *p = xcb_query_pointer_reply(comp.conn,
            xcb_query_pointer(comp.conn, comp.root), NULL);
        if (p) {
            *ox = p->root_x;
            *oy = p->root_y;
            free(p);
            return;
        }
    } else if (cfg.origin == ORIGIN_OUTPUT) {
        CompRect centre = { r.x + r.w / 2, r.y + r.h / 2, 1, 1 };
        for (int i = 0; i < comp.output_count; i++) {
            CompRect hit;
            if (rect_intersect(&centre, &comp.outputs[i].rect, &hit)) {
                *ox = comp.outputs[i].rect.x + comp.outputs[i].rect.w / 2.0f;
                *oy = comp.outputs[i].rect.y + comp.outputs[i].rect.h / 2.0f;
                return;
            }
        }
    }

    *ox = r.x + r.w / 2.0f;
    *oy = r.y + r.h / 2.0f;
}

static void scale_transform(CompEffect *e, float p, CompTransform *t)
{
    ScaleData *d = e->data;
    float s = comp_lerp(1.0f, cfg.to, p);   /* the inverse of scale-in */

    comp_transform_identity(t);
    comp_transform_translate(t, -d->ox, -d->oy);
    comp_transform_scale(t, s, s);
    comp_transform_translate(t, d->ox, d->oy);
}

static void scale_update(CompEffect *e, double now)
{
    ScaleData *d = e->data;

    CompRect previous = d->covered;

    CompTransform t;
    scale_transform(e, comp_ease_out(comp_progress(now, e->start_time, e->duration)), &t);
    comp_transform_bbox(&t, &d->geometry, &d->covered);

    output_damage_rect(&previous);
    output_damage_rect(&d->covered);
}

static void scale_apply(CompEffect *e, CompScene *s, CompOutput *o)
{
    float p = comp_ease_out(comp_progress(comp_now_ms(), e->start_time, e->duration));

    for (int i = 0; i < s->count; i++) {
        CompSceneNode *n = &s->nodes[i];
        if (n->win != e->window)
            continue;

        scale_transform(e, p, &n->transform);

        CompRect bbox;
        comp_transform_bbox(&n->transform, &n->geometry, &bbox);
        if (!rect_intersect(&bbox, &o->rect, &n->visible_rect))
            n->visible_rect = (CompRect){ 0, 0, 0, 0 };
        return;
    }
}

static bool scale_finished(const CompEffect *e, double now)
{
    return now >= e->start_time + e->duration;
}

static void scale_destroy(CompEffect *e)
{
    window_release(e->window);
    free(e->data);
    e->data = NULL;
}

static const CompEffectOps scale_ops = {
    .name     = "scale-out",
    .update   = scale_update,
    .apply    = scale_apply,
    .finished = scale_finished,
    .destroy  = scale_destroy,
};

static void on_event(CompWindow *w, const CompEvent *event)
{
    (void)event;

    if (w->input_only || w->wm_layer[0])
        return;
    if (!kind_enabled(w->kind))
        return;
    if (!renderer_window_has_content(w))
        return;

    double duration = effect_duration("scale-out");
    if (duration <= 0.0)
        return;

    CompEffect *e = calloc(1, sizeof(*e));
    ScaleData *d = calloc(1, sizeof(*d));
    if (!e || !d) {
        free(e);
        free(d);
        return;
    }

    origin_for(w, &d->ox, &d->oy);
    d->geometry = window_rect(w);
    d->covered = d->geometry;

    e->ops = &scale_ops;
    e->window = w;
    e->start_time = comp_now_ms();
    e->duration = duration;
    e->data = d;

    window_retain(w);
    effects_add(e);
    output_damage_rect(&d->covered);
}

static bool on_config_key(const char *key, const char *value)
{
    if (strcmp(key, "windows") == 0) {
        cfg.windows = atoi(value) != 0;
    } else if (strcmp(key, "menus") == 0) {
        cfg.menus = atoi(value) != 0;
    } else if (strcmp(key, "docks") == 0) {
        cfg.docks = atoi(value) != 0;
    } else if (strcmp(key, "to") == 0) {
        float f = (float)atof(value);
        /* Above 1 is a real answer, not a mistake: a window that swells
         * slightly as it dissolves is a common and good-looking close.
         * The floor is not 0 -- a window scaled to nothing has no pixels
         * left to sample and the last frames turn to smear. */
        if (f < 0.05f) f = 0.05f;
        if (f > 4.0f) f = 4.0f;
        cfg.to = f;
    } else if (strcmp(key, "origin") == 0) {
        if (strcmp(value, "pointer") == 0)     cfg.origin = ORIGIN_POINTER;
        else if (strcmp(value, "output") == 0) cfg.origin = ORIGIN_OUTPUT;
        else if (strcmp(value, "window") == 0) cfg.origin = ORIGIN_WINDOW;
        else return false;
    } else {
        return false;
    }
    return true;
}

const CompEffectModule effect_scale_out = {
    .name             = "scale-out",
    .default_enabled  = false,
    .default_duration = 1.0,
    .default_events   = COMP_EVENT_BIT(COMP_EVENT_CLOSE),
    .window_event     = on_event,
    .config_key       = on_config_key,
};
