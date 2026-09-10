/*
 * Scale in (section 24.2): a window that has just appeared grows into
 * place from a smaller version of itself.
 *
 * The destination is always the window's real geometry; what is
 * configurable is the size it starts at and the point it grows out of:
 *
 *   origin = window    the window's own centre (default)
 *   origin = pointer   where the mouse is, so a menu appears to come out
 *                      of the click that opened it
 *   origin = output    the centre of the monitor the window is on
 *
 * kicomp.conf:
 *
 *   [effect:scale-in]
 *   enabled  = 1
 *   duration = 1.0     # multiple of the global animation unit
 *   events   = open,restore,desktop-enter
 *   windows  = windows,menus
 *   from     = 0.8     # starting size, as a fraction of the final one
 *   origin   = window  # window | pointer | output
 *
 * Several instances may exist, each with its own numbers -- see the
 * README: [effect:scale-in:minimize] with a different origin and size is
 * a section, not a second effect.
 */
#include "../effect.h"
#include "../animation.h"
#include "../output.h"
#include "../window.h"
#include "../transform.h"

#include <stdlib.h>
#include <string.h>

typedef enum {
    ORIGIN_WINDOW,
    ORIGIN_POINTER,
    ORIGIN_OUTPUT,
} ScaleOrigin;

/* One of these per instance (effect.h's CompEffectModule::config_size),
 * which is what lets two [effect:scale-in:*] sections animate the same
 * window with different numbers. */
typedef struct {
    float size;            /* starting size, as a fraction of the final one */
    ScaleOrigin origin;
} ScaleConfig;

typedef struct {
    const ScaleConfig *cfg;
    /* Whether the point below is a point in space, queried once (pointer,
     * output), or has to be re-read from the window every frame -- see
     * origin_now(). */
    bool origin_fixed;
    float ox, oy;          /* that point, when it is fixed; root coordinates */
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

    if (strcmp(key, "from") == 0) {
        float f = (float)atof(value);
        /* Above 1 is a real answer, not a mistake: above 1 shrinks into place instead of growing. The floor
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

/* The point the window grows out of, for this frame.
 *
 * origin = pointer and origin = output name a point in *space*, so each
 * is queried once when the effect starts: the pointer keeps moving, and an
 * origin that moved with it would drag the animation sideways.
 *
 * origin = window names the window's own centre, which is not a point in
 * space -- it is wherever the window currently is, so it has to be read
 * again every frame. Freezing it was a real bug rather than a purity
 * argument. A toolkit popup is routinely mapped somewhere that is not
 * where it will be shown and moved into place a beat later (Firefox's tab
 * previews are mapped below the bottom of the screen), so `open` fires
 * while the window is still parked off-screen, and the centre frozen
 * there is nowhere near the window by the time anyone sees it. Scaling
 * about a point that far away does not merely resize the window, it
 * *translates* it: the popup came sliding in from beyond the bottom edge,
 * which is not an animation anybody configured. */
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
         * honest fallback, not (0,0). Frozen here like any other fixed
         * origin -- it stands in for a point that could not be read, not
         * for origin = window, which never reaches this function. */
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

/* The origin to use right now: the frozen point for pointer/output, and
 * the window's live centre for origin = window. */
static void origin_now(CompEffect *e, float *ox, float *oy)
{
    ScaleData *d = e->data;

    if (d->origin_fixed) {
        *ox = d->ox;
        *oy = d->oy;
        return;
    }

    CompRect r = window_rect(e->window);
    *ox = r.x + r.w / 2.0f;
    *oy = r.y + r.h / 2.0f;
}

static void scale_transform(CompEffect *e, float p, CompTransform *t)
{
    ScaleData *d = e->data;
    float s = comp_lerp(d->cfg->size, 1.0f, p);

    float ox, oy;
    origin_now(e, &ox, &oy);

    comp_transform_identity(t);
    comp_transform_translate(t, -ox, -oy);
    comp_transform_scale(t, s, s);
    comp_transform_translate(t, ox, oy);
}

static void scale_update(CompEffect *e, double now)
{
    ScaleData *d = e->data;

    CompRect previous = d->covered;

    CompTransform t;
    scale_transform(e, effect_ease(e, comp_progress(now, e->start_time, e->duration)), &t);
    /* The window's rect as it is now, not as it was when the effect
     * started: a window that moved mid-animation is drawn at its new
     * place, and damaging the old one leaves the new one unrepainted. */
    CompRect geometry = window_rect(e->window);
    comp_transform_bbox(&t, &geometry, &d->covered);

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
    free(e->data);
    e->data = NULL;
}

static const CompEffectOps scale_ops = {
    .name     = "scale-in",
    .update   = scale_update,
    .apply    = scale_apply,
    .finished = scale_finished,
    .destroy  = scale_destroy,
};

static void on_event(CompWindow *w, const CompEvent *event,
                     const CompEffectInstance *self)
{
    (void)event;

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
    /* Only pointer and output name a point in space worth freezing;
     * origin = window is re-read every frame by origin_now(). */
    d->origin_fixed = d->cfg->origin != ORIGIN_WINDOW;
    if (d->origin_fixed)
        origin_for(w, d->cfg, &d->ox, &d->oy);
    d->covered = window_rect(w);

    e->ops = &scale_ops;
    e->instance = self;
    e->window = w;
    e->start_time = comp_now_ms();
    e->duration = duration;
    e->data = d;

    effects_add(e);
    output_damage_rect(&d->covered);
}

const CompEffectModule effect_scale_in = {
    .name             = "scale-in",
    .default_enabled  = false,
    .default_duration = 1.0,
    /* Weight at the destination: it arrives gently, which is what reads
     * as settling into place. kicomp.conf's easing= overrides it. */
    .default_easing   = COMP_EASE_OUT,
    .default_events   = COMP_EVENT_BIT(COMP_EVENT_OPEN) |
                        COMP_EVENT_BIT(COMP_EVENT_RESTORE) |
                        COMP_EVENT_BIT(COMP_EVENT_DESKTOP_ENTER),
    .default_windows  = COMP_WINDOWS_ALL & ~(COMP_WINDOW_BIT(COMP_WINDOW_DESKTOP) |
                                             COMP_WINDOW_BIT(COMP_WINDOW_DOCK)),
    .config_size      = sizeof(ScaleConfig),
    .config_defaults  = config_defaults,
    .config_key       = config_key,
    .window_event     = on_event,
};
