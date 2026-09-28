/*
 * Slide in: a window that has just appeared arrives from a point a little
 * off from its real position, sliding the rest of the way in, instead of
 * simply being there.
 *
 * The destination is always the window's real geometry; what is
 * configurable is how far off the start is and along which axis:
 *
 *   offset     how far displaced at the start, as a fraction of the
 *              window's own size along the slide axis (default 0.3)
 *   direction  auto (default): whichever axis points at the edge of the
 *              output nearest the window's centre -- horizontal if that
 *              is the left or right edge, vertical if it is top or
 *              bottom, and the sign follows that edge, so the window
 *              looks like it is arriving from the side of the screen it
 *              is closest to. horizontal / vertical pin the axis and
 *              still pick the sign from the nearest edge on that axis.
 *
 * This is aimed at popups more than ordinary windows -- a launcher page,
 * a volume flyout, a tooltip -- which is why the default window list is
 * `popups` rather than everything: a normal window sliding in from 30%
 * off is a bigger visual jump than one that just fades or grows into
 * place.
 *
 * kicomp.conf:
 *
 *   [effect:slide-in]
 *   enabled   = 1
 *   duration  = 1.0     # multiple of the global animation unit
 *   events    = open,restore,desktop-enter
 *   windows   = popups
 *   offset    = 0.3      # fraction of the window's size on the slide axis
 *   direction = auto     # auto | horizontal | vertical
 *
 * Several instances may exist, each with its own numbers -- see the
 * README: [effect:slide-in:launcher] with a different offset is a
 * section, not a second effect.
 */
#include "../effect.h"
#include "../animation.h"
#include "../output.h"
#include "../window.h"
#include "../transform.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    SLIDE_AUTO,
    SLIDE_HORIZONTAL,
    SLIDE_VERTICAL,
} SlideDirection;

/* One of these per instance (effect.h's CompEffectModule::config_size),
 * which is what lets two [effect:slide-in:*] sections animate the same
 * window with different numbers. */
typedef struct {
    float offset;          /* fraction of the window's own size, on the slide axis */
    SlideDirection direction;
} SlideConfig;

typedef struct {
    const SlideConfig *cfg;
    float dx, dy;       /* the full displacement at p=0; root pixels, frozen at start */
    CompRect covered;   /* what the last frame drew, for damage */
} SlideData;

static void config_defaults(void *config)
{
    SlideConfig *c = config;
    c->offset = 0.3f;
    c->direction = SLIDE_AUTO;
}

static bool config_key(void *config, const char *key, const char *value)
{
    SlideConfig *c = config;

    if (strcmp(key, "offset") == 0) {
        float f = (float)atof(value);
        /* Above 1 is a real answer -- a start point further than the
         * window's own size away is just a longer glide. The floor is
         * not 0, which would be no effect at all rather than a small one. */
        if (f < 0.02f) f = 0.02f;
        if (f > 3.0f) f = 3.0f;
        c->offset = f;
    } else if (strcmp(key, "direction") == 0) {
        if (strcmp(value, "horizontal") == 0)   c->direction = SLIDE_HORIZONTAL;
        else if (strcmp(value, "vertical") == 0) c->direction = SLIDE_VERTICAL;
        else if (strcmp(value, "auto") == 0)     c->direction = SLIDE_AUTO;
        else return false;
    } else {
        return false;
    }
    return true;
}

/* The full displacement vector for this window, computed once when the
 * effect starts: which edge of its output is nearest its centre decides
 * both the axis (for `auto`) and the sign (always), so a popup pinned to
 * the right of the screen slides in from further right, never from the
 * left. A window that isn't over any known output falls back to sliding
 * in from the left/top, which is as good a guess as any for something
 * off-screen. */
static void offset_for(CompWindow *w, const SlideConfig *cfg, float *dx, float *dy)
{
    CompRect r = window_rect(w);
    float cx = r.x + r.w / 2.0f;
    float cy = r.y + r.h / 2.0f;

    CompRect out = { 0, 0, 0, 0 };
    bool have_out = false;
    for (int i = 0; i < comp.output_count; i++) {
        CompRect centre = { r.x + r.w / 2, r.y + r.h / 2, 1, 1 };
        CompRect hit;
        if (rect_intersect(&centre, &comp.outputs[i].rect, &hit)) {
            out = comp.outputs[i].rect;
            have_out = true;
            break;
        }
    }

    /* Distances from the window's centre to each edge of its output;
     * whichever is smallest names both the nearest edge and, for `auto`,
     * the axis to slide along. */
    float d_left = have_out ? cx - out.x : 1.0f;
    float d_right = have_out ? (out.x + out.w) - cx : 0.0f;
    float d_top = have_out ? cy - out.y : 1.0f;
    float d_bottom = have_out ? (out.y + out.h) - cy : 0.0f;

    bool horizontal;
    if (cfg->direction == SLIDE_HORIZONTAL)
        horizontal = true;
    else if (cfg->direction == SLIDE_VERTICAL)
        horizontal = false;
    else
        horizontal = fminf(d_left, d_right) <= fminf(d_top, d_bottom);

    *dx = 0.0f;
    *dy = 0.0f;
    if (horizontal)
        *dx = (d_left <= d_right ? -1.0f : 1.0f) * cfg->offset * r.w;
    else
        *dy = (d_top <= d_bottom ? -1.0f : 1.0f) * cfg->offset * r.h;
}

