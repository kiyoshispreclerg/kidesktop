/*
 * visual-bell: the window that rang jumps, instead of the machine
 * beeping.
 *
 * A terminal with an empty input line answers a backspace by ringing the
 * bell. Most desktops turn that into a sound nobody wants or into
 * nothing at all; a compositor can turn it into the smallest possible
 * gesture from the window itself -- which is also the only place a bell
 * has ever really pointed at.
 *
 * Which window rang is not in core X: the core protocol's Bell request
 * has no sender and no target. XKB's BellNotify has both, which is why
 * this is the one effect that needs an extension of its own (main.c).
 * When the ringing client named no window -- a plain XBell(), which is
 * what a terminal does -- the focused window is the one that rang,
 * because that is where the keystroke went.
 *
 * Deliberately small. The point is to be noticed without being watched:
 * one short pulse, a few percent of scale, and out. Something bigger
 * turns every stray backspace into an event.
 *
 * kicomp.conf:
 *
 *   [effect:visual-bell]
 *   enabled = 1
 *   duration = 0.9              # multiples of animation_duration
 *   events  = bell
 *   amount  = 0.035             # peak scale: 0.035 is 3.5% bigger
 *   pulses  = 1                 # 2 makes it a double-take
 *   origin  = window            # window | pointer, like scale-in's
 */
#include "../effect.h"
#include "../animation.h"
#include "../output.h"
#include "../window.h"
#include "../transform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef struct {
    CompRect window;
    float cx, cy;      /* what it grows about, in root coordinates */
} BellData;

typedef struct {
    float amount;
    int pulses;
    bool from_pointer;
} BellConfig;

static const CompEffectOps bell_ops;

/* One window at a time: a bell that arrives while the last one is still
 * running restarts it rather than stacking a second scale on top of the
 * first, which would double the size rather than repeat the gesture. */
static CompEffect *active;
static CompWindow *active_window;

static void bell_update(CompEffect *e, double now)
{
    BellData *d = e->data;
    (void)now;

    /* Grown by the peak, because that is how far outside its own
     * rectangle the window is briefly drawn. */
    const BellConfig *cfg = e->instance->config;
    int grow = (int)((float)(d->window.w > d->window.h ? d->window.w : d->window.h)
                     * cfg->amount) + 2;

    CompRect r = d->window;
    r.x -= grow;
    r.y -= grow;
    r.w += grow * 2;
    r.h += grow * 2;
    output_damage_rect(&r);
}

static void bell_apply(CompEffect *e, CompScene *s, CompOutput *o)
{
    BellData *d = e->data;
    const BellConfig *cfg = e->instance->config;

    float p = comp_progress(comp_now_ms(), e->start_time, e->duration);

    /* Up and back down, `pulses` times: sin over the whole run, so it
     * starts and ends at exactly 1.0 whatever the easing would have
     * done to it -- a bell that ends a fraction of a percent large
     * leaves the window subtly the wrong size until something else
     * redraws it. */
    int pulses = cfg->pulses > 0 ? cfg->pulses : 1;
    float wave = sinf(p * (float)pulses * 3.14159265f);
    float scale = 1.0f + cfg->amount * wave;

    for (int i = 0; i < s->count; i++) {
        CompSceneNode *n = &s->nodes[i];
        if (n->win != e->window)
            continue;

        comp_transform_identity(&n->transform);
        comp_transform_translate(&n->transform, -d->cx, -d->cy);
        comp_transform_scale(&n->transform, scale, scale);
        comp_transform_translate(&n->transform, d->cx, d->cy);

        CompRect at = n->geometry;
        float grow_w = (float)at.w * (scale - 1.0f);
        float grow_h = (float)at.h * (scale - 1.0f);
        at.x -= (int)(grow_w / 2.0f + 1.0f);
        at.y -= (int)(grow_h / 2.0f + 1.0f);
        at.w += (int)(grow_w + 2.0f);
        at.h += (int)(grow_h + 2.0f);

        CompRect vis;
        if (rect_intersect(&at, &o->rect, &vis))
            n->visible_rect = vis;
        else
            n->visible_rect = (CompRect){ 0, 0, 0, 0 };
        return;
    }
}

