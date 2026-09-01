/*
 * Smooth move: a window being dragged is drawn a little behind where the
 * pointer has actually put it, catching up continuously, so a stream of
 * configures that arrives in uneven steps -- which is what a drag always
 * is -- reads as one smooth glide instead of a series of small jumps.
 *
 * This is the case the geometry effect deliberately refuses (see its
 * header): it answers to the jumps a WM makes a window take, and bows out
 * of interactive drags because animating each step of one would leave the
 * window trailing the pointer by a whole animation. This effect is that
 * refusal turned into a feature, with the two things that make the
 * difference:
 *
 *   - the lag is a filter, not an animation. There is no "from" and "to"
 *     to travel between; there is an offset between where the window looks
 *     like it is and where it really is, and that offset decays towards
 *     zero the whole time. Every new configure adds to it, the decay eats
 *     it, and the window is always converging on the truth rather than
 *     replaying a path towards a destination that has already changed.
 *   - the offset is capped. Smoothing is worth a few pixels of lag and
 *     nothing more: past that the titlebar visibly separates from the
 *     pointer holding it, which reads as the compositor being slow, not as
 *     the window being smooth. max_lag= is that ceiling, and the window
 *     never falls further behind than it however fast the drag is.
 *
 * The decay is exponential and computed from elapsed time, never from a
 * frame count (animation.h): averaging the last N frames' positions -- the
 * obvious way to write this -- quietly assumes the frames are evenly
 * spaced, and ours are not (a frame is painted when something is dirty).
 * An exponential filter is what that average converges to when you stop
 * assuming that, and it costs one multiply.
 *
 *   offset *= exp(-dt / tau)
 *
 * where tau is this instance's duration: how long the window takes to
 * close roughly two thirds of the gap. Short (a quarter unit) is a
 * de-jitter; long (a whole unit) is a visible glide.
 *
 * kicomp.conf:
 *
 *   [effect:smooth-move]
 *   enabled  = 1
 *   duration = 0.35    # multiple of the global animation unit = tau
 *   events   = move
 *   windows  = windows
 *   max_lag  = 48      # px the picture may fall behind the pointer
 *   resize   = 0       # smooth drags that resize as well as move
 *
 * The logical geometry is never touched (section 27): the window really is
 * under the pointer the whole time, input and all. Only the picture is
 * behind, and only by max_lag pixels at the very most.
 */
#include "../effect.h"
#include "../animation.h"
#include "../output.h"
#include "../window.h"
#include "../transform.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Below this the offset is not worth another frame. */
#define SETTLED_PX 0.5f

typedef struct {
    int max_lag;
    bool resize;
} SmoothConfig;

typedef struct {
    const SmoothConfig *cfg;

    /* Where the picture is, relative to where the window really is. */
    float ox, oy;

    double last_ms;      /* when the offset was last decayed */
    double tau;          /* ms; the offset's half-life, near enough */

    CompRect covered;    /* what the last frame drew, for damage */
} SmoothData;

/* The one window this module smooths, kept here rather than looked up in
 * the core's list -- the same arrangement geometry.c uses, and for the same
 * reason: only one window is ever being dragged. */
static CompEffect *active;
static CompWindow *active_window;

static void config_defaults(void *config)
{
    SmoothConfig *c = config;
    c->max_lag = 48;
    c->resize = false;
}

static bool config_key(void *config, const char *key, const char *value)
{
    SmoothConfig *c = config;

    if (strcmp(key, "max_lag") == 0) {
        int px = atoi(value);
        if (px < 0) px = 0;
        if (px > 400) px = 400;
        c->max_lag = px;
        return true;
    }
    if (strcmp(key, "resize") == 0) {
        c->resize = atoi(value) != 0;
        return true;
    }
    return false;
}

/* Keeps the offset inside the configured ceiling, as a vector: clamping
 * each axis on its own would bend a diagonal drag towards the axes. */
static void clamp_offset(SmoothData *d)
{
    float max = (float)d->cfg->max_lag;
    if (max <= 0.0f) {
        d->ox = d->oy = 0.0f;
        return;
    }

    float len = sqrtf(d->ox * d->ox + d->oy * d->oy);
    if (len <= max || len <= 0.0f)
        return;

    float k = max / len;
    d->ox *= k;
    d->oy *= k;
}

static CompRect rect_now(CompEffect *e)
{
    SmoothData *d = e->data;
    CompRect r = window_rect(e->window);
    r.x += (int)(d->ox + (d->ox < 0.0f ? -0.5f : 0.5f));
    r.y += (int)(d->oy + (d->oy < 0.0f ? -0.5f : 0.5f));
    return r;
}

