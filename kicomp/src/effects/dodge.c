/*
 * Dodge: when a window is raised and takes focus, the windows that were
 * covering it step aside -- each towards whichever edge is nearest -- and
 * settle back where they were, so the raise reads as things making room
 * rather than as one rectangle appearing on top of another between two
 * frames.
 *
 * Two things about it are worth stating, because they are what makes it
 * an effect rather than a window manager feature:
 *
 *   - Nothing moves. The WM has already raised the focused window and the
 *     others are already behind it, with their real geometry untouched
 *     (section 27); this only changes where they are *drawn*, for as long
 *     as the animation lasts. Input, focus and every EWMH property are at
 *     the true place from the first instant.
 *   - It is the only effect here that animates a window other than the
 *     one the event happened to. The event arrives for the window that
 *     gained focus; what gets animated is everything that was covering
 *     it. The interface already allowed that -- an effect
 *     names the window it animates, which is not required to be the one
 *     it was told about -- so each dodging window gets its own effect and
 *     dies with its own window if it goes away.
 *
 * Which way a window dodges is the cheapest honest answer: of the four
 * directions that would clear the overlap, the one that needs the least
 * movement. A window overlapping a little on the left slides left; one
 * overlapping at the bottom drops down. `strength` decides how much of
 * that distance is actually travelled: all of it, by default, because a
 * window that only half clears the one it was covering has not made room,
 * it has twitched.
 *
 * Three kinds of window are left alone, all for the same reason -- they
 * are not in anyone's way:
 *
 *   - anything always-on-top (_NET_WM_STATE_ABOVE): it is in front on
 *     purpose and stays there;
 *   - the focused window's own siblings (window.h's windows_same_group):
 *     an application's windows are one thing on screen;
 *   - and every window, when the focus came with the window *appearing*
 *     (effect.h's with_appear): nothing was covering a window that wasn't
 *     there a moment ago.
 *
 * kicomp.conf:
 *
 *   [effect:dodge]
 *   enabled      = 1
 *   duration     = 1.5     # multiple of the global animation unit
 *   events       = focus
 *   windows      = windows
 *   easing       = in-out  # shapes each half of the swing
 *   strength     = 1.0     # fraction of the distance that would clear it
 *   clearance    = 8       # px of gap left between them once aside
 *   max_distance = 0       # px ceiling; 0 = whatever clearing it takes
 *   raise_at     = 0.5     # when the focused window is allowed forward
 *
 * The motion is out and back within that one duration, not out in one
 * animation and back in another: a window that stepped aside and stayed
 * aside would be lying about where it is for as long as it kept it up.
 * Three points, then -- where it is, aside, where it is again -- and
 * `easing=` shapes each of the two journeys between them (see swing()).
 */
#include "../effect.h"
#include "../animation.h"
#include "../output.h"
#include "../window.h"
#include "../transform.h"

#include <stdlib.h>
#include <string.h>

/* Below this the dodge is invisible and not worth a frame. */
#define MIN_DODGE_PX 4

typedef struct {
    float strength;
    int clearance;
    int max_distance;
    float raise_at;
} DodgeConfig;

typedef struct {
    int off_x, off_y;     /* the displacement at the far end of the swing */
    CompRect covered;     /* what the last frame drew, for damage */
} DodgeData;

static void config_defaults(void *config)
{
    DodgeConfig *c = config;
    /* All the way out of the way, by default: a window that only half
     * clears the one it was covering hasn't made room, it has twitched. */
    c->strength = 1.0f;
    c->clearance = 8;         /* a little past touching, not flush against it */
    c->max_distance = 0;      /* no ceiling */
    /* Halfway: the moment the dodgers are fully out of the way, which is
     * the point of the whole effect -- the window comes forward because
     * room was made for it. */
    c->raise_at = 0.5f;
}

static bool config_key(void *config, const char *key, const char *value)
{
    DodgeConfig *c = config;

    if (strcmp(key, "strength") == 0) {
        float s = (float)atof(value);
        if (s < 0.0f) s = 0.0f;
        if (s > 1.0f) s = 1.0f;
        c->strength = s;
        return true;
    }
    if (strcmp(key, "max_distance") == 0) {
        int px = atoi(value);
        if (px < 0) px = 0;
        if (px > 4000) px = 4000;
        c->max_distance = px;   /* 0 = no ceiling */
        return true;
    }
    if (strcmp(key, "raise_at") == 0) {
        float f = (float)atof(value);
        if (f < 0.0f) f = 0.0f;
        if (f > 1.0f) f = 1.0f;
        c->raise_at = f;
        return true;
    }
    if (strcmp(key, "clearance") == 0) {
        int px = atoi(value);
        if (px < 0) px = 0;
        if (px > 1000) px = 1000;
        c->clearance = px;
        return true;
    }
    return false;
}

