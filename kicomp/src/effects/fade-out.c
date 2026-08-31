/*
 * Fade out (section 24.1, the other half): a window that has gone away
 * dissolves instead of vanishing between one frame and the next.
 *
 * This is the first effect that outlives its subject. By the time it
 * starts, X has already unmapped the window and the application may
 * already be gone; what is being drawn is the contents pixmap the
 * compositor named while the window was still up, held alive by
 * window_retain() until the animation is done (see window.h). The retain
 * is released in destroy, so a cancelled effect -- the window came back,
 * the compositor is shutting down -- frees it just the same.
 *
 * kicomp.conf:
 *
 *   [effect:fade-out]
 *   enabled  = 1
 *   duration = 1.0     # multiple of the global animation unit
 *   events   = close   # close,minimize,desktop-leave, ...
 *   windows  = all
 */
#include "../effect.h"
#include "../animation.h"
#include "../output.h"
#include "../window.h"
#include "../renderer.h"

#include <stdlib.h>

static void fade_update(CompEffect *e, double now)
{
    (void)now;
    CompRect r = window_rect(e->window);
    output_damage_rect(&r);
}

static void fade_apply(CompEffect *e, CompScene *s, CompOutput *o)
{
    (void)o;

    float p = comp_ease_out(comp_progress(comp_now_ms(), e->start_time, e->duration));

    for (int i = 0; i < s->count; i++) {
        if (s->nodes[i].win != e->window)
            continue;
        s->nodes[i].opacity *= (1.0f - p);
        return;
    }
}

static bool fade_finished(const CompEffect *e, double now)
{
    return now >= e->start_time + e->duration;
}

static void fade_destroy(CompEffect *e)
{
    /* Always, however the effect ended -- this is what lets the window
     * (and its pixmap, and possibly the mirror entry itself) go. */
    window_release(e->window);
}

static const CompEffectOps fade_ops = {
    .name     = "fade-out",
    .update   = fade_update,
    .apply    = fade_apply,
    .finished = fade_finished,
    .destroy  = fade_destroy,
};

static void on_event(CompWindow *w, const CompEvent *event,
                     const CompEffectInstance *self)
{
    (void)event;

    /* Nothing was ever drawn for it: there is no picture to fade, and
     * asking for one now would fail -- the window is already unmapped. */
    if (!renderer_window_has_content(w))
        return;

    double duration = effect_instance_duration(self);
    if (duration <= 0.0)
        return;

    CompEffect *e = calloc(1, sizeof(*e));
    if (!e)
        return;

    e->ops = &fade_ops;
    e->instance = self;
    e->window = w;
    e->start_time = comp_now_ms();
    e->duration = duration;

    window_retain(w);
    effects_add(e);

    CompRect r = window_rect(w);
    output_damage_rect(&r);
}

const CompEffectModule effect_fade_out = {
    .name             = "fade-out",
    .default_enabled  = true,
    .default_duration = 1.0,
    .default_events   = COMP_EVENT_BIT(COMP_EVENT_CLOSE),
    .default_windows  = COMP_WINDOWS_ALL & ~COMP_WINDOW_BIT(COMP_WINDOW_DESKTOP),
    .window_event     = on_event,
};
