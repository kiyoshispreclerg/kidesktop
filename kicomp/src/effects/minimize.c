/*
 * Minimize / restore: the window scales between where it lives and the
 * little box the taskbar reserved for it, so it is visibly going
 * *somewhere* rather than just disappearing.
 *
 * Where it goes comes from `_NET_WM_ICON_GEOMETRY`, which a taskbar
 * publishes on each client window it lists (xispanel's tasklist does).
 * With no taskbar saying anything, the window collapses toward the
 * bottom edge of its own output, which is where a taskbar would most
 * likely have been -- a guess, but a better one than the centre of the
 * screen or the corner of the world.
 *
 * Minimizing draws a window X has already unmapped: the effect keeps it
 * alive with window_retain() (see window.h), exactly as fade-out does,
 * and releases it when the animation ends however it ends.
 *
 * kicomp.conf:
 *
 *   [effect:minimize]
 *   enabled  = 1
 *   duration = 1.0     # multiple of the global animation unit
 *   events   = minimize,restore
 *   windows  = windows
 *   fade     = 1       # fade out along the way as well as shrink
 *
 * If you also have fade-out answering to `minimize`, turn one of the two
 * off -- they will otherwise both animate the same disappearance.
 */
#include "../effect.h"
#include "../animation.h"
#include "../output.h"
#include "../window.h"
#include "../renderer.h"
#include "../transform.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    bool fade;
} MinimizeConfig;

typedef struct {
    const MinimizeConfig *cfg;
    bool going_away;      /* minimize, as opposed to restore */

    CompRect window;      /* where the window really is */
    CompRect icon;        /* where it is going (or coming from) */

    CompRect covered;     /* what the last frame drew, for damage */
} MinimizeData;

static void config_defaults(void *config)
{
    MinimizeConfig *c = config;
    c->fade = true;
}

static bool config_key(void *config, const char *key, const char *value)
{
    MinimizeConfig *c = config;

    if (strcmp(key, "fade") == 0) {
        c->fade = atoi(value) != 0;
        return true;
    }
    return false;
}

/* The taskbar's box for this window, in root coordinates. False when
 * nothing published one. */
static bool icon_geometry(CompWindow *w, CompRect *out)
{
    if (comp.atoms.net_wm_icon_geometry == XCB_NONE || w->client == XCB_NONE)
        return false;

    xcb_get_property_reply_t *r = xcb_get_property_reply(comp.conn,
        xcb_get_property(comp.conn, 0, w->client, comp.atoms.net_wm_icon_geometry,
                         XCB_ATOM_CARDINAL, 0, 4), NULL);
    if (!r)
        return false;

    bool ok = false;
    if (r->type == XCB_ATOM_CARDINAL && r->format == 32 &&
        xcb_get_property_value_length(r) >= 16) {
        uint32_t *v = xcb_get_property_value(r);
        out->x = (int)v[0];
        out->y = (int)v[1];
        out->w = (int)v[2];
        out->h = (int)v[3];
        ok = (out->w > 0 && out->h > 0);
    }
    free(r);
    return ok;
}

/* No taskbar hint: a flat box at the bottom edge of the output the window
 * is on, under its own centre. */
static CompRect fallback_target(const CompRect *win)
{
    CompRect centre = { win->x + win->w / 2, win->y + win->h / 2, 1, 1 };

    for (int i = 0; i < comp.output_count; i++) {
        CompRect hit;
        if (!rect_intersect(&centre, &comp.outputs[i].rect, &hit))
            continue;
        CompRect o = comp.outputs[i].rect;
        return (CompRect){ win->x + win->w / 2 - 40, o.y + o.h - 4, 80, 4 };
    }

    return (CompRect){ win->x + win->w / 2 - 40, win->y + win->h, 80, 4 };
}

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

/* Where the window is drawn at this point in the animation. */
static CompRect rect_for(CompEffect *e, float p)
{
    MinimizeData *d = e->data;
    return d->going_away ? lerp_rect(&d->window, &d->icon, p)
                         : lerp_rect(&d->icon, &d->window, p);
}

static void minimize_update(CompEffect *e, double now)
{
    MinimizeData *d = e->data;

    CompRect previous = d->covered;
    d->covered = rect_for(e, effect_ease(e, comp_progress(now, e->start_time, e->duration)));

    output_damage_rect(&previous);
    output_damage_rect(&d->covered);
}

