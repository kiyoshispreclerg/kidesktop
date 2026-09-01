/*
 * Desktop wall (section 24.2): switching desktops slides the whole set of
 * windows off one side of the output while the incoming ones slide in from
 * the other, as though the desktops were panels of one long wall and the
 * output were a window onto it.
 *
 * This is the first effect that moves more than one window at a time, and
 * it does so without the interface needing anything new: the WM unmaps
 * every window of the desktop being left and maps every window of the one
 * being entered, so what arrives here is one desktop-leave per window on
 * the way out and one desktop-enter per window on the way in (window.c
 * classifies them by the desktop property having just changed). Each gets
 * its own effect, they all run with the same duration and the same easing,
 * and the result reads as one motion because it is one motion, described a
 * window at a time.
 *
 * Which way it slides comes from the WM, not from a guess: desktop.h reads
 * the current desktop per output (_KIWM_OUTPUT_DESKTOP, or plain
 * _NET_CURRENT_DESKTOP elsewhere) and the grid they sit in
 * (_NET_DESKTOP_LAYOUT), and reports the step between the old cell and the
 * new one. Going right, the wall pans right: the outgoing windows leave to
 * the left and the incoming ones arrive from the right. Going up, the same
 * thing vertically -- a wall with rows is a wall all the same.
 *
 * The windows on their way out have already been unmapped by the WM, so
 * this holds them with window_retain() exactly as fade-out does, and
 * releases them when the animation ends however it ends.
 *
 * kicomp.conf:
 *
 *   [effect:desktop-wall]
 *   enabled  = 1
 *   duration = 1.5     # multiple of the global animation unit
 *   easing   = in-out  # a pan, weighted at neither end
 *   events   = desktop-leave,desktop-enter
 *   windows  = all
 *   distance = 1.0     # how far, as a fraction of the output's size
 *   fade     = 0       # dim towards the edges as well as slide
 *   crossing = fade    # what happens to the part on another output
 *
 * `distance = 1.0` is the wall proper: a window ends exactly one screen
 * away, so the two desktops never overlap. Less than that and the desktops
 * slide over each other (a shorter, softer motion); more and they pull
 * apart with a gap of background between them.
 *
 * The wall pans exactly one output: the one whose desktop changed. With
 * two monitors side by side that is not a detail -- a window sliding out
 * of the left monitor would otherwise slide *into* the right one, showing
 * one desktop's transition on another desktop that isn't going anywhere.
 *
 * That leaves the windows straddling the boundary, whose far piece is on
 * an output that is staying put. It can't slide (same reason) and it
 * can't stay (its window is leaving), so `crossing=` decides:
 *
 *   fade  it dissolves where it is, in step with the slide (default)
 *   hide  it goes at once
 */
#include "../effect.h"
#include "../animation.h"
#include "../desktop.h"
#include "../output.h"
#include "../window.h"
#include "../renderer.h"
#include "../transform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* What to do with the part of a window that reaches onto an output which
 * is *not* switching desktop (see crossing= below). */
typedef enum {
    CROSSING_FADE = 0,
    CROSSING_HIDE,
} WallCrossing;

typedef struct {
    bool fade;
    float distance;
    WallCrossing crossing;
} WallConfig;

typedef struct {
    const WallConfig *cfg;
    bool leaving;         /* desktop-leave, as opposed to desktop-enter */

    /* The output whose desktop changed -- the only one the wall pans.
     * Everything else on screen belongs to a desktop that is not moving,
     * and sliding a window across it would be showing one output's
     * transition on another one's picture. */
    int output_id;
    CompRect output_rect;

    /* How far the viewport travels, in pixels, and which way: one output
     * in the direction of the switch. A leaving window ends up at minus
     * this (it is pushed off the far side), an entering one starts at plus
     * it (it comes in from the side being travelled towards). */
    int off_x, off_y;

    CompRect covered;     /* what the last frame drew, for damage */
} WallData;

static void config_defaults(void *config)
{
    WallConfig *c = config;
    c->fade = false;
    c->distance = 1.0f;
    c->crossing = CROSSING_FADE;
}