/* Out and back: three points -- where it is, fully aside, where it is
 * again -- and the two journeys between them.
 *
 * `easing=` is what shapes each of those journeys, exactly as it does for
 * every other effect: the curve is applied to each half rather than to
 * the animation as a whole, so `out` means the window eases into being
 * aside *and* eases back into place, instead of easing into the halfway
 * point and lurching through the second half. The two halves are
 * therefore mirror images, which is what "goes aside and comes back"
 * should look like.
 *
 * There is no pause at the far end. A stop would be another way of
 * spending the same duration, and one the easing can't express: `in-out`
 * already spends most of the time near the ends, which is the same
 * reading without a frozen window in the middle of it. */
static float swing(CompEffect *e, float p)
{
    float half = (p < 0.5f) ? (p / 0.5f) : ((1.0f - p) / 0.5f);
    if (half < 0.0f)
        half = 0.0f;
    return effect_ease(e, half);
}

/* Takes raw progress, not eased progress: the curve belongs to each half
 * of the journey (swing), and easing the whole animation first would bend
 * the two halves against each other. */
static CompRect rect_for(CompEffect *e, float p)
{
    DodgeData *d = e->data;
    float k = swing(e, p);

    CompRect r = window_rect(e->window);
    r.x += (int)((float)d->off_x * k + ((float)d->off_x * k < 0.0f ? -0.5f : 0.5f));
    r.y += (int)((float)d->off_y * k + ((float)d->off_y * k < 0.0f ? -0.5f : 0.5f));
    return r;
}

static void dodge_update(CompEffect *e, double now)
{
    DodgeData *d = e->data;

    CompRect previous = d->covered;
    d->covered = rect_for(e, comp_progress(now, e->start_time, e->duration));

    output_damage_rect(&previous);
    output_damage_rect(&d->covered);
}

static void dodge_apply(CompEffect *e, CompScene *s, CompOutput *o)
{
    CompRect at = rect_for(e, comp_progress(comp_now_ms(), e->start_time, e->duration));

    for (int i = 0; i < s->count; i++) {
        CompSceneNode *n = &s->nodes[i];
        if (n->win != e->window)
            continue;

        comp_transform_identity(&n->transform);
        comp_transform_translate(&n->transform,
                                 (float)(at.x - n->geometry.x),
                                 (float)(at.y - n->geometry.y));

        if (!rect_intersect(&at, &o->rect, &n->visible_rect))
            n->visible_rect = (CompRect){ 0, 0, 0, 0 };
        return;
    }
}

static bool dodge_finished(const CompEffect *e, double now)
{
    return now >= e->start_time + e->duration;
}

static void dodge_destroy(CompEffect *e)
{
    free(e->data);
    e->data = NULL;
}

static const CompEffectOps dodge_ops = {
    .name     = "dodge",
    .update   = dodge_update,
    .apply    = dodge_apply,
    .finished = dodge_finished,
    .destroy  = dodge_destroy,
};

/* ------------------------------------------------------------------ */
/* holding the raise back                                              */
/* ------------------------------------------------------------------ */

/* The other half of the effect, and the half that makes it mean
 * something: the focused window is drawn at its *old* depth until the
 * windows that were covering it have got out of the way, so it looks like
 * it came forward because they made room -- rather than appearing on top
 * first and having them shuffle aside afterwards, which reads as two
 * unrelated things happening at once.
 *
 * Drawing order only (scene_move_node): the WM raised the window and it
 * is raised -- clicks, focus and the stacking properties all say so from
 * the first instant. This is the same lie a transform tells about x and
 * y, told about z instead, and it lasts a fraction of a second.
 */
/* Which windows are drawn over the focused one while the raise is held
 * back -- by id, not by pointer: one of them can be destroyed mid-
 * animation, and an id that no longer matches anything is simply not
 * found, where a dangling pointer would have to be. */
#define MAX_HELD_ABOVE 32

typedef struct {
    xcb_window_t above[MAX_HELD_ABOVE];
    int count;
} HoldData;

static void hold_update(CompEffect *e, double now)
{
    (void)now;
    /* Nothing moves, but what is drawn over what changes: without saying
     * so, no frame would be painted at the moment the window comes
     * forward. */
    CompRect r = window_rect(e->window);
    output_damage_rect(&r);
}