static void slide_transform(CompEffect *e, float p, CompTransform *t)
{
    SlideData *d = e->data;
    float k = 1.0f - p;

    comp_transform_identity(t);
    comp_transform_translate(t, d->dx * k, d->dy * k);
}

static void slide_update(CompEffect *e, double now)
{
    SlideData *d = e->data;

    CompRect previous = d->covered;

    CompTransform t;
    slide_transform(e, effect_ease(e, comp_progress(now, e->start_time, e->duration)), &t);
    /* The window's rect as it is now, not as it was when the effect
     * started: a window that moved mid-animation is drawn at its new
     * place, and damaging the old one leaves the new one unrepainted. */
    CompRect geometry = window_rect(e->window);
    comp_transform_bbox(&t, &geometry, &d->covered);

    output_damage_rect(&previous);
    output_damage_rect(&d->covered);
}

static void slide_apply(CompEffect *e, CompScene *s, CompOutput *o)
{
    float p = effect_ease(e, comp_progress(comp_now_ms(), e->start_time, e->duration));

    for (int i = 0; i < s->count; i++) {
        CompSceneNode *n = &s->nodes[i];
        if (n->win != e->window)
            continue;

        slide_transform(e, p, &n->transform);

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

static bool slide_finished(const CompEffect *e, double now)
{
    return now >= e->start_time + e->duration;
}

static void slide_destroy(CompEffect *e)
{
    free(e->data);
    e->data = NULL;
}

static const CompEffectOps slide_ops = {
    .name     = "slide-in",
    .update   = slide_update,
    .apply    = slide_apply,
    .finished = slide_finished,
    .destroy  = slide_destroy,
};

static void on_event(CompWindow *w, const CompEvent *event,
                     const CompEffectInstance *self)
{
    (void)event;

    double duration = effect_instance_duration(self);
    if (duration <= 0.0)
        return;

    CompEffect *e = calloc(1, sizeof(*e));
    SlideData *d = calloc(1, sizeof(*d));
    if (!e || !d) {
        free(e);
        free(d);
        return;
    }

    d->cfg = self->config;
    offset_for(w, d->cfg, &d->dx, &d->dy);
    d->covered = window_rect(w);

    e->ops = &slide_ops;
    e->instance = self;
    e->window = w;
    e->start_time = comp_now_ms();
    e->duration = duration;
    e->data = d;

    effects_add(e);
    output_damage_rect(&d->covered);
}

const CompEffectModule effect_slide_in = {
    .name             = "slide-in",
    .default_enabled  = false,
    .default_duration = 1.0,
    /* Weight at the destination: it arrives gently, which is what reads
     * as settling into place. kicomp.conf's easing= overrides it. */
    .default_easing   = COMP_EASE_OUT,
    .default_events   = COMP_EVENT_BIT(COMP_EVENT_OPEN) |
                        COMP_EVENT_BIT(COMP_EVENT_RESTORE) |
                        COMP_EVENT_BIT(COMP_EVENT_DESKTOP_ENTER),
    /* The `popups` group (effect.c): menus, tooltips, notifications, DND
     * -- the xisserve pages and xispanel tooltips/toasts this was built
     * for -- and not ordinary windows, for which a 30% jump reads as too
     * much motion. */
    .default_windows  = COMP_WINDOW_BIT(COMP_WINDOW_MENU) |
                        COMP_WINDOW_BIT(COMP_WINDOW_DROPDOWN_MENU) |
                        COMP_WINDOW_BIT(COMP_WINDOW_POPUP_MENU) |
                        COMP_WINDOW_BIT(COMP_WINDOW_COMBO) |
                        COMP_WINDOW_BIT(COMP_WINDOW_TOOLTIP) |
                        COMP_WINDOW_BIT(COMP_WINDOW_NOTIFICATION) |
                        COMP_WINDOW_BIT(COMP_WINDOW_DND),
    .config_size      = sizeof(SlideConfig),
    .config_defaults  = config_defaults,
    .config_key       = config_key,
    .window_event     = on_event,
};
