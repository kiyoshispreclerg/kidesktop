/*
 * zoom: one screen magnified, under Meta and the wheel.
 *
 * Not a mode (input.h): nothing is grabbed while it runs, no key is held,
 * and the desktop underneath keeps working exactly as it did -- windows
 * take focus, menus open, text is typed. The screen is simply being
 * looked at through a lens, and the lens stays until it is wound back
 * out. That is the difference between this and every other effect here:
 * it has no end of its own to animate towards.
 *
 * *One* screen. The zoom is per output and stays inside it: the lens
 * never shows anything beyond that monitor's own rectangle, and the other
 * monitors are not touched at all. A window that straddles two of them is
 * magnified on this one and left alone on the other, which is what
 * "contained in one screen" has to mean when the thing being magnified is
 * a screen rather than a window.
 *
 * What is animated is the *view rectangle* -- the part of the output that
 * fills it -- rather than a magnification factor and a centre. It is the
 * same thing said differently, and it is the form in which "stay inside
 * the screen" is one clamp rather than three: the rectangle is kept
 * within the output's own, so the edge of the desktop can never be pulled
 * into the middle of the screen.
 *
 * kicomp.conf:
 *
 *   [effect:zoom]
 *   enabled  = 1
 *   zoom_in  = Meta+WheelUp     # any key or button (input.h's grammar)
 *   zoom_out = Meta+WheelDown
 *   step     = 0.25             # each notch magnifies by this much
 *   max      = 8.0              # how far in it will go
 *   duration = 0.6              # multiples of animation_duration
 */
#include "../effect.h"
#include "../animation.h"
#include "../output.h"
#include "../input.h"
#include "../window.h"
#include "../transform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef struct {
    int output_id;

    /* The part of the output that fills the output. Equal to the output's
     * own rectangle at rest, and smaller the further in it goes. */
    CompRect from, to, current;

    double leg_start;
    double leg_ms;
    bool closing;        /* on its way back to no zoom at all */
} ZoomData;

typedef struct {
    char in_key[64];
    char out_key[64];
    float step;
    float max;
} ZoomConfig;

static const CompEffectOps zoom_ops;

/* One at a time, and one per session rather than one per output: the
 * wheel points at whichever screen the pointer is on, and zooming a
 * second screen while the first is still magnified would leave the first
 * one magnified with nothing driving it. */
static CompEffect *active;

static CompOutput *output_by_id(int id)
{
    for (int i = 0; i < comp.output_count; i++)
        if (comp.outputs[i].id == id)
            return &comp.outputs[i];
    return NULL;
}

