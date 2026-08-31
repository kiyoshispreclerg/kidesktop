/*
 * Geometry change (section 24.4): a window that jumps to a new size or
 * place -- maximize, restore, tile to half the screen, a snap -- slides
 * and scales there instead of teleporting, over one unit of the user's
 * global animation time (kicomp.conf's animation_duration=, times this
 * effect's own multiplier).
 *
 * This is the first effect, and it exists as much to prove the interface
 * as to look nice: the whole file touches nothing but its own state and
 * the scene nodes it is handed, and adding it to the compositor was one
 * line in effect.c's table (section 42).
 *
 * The logical geometry is never touched (section 27). The WM has already
 * moved the window and everything -- input, focus, EWMH -- is at the new
 * place from the first instant. Only the picture is behind.
 *
 * Which events it answers to is configuration, not code: by default
 * every jump the WM makes a window take *except* shade, which has its own
 * effect and must not have this sliding underneath it.
 *
 * What it deliberately does NOT animate: an interactive drag. A
 * move/resize drag arrives as a stream of configures, and animating those
 * would put the window visibly behind the pointer. Without an IPC to ask
 * the WM "is this a drag?" (section 32, still absent), the stream itself
 * is the signal -- see window.c, which times the gap between configures.
 */
#include "../effect.h"
#include "../animation.h"
#include "../output.h"
#include "../transform.h"

#include <stdlib.h>

/* Below this the animation is more distracting than the jump it hides:
 * a one-pixel nudge or a shadow-sized resize shouldn't slide. */
#define MIN_DELTA_PX 24

typedef struct {
    CompRect from;      /* where it looked like it was */
    CompRect to;        /* where the WM actually put it */
    CompRect current;   /* interpolated, for damage bookkeeping */
} GeometryData;

static const CompEffectOps geometry_ops;

/* The one window this module is animating, if any -- kept here rather
 * than looked up in the core's list, so effect.h stays a two-struct
 * interface and the core never has to answer "who is animating what". */
static CompEffect *active;
static CompWindow *active_window;

static CompRect lerp_rect(const CompRect *a, const CompRect *b, float p)
{
    CompRect r;
    r.x = (int)(comp_lerp((float)a->x, (float)b->x, p) + 0.5f);
    r.y = (int)(comp_lerp((float)a->y, (float)b->y, p) + 0.5f);
    r.w = (int)(comp_lerp((float)a->w, (float)b->w, p) + 0.5f);
    r.h = (int)(comp_lerp((float)a->h, (float)b->h, p) + 0.5f);
    if (r.w < 1) r.w = 1;
    if (r.h < 1) r.h = 1;
    return r;
}

static void geometry_update(CompEffect *e, double now)
{
    GeometryData *d = e->data;

    CompRect previous = d->current;
    float p = comp_ease_out(comp_progress(now, e->start_time, e->duration));
    d->current = lerp_rect(&d->from, &d->to, p);

    /* Both the rectangle being vacated and the one being entered have to
     * be repainted, and only the outputs they touch (section 39). */
    output_damage_rect(&previous);
    output_damage_rect(&d->current);
}

static void geometry_apply(CompEffect *e, CompScene *s, CompOutput *o)
{
    GeometryData *d = e->data;

    for (int i = 0; i < s->count; i++) {
        CompSceneNode *n = &s->nodes[i];
        if (n->win != e->window)
            continue;

        /* n->geometry is where the window really is (d->to, give or take
         * a configure still in flight). The transform maps that rectangle
         * onto the interpolated one -- scale about the real origin, then
         * translate the difference. The renderer inverts it to sample the
         * window's pixmap; nothing here rewrites the geometry itself. */
        float sx = (float)d->current.w / (float)(n->geometry.w > 0 ? n->geometry.w : 1);
        float sy = (float)d->current.h / (float)(n->geometry.h > 0 ? n->geometry.h : 1);

        comp_transform_identity(&n->transform);
        comp_transform_translate(&n->transform, (float)-n->geometry.x, (float)-n->geometry.y);
        comp_transform_scale(&n->transform, sx, sy);
        comp_transform_translate(&n->transform, (float)d->current.x, (float)d->current.y);

        /* The visible part follows the animated rectangle, not the real
         * one -- otherwise the window would be clipped to where it is
         * going while being drawn where it still looks like it is. */
        CompRect vis;
        if (rect_intersect(&d->current, &o->rect, &vis))
            n->visible_rect = vis;
        else
            n->visible_rect = (CompRect){ 0, 0, 0, 0 };
        return;
    }
}