static void hold_apply(CompEffect *e, CompScene *s, CompOutput *o)
{
    (void)o;
    HoldData *d = e->data;

    int index = -1;
    for (int i = 0; i < s->count; i++) {
        if (s->nodes[i].win == e->window) {
            index = i;
            break;
        }
    }
    if (index < 0)
        return;

    /* Back to just under the lowest of the windows that were covering it
     * -- the bottom of that group. Everything that was already behind it
     * stays behind it.
     *
     * The list was taken when the effect started and is not recomputed:
     * "was above" is answered from a snapshot of the stacking that the
     * core refreshes at the end of every batch of events (window.c), so
     * by the second frame of this animation the raise is simply history
     * and nothing was above anything. Freezing the answer here is what
     * makes the hold last longer than one frame. */
    for (int i = 0; i < index; i++) {
        for (int j = 0; j < d->count; j++) {
            if (s->nodes[i].win->id != d->above[j])
                continue;
            scene_move_node(s, index, i);
            return;
        }
    }
}

static bool hold_finished(const CompEffect *e, double now)
{
    return now >= e->start_time + e->duration;
}

static void hold_destroy(CompEffect *e)
{
    free(e->data);
    e->data = NULL;
}

static const CompEffectOps hold_ops = {
    .name     = "dodge-raise",
    .update   = hold_update,
    .apply    = hold_apply,
    .finished = hold_finished,
    .destroy  = hold_destroy,
};

static void hold_raise(CompWindow *focused, const CompEffectInstance *self,
                       double duration, const xcb_window_t *above, int count)
{
    if (count <= 0)
        return;

    CompEffect *e = calloc(1, sizeof(*e));
    HoldData *d = calloc(1, sizeof(*d));
    if (!e || !d) {
        free(e);
        free(d);
        return;
    }

    if (count > MAX_HELD_ABOVE)
        count = MAX_HELD_ABOVE;
    for (int i = 0; i < count; i++)
        d->above[i] = above[i];
    d->count = count;

    e->data = d;
    e->ops = &hold_ops;
    e->instance = self;
    e->window = focused;
    e->start_time = comp_now_ms();
    e->duration = duration;

    effects_add(e);

    CompRect r = window_rect(focused);
    output_damage_rect(&r);
}

/* How far `w` would have to move, and in which direction, to stop
 * overlapping `focused` -- taking whichever of the four is shortest.
 * False when they don't overlap at all. */
static bool escape_vector(const CompRect *w, const CompRect *focused,
                          int *dx, int *dy)
{
    CompRect overlap;
    if (!rect_intersect(w, focused, &overlap))
        return false;

    int left  = (w->x + w->w) - focused->x;          /* move left by this */
    int right = (focused->x + focused->w) - w->x;    /* move right by this */
    int up    = (w->y + w->h) - focused->y;
    int down  = (focused->y + focused->h) - w->y;

    int best = left;
    *dx = -left;
    *dy = 0;

    if (right < best) {
        best = right;
        *dx = right;
        *dy = 0;
    }
    if (up < best) {
        best = up;
        *dx = 0;
        *dy = -up;
    }
    if (down < best) {
        best = down;
        *dx = 0;
        *dy = down;
    }

    return true;
}


/* True when this window actually got an effect: the caller counts them,
 * because holding the raise back is only honest if something is
 * getting out of the way. */
static bool start_for(CompWindow *w, const CompRect *focus_rect,
                      const CompEffectInstance *self, double duration)
{
    const DodgeConfig *cfg = self->config;

    int dx, dy;
    CompRect here = window_rect(w);
    if (!escape_vector(&here, focus_rect, &dx, &dy))
        return false;

    /* The escape vector runs along one axis, so its length is whichever
     * component isn't zero. */
    float need = (float)(dx != 0 ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy));

    /* How far it actually goes: the fraction of that asked for, plus the
     * clearance -- the gap left between the two windows once aside. Just
     * clearing the edge puts them flush against each other, which reads
     * as one window stuck to the other rather than as having got out of
     * the way. Then the ceiling, so a window that happened to be almost
     * entirely covered isn't thrown off the screen. */
    float travel = need * cfg->strength + (float)cfg->clearance;
    if (cfg->max_distance > 0 && travel > (float)cfg->max_distance)
        travel = (float)cfg->max_distance;

    if (travel < (float)MIN_DODGE_PX)
        return false;

    float sx = (dx != 0) ? (dx < 0 ? -travel : travel) : 0.0f;
    float sy = (dy != 0) ? (dy < 0 ? -travel : travel) : 0.0f;

    CompEffect *e = calloc(1, sizeof(*e));
    DodgeData *d = calloc(1, sizeof(*d));
    if (!e || !d) {
        free(e);
        free(d);
        return false;
    }

    d->off_x = (int)(sx + (sx < 0.0f ? -0.5f : 0.5f));
    d->off_y = (int)(sy + (sy < 0.0f ? -0.5f : 0.5f));
    d->covered = here;

    e->ops = &dodge_ops;
    e->instance = self;
    e->window = w;
    e->start_time = comp_now_ms();
    e->duration = duration;
    e->data = d;

    effects_add(e);
    output_damage_rect(&here);
    return true;
}