static bool config_key(void *config, const char *key, const char *value)
{
    WallConfig *c = config;

    if (strcmp(key, "fade") == 0) {
        c->fade = atoi(value) != 0;
        return true;
    }
    if (strcmp(key, "distance") == 0) {
        float d = (float)atof(value);
        if (d < 0.0f) d = 0.0f;
        if (d > 4.0f) d = 4.0f;
        c->distance = d;
        return true;
    }
    if (strcmp(key, "crossing") == 0) {
        if (strcmp(value, "fade") == 0)
            c->crossing = CROSSING_FADE;
        else if (strcmp(value, "hide") == 0)
            c->crossing = CROSSING_HIDE;
        else
            fprintf(stderr, "kicomp: config: unknown crossing '%s' "
                            "(fade | hide)\n", value);
        return true;
    }
    return false;
}

/* The output this window sits on, or NULL if it sits on none (which is
 * possible: a window can be parked off-screen). */
static const CompOutput *output_for(const CompRect *r)
{
    CompRect centre = { r->x + r->w / 2, r->y + r->h / 2, 1, 1 };

    for (int i = 0; i < comp.output_count; i++) {
        CompRect hit;
        if (rect_intersect(&centre, &comp.outputs[i].rect, &hit))
            return &comp.outputs[i];
    }
    return NULL;
}

/* Where the window is drawn at this point in the animation: the real
 * rectangle displaced by however much of the offset is left. */
static CompRect rect_for(CompEffect *e, float p)
{
    WallData *d = e->data;

    /* Leaving: from nothing to a whole output *against* the direction of
     * travel -- the desktop being left slides off the trailing side.
     * Entering: from a whole output *along* it, arriving at where the
     * window really is. Two signs, not one: both desktops move the same
     * way, which is what makes the pair read as one wall panning rather
     * than as two windows passing each other. */
    float k = d->leaving ? -p : (1.0f - p);

    CompRect r = window_rect(e->window);
    r.x += (int)((float)d->off_x * k + ((float)d->off_x * k < 0.0f ? -0.5f : 0.5f));
    r.y += (int)((float)d->off_y * k + ((float)d->off_y * k < 0.0f ? -0.5f : 0.5f));
    return r;
}

static void wall_update(CompEffect *e, double now)
{
    WallData *d = e->data;

    CompRect previous = d->covered;
    d->covered = rect_for(e, effect_ease(e, comp_progress(now, e->start_time, e->duration)));

    /* Only ever the output that is switching: the pan is drawn there and
     * nowhere else, so damaging where the animated rectangle *would* have
     * reached would be waking a neighbouring output up every frame to
     * repaint something it is not being shown. */
    CompRect hit;
    if (rect_intersect(&previous, &d->output_rect, &hit))
        output_damage_rect(&hit);
    if (rect_intersect(&d->covered, &d->output_rect, &hit))
        output_damage_rect(&hit);

    /* The piece crossing onto another output is dissolving in place, and
     * a fade is a change with no motion: nothing else would mark that
     * output dirty, and the fade would freeze on its first frame. */
    if (d->cfg->crossing == CROSSING_FADE) {
        CompRect real = window_rect(e->window);
        output_damage_rect(&real);
    }
}

static void wall_apply(CompEffect *e, CompScene *s, CompOutput *o)
{
    WallData *d = e->data;

    float p = effect_ease(e, comp_progress(comp_now_ms(), e->start_time, e->duration));
    CompRect at = rect_for(e, p);

    for (int i = 0; i < s->count; i++) {
        CompSceneNode *n = &s->nodes[i];
        if (n->win != e->window)
            continue;

        if (o->id != d->output_id) {
            /* This output is not the one switching desktop, and what is on
             * screen here belongs to a desktop that is staying put. The
             * node only exists at all because the window reaches across
             * the boundary -- its geometry overlaps two outputs -- and
             * that piece must not be dragged along: sliding it would show
             * one monitor's desktop change happening on another monitor's
             * picture.
             *
             * So the piece stays exactly where it is and dissolves (or
             * simply goes, with crossing=hide). No transform, no motion:
             * the node keeps the rectangle scene.c already clipped to this
             * output, which is precisely the part that crosses. */
            if (d->cfg->crossing == CROSSING_HIDE) {
                n->visible_rect = (CompRect){ 0, 0, 0, 0 };
                return;
            }
            n->opacity *= d->leaving ? (1.0f - p) : p;
            return;
        }

        /* A pure translation: nothing about a wall scales or distorts, the
         * whole desktop simply moves sideways. */
        comp_transform_identity(&n->transform);
        comp_transform_translate(&n->transform,
                                 (float)(at.x - n->geometry.x),
                                 (float)(at.y - n->geometry.y));

        if (d->cfg->fade) {
            float visible = d->leaving ? (1.0f - p) : p;
            n->opacity *= 0.25f + 0.75f * visible;
        }

        if (!rect_intersect(&at, &o->rect, &n->visible_rect))
            n->visible_rect = (CompRect){ 0, 0, 0, 0 };
        return;
    }
}