static bool geometry_finished(const CompEffect *e, double now)
{
    return now >= e->start_time + e->duration;
}

static void geometry_destroy(CompEffect *e)
{
    if (e == active) {
        active = NULL;
        active_window = NULL;
    }
    free(e->data);
    e->data = NULL;
}

static const CompEffectOps geometry_ops = {
    .name     = "geometry",
    .update   = geometry_update,
    .apply    = geometry_apply,
    .finished = geometry_finished,
    .destroy  = geometry_destroy,
};

static void on_event(CompWindow *w, const CompEvent *event,
                     const CompEffectInstance *self)
{
    const CompRect *from = &event->from;
    const CompRect *to = &event->to;

    if (!w->mapped || w->input_only)
        return;

    /* A drag: the window has to stay under the pointer. If one was
     * already animating this window when the drag started, it is now
     * lying about where the window is -- drop it. */
    if (event->interactive) {
        if (active && active_window == w)
            active->duration = 0.0;   /* retired on the next update */
        return;
    }

    int dx = to->x - from->x, dy = to->y - from->y;
    int dw = to->w - from->w, dh = to->h - from->h;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    if (dw < 0) dw = -dw;
    if (dh < 0) dh = -dh;

    if (dx < MIN_DELTA_PX && dy < MIN_DELTA_PX &&
        dw < MIN_DELTA_PX && dh < MIN_DELTA_PX)
        return;

    /* Already animating this window (maximize, then immediately
     * fullscreen): keep the picture where it currently *looks* like it
     * is and retarget from there, so the two moves read as one
     * continuous motion instead of a jump back to the start. */
    CompRect origin = *from;
    if (active && active_window == w) {
        GeometryData *old = active->data;
        origin = old->current;
        active->duration = 0.0;
    }

    CompEffect *e = calloc(1, sizeof(*e));
    GeometryData *d = calloc(1, sizeof(*d));
    if (!e || !d) {
        free(e);
        free(d);
        return;
    }

    d->from = origin;
    d->to = *to;
    d->current = origin;

    e->ops = &geometry_ops;
    e->instance = self;
    e->window = w;
    e->start_time = comp_now_ms();
    /* This instance's own multiple of the user's global animation unit --
     * never a number of milliseconds spelled out here (kicomp.conf's
     * animation_duration=, and the instance's duration=). */
    e->duration = effect_instance_duration(self);
    e->data = d;

    active = e;
    active_window = w;
    effects_add(e);

    output_damage_rect(&origin);
    output_damage_rect(to);
}

const CompEffectModule effect_geometry = {
    .name             = "geometry",
    .default_enabled  = true,
    /* One unit: a plain, unremarkable transition. Something meant to feel
     * instant would ask for 0.5, a big desktop-wide one for 2.0. */
    .default_duration = 1.0,
    /* Every kind of jump the WM can make a window take -- but not shade,
     * which has an effect of its own that must not have this sliding
     * underneath it (kicomp.conf: events=). */
    /* Ordinary windows; a menu or a panel being moved by the WM is not
     * something to animate. */
    .default_windows  = COMP_WINDOW_BIT(COMP_WINDOW_UNKNOWN) |
                        COMP_WINDOW_BIT(COMP_WINDOW_NORMAL) |
                        COMP_WINDOW_BIT(COMP_WINDOW_DIALOG) |
                        COMP_WINDOW_BIT(COMP_WINDOW_UTILITY) |
                        COMP_WINDOW_BIT(COMP_WINDOW_TOOLBAR),
    .default_events   = COMP_EVENT_BIT(COMP_EVENT_MAXIMIZE) |
                        COMP_EVENT_BIT(COMP_EVENT_UNMAXIMIZE) |
                        COMP_EVENT_BIT(COMP_EVENT_FULLSCREEN) |
                        COMP_EVENT_BIT(COMP_EVENT_UNFULLSCREEN) |
                        COMP_EVENT_BIT(COMP_EVENT_MOVE),
    .window_event     = on_event,
};
