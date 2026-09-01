/*
 * Shade / unshade: the window rolls up behind its own titlebar, and
 * unrolls back out of it.
 *
 * No scaling and no distortion anywhere in it -- the content is never
 * squashed, it is *cropped*: the visible height shrinks from the whole
 * window down to the titlebar (or grows back), and the pixels that stay
 * visible are the same pixels at the same size the whole way, which is
 * what makes it read as a blind rolling up rather than a window being
 * squeezed.
 *
 * The two directions need different pixels, for a reason worth stating:
 *
 *   shade    by the time the compositor is told what happened, the WM has
 *            already collapsed the frame to its titlebar and unmapped the
 *            client -- the content that has to roll up is gone from the
 *            live pixmap. It is drawn from the stash instead: the
 *            contents the resize replaced, kept rather than freed
 *            (renderer.h), held for as long as this effect runs.
 *   unshade  the frame is already full-size again and the client is
 *            mapped, so the live contents are the right ones; only the
 *            crop grows.
 *
 * kicomp.conf:
 *
 *   [effect:shade]
 *   enabled  = 1
 *   duration = 1.0     # multiple of the global animation unit
 *   events   = shade,unshade
 *   windows  = windows
 *
 * Keep `shade` out of [effect:geometry]'s events (it is out by default),
 * or the window will slide and scale under this at the same time.
 */
#include "../effect.h"
#include "../animation.h"
#include "../output.h"
#include "../window.h"
#include "../renderer.h"

#include <stdlib.h>

typedef struct {
    bool rolling_up;     /* shade, as opposed to unshade */
    bool from_stash;     /* draw the contents the resize replaced */

    CompRect content;    /* the full window rectangle being cropped */
    int collapsed_h;     /* the height left when fully rolled up */

    CompRect covered;    /* what the last frame drew, for damage */
} ShadeData;

/* How tall the window is when rolled up: whatever the WM left of it. For
 * kiwm that is the titlebar, which is the whole point -- the compositor
 * doesn't need to know what a titlebar is, only how tall the window
 * ended up. */
static int collapsed_height(const CompEvent *event, bool rolling_up)
{
    int h = rolling_up ? event->to.h : event->from.h;
    return h > 0 ? h : 1;
}

static CompRect crop_for(CompEffect *e, float p)
{
    ShadeData *d = e->data;

    /* Rolling up: full height down to the collapsed one. Unrolling: the
     * other way. The top edge never moves -- the window rolls into its
     * titlebar, not away from it. */
    CompRect r = d->content;
    float full = (float)d->content.h;
    float small = (float)d->collapsed_h;
    float h = d->rolling_up ? comp_lerp(full, small, p)
                            : comp_lerp(small, full, p);

    r.h = (int)(h + 0.5f);
    if (r.h < 1)
        r.h = 1;
    return r;
}

static void shade_update(CompEffect *e, double now)
{
    ShadeData *d = e->data;

    CompRect previous = d->covered;
    d->covered = crop_for(e, effect_ease(e, comp_progress(now, e->start_time, e->duration)));

    output_damage_rect(&previous);
    output_damage_rect(&d->covered);
}

static void shade_apply(CompEffect *e, CompScene *s, CompOutput *o)
{
    ShadeData *d = e->data;

    float p = effect_ease(e, comp_progress(comp_now_ms(), e->start_time, e->duration));
    CompRect crop = crop_for(e, p);

    for (int i = 0; i < s->count; i++) {
        CompSceneNode *n = &s->nodes[i];
        if (n->win != e->window)
            continue;

        if (d->from_stash) {
            /* The node describes the window as it is now (a titlebar);
             * these contents are what it was before. */
            n->use_stash = true;
            n->geometry = d->content;
        }

        /* The crop *is* the drawing: visible_rect says which part of the
         * contents reaches the output, and the renderer takes the source
         * offset from the geometry -- so cutting the rectangle short cuts
         * the picture short, at the same scale. */
        if (!rect_intersect(&crop, &o->rect, &n->visible_rect))
            n->visible_rect = (CompRect){ 0, 0, 0, 0 };
        return;
    }
}

static bool shade_finished(const CompEffect *e, double now)
{
    return now >= e->start_time + e->duration;
}

static void shade_destroy(CompEffect *e)
{
    ShadeData *d = e->data;
    if (d && d->from_stash)
        renderer_stash_release(e->window);
    free(e->data);
    e->data = NULL;
}

static const CompEffectOps shade_ops = {
    .name     = "shade",
    .update   = shade_update,
    .apply    = shade_apply,
    .finished = shade_finished,
    .destroy  = shade_destroy,
};

static void on_event(CompWindow *w, const CompEvent *event,
                     const CompEffectInstance *self)
{
    bool rolling_up = (event->kind == COMP_EVENT_SHADE);

    double duration = effect_instance_duration(self);
    if (duration <= 0.0)
        return;

    CompEffect *e = calloc(1, sizeof(*e));
    ShadeData *d = calloc(1, sizeof(*d));
    if (!e || !d) {
        free(e);
        free(d);
        return;
    }

    d->rolling_up = rolling_up;
    d->collapsed_h = collapsed_height(event, rolling_up);

    if (rolling_up) {
        /* The window's full contents are only in the stash now. Without
         * one -- nothing was ever drawn, or the resize came through some
         * path that didn't stash -- there is nothing to roll up, and a
         * squashed titlebar is not an improvement. */
        if (!renderer_window_has_stash(w)) {
            free(e);
            free(d);
            return;
        }
        d->from_stash = true;
        d->content = renderer_window_stash_rect(w);
        renderer_stash_hold(w);
    } else {
        /* Unrolling draws the window's live contents, which the resize
         * that just happened deliberately unbound -- they are named again
         * on the next paint. Asking for them here would refuse every
         * unshade there is. */
        d->content = window_rect(w);
    }

    d->covered = d->content;

    e->ops = &shade_ops;
    e->instance = self;
    e->window = w;
    e->start_time = comp_now_ms();
    e->duration = duration;
    e->data = d;

    effects_add(e);
    output_damage_rect(&d->content);
}

const CompEffectModule effect_shade = {
    .name             = "shade",
    .default_enabled  = true,
    .default_duration = 1.0,
    .default_easing   = COMP_EASE_OUT,
    .default_events   = COMP_EVENT_BIT(COMP_EVENT_SHADE) |
                        COMP_EVENT_BIT(COMP_EVENT_UNSHADE),
    /* Only things that have a titlebar to roll into. */
    .default_windows  = COMP_WINDOW_BIT(COMP_WINDOW_UNKNOWN) |
                        COMP_WINDOW_BIT(COMP_WINDOW_NORMAL) |
                        COMP_WINDOW_BIT(COMP_WINDOW_DIALOG) |
                        COMP_WINDOW_BIT(COMP_WINDOW_UTILITY) |
                        COMP_WINDOW_BIT(COMP_WINDOW_TOOLBAR),
    .window_event     = on_event,
};