static void on_event(CompWindow *w, const CompEvent *event,
                     const CompEffectInstance *self)
{
    if (!w->mapped || w->input_only)
        return;

    /* A window that has just opened or come back takes focus as a matter
     * of course, and it had no previous position for anything to have
     * been covering -- nothing "made room" for it, it simply arrived. The
     * windows it now overlaps were not in its way a moment ago, because a
     * moment ago it wasn't there. */
    if (event->with_appear)
        return;

    double duration = effect_instance_duration(self);
    if (duration <= 0.0)
        return;

    CompRect focus_rect = window_rect(w);

    /* Only what was *covering* it: overlapping, and drawn on top of it
     * before the raise. Asking the current stacking instead ("everything
     * below it now") looks like the same question and is not: the WM
     * raises and focuses in one gesture, so by now everything overlapping
     * is below, including windows that were always behind it and never
     * covered anything -- a maximized browser two layers down would step
     * aside for a window it was never in front of. window_was_above()
     * compares the stacking from before the batch, which is the only
     * place that answer still exists.
     *
     * The type filter is applied here rather than by the core, because
     * the core filtered on the window the *event* was about: it is the
     * dodging windows that have to be of a type the user pointed this
     * at, not the one that took focus. */
    xcb_window_t dodgers[MAX_HELD_ABOVE];
    int dodging = 0;

    for (CompWindow *other = comp.stack; other; other = other->next) {
        if (other == w || !other->mapped || other->input_only)
            continue;
        if (other->wm_layer[0] || other->zombie)
            continue;
        if (!(self->windows & COMP_WINDOW_BIT(other->type)))
            continue;

        /* Always-on-top windows don't dodge: they are in front by the
         * user's own instruction and will still be in front when this is
         * over, so moving them aside would be a window getting out of the
         * way of something it is not in the way of. */
        if (other->state & COMP_STATE_ABOVE)
            continue;

        /* Nor does an application dodge itself. A machine window and its
         * detached mini-toolbar, a main window and its palette, a dialog
         * and its parent -- those are one thing on screen, and shoving
         * one aside to reveal another is not making room, it is taking a
         * program apart (window.h's windows_same_group). */
        if (windows_same_group(other, w))
            continue;

        if (!window_was_above(other, w))
            continue;

        if (!start_for(other, &focus_rect, self, duration))
            continue;
        if (dodging < MAX_HELD_ABOVE)
            dodgers[dodging] = other->id;
        dodging++;
    }

    /* Nothing stepped aside: there is nothing for the raise to wait for,
     * and holding it back would be a delay for its own sake. */
    if (dodging == 0)
        return;

    const DodgeConfig *cfg = self->config;
    if (cfg->raise_at > 0.0f)
        hold_raise(w, self, duration * cfg->raise_at, dodgers,
                   dodging < MAX_HELD_ABOVE ? dodging : MAX_HELD_ABOVE);
}

const CompEffectModule effect_dodge = {
    .name             = "dodge",
    /* Off by default: it moves windows the user did not touch, which is a
     * strong opinion for a compositor to have without being asked. */
    .default_enabled  = false,
    /* Two journeys in one duration, so a little longer than a single
     * window's move. */
    .default_duration = 1.5,
    /* Applied to each half of the swing, not to the animation as a whole:
     * it eases out of the way and eases back into place. */
    .default_easing   = COMP_EASE_IN_OUT,
    .default_events   = COMP_EVENT_BIT(COMP_EVENT_FOCUS),
    /* Which windows may dodge -- not which may cause a dodge. */
    .default_windows  = COMP_WINDOW_BIT(COMP_WINDOW_UNKNOWN) |
                        COMP_WINDOW_BIT(COMP_WINDOW_NORMAL) |
                        COMP_WINDOW_BIT(COMP_WINDOW_DIALOG) |
                        COMP_WINDOW_BIT(COMP_WINDOW_UTILITY) |
                        COMP_WINDOW_BIT(COMP_WINDOW_TOOLBAR),
    .config_size      = sizeof(DodgeConfig),
    .config_defaults  = config_defaults,
    .config_key       = config_key,
    .window_event     = on_event,
};