static bool bell_finished(const CompEffect *e, double now)
{
    return now >= e->start_time + e->duration;
}

static void bell_destroy(CompEffect *e)
{
    if (e == active) {
        active = NULL;
        active_window = NULL;
    }
    free(e->data);
    e->data = NULL;
}

static const CompEffectOps bell_ops = {
    .name     = "visual-bell",
    .update   = bell_update,
    .apply    = bell_apply,
    .finished = bell_finished,
    .destroy  = bell_destroy,
};

static void on_event(CompWindow *w, const CompEvent *event,
                     const CompEffectInstance *self)
{
    const BellConfig *cfg = self->config;
    (void)event;

    if (!w->mapped || w->input_only)
        return;

    /* Ringing again while it is still ringing: restart, so a key held
     * down against an empty line pulses steadily instead of growing. */
    if (active && active_window == w) {
        active->start_time = comp_now_ms();
        return;
    }

    CompEffect *e = calloc(1, sizeof(*e));
    BellData *d = calloc(1, sizeof(*d));
    if (!e || !d) {
        free(e);
        free(d);
        return;
    }

    d->window = window_rect(w);
    d->cx = (float)d->window.x + (float)d->window.w / 2.0f;
    d->cy = (float)d->window.y + (float)d->window.h / 2.0f;

    if (cfg->from_pointer) {
        /* About the pointer instead: the window leans towards the hand
         * that caused it, which reads as the window answering you. */
        xcb_query_pointer_reply_t *r = xcb_query_pointer_reply(comp.conn,
            xcb_query_pointer(comp.conn, comp.root), NULL);
        if (r) {
            d->cx = (float)r->root_x;
            d->cy = (float)r->root_y;
            free(r);
        }
    }

    e->ops = &bell_ops;
    e->instance = self;
    e->window = w;
    e->start_time = comp_now_ms();
    e->duration = effect_instance_duration(self);
    e->data = d;

    active = e;
    active_window = w;
    effects_add(e);
}

static void bell_defaults(void *config)
{
    BellConfig *c = config;
    c->amount = 0.035f;
    c->pulses = 1;
    c->from_pointer = false;
}

static bool bell_config_key(void *config, const char *key, const char *value)
{
    BellConfig *c = config;

    if (!strcmp(key, "amount")) {
        c->amount = (float)atof(value);
        if (c->amount < 0.0f) c->amount = 0.0f;
        if (c->amount > 0.5f) c->amount = 0.5f;
        return true;
    }
    if (!strcmp(key, "pulses")) {
        c->pulses = atoi(value);
        if (c->pulses < 1) c->pulses = 1;
        if (c->pulses > 6) c->pulses = 6;
        return true;
    }
    if (!strcmp(key, "origin")) {
        if (!strcmp(value, "pointer"))
            c->from_pointer = true;
        else if (!strcmp(value, "window"))
            c->from_pointer = false;
        else
            fprintf(stderr, "kicomp: config: unknown origin '%s'\n", value);
        return true;
    }
    return false;
}

const CompEffectModule effect_visual_bell = {
    .name             = "visual-bell",
    .default_enabled  = true,
    /* Short: this is a flinch, not a transition. */
    .default_duration = 0.9,
    .default_easing   = COMP_EASE_LINEAR,
    .default_events   = COMP_EVENT_BIT(COMP_EVENT_BELL),
    /* Every kind of window, including the ones effects usually leave
     * alone: whatever rang, rang. */
    .default_windows  = 0xffffffffu,

    .window_event     = on_event,
    .config_size      = sizeof(BellConfig),
    .config_defaults  = bell_defaults,
    .config_key       = bell_config_key,
};