static void minimize_apply(CompEffect *e, CompScene *s, CompOutput *o)
{
    MinimizeData *d = e->data;

    float p = effect_ease(e, comp_progress(comp_now_ms(), e->start_time, e->duration));
    CompRect at = rect_for(e, p);

    for (int i = 0; i < s->count; i++) {
        CompSceneNode *n = &s->nodes[i];
        if (n->win != e->window)
            continue;

        /* Map the window's real rectangle onto the animated one: scale
         * about its own origin, then translate. The logical geometry is
         * untouched -- the WM has the window where it has it, this only
         * changes where it is drawn. */
        float sx = (float)at.w / (float)(n->geometry.w > 0 ? n->geometry.w : 1);
        float sy = (float)at.h / (float)(n->geometry.h > 0 ? n->geometry.h : 1);

        comp_transform_identity(&n->transform);
        comp_transform_translate(&n->transform, (float)-n->geometry.x, (float)-n->geometry.y);
        comp_transform_scale(&n->transform, sx, sy);
        comp_transform_translate(&n->transform, (float)at.x, (float)at.y);

        if (d->cfg->fade) {
            /* Fading with the shrink, not instead of it: a window that
             * merely got small would look like it went behind something.
             * Never all the way to nothing before the end, or the last
             * third of the motion is invisible. */
            float visible = d->going_away ? (1.0f - p) : p;
            n->opacity *= 0.25f + 0.75f * visible;
        }

        if (!rect_intersect(&at, &o->rect, &n->visible_rect))
            n->visible_rect = (CompRect){ 0, 0, 0, 0 };
        return;
    }
}

static bool minimize_finished(const CompEffect *e, double now)
{
    return now >= e->start_time + e->duration;
}

static void minimize_destroy(CompEffect *e)
{
    MinimizeData *d = e->data;
    if (d && d->going_away)
        window_release(e->window);
    free(e->data);
    e->data = NULL;
}

static const CompEffectOps minimize_ops = {
    .name     = "minimize",
    .update   = minimize_update,
    .apply    = minimize_apply,
    .finished = minimize_finished,
    .destroy  = minimize_destroy,
};

static void on_event(CompWindow *w, const CompEvent *event,
                     const CompEffectInstance *self)
{
    bool going_away = (event->kind == COMP_EVENT_MINIMIZE);

    /* Minimizing draws a window X has already unmapped, so the contents
     * bound before it went away are all there will ever be: without them
     * there is nothing to shrink. Restoring is the opposite -- the window
     * was just mapped and its pixmap is deliberately unbound, to be named
     * fresh on the next paint -- so asking for contents there would
     * refuse every restore there is. */
    if (going_away && !renderer_window_has_content(w))
        return;

    double duration = effect_instance_duration(self);
    if (duration <= 0.0)
        return;

    CompEffect *e = calloc(1, sizeof(*e));
    MinimizeData *d = calloc(1, sizeof(*d));
    if (!e || !d) {
        free(e);
        free(d);
        return;
    }

    d->cfg = self->config;
    d->going_away = going_away;
    d->window = window_rect(w);
    if (!icon_geometry(w, &d->icon))
        d->icon = fallback_target(&d->window);
    d->covered = d->window;

    e->ops = &minimize_ops;
    e->instance = self;
    e->window = w;
    e->start_time = comp_now_ms();
    e->duration = duration;
    e->data = d;

    if (going_away)
        window_retain(w);

    effects_add(e);
    output_damage_rect(&d->window);
    output_damage_rect(&d->icon);
}

const CompEffectModule effect_minimize = {
    .name             = "minimize",
    .default_enabled  = true,
    .default_duration = 1.0,
    .default_easing   = COMP_EASE_OUT,
    .default_events   = COMP_EVENT_BIT(COMP_EVENT_MINIMIZE) |
                        COMP_EVENT_BIT(COMP_EVENT_RESTORE),
    .default_windows  = COMP_WINDOW_BIT(COMP_WINDOW_UNKNOWN) |
                        COMP_WINDOW_BIT(COMP_WINDOW_NORMAL) |
                        COMP_WINDOW_BIT(COMP_WINDOW_DIALOG) |
                        COMP_WINDOW_BIT(COMP_WINDOW_UTILITY) |
                        COMP_WINDOW_BIT(COMP_WINDOW_TOOLBAR),
    .config_size      = sizeof(MinimizeConfig),
    .config_defaults  = config_defaults,
    .config_key       = config_key,
    .window_event     = on_event,
};
