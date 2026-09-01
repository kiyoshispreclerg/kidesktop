/*
 * Scale out (section 24.3): the closing half of scale-in, and its exact
 * mirror -- the window starts at its real size and goes to the configured
 * one, around the same origin scale-in would have grown out of.
 *
 * Like fade-out, it draws a window that is already gone, held by
 * window_retain() until the animation ends (see window.h).
 *
 * The destination is always the window's real geometry; what is
 * configurable is the size it ends at and the point it shrinks into:
 *
 *   origin = window    the window's own centre (default)
 *   origin = pointer   where the mouse is, so a menu appears to come out
 *                      of the click that opened it
 *   origin = output    the centre of the monitor the window is on
 *
 * kicomp.conf:
 *
 *   [effect:scale-out]
 *   enabled  = 0
 *   duration = 1.0     # multiple of the global animation unit
 *   events   = close
 *   windows  = windows,menus
 *   to     = 0.8     # final size, as a fraction of the real one
 *   origin   = window  # window | pointer | output
 *
 * Several instances may exist, each with its own numbers -- see the
 * README: [effect:scale-out:minimize] with a different origin and size is
 * a section, not a second effect.
 */
#include "../effect.h"
#include "../animation.h"
#include "../output.h"
#include "../window.h"
#include "../transform.h"
#include "../renderer.h"

#include <stdlib.h>
#include <string.h>

typedef enum {
    ORIGIN_WINDOW,
    ORIGIN_POINTER,
    ORIGIN_OUTPUT,
} ScaleOrigin;

/* One of these per instance (effect.h's CompEffectModule::config_size),
 * which is what lets two [effect:scale-out:*] sections animate the same
 * window with different numbers. */
typedef struct {
    float size;            /* final size, as a fraction of the real one */
    ScaleOrigin origin;
} ScaleConfig;

typedef struct {
    const ScaleConfig *cfg;
    float ox, oy;          /* the fixed point, root coordinates */
    CompRect geometry;     /* the window's rect, frozen at the start */
    CompRect covered;      /* what the last frame drew, for damage */
} ScaleData;

static void config_defaults(void *config)
{
    ScaleConfig *c = config;
    c->size = 0.8f;
    c->origin = ORIGIN_WINDOW;
}

static bool config_key(void *config, const char *key, const char *value)
{
    ScaleConfig *c = config;

    if (strcmp(key, "to") == 0) {
        float f = (float)atof(value);
        /* Above 1 is a real answer, not a mistake: a window that swells slightly as it dissolves is a common close. The floor
         * is not 0 -- a window scaled to nothing has no pixels left to
         * sample and those frames turn to smear. */
        if (f < 0.05f) f = 0.05f;
        if (f > 4.0f) f = 4.0f;
        c->size = f;
    } else if (strcmp(key, "origin") == 0) {
        if (strcmp(value, "pointer") == 0)     c->origin = ORIGIN_POINTER;
        else if (strcmp(value, "output") == 0) c->origin = ORIGIN_OUTPUT;
        else if (strcmp(value, "window") == 0) c->origin = ORIGIN_WINDOW;
        else return false;
    } else {
        return false;
    }
    return true;
}

/* The point the window grows out of (or shrinks into). Queried once, when
 * the effect starts: the pointer keeps moving, and an origin that moved
 * with it would drag the animation sideways. */
static void origin_for(CompWindow *w, const ScaleConfig *cfg, float *ox, float *oy)
{
    CompRect r = window_rect(w);

    if (cfg->origin == ORIGIN_POINTER) {
        xcb_query_pointer_reply_t *p = xcb_query_pointer_reply(comp.conn,
            xcb_query_pointer(comp.conn, comp.root), NULL);
        if (p) {
            *ox = p->root_x;
            *oy = p->root_y;
            free(p);
            return;
        }
        /* No pointer on this screen: the window's own centre is the
         * honest fallback, not (0,0). */
    } else if (cfg->origin == ORIGIN_OUTPUT) {
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
    float s = comp_lerp(1.0f, d->cfg->size, p);

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
    scale_transform(e, effect_ease(e, comp_progress(now, e->start_time, e->duration)), &t);
    comp_transform_bbox(&t, &d->geometry, &d->covered);

    output_damage_rect(&previous);
    output_damage_rect(&d->covered);
}

static void scale_apply(CompEffect *e, CompScene *s, CompOutput *o)
{
    float p = effect_ease(e, comp_progress(comp_now_ms(), e->start_time, e->duration));

    for (int i = 0; i < s->count; i++) {
        CompSceneNode *n = &s->nodes[i];
        if (n->win != e->window)
            continue;

        scale_transform(e, p, &n->transform);

        /* The area actually covered right now, clipped to this output --
         * the node's own geometry is where the window is, which is not
         * where it is being drawn. */
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

static void on_event(CompWindow *w, const CompEvent *event,
                     const CompEffectInstance *self)
{
    (void)event;

    /* Nothing was ever drawn for it: there are no pixels to scale, and
     * naming a pixmap now would fail -- the window is already gone. */
    if (!renderer_window_has_content(w))
        return;

    double duration = effect_instance_duration(self);
    if (duration <= 0.0)
        return;

    CompEffect *e = calloc(1, sizeof(*e));
    ScaleData *d = calloc(1, sizeof(*d));
    if (!e || !d) {
        free(e);
        free(d);
        return;
    }

    d->cfg = self->config;
    origin_for(w, d->cfg, &d->ox, &d->oy);
    d->geometry = window_rect(w);
    d->covered = d->geometry;

    e->ops = &scale_ops;
    e->instance = self;
    e->window = w;
    e->start_time = comp_now_ms();
    e->duration = duration;
    e->data = d;

    window_retain(w);
    effects_add(e);
    output_damage_rect(&d->covered);
}

const CompEffectModule effect_scale_out = {
    .name             = "scale-out",
    .default_enabled  = false,
    .default_duration = 1.0,
    /* Weight at the destination: it arrives gently, which is what reads
     * as settling into place. kicomp.conf's easing= overrides it. */
    .default_easing   = COMP_EASE_OUT,
    .default_events   = COMP_EVENT_BIT(COMP_EVENT_CLOSE),
    .default_windows  = COMP_WINDOWS_ALL & ~(COMP_WINDOW_BIT(COMP_WINDOW_DESKTOP) |
                                             COMP_WINDOW_BIT(COMP_WINDOW_DOCK)),
    .config_size      = sizeof(ScaleConfig),
    .config_defaults  = config_defaults,
    .config_key       = config_key,
    .window_event     = on_event,
};