static CompOutput *output_at(int x, int y)
{
    for (int i = 0; i < comp.output_count; i++) {
        CompOutput *o = &comp.outputs[i];
        if (x >= o->rect.x && x < o->rect.x + o->rect.w &&
            y >= o->rect.y && y < o->rect.y + o->rect.h)
            return o;
    }
    return NULL;
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

/* The view rectangle, kept inside the screen it belongs to. A lens that
 * can wander past the edge shows the desktop ending in mid-screen, which
 * reads as a bug however deliberate it was. */
static CompRect clamp_view(const CompRect *view, const CompRect *screen)
{
    CompRect v = *view;

    if (v.w > screen->w) v.w = screen->w;
    if (v.h > screen->h) v.h = screen->h;
    if (v.w < 1) v.w = 1;
    if (v.h < 1) v.h = 1;

    if (v.x < screen->x)
        v.x = screen->x;
    if (v.x + v.w > screen->x + screen->w)
        v.x = screen->x + screen->w - v.w;
    if (v.y < screen->y)
        v.y = screen->y;
    if (v.y + v.h > screen->y + screen->h)
        v.y = screen->y + screen->h - v.h;

    return v;
}

/* The lens itself: what takes the view rectangle to the whole screen. */
static void lens_matrix(const CompRect *view, const CompRect *screen,
                        CompTransform *out)
{
    float sx = (float)screen->w / (float)(view->w > 0 ? view->w : 1);
    float sy = (float)screen->h / (float)(view->h > 0 ? view->h : 1);

    comp_transform_identity(out);
    comp_transform_translate(out, (float)-view->x, (float)-view->y);
    comp_transform_scale(out, sx, sy);
    comp_transform_translate(out, (float)screen->x, (float)screen->y);
}

static float leg_progress(const ZoomData *d, const CompEffect *e, double now)
{
    if (d->leg_ms <= 0.0)
        return 1.0f;
    return effect_ease(e, comp_progress(now, d->leg_start, d->leg_ms));
}

static void zoom_update(CompEffect *e, double now)
{
    ZoomData *d = e->data;

    float p = leg_progress(d, e, now);
    CompRect was = d->current;
    d->current = lerp_rect(&d->from, &d->to, p);

    /* Only while it is actually moving: a screen held at a fixed
     * magnification is a still picture, and repainting it sixty times a
     * second for nothing is the cost this compositor is careful about
     * everywhere else. */
    if (p < 1.0f || was.x != d->current.x || was.y != d->current.y ||
        was.w != d->current.w || was.h != d->current.h) {
        CompOutput *o = output_by_id(d->output_id);
        if (o)
            output_damage_rect(&o->rect);
    }
}

static void zoom_apply(CompEffect *e, CompScene *s, CompOutput *o)
{
    ZoomData *d = e->data;

    if (o->id != d->output_id)
        return;

    CompTransform lens;
    lens_matrix(&d->current, &o->rect, &lens);

    /* The wallpaper goes through the same lens, and it is drawn before
     * the scene exists -- hence the output carrying it (comp.h). */
    o->view = lens;

    for (int i = 0; i < s->count; i++) {
        CompSceneNode *n = &s->nodes[i];

        /* Composed rather than assigned: another effect may already be
         * moving this window, and being magnified does not stop it. */
        comp_transform_multiply(&n->transform, &lens, &n->transform);

        /* What of it is on this screen, after the lens. Everything is
         * clipped to the output because the zoom is contained in it --
         * a window magnified past the edge does not appear on the
         * neighbour. */
        CompRect at = comp_transform_rect(&n->transform, &n->geometry);
        CompRect vis;
        if (rect_intersect(&at, &o->rect, &vis))
            n->visible_rect = vis;
        else
            n->visible_rect = (CompRect){ 0, 0, 0, 0 };
    }
}

static bool zoom_finished(const CompEffect *e, double now)
{
    const ZoomData *d = e->data;
    if (!d->closing)
        return false;
    return now >= d->leg_start + d->leg_ms;
}

static void zoom_destroy(CompEffect *e)
{
    ZoomData *d = e->data;

    /* The lens goes with it: scene.c resets it every frame, but the frame
     * that retires this effect must not be drawn through half of one. */
    CompOutput *o = d ? output_by_id(d->output_id) : NULL;
    if (o) {
        comp_transform_identity(&o->view);
        output_damage_rect(&o->rect);
    }

    if (e == active)
        active = NULL;
    free(e->data);
    e->data = NULL;
}

static const CompEffectOps zoom_ops = {
    .name     = "zoom",
    .update   = zoom_update,
    .apply    = zoom_apply,
    .finished = zoom_finished,
    .destroy  = zoom_destroy,
};

/* ------------------------------------------------------------------ */
/* the wheel                                                           */
/* ------------------------------------------------------------------ */

static void step_zoom(const CompEffectInstance *self, bool in)
{
    const ZoomConfig *cfg = self->config;

    int px = 0, py = 0;
    if (!input_pointer_position(&px, &py))
        return;

    CompOutput *o = output_at(px, py);
    if (!o)
        return;

    /* Where it is now: the current view if this screen is already
     * magnified, the whole screen if it is not. */
    ZoomData *d = NULL;
    if (active) {
        ZoomData *ad = active->data;
        if (ad->output_id != o->id)
            return;          /* another screen is zoomed; leave it alone */
        d = ad;
    }

    CompRect view = d ? d->current : o->rect;

    float level = (float)o->rect.w / (float)(view.w > 0 ? view.w : 1);
    float want = in ? level * (1.0f + cfg->step) : level / (1.0f + cfg->step);

    if (want > cfg->max)
        want = cfg->max;
    if (want < 1.0f)
        want = 1.0f;

    /* The new view, the size the level asks for, kept around the pointer:
     * the point under the cursor is the one that should not move, which
     * is what makes wheel zoom feel like it is pointing at something. */
    CompRect target;
    target.w = (int)((float)o->rect.w / want + 0.5f);
    target.h = (int)((float)o->rect.h / want + 0.5f);

    float fx = (float)(px - view.x) / (float)(view.w > 0 ? view.w : 1);
    float fy = (float)(py - view.y) / (float)(view.h > 0 ? view.h : 1);
    target.x = px - (int)(fx * (float)target.w + 0.5f);
    target.y = py - (int)(fy * (float)target.h + 0.5f);
    target = clamp_view(&target, &o->rect);

    if (!d) {
        if (!in)
            return;          /* not zoomed and asked to zoom out: nothing */

        CompEffect *e = calloc(1, sizeof(*e));
        d = calloc(1, sizeof(*d));
        if (!e || !d) {
            free(e);
            free(d);
            return;
        }

        d->output_id = o->id;
        d->from = d->current = o->rect;

        e->ops = &zoom_ops;
        e->instance = self;
        e->window = NULL;    /* a screen, not a window */
        e->start_time = comp_now_ms();
        e->duration = 0.0;   /* it ends when it is wound back out */
        e->data = d;

        active = e;
        effects_add(e);
    }

    d->from = d->current;
    d->to = target;
    d->leg_start = comp_now_ms();
    d->leg_ms = effect_instance_duration(self);
    /* Back to no magnification at all: this leg is the last one. */
    d->closing = (want <= 1.001f);

    output_damage_rect(&o->rect);
}

static void on_in(void *data)  { step_zoom(data, true); }
static void on_out(void *data) { step_zoom(data, false); }

static void zoom_init(const CompEffectInstance *self)
{
    const ZoomConfig *cfg = self->config;

    /* Each of the two takes a list, like every other binding here. */
    char buf[sizeof(cfg->in_key)];
    for (int which = 0; which < 2; which++) {
        snprintf(buf, sizeof(buf), "%s", which ? cfg->out_key : cfg->in_key);

        char *save = NULL;
        for (char *tok = strtok_r(buf, ",", &save); tok;
             tok = strtok_r(NULL, ",", &save)) {
            while (*tok == ' ' || *tok == '\t')
                tok++;
            char *end = tok + strlen(tok);
            while (end > tok && (end[-1] == ' ' || end[-1] == '\t'))
                *--end = '\0';
            if (*tok)
                input_bind_hotkey(tok, which ? on_out : on_in, (void *)self);
        }
    }
}

static void zoom_defaults(void *config)
{
    ZoomConfig *c = config;
    snprintf(c->in_key, sizeof(c->in_key), "%s", "Meta+WheelUp");
    snprintf(c->out_key, sizeof(c->out_key), "%s", "Meta+WheelDown");
    c->step = 0.25f;
    c->max = 8.0f;
}

static bool zoom_config_key(void *config, const char *key, const char *value)
{
    ZoomConfig *c = config;

    if (!strcmp(key, "zoom_in")) {
        snprintf(c->in_key, sizeof(c->in_key), "%s", value);
        return true;
    }
    if (!strcmp(key, "zoom_out")) {
        snprintf(c->out_key, sizeof(c->out_key), "%s", value);
        return true;
    }
    if (!strcmp(key, "step")) {
        c->step = (float)atof(value);
        if (c->step < 0.02f) c->step = 0.02f;
        if (c->step > 4.0f) c->step = 4.0f;
        return true;
    }
    if (!strcmp(key, "max")) {
        c->max = (float)atof(value);
        if (c->max < 1.1f) c->max = 1.1f;
        if (c->max > 64.0f) c->max = 64.0f;
        return true;
    }
    return false;
}

const CompEffectModule effect_zoom = {
    .name             = "zoom",
    .default_enabled  = true,
    /* Short: a wheel notch should land almost at once, or the next notch
     * arrives before this one finished and the screen swims. */
    .default_duration = 0.6,
    .default_easing   = COMP_EASE_OUT,
    .default_events   = 0,        /* its own bindings, like the modes */
    .default_windows  = 0xffffffffu,

    .init             = zoom_init,
    .config_size      = sizeof(ZoomConfig),
    .config_defaults  = zoom_defaults,
    .config_key       = zoom_config_key,
};