static void smooth_update(CompEffect *e, double now)
{
    SmoothData *d = e->data;

    double dt = now - d->last_ms;
    if (dt < 0.0)
        dt = 0.0;
    d->last_ms = now;

    /* Exponential decay, from the time actually elapsed: a frame that took
     * twice as long closes twice as much of the gap, so an output dropping
     * frames stays on the same curve instead of lagging further behind. */
    if (d->tau > 0.0) {
        float k = expf(-(float)(dt / d->tau));
        d->ox *= k;
        d->oy *= k;
    } else {
        d->ox = d->oy = 0.0f;
    }

    if (fabsf(d->ox) < SETTLED_PX) d->ox = 0.0f;
    if (fabsf(d->oy) < SETTLED_PX) d->oy = 0.0f;

    CompRect previous = d->covered;
    d->covered = rect_now(e);

    output_damage_rect(&previous);
    output_damage_rect(&d->covered);
}

static void smooth_apply(CompEffect *e, CompScene *s, CompOutput *o)
{
    CompRect at = rect_now(e);

    for (int i = 0; i < s->count; i++) {
        CompSceneNode *n = &s->nodes[i];
        if (n->win != e->window)
            continue;

        comp_transform_identity(&n->transform);
        comp_transform_translate(&n->transform,
                                 (float)(at.x - n->geometry.x),
                                 (float)(at.y - n->geometry.y));

        if (!rect_intersect(&at, &o->rect, &n->visible_rect))
            n->visible_rect = (CompRect){ 0, 0, 0, 0 };
        return;
    }
}

/* Done when the picture has caught up. There is no duration to run out:
 * the effect lives for as long as the drag keeps feeding it, and ends a
 * few frames after the drag stops. */
static bool smooth_finished(const CompEffect *e, double now)
{
    SmoothData *d = e->data;
    (void)now;
    return d->ox == 0.0f && d->oy == 0.0f;
}

static void smooth_destroy(CompEffect *e)
{
    if (e == active) {
        active = NULL;
        active_window = NULL;
    }
    free(e->data);
    e->data = NULL;
}

static const CompEffectOps smooth_ops = {
    .name     = "smooth-move",
    .update   = smooth_update,
    .apply    = smooth_apply,
    .finished = smooth_finished,
    .destroy  = smooth_destroy,
};

static void on_event(CompWindow *w, const CompEvent *event,
                     const CompEffectInstance *self)
{
    const SmoothConfig *cfg = self->config;

    /* Only a drag. A window the WM moved in one jump is the geometry
     * effect's business -- it has a real destination to travel to, which
     * is a different animation from this one. */
    if (!event->interactive || !w->mapped || w->input_only)
        return;

    /* A resize drag moves the window's edges, not the window: smoothing it
     * would mean drawing the frame at a size the client has not painted
     * yet, so it is off unless asked for. */
    if (!cfg->resize &&
        (event->to.w != event->from.w || event->to.h != event->from.h))
        return;

    double tau = effect_instance_duration(self);
    if (tau <= 0.0 || cfg->max_lag <= 0)
        return;

    double now = comp_now_ms();

    CompEffect *e = active;
    if (!e || active_window != w) {
        e = calloc(1, sizeof(*e));
        SmoothData *d = calloc(1, sizeof(*d));
        if (!e || !d) {
            free(e);
            free(d);
            return;
        }

        d->cfg = cfg;
        d->tau = tau;
        d->last_ms = now;
        d->covered = window_rect(w);

        e->ops = &smooth_ops;
        e->instance = self;
        e->window = w;
        e->start_time = now;
        e->duration = tau;   /* reported in the log; the decay is what ends it */
        e->data = d;

        active = e;
        active_window = w;
        effects_add(e);
    }

    /* The window has just jumped by (to - from). Adding the opposite to
     * the offset leaves the picture exactly where it was a moment ago, and
     * the decay carries it from there -- which is the whole trick: the
     * jump never appears, only the catching up. */
    SmoothData *d = e->data;
    d->ox -= (float)(event->to.x - event->from.x);
    d->oy -= (float)(event->to.y - event->from.y);
    clamp_offset(d);

    output_damage_rect(&d->covered);
    d->covered = rect_now(e);
    output_damage_rect(&d->covered);
}

const CompEffectModule effect_smooth_move = {
    .name             = "smooth-move",
    /* Off by default: a few pixels of deliberate lag under the pointer is
     * a taste, not an improvement everyone wants, and it is the one effect
     * here that touches something the user is actively holding. */
    .default_enabled  = false,
    /* The time constant, not a length: a third of a unit smooths the steps
     * out without the window visibly trailing. */
    .default_duration = 0.35,
    /* Nothing to weight -- the curve is the exponential decay itself, and
     * an easing on top of it would be a curve applied to a curve. */
    .default_easing   = COMP_EASE_LINEAR,
    .default_events   = COMP_EVENT_BIT(COMP_EVENT_MOVE),
    .default_windows  = COMP_WINDOW_BIT(COMP_WINDOW_UNKNOWN) |
                        COMP_WINDOW_BIT(COMP_WINDOW_NORMAL) |
                        COMP_WINDOW_BIT(COMP_WINDOW_DIALOG) |
                        COMP_WINDOW_BIT(COMP_WINDOW_UTILITY) |
                        COMP_WINDOW_BIT(COMP_WINDOW_TOOLBAR),
    .config_size      = sizeof(SmoothConfig),
    .config_defaults  = config_defaults,
    .config_key       = config_key,
    .window_event     = on_event,
};
