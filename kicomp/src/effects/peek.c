/*
 * peek: hovering a window's thumbnail in the panel makes every other
 * window dissolve, so the one being pointed at is seen on its own
 * (Windows 7's Aero Peek, kwin's "highlight window").
 *
 * The panel knows what is under the pointer and the compositor is the
 * only one that can paint, so the two talk through one root property:
 *
 *   _KICOMP_PEEK   WINDOW[1]   the client window being hovered; deleted
 *                              (or 0) when the hover ends
 *
 * The property is the state, not an event: the panel rewrites it about
 * once a second while the hover lasts, and a peek that is not renewed for
 * PEEK_TIMEOUT_MS ends by itself -- a panel that died mid-hover must not
 * leave the desktop invisible.
 *
 * What it draws, per window, is one opacity ramp: the hovered window
 * stays (or comes) to 1, the others of its output go to 0, and a window
 * that is minimized (stowed) is brought in from its kept picture, but
 * only when it belongs to the desktop its output is showing now. Moving
 * from one thumbnail to the next retargets every ramp from where it is,
 * so the windows cross-fade instead of snapping. Windows are removed from
 * the scene once they are fully transparent, so a peek costs less to draw
 * than the desktop it hides.
 *
 * Only normal windows take part: docks, menus, the wallpaper and the
 * panel's own popup are never faded.
 *
 * kicomp.conf:
 *
 *   [effect:peek]
 *   enabled  = 1       # off by default
 *   duration = 1.0     # multiples of animation_duration
 *   easing   = linear
 */
#include "../effect.h"
#include "../animation.h"
#include "../output.h"
#include "../window.h"
#include "../desktop.h"

#include <stdlib.h>
#include <string.h>

#define PEEK_TIMEOUT_MS 3000.0
#define MAX_RAMPS 128
#define GONE 0.002f

typedef struct {
    CompWindow *w;
    float from, to;
    double t0;
} Ramp;

typedef struct {
    Ramp ramps[MAX_RAMPS];
    int count;
    CompWindow *target;
    bool ending;
    bool was_moving;
    double last_renew;
    CompRect area;      /* the output being peeked, for damage */
} PeekData;

static const CompEffectOps peek_ops;
static CompEffect *fx;

static bool fadeable(const CompWindow *w)
{
    if (!w->mapped || w->input_only || w->wm_layer[0])
        return false;
    switch (w->type) {
    case COMP_WINDOW_UNKNOWN:       /* no type set: an ordinary window... */
        return !w->override_redirect;   /* ...unless nobody manages it */
    case COMP_WINDOW_NORMAL:
    case COMP_WINDOW_DIALOG:
    case COMP_WINDOW_UTILITY:
    case COMP_WINDOW_TOOLBAR:
    case COMP_WINDOW_SPLASH:
        return true;
    default:
        return false;
    }
}

/* What a window looks like with no peek: drawn, or kept out of the scene
 * (minimized). */
static float natural(const CompWindow *w)
{
    return w->mapped && !w->stowed ? 1.0f : 0.0f;
}

static const CompOutput *output_of(const CompWindow *w)
{
    CompRect r = window_rect(w);
    const CompOutput *best = NULL;
    long best_area = 0;

    for (int i = 0; i < comp.output_count; i++) {
        CompRect hit;
        if (!rect_intersect(&r, &comp.outputs[i].rect, &hit))
            continue;
        long a = (long)hit.w * hit.h;
        if (a > best_area) {
            best_area = a;
            best = &comp.outputs[i];
        }
    }
    return best;
}

static bool settled(const CompEffect *e, const Ramp *r, double now)
{
    return e->duration <= 0.0 || now >= r->t0 + e->duration;
}

static float ramp_value(const CompEffect *e, const Ramp *r, double now)
{
    if (settled(e, r, now))
        return r->to;
    float v = comp_lerp(r->from, r->to,
                        effect_ease(e, comp_progress(now, r->t0, e->duration)));
    return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
}

static Ramp *ramp_find(PeekData *d, const CompWindow *w)
{
    for (int i = 0; i < d->count; i++)
        if (d->ramps[i].w == w)
            return &d->ramps[i];
    return NULL;
}

static Ramp *ramp_for(PeekData *d, CompWindow *w, double now)
{
    Ramp *r = ramp_find(d, w);
    if (r)
        return r;
    if (d->count >= MAX_RAMPS)
        return NULL;
    r = &d->ramps[d->count++];
    r->w = w;
    r->from = r->to = natural(w);
    r->t0 = now;
    return r;
}

/* Aim every ramp at "`t` shown, the rest of its output gone", from
 * wherever each one is now. Windows that are no longer part of the peek
 * (the other output, the previous target) head back to how they look
 * with none. */
static void retarget(CompEffect *e, PeekData *d, CompWindow *t, double now)
{
    for (int i = 0; i < d->count; i++) {
        Ramp *r = &d->ramps[i];
        r->from = ramp_value(e, r, now);
        r->to = natural(r->w);
        r->t0 = now;
    }

    const CompOutput *out = output_of(t);
    d->target = t;
    d->ending = false;
    d->was_moving = true;
    if (out)
        d->area = out->rect;

    for (CompWindow *w = comp.stack; w; w = w->next) {
        if (w != t && !(fadeable(w) && output_of(w) == out))
            continue;
        Ramp *r = ramp_for(d, w, now);
        if (r)
            r->to = (w == t) ? 1.0f : 0.0f;
    }
    Ramp *r = ramp_for(d, t, now);
    if (r)
        r->to = 1.0f;
}