static bool wall_finished(const CompEffect *e, double now)
{
    return now >= e->start_time + e->duration;
}

static void wall_destroy(CompEffect *e)
{
    WallData *d = e->data;
    if (d && d->leaving)
        window_release(e->window);
    free(e->data);
    e->data = NULL;
}

static const CompEffectOps wall_ops = {
    .name     = "desktop-wall",
    .update   = wall_update,
    .apply    = wall_apply,
    .finished = wall_finished,
    .destroy  = wall_destroy,
};

static void on_event(CompWindow *w, const CompEvent *event,
                     const CompEffectInstance *self)
{
    bool leaving = (event->kind == COMP_EVENT_DESKTOP_LEAVE);

    /* A window on its way out has already been unmapped: the contents
     * named while it was still up are all there will ever be. An entering
     * window is the opposite -- just mapped, its pixmap deliberately
     * unbound and named again on the next paint -- so asking there would
     * refuse every arrival. */
    if (leaving && !renderer_window_has_content(w))
        return;

    CompRect here = window_rect(w);

    const CompOutput *o = output_for(&here);
    if (!o)
        return;

    /* Which way this output's desktop just moved. No answer means the
     * switch wasn't this output's, or the WM publishes nothing to go by --
     * either way there is no honest direction to slide in, and sliding
     * anyway would be inventing one. */
    int dx = 0, dy = 0;
    if (!desktop_switch_for_rect(&here, &dx, &dy))
        return;

    double duration = effect_instance_duration(self);
    if (duration <= 0.0)
        return;

    CompEffect *e = calloc(1, sizeof(*e));
    WallData *d = calloc(1, sizeof(*d));
    if (!e || !d) {
        free(e);
        free(d);
        return;
    }

    d->cfg = self->config;
    d->leaving = leaving;
    d->output_id = o->id;
    d->output_rect = o->rect;

    /* One output in the direction of travel; rect_for() applies it with
     * the sign each half of the switch needs. */
    d->off_x = dx * (int)((float)o->rect.w * d->cfg->distance);
    d->off_y = dy * (int)((float)o->rect.h * d->cfg->distance);

    e->ops = &wall_ops;
    e->instance = self;
    e->window = w;
    e->start_time = comp_now_ms();
    e->duration = duration;
    e->data = d;

    d->covered = rect_for(e, 0.0f);

    if (leaving)
        window_retain(w);

    effects_add(e);
    output_damage_rect(&o->rect);
}

const CompEffectModule effect_desktop_wall = {
    .name             = "desktop-wall",
    .default_enabled  = true,
    /* Longer than a single window's animation: this is the whole screen
     * moving, and at one unit it reads as a flicker rather than a pan. */
    .default_duration = 1.5,
    /* A pan is weighted at neither end -- it starts, it travels, it stops.
     * `out` would have the wall lurch away and creep in. */
    .default_easing   = COMP_EASE_IN_OUT,
    .default_events   = COMP_EVENT_BIT(COMP_EVENT_DESKTOP_LEAVE) |
                        COMP_EVENT_BIT(COMP_EVENT_DESKTOP_ENTER),
    /* Everything that travels with the desktop. Panels and the desktop
     * window itself don't -- they stay put across a switch, so they never
     * produce these events in the first place, and leaving them in the
     * mask costs nothing while covering a WM that does move them. */
    .default_windows  = COMP_WINDOWS_ALL & ~COMP_WINDOW_BIT(COMP_WINDOW_DESKTOP),
    .config_size      = sizeof(WallConfig),
    .config_defaults  = config_defaults,
    .config_key       = config_key,
    .window_event     = on_event,
};
