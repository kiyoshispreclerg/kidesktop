/*
 * Fade in (section 24.1): a window that has just appeared comes up from
 * transparent instead of arriving all at once.
 *
 * Opening and closing are two separate effects on purpose -- wanting one
 * without the other is a normal preference, and closing needs machinery
 * this one doesn't (a window that is already gone has to be kept alive to
 * be faded out; see the README).
 *
 * kicomp.conf:
 *
 *   [effect:fade-in]
 *   enabled  = 1
 *   duration = 1.0     # multiple of the global animation unit
 *   windows  = 1       # ordinary windows
 *   menus    = 1       # menus, popups, tooltips, notifications
 *   docks    = 1       # panels
 */
#include "../effect.h"
#include "../animation.h"
#include "../output.h"
#include "../window.h"

#include <stdlib.h>
#include <string.h>

static struct {
    bool windows;
    bool menus;
    bool docks;
} cfg = { true, true, true };

static bool kind_enabled(CompWindowKind kind)
{
    switch (kind) {
    case COMP_WINDOW_MENU:    return cfg.menus;
    case COMP_WINDOW_DOCK:    return cfg.docks;
    case COMP_WINDOW_DESKTOP: return false;   /* the wallpaper doesn't "open" */
    default:                  return cfg.windows;
    }
}

static void fade_update(CompEffect *e, double now)
{
    (void)now;
    /* The window's own rectangle is all this effect touches -- no
     * transform, so nothing extends past it. */
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
        /* Multiplied, not assigned: _NET_WM_WINDOW_OPACITY (already in
         * the node) is the window's baseline, and a fade is a fraction of
         * whatever that is -- a half-transparent terminal must not become
         * opaque just because it was opening. */
        s->nodes[i].opacity *= p;
        return;
    }
}

static bool fade_finished(const CompEffect *e, double now)
{
    return now >= e->start_time + e->duration;
}

static const CompEffectOps fade_ops = {
    .name     = "fade-in",
    .update   = fade_update,
    .apply    = fade_apply,
    .finished = fade_finished,
};

static void on_event(CompWindow *w, const CompEvent *event)
{
    (void)event;

    if (w->input_only || w->wm_layer[0])
        return;
    if (!kind_enabled(w->kind))
        return;

    double duration = effect_duration("fade-in");
    if (duration <= 0.0)
        return;

    CompEffect *e = calloc(1, sizeof(*e));
    if (!e)
        return;

    e->ops = &fade_ops;
    e->window = w;
    e->start_time = comp_now_ms();
    e->duration = duration;

    effects_add(e);

    CompRect r = window_rect(w);
    output_damage_rect(&r);
}

static bool on_config_key(const char *key, const char *value)
{
    if (strcmp(key, "windows") == 0)      cfg.windows = atoi(value) != 0;
    else if (strcmp(key, "menus") == 0)   cfg.menus   = atoi(value) != 0;
    else if (strcmp(key, "docks") == 0)   cfg.docks   = atoi(value) != 0;
    else return false;
    return true;
}

const CompEffectModule effect_fade_in = {
    .name             = "fade-in",
    .default_enabled  = true,
    .default_duration = 1.0,
    .default_events   = COMP_EVENT_BIT(COMP_EVENT_OPEN) |
                        COMP_EVENT_BIT(COMP_EVENT_RESTORE) |
                        COMP_EVENT_BIT(COMP_EVENT_DESKTOP_ENTER),
    .window_event     = on_event,
    .config_key       = on_config_key,
};