static void end_peek(CompEffect *e, PeekData *d, double now)
{
    if (d->ending)
        return;
    d->ending = true;
    d->target = NULL;
    d->was_moving = true;
    for (int i = 0; i < d->count; i++) {
        Ramp *r = &d->ramps[i];
        r->from = ramp_value(e, r, now);
        r->to = natural(r->w);
        r->t0 = now;
    }
}

static void peek_update(CompEffect *e, double now)
{
    PeekData *d = e->data;

    if (!d->ending && now - d->last_renew > PEEK_TIMEOUT_MS)
        end_peek(e, d, now);

    bool moving = false;
    for (int i = 0; i < d->count; i++)
        if (!settled(e, &d->ramps[i], now))
            moving = true;

    /* One more after the last ramp lands, for the picture it ends on. */
    if ((moving || d->was_moving) && d->area.w > 0)
        output_damage_rect(&d->area);
    d->was_moving = moving;

    /* A ramp that has arrived where the window would be anyway says
     * nothing any more. */
    for (int i = 0; i < d->count;) {
        Ramp *r = &d->ramps[i];
        if (settled(e, r, now) && r->to == natural(r->w))
            *r = d->ramps[--d->count];
        else
            i++;
    }
}

static void peek_apply(CompEffect *e, CompScene *s, CompOutput *o)
{
    (void)o;
    PeekData *d = e->data;
    double now = comp_now_ms();
    int keep = 0;

    for (int i = 0; i < s->count; i++) {
        CompSceneNode *n = &s->nodes[i];
        const Ramp *r = ramp_find(d, n->win);
        if (r) {
            float v = ramp_value(e, r, now);
            if (v <= GONE)
                continue;
            n->opacity *= v;
        }
        if (keep != i)
            s->nodes[keep] = *n;
        s->nodes[keep].z = keep;
        keep++;
    }
    s->count = keep;
}

static bool peek_finished(const CompEffect *e, double now)
{
    (void)now;
    const PeekData *d = e->data;
    return d->ending && d->count == 0;
}

static void peek_window_gone(CompEffect *e, CompWindow *w)
{
    PeekData *d = e->data;

    for (int i = 0; i < d->count; i++) {
        if (d->ramps[i].w == w) {
            d->ramps[i] = d->ramps[--d->count];
            break;
        }
    }
    if (d->target == w)
        end_peek(e, d, comp_now_ms());
}

static void peek_destroy(CompEffect *e)
{
    PeekData *d = e->data;
    if (d && d->area.w > 0)
        output_damage_rect(&d->area);
    free(d);
    e->data = NULL;
    if (fx == e)
        fx = NULL;
}

static const CompEffectOps peek_ops = {
    .name        = "peek",
    .update      = peek_update,
    .apply       = peek_apply,
    .finished    = peek_finished,
    .window_gone = peek_window_gone,
    .destroy     = peek_destroy,
};

/* Whether a minimized window is part of the peek right now, which is what
 * lets its kept picture into the scene (scene_wants_window). */
bool peek_shows_stowed(const CompWindow *w)
{
    if (!fx)
        return false;
    return ramp_find(fx->data, w) != NULL;
}

/* A window can only be peeked where it could be seen now: drawn, or
 * minimized on the desktop its output is showing. */
static bool peekable(const CompWindow *w)
{
    if (w->input_only || w->wm_layer[0])
        return false;
    if (w->mapped)
        return true;
    if (!w->stowed)
        return false;

    int desk, out_index;
    if (!desktop_of_window(w, &desk, &out_index))
        return false;
    if (desk == COMP_DESKTOP_ALL)
        return true;

    const CompOutput *o = NULL;
    if (out_index >= 0) {
        for (int i = 0; i < comp.output_count; i++)
            if (desktop_output_index(&comp.outputs[i]) == out_index)
                o = &comp.outputs[i];
    }
    if (!o)
        o = output_of(w);
    return o && desktop_current_for_output(o) == desk;
}

void peek_request(xcb_window_t id)
{
    const CompEffectInstance *inst = effect_base_instance("peek");
    if (!effects_enabled() || !inst || !inst->enabled)
        return;

    double now = comp_now_ms();
    CompWindow *t = NULL;
    if (id != XCB_NONE) {
        t = window_find_by_client(id);
        if (!t)
            t = window_find(id);
    }
    if (t && !peekable(t))
        t = NULL;

    if (!t) {
        if (fx)
            end_peek(fx, fx->data, now);
        return;
    }

    if (!fx) {
        CompEffect *e = calloc(1, sizeof(*e));
        PeekData *d = calloc(1, sizeof(*d));
        if (!e || !d) {
            free(e);
            free(d);
            return;
        }
        e->ops = &peek_ops;
        e->instance = inst;
        e->start_time = now;
        e->duration = effect_instance_duration(inst);
        e->data = d;
        fx = e;
        effects_add(e);
    }

    PeekData *d = fx->data;
    d->last_renew = now;
    if (d->target == t && !d->ending)
        return;
    retarget(fx, d, t, now);
}

const CompEffectModule effect_peek = {
    .name             = "peek",
    .default_enabled  = false,
    .default_duration = 1.0,
    .default_easing   = COMP_EASE_LINEAR,
    .default_events   = 0,
    .default_windows  = 0,
};
