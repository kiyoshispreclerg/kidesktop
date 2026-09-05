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
 *   follow   = pointer          # pointer | proportional | centred | off
 *                               # how the lens tracks the pointer; the
 *                               # default keeps what is under the cursor
 *                               # under the cursor, which is what makes
 *                               # windows still clickable while zoomed
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

    /* Where the pointer was when the lens was last moved for it, and
     * when it was last asked. Asked rather than listened for: motion
     * events go to whatever window the pointer is over, and a compositor
     * that selected them on the root would hear nothing at all while the
     * pointer is over an application. One round trip per poll, and only
     * while a lens is up. */
    int last_px, last_py;
    double last_poll;
} ZoomData;

typedef enum {
    /* The lens is anchored *on the pointer*: whatever is under the
     * cursor stays under the cursor, magnified in place.
     *
     * This is the default, and the reason is not aesthetics. X does not
     * magnify input -- the fork's own X-INPUT-SCALE confines the pointer
     * to a rectangle and deliberately does no coordinate remapping -- so
     * a click lands where the pointer really is, not where the picture
     * puts it. Under any other anchoring, the window you can see under
     * the cursor is not the window you would hit: the desktop becomes a
     * picture of itself. Anchored here, the two agree everywhere except
     * where the view runs into the edge of the screen and has to stop. */
    FOLLOW_POINTER,
    FOLLOW_PROPORTIONAL,  /* where the pointer is across the screen is
                           * where the lens is across the desktop */
    FOLLOW_CENTRED,       /* the pointer's target is kept in the middle */
    FOLLOW_OFF,           /* the lens stays where the wheel left it */
} ZoomFollow;

typedef struct {
    char in_key[64];
    char out_key[64];
    float step;
    float max;
    ZoomFollow follow;
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

/* How often the pointer is asked about while zoomed. Fast enough that
 * panning does not feel sampled, slow enough that it is not a round trip
 * per pass of a loop that runs whenever anything at all happens. */
#define FOLLOW_POLL_MS 12.0

/* The lens follows the pointer. Proportional by default: where the
 * pointer sits across the screen is where the lens sits across the
 * desktop, so pushing into a corner shows that corner and the whole
 * desktop is reachable without the view ever leaving the screen. The
 * alternative people expect is centred, which keeps the pointer in the
 * middle and moves the world under it. */
static void follow_pointer(CompEffect *e, ZoomData *d, double now)
{
    const ZoomConfig *cfg = e->instance->config;

    if (cfg->follow == FOLLOW_OFF || d->closing)
        return;
    if (now - d->last_poll < FOLLOW_POLL_MS)
        return;
    d->last_poll = now;

    int px = 0, py = 0;
    if (!input_pointer_position(&px, &py))
        return;
    if (px == d->last_px && py == d->last_py)
        return;
    d->last_px = px;
    d->last_py = py;

    CompOutput *o = output_by_id(d->output_id);
    if (!o)
        return;
    /* The pointer left this screen: the lens belongs to this one, so it
     * simply stays where it was. */
    if (px < o->rect.x || px >= o->rect.x + o->rect.w ||
        py < o->rect.y || py >= o->rect.y + o->rect.h)
        return;

    CompRect view = d->to;
    if (view.w >= o->rect.w && view.h >= o->rect.h)
        return;                       /* not magnified: nothing to pan */

    if (cfg->follow == FOLLOW_POINTER) {
        /* Solve lens(p) = p: the pointer's own position is the one the
         * lens leaves alone, so the pixels under it are the pixels that
         * would be clicked. */
        float k = (float)o->rect.w / (float)(view.w > 0 ? view.w : 1);
        view.x = px - (int)(((float)(px - o->rect.x)) / k + 0.5f);
        view.y = py - (int)(((float)(py - o->rect.y)) / k + 0.5f);
    } else if (cfg->follow == FOLLOW_CENTRED) {
        view.x = px - view.w / 2;
        view.y = py - view.h / 2;
    } else {
        float fx = (float)(px - o->rect.x) / (float)(o->rect.w > 1 ? o->rect.w - 1 : 1);
        float fy = (float)(py - o->rect.y) / (float)(o->rect.h > 1 ? o->rect.h - 1 : 1);
        view.x = o->rect.x + (int)(fx * (float)(o->rect.w - view.w) + 0.5f);
        view.y = o->rect.y + (int)(fy * (float)(o->rect.h - view.h) + 0.5f);
    }

    view = clamp_view(&view, &o->rect);
    if (view.x == d->to.x && view.y == d->to.y)
        return;

    /* A short leg rather than a jump: the same easing the wheel gets,
     * but brief, so panning is smooth without feeling like it is being
     * dragged along behind the hand. */
    d->from = d->current;
    d->to = view;
    d->leg_start = now;
    d->leg_ms = effect_instance_duration(e->instance) * 0.35;
    if (d->leg_ms < 1.0)
        d->leg_ms = 1.0;
}

static void zoom_update(CompEffect *e, double now)
{
    ZoomData *d = e->data;

    follow_pointer(e, d, now);

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

    /* And that is the whole of it: the nodes are not touched.
     *
     * The lens belongs to the *output*, so both backends apply it where
     * they already turn root coordinates into pixels -- which means a
     * magnified window keeps its shadow, its rounded corners and its
     * dense layers, because every one of those is drawn through the same
     * mapping. Composing the lens into each node's transform instead
     * looked equivalent and was not: a node carrying a scale is a node
     * whose silhouette neither backend can clip to and whose shadow
     * XRender skips, so a zoomed desktop came out with square corners
     * and no shadows at all. */
    (void)s;
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
    c->follow = FOLLOW_POINTER;
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
    if (!strcmp(key, "follow")) {
        if (!strcmp(value, "pointer"))
            c->follow = FOLLOW_POINTER;
        else if (!strcmp(value, "proportional"))
            c->follow = FOLLOW_PROPORTIONAL;
        else if (!strcmp(value, "centred") || !strcmp(value, "centered"))
            c->follow = FOLLOW_CENTRED;
        else if (!strcmp(value, "off") || !strcmp(value, "0"))
            c->follow = FOLLOW_OFF;
        else
            fprintf(stderr, "kicomp: config: unknown follow '%s'\n", value);
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
