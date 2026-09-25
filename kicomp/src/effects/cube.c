/*
 * cube: the desktops as the faces of a turning prism.
 *
 * One cube per output, never one across all of them. Everything in this
 * compositor is per output -- scene, target, presenter, damage, and the
 * frame clock right down to its vblank phase -- and two monitors here
 * present at 60.00 and 59.89 Hz, at different instants. A cube spanning
 * both would be one scene drawn into two targets that fly at different
 * times, which is a seam that tears down the middle. And under kiwm each
 * output has a desktop of its own, so a single cube would have no single
 * front face to speak of. Per output is the design, not a concession.
 *
 * N desktops make an N-sided prism, the way Compiz does it: four is the
 * cube everyone means, three is a triangle, six a hexagon, and two is a
 * sheet of paper with a side each. The faces are the output's own
 * rectangle, stood up around a common axis at the apothem -- half the
 * width over tan(pi/N) -- which is what makes every N look right without
 * a number tuned per shape.
 *
 * Phase 0 is the desktop exactly as it is: the front face lands at z = 0,
 * where the projection does not scale it, the cube has not turned and
 * the other faces are behind it and culled. Opening pushes the whole
 * thing away from the eye and closing brings it back, so the mode is
 * entered and left by moving rather than by cutting -- and at either end
 * what is on screen is the real desktop, pixel for pixel.
 *
 * Nothing here switches a desktop. On release the nearest face is asked
 * for from the window manager (desktop_request_switch) exactly as a
 * pager would ask, and what that means is the WM's business.
 */
#include "../effect.h"
#include "../window.h"
#include "../output.h"
#include "../input.h"
#include "../scene.h"
#include "../transform.h"
#include "../animation.h"
#include "../renderer.h"
#include "../desktop.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FACES 16

/* Long enough that a frame or two of stall does not put a face out, and
 * renewed well inside it: kiwm caps a hold at two seconds of its own
 * accord, so this has to be asked again rather than asked once for as
 * long as the cube might be held open. */
#define HOLD_MS       1500
#define HOLD_RENEW_MS 500

/* How quickly the drawn angle catches up with where a drag is pulling it.
 * Motion events do not arrive on the same clock as frames do -- a
 * constant mouse speed can still land two events in one frame and none in
 * the next -- so painting the raw delta every frame turns that jitter
 * straight into jitter in the cube's angular speed, which a rotation
 * makes very easy to see. Chasing the target with a time-constant filter
 * instead means the picture's speed is a function of the clock, not of
 * how the events happened to bunch up. */
#define DRAG_SMOOTH_MS 35.0

/* How quickly the drawn zoom catches up with where the wheel has sent
 * it. Slower than DRAG_SMOOTH_MS on purpose: a drag is a continuous
 * motion already, chased only to smooth out event jitter, but a wheel
 * arrives as discrete notches -- without its own chase each one would
 * snap the eye straight to its new distance, which through a face is
 * a jump cut rather than a walk. */
#define ZOOM_SMOOTH_MS 150.0


typedef struct {
    char hotkey[128];
    /* Turning the cube by one face without a drag: the same mode, opened
     * and closed by itself. */
    char hotkey_next[128];
    char hotkey_prev[128];

    /* How far the cube stands off from the eye once it is open, and how
     * strong the projection is -- both as fractions of the output's
     * width, so one setting looks the same on every monitor. */
    float zoom;
    /* And the distance a keyed turn uses, which is its own taste: 0
     * leaves the front face filling the screen, so the desktops sweep
     * past at full size instead of the cube backing away first. */
    float flick_zoom;
    float perspective;

    /* A drag across the whole output turns the cube this many times. */
    float turns;

    /* The wheel, while a mouse drag holds the cube open: each notch
     * moves `zoom` this much, in the same fraction-of-width units as
     * `zoom` itself. Scrolling in past zero walks the eye through the
     * near face and into the cube -- see CUBE_NEAR_EPS and `inside` in
     * cube_apply for how a face that the eye has reached is dropped
     * instead of flipping inside out. */
    float zoom_step;
    float zoom_min;
    float zoom_max;

    /* How far the vertical drag may tilt it, in degrees. 90 is looking
     * at it exactly from above or below, which is as far as there is to
     * go. */
    float tilt_max;

    /* How far the first window floats off its face, and how far each
     * one after it floats off the one below -- Compiz's 3D windows.
     * Both in root pixels: a window's distance from its own desktop is
     * not a fraction of anything. */
    float window_gap;
    float window_spacing;

    /* The top and bottom, as a colour with an alpha of its own: they are
     * only ever seen once the cube is tilted, and how solid they should
     * be is a matter of taste rather than of correctness. */
    float cap_r, cap_g, cap_b, cap_a;
    float back_r, back_g, back_b;       /* behind the cube */
    float background;                   /* how solid that is */

    /* How much of the other desktops is kept alive while the cube is
     * open. Their windows are unmapped -- there is no pixmap of a window
     * on a desktop nobody is showing -- so a face is empty unless the
     * window manager is asked to hold them up.
     *
     *   desktop hold nothing: the other faces show their wallpaper only
     *           (`none` says the same)
     *   active  the last window used on each, which is the one that
     *           makes a face recognisable for the least work
     *   all     every window of every desktop
     *
     * Minimized windows are never on a face, whatever this says: they
     * are not on their desktop, and a face is the desktop as it would
     * look (cube_apply). Nor could they be held -- kiwm refuses.
     *
     * Left out of the section, it is kicomp's own live_windows
     * (comp.h): what the compositor keeps live all the time is what a
     * cube finds live when it opens. */
    CompLiveWindows live;

    /* The wallpaper lies on its face; so do the panels, over it. A panel
     * floating off the surface with the windows reads as a window, which
     * it is not -- it is part of the desktop it is on. */
    bool flat_docks;

    /* How see-through the cube's shell -- its faces, their wallpaper and
     * panels, and the caps -- goes while a *mouse* drag is turning it: 0
     * solid (the default), 1 gone entirely. The windows themselves stay
     * solid, and both they and the wallpaper are drawn on the faces
     * turned away too, so a drag becomes a way to look at every desktop
     * -- its wallpaper and its windows -- at once, floating where the
     * turn puts them. Not applied to a keyed turn (hotkey_next/prev):
     * that one flicks past a single face and shuts itself, with nothing
     * to look through. */
    float spin_transparency;

    /* Whether a face carries its own backing quad (cap_r/g/b/cap_a) at
     * all, sides and caps alike. spin_transparency already fades the
     * wallpaper's own opacity (it is a window like any other put on the
     * face), so with a wallpaper on every desktop the backing quad is a
     * second, redundant layer under it -- this is a way to try the cube
     * as nothing but that prism of wallpapers, no shell of its own
     * showing through when spun. The caps have no wallpaper to fall back
     * on, so turning this off simply removes them too. On by default,
     * matching how the cube always looked before this existed. */
    bool shell;
} CubeConfig;

typedef struct {
    int output_id;
    int faces;                  /* desktops, and so sides */
    int first_desktop;          /* the one on face 0 */

    /* The turn, in radians, and the tilt. `angle` is where the cube is;
     * `angle_to` where it is heading once the drag lets go. */
    float angle;
    float angle_from, angle_to;
    double angle_time;
    bool settling;              /* gliding to the face that was chosen */

    float tilt;
    float tilt_from;            /* where the tilt was when settling began */

    /* Where a live drag is pulling the angle and tilt towards -- the raw
     * sum of pointer motion, unsmoothed. `angle`/`tilt` chase these once
     * per frame (cube_update) rather than jumping straight to them. */
    float angle_target;
    float tilt_target;
    double drag_tick;           /* when that chase last advanced */

    /* The point the pointer is pinned to while the drag lasts: every
     * motion is measured from it and the pointer put back on it. */
    int drag_x, drag_y;
    bool dragging;

    double held_at;             /* when the holds were last renewed */

    float zoom;                 /* how far back, drawn from -- chases zoom_target */
    float zoom_target;          /* where the wheel has walked the eye to */
    double zoom_tick;           /* when that chase last advanced */
    float window_gap;           /* and how far the windows stand off it */
    float window_spacing;

    float phase;                /* 0 the plain desktop, 1 the open cube */
    float phase_from, phase_to;
    double phase_time;
    bool closing;

    /* Opened by a key rather than held by a button: it turns one face
     * and shuts itself, so there is no release to wait for. */
    bool flick;
} CubeData;

static const CompEffectOps cube_ops;
static CompEffect *active;

/* face_of_window()'s answer for a window that is on every desktop, and
 * so has to be drawn on every side. */
#define CUBE_EVERY_FACE (-2)

static int face_of_window(const CubeData *d, const CompOutput *o, CompWindow *w);
static const CompEffectInstance *bound_instance;

/* ------------------------------------------------------------------ */
/* geometry                                                            */
/* ------------------------------------------------------------------ */

/* Distance from the axis to the middle of a face. For two faces there is
 * no inside at all -- the prism is a sheet -- so the apothem is zero and
 * the two sides land back to back, which is exactly what Compiz draws. */
static float apothem_of(const CompOutput *o, int faces)
{
    if (faces < 3)
        return 0.0f;
    return ((float)o->rect.w * 0.5f) / tanf((float)M_PI / (float)faces);
}

/* The matrix that puts face `i` where it belongs and then turns the whole
 * prism to where the drag has it.
 *
 * Read downwards: the face's own rectangle is centred on the origin,
 * stood around the axis and pushed out to the surface; the prism is then
 * turned and tilted as one, moved away from the eye, projected, and put
 * back in the middle of the output.
 *
 * The push away is `apothem + back`, so at back = 0 the front face's
 * surface sits at exactly z = 0 and the projection leaves it alone --
 * which is why phase 0 is the untouched desktop rather than an
 * approximation of it. */
/* The same placement as a face, `off` pixels out in front of it: 0 is
 * the face itself, and a window floating above its desktop is the very
 * same rectangle a little nearer the viewer. One function for both so a
 * window and the face under it cannot be placed by two pieces of
 * arithmetic that might disagree. */
static void face_transform_at(CompTransform *t, const CompOutput *o,
                              const CubeConfig *cfg, const CubeData *d,
                              int i, float off)
{
    float step = 2.0f * (float)M_PI / (float)d->faces;
    float own = step * (float)i;
    float r = apothem_of(o, d->faces);
    float back = (float)o->rect.w * d->zoom * d->phase;

    float cx = (float)o->rect.x + (float)o->rect.w * 0.5f;
    float cy = (float)o->rect.y + (float)o->rect.h * 0.5f;

    comp_transform_identity(t);
    comp_transform_translate(t, -cx, -cy);
    /* Out to the surface *before* the face is swung round, not after:
     * these compose as "applied next", so a push after the rotation
     * would move the face in world z rather than along its own normal,
     * and the prism would come out as a stack of sheets sliding through
     * each other instead of a ring of faces. Pushed out first and turned
     * second, every face ends up at the apothem facing outwards. */
    comp_transform_translate_z(t, r + off);
    comp_transform_rotate_y(t, own);
    comp_transform_rotate_y(t, d->angle);
    comp_transform_rotate_x(t, d->tilt);
    comp_transform_translate_z(t, -(r + back));
    comp_transform_perspective(t, (float)o->rect.w * cfg->perspective);
    comp_transform_translate(t, cx, cy);
}

static void face_transform(CompTransform *t, const CompOutput *o,
                           const CubeConfig *cfg, const CubeData *d, int i)
{
    face_transform_at(t, o, cfg, d, i, 0.0f);
}

/* A cap: the lid or the floor, laid flat across the prism and carried
 * through the same turn and tilt as the sides.
 *
 * A solid is a rectangle and a prism's cross-section is an N-gon, so for
 * four faces this is exact and for any other N it is the square around
 * it -- visible as a little overhang at the corners. Worth taking: a cap
 * is only ever seen while the cube is tilted, it is one flat colour, and
 * the alternative is a polygon primitive nothing else would use.
 *
 * `up` is +1 for the lid and -1 for the floor. */
static void cap_transform(CompTransform *t, const CompOutput *o,
                          const CubeConfig *cfg, const CubeData *d, int up)
{
    float r = apothem_of(o, d->faces);
    float back = (float)o->rect.w * d->zoom * d->phase;

    float cx = (float)o->rect.x + (float)o->rect.w * 0.5f;
    float cy = (float)o->rect.y + (float)o->rect.h * 0.5f;

    comp_transform_identity(t);
    comp_transform_translate(t, -cx, -cy);
    /* Laid flat, each one turned its own way so that its front points
     * out of the prism -- the lid upwards, the floor downwards. Laying
     * both down with the same rotation leaves one of them inside out,
     * and the winding test then culls whichever one you are looking at. */
    comp_transform_rotate_x(t, (float)up * (float)M_PI * 0.5f);
    comp_transform_translate(t, 0.0f, (float)up * (float)o->rect.h * -0.5f);
    comp_transform_rotate_y(t, d->angle);
    comp_transform_rotate_x(t, d->tilt);
    comp_transform_translate_z(t, -(r + back));
    comp_transform_perspective(t, (float)o->rect.w * cfg->perspective);
    comp_transform_translate(t, cx, cy);
}

/* The square a cap is drawn as, centred on the output. */
static CompRect cap_rect(const CompOutput *o, const CubeData *d)
{
    float r = apothem_of(o, d->faces);
    int side = (int)(r * 2.0f + 0.5f);
    if (side < 1)
        side = (int)o->rect.w;      /* two faces: no inside to cap */

    return (CompRect){
        o->rect.x + (o->rect.w - side) / 2,
        o->rect.y + (o->rect.h - side) / 2,
        side, side
    };
}

/* How far away a face's middle ends up, as the w the projection divides
 * by: bigger is further. The matrix's last row is exactly that question
 * asked of a point, and a flat node's z contributes nothing to it, so
 * this needs no separate depth of its own. */
static float face_depth(const CompTransform *t, const CompOutput *o)
{
    float x = (float)o->rect.x + (float)o->rect.w * 0.5f;
    float y = (float)o->rect.y + (float)o->rect.h * 0.5f;
    return t->m[3][0] * x + t->m[3][1] * y + t->m[3][3];
}

/* face_depth() is the w the projection divides by, and it is exactly
 * `1 - z/distance` (comp_transform_perspective): it falls to zero as a
 * plane reaches the eye and goes negative once the eye has passed
 * through it, at which point dividing by it does not draw the plane
 * closer any more, it mirrors it. There being no clip plane in this
 * pipeline -- the renderers draw whatever quad they are handed, they do
 * not cut one -- a plane the eye has reached has to be dropped whole
 * rather than let its w cross zero, which is what lets the wheel walk
 * the eye through a face instead of turning it inside out at the
 * threshold. The epsilon is a hair short of the actual singularity so
 * the last visible sliver is still comfortably a projection. */
#define CUBE_NEAR_EPS 0.08f

static bool cube_past_eye(const CompTransform *t, const CompOutput *o)
{
    return face_depth(t, o) <= CUBE_NEAR_EPS;
}

/* How much a face nearing the eye has to be pulled towards fully solid
 * before it is dropped, so that crossing into `inside` (cube_apply) is
 * a face fading to solid and vanishing rather than the whole shell
 * popping from see-through to opaque in one frame the instant the eye
 * passes zero. 0 once still comfortably away (spin_transparency's own
 * veil applies unchanged); 1 at the cutoff, an instant before
 * cube_past_eye would drop it. */
#define CUBE_NEAR_FADE 0.5f

static float cube_near_solid(const CompTransform *t, const CompOutput *o)
{
    float w = face_depth(t, o);
    float band = CUBE_NEAR_EPS + CUBE_NEAR_FADE;
    if (w >= band)
        return 0.0f;
    if (w <= CUBE_NEAR_EPS)
        return 1.0f;
    float x = (band - w) / CUBE_NEAR_FADE;
    return x * x * (3.0f - 2.0f * x);      /* smoothstep */
}

/* Is this face turned towards the viewer?
 *
 * By the winding of its corners once they have landed on screen: a quad
 * seen from behind comes out mirrored, and the sign of the cross product
 * of two of its edges says which. Cheaper and less error-prone than
 * carrying a normal through the same chain of rotations, and it cannot
 * disagree with what is actually drawn, because it is measured from it. */
static bool face_faces_us(const CompTransform *t, const CompRect *r)
{
    float x0, y0, x1, y1, x2, y2;
    comp_transform_point(t, (float)r->x, (float)r->y, &x0, &y0);
    comp_transform_point(t, (float)(r->x + r->w), (float)r->y, &x1, &y1);
    comp_transform_point(t, (float)r->x, (float)(r->y + r->h), &x2, &y2);

    float cross = (x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0);
    return cross > 0.0f;
}

/* ------------------------------------------------------------------ */
/* the mode                                                            */
/* ------------------------------------------------------------------ */

static CompOutput *output_by_id(int id)
{
    for (int i = 0; i < comp.output_count; i++)
        if (comp.outputs[i].id == id)
            return &comp.outputs[i];
    return NULL;
}

static CompOutput *output_of(const CompRect *r)
{
    CompOutput *best = NULL;
    long best_area = 0;
    for (int i = 0; i < comp.output_count; i++) {
        CompRect hit;
        if (!rect_intersect(r, &comp.outputs[i].rect, &hit))
            continue;
        long a = (long)hit.w * hit.h;
        if (a > best_area) {
            best_area = a;
            best = &comp.outputs[i];
        }
    }
    return best;
}

static void mark_dirty(const CubeData *d)
{
    /* The cube covers the output and every frame of a turn changes all of
     * it; working out a smaller region would cost more than it saves.
     * Scoped to this output alone, not output_damage_all() -- a cube open
     * on one monitor has no business repainting the others every tick. */
    CompOutput *o = output_by_id(d->output_id);
    if (!o)
        return;
    output_damage_rect(&o->rect);

    /* Except for the sliver of a window that hangs over onto a
     * neighbouring monitor (cube_apply's other-monitors branch): that
     * fade lives on an output which is not this one, so damaging only
     * our own screen above never reaches it, and it would freeze
     * wherever `d->phase` happened to be the last time something else
     * damaged that monitor. Just the window's own rect, not the whole
     * neighbour -- the fade is the only thing there that is changing. */
    for (CompWindow *w = comp.stack; w; w = w->next) {
        if (w->wm_layer[0])
            continue;
        if (face_of_window(d, o, w) == -1)
            continue;
        CompRect r = window_rect(w);
        output_damage_window_rect(w, &r);
    }
}

static void phase_to(CubeData *d, float to, double now)
{
    d->phase_from = d->phase;
    d->phase_to = to;
    d->phase_time = now;
}

/* How many faces round from where it started the cube is nearest to --
 * a whole number of steps, which may be negative or past a full turn.
 *
 * Kept as the count rather than reduced to a face index, because the
 * count is what the cube has to turn *to*. Reducing first and settling
 * on that face's own angle is a longer way round whenever the two differ
 * by a turn: at one step backwards the nearest face is the last one, and
 * aiming at its canonical angle sends the cube all the way forwards
 * through every other face to reach a position it is already at. */
static int nearest_turn(const CubeData *d)
{
    float step = 2.0f * (float)M_PI / (float)d->faces;
    return (int)lrintf(-d->angle / step);
}

/* And which face that is, which is the same count brought back into
 * range -- the only place the two should be confused is here. */
static int face_of_turn(const CubeData *d, int turn)
{
    int k = turn % d->faces;
    if (k < 0)
        k += d->faces;
    return k;
}

static void close_mode(CompEffect *e)
{
    CubeData *d = e->data;
    if (d->closing)
        return;

    if (!d->flick) {
        input_release();
        /* Shown back, and the pointer already free to move, the moment
         * the way out begins -- not only once the settle animation
         * finishes and the effect is torn down. The user let go of the
         * drag or picked a face; there is nothing left for the pointer
         * to do here. */
        input_cursor_hide(false);
    }

    /* The face it landed on, asked for rather than done: a compositor
     * does not switch desktops, it says which one the user chose and
     * the window manager decides what that means. */
    CompOutput *o = output_by_id(d->output_id);
    int turn = nearest_turn(d);
    int face = face_of_turn(d, turn);
    if (o && face != 0) {
        int want = (d->first_desktop + face) % d->faces;

/* The desktop change this is about to ask for is one the user has just
 * watched happen on the faces of the cube, so nothing else may animate
 * it -- without this the wall slides the new desktop in over the top of
 * the cube lying back down. Long enough to cover the way out and the
 * round trip the events take to come back through the window manager. */
        double cover = comp.claim_ms;
        effects_claim(COMP_EVENT_DESKTOP_LEAVE, o->id, cover);
        effects_claim(COMP_EVENT_DESKTOP_ENTER, o->id, cover);

        desktop_request_switch(o, want);
    }

    /* Settle onto that face on the way out, so the picture that lies back
     * down is the one being switched to -- at the turn it is nearest,
     * never at that face's canonical angle, which can be most of a
     * revolution away from where the cube is standing. */
    float step = 2.0f * (float)M_PI / (float)d->faces;
    d->angle_from = d->angle;
    d->angle_to = -step * (float)turn;
    d->angle_time = comp_now_ms();
    d->tilt_from = d->tilt;
    d->settling = true;
    d->dragging = false;

    d->closing = true;
    phase_to(d, 0.0f, comp_now_ms());
    active = NULL;
    mark_dirty(d);
}

/* ------------------------------------------------------------------ */
/* input                                                               */
/* ------------------------------------------------------------------ */

static void on_motion(void *data, int root_x, int root_y)
{
    CompEffect *e = data;
    CubeData *d = e->data;
    const CubeConfig *cfg = e->instance->config;
    CompOutput *o = output_by_id(d->output_id);
    if (!o || !d->dragging || d->closing)
        return;

    /* Every motion measured against the anchor and the pointer put back
     * on it, rather than against where the drag began.
     *
     * Turning a cube is something the user goes on doing past the point
     * where the cursor would have run into the side of the screen, and a
     * drag measured from its origin simply stops there -- the pointer
     * cannot move any further, so neither can the cube. Measuring the
     * step and giving the pointer back its place makes the travel
     * unbounded. The warp arrives as one more motion, at the anchor, a
     * distance of zero from it, so this does not feed back on itself.
     *
     * A margin-based version of this once warped only after the pointer
     * drifted a third of the output's shorter side, to cut down on
     * XWarpPointer round trips. That broke a drag started near the edge
     * of the screen: the real pointer gets clamped there by the X server
     * long before it drifts far enough to reach the margin, so it never
     * warps and the cube stops turning exactly the way it did with no
     * anchor at all. Warping every event is the only way that holds for
     * every starting position. */
    int dx = root_x - d->drag_x;
    int dy = root_y - d->drag_y;
    if (dx == 0 && dy == 0)
        return;
    input_pointer_warp(d->drag_x, d->drag_y);

    d->angle_target += (float)dx / (float)o->rect.w * cfg->turns * 2.0f * (float)M_PI;

    /* Pulling down leans the cube back, the way pulling the near edge of
     * a box towards you tips its top into view. The tilt stops at the
     * poles: past looking straight down there is nothing further to see,
     * only the cube upside down. */
    float limit = cfg->tilt_max * (float)M_PI / 180.0f;
    d->tilt_target -= (float)dy / (float)o->rect.h * limit * 2.0f;
    if (d->tilt_target > limit) d->tilt_target = limit;
    if (d->tilt_target < -limit) d->tilt_target = -limit;
}

/* Buttons 4/5 are the wheel (input.h) -- X reports a notch as a press
 * immediately followed by a release, so only the press need move the
 * eye, or the same notch would count twice. Up walks the eye in,
 * through the near face and on into the cube; down backs it out. */
static void cube_zoom_wheel(CompEffect *e, int dir)
{
    CubeData *d = e->data;
    const CubeConfig *cfg = e->instance->config;

    if (d->closing)
        return;

    d->zoom_target += cfg->zoom_step * (float)dir;
    if (d->zoom_target < cfg->zoom_min) d->zoom_target = cfg->zoom_min;
    if (d->zoom_target > cfg->zoom_max) d->zoom_target = cfg->zoom_max;

    mark_dirty(d);
}

static void on_button(void *data, int root_x, int root_y, uint8_t button,
                      bool pressed)
{
    CompEffect *e = data;
    (void)root_x;
    (void)root_y;

    if (button == 4 || button == 5) {
        if (pressed)
            cube_zoom_wheel(e, button == 4 ? -1 : 1);
        return;
    }

    /* The mode lasts exactly as long as the button is down -- it was
     * opened by the press, and letting go is the whole of choosing. */
    if (!pressed)
        close_mode(e);
}

static bool on_key(void *data, xcb_keysym_t sym, const char *text,
                   uint16_t mods)
{
    CompEffect *e = data;
    (void)text;
    (void)mods;

    if (sym == 0xff1b) {        /* Escape: leave it where it started */
        CubeData *d = e->data;
        d->angle = 0.0f;
        d->settling = false;
        close_mode(e);
        return true;
    }
    return false;
}

static const CompInputHandler cube_input = {
    .key    = on_key,
    .motion = on_motion,
    .button = on_button,
};

/* ------------------------------------------------------------------ */
/* animation                                                           */
/* ------------------------------------------------------------------ */

static float eased(const CompEffect *e, double since, double now)
{
    double len = effect_instance_duration(e->instance);
    if (len <= 0.0)
        len = 1.0;
    double t = (now - since) / len;
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    return comp_ease(e->instance->easing, (float)t);
}

/* The window last used on `desktop` of this output, by the compositor's
 * own focus record -- X keeps no focus history to read. What `active`
 * holds: one window is what makes a face recognisable, and holding one
 * is a fraction of the cost of holding all of them. */
static CompWindow *last_used_on(const CubeData *d, const CompOutput *o, int face)
{
    CompWindow *best = NULL;

    for (CompWindow *w = comp.stack; w; w = w->next) {
        if (w->input_only || w->zombie || w->wm_layer[0])
            continue;
        if (w->state & COMP_STATE_MINIMIZED)
            continue;               /* not on its face; cannot be held */
        if (face_of_window(d, o, w) != face)
            continue;
        if (!best || w->focus_serial > best->focus_serial)
            best = w;
    }
    return best;
}

/* Asks the window manager to keep the other desktops' windows up.
 *
 * Renewed rather than asked once: kiwm caps a hold at two seconds of its
 * own accord, and a cube is held open for as long as the user keeps
 * turning it. Not while the mode is closing -- everything is on its way
 * back to where it belongs and the faces are about to stop being looked
 * at. */
static void hold_live_windows(CompEffect *e, double now)
{
    CubeData *d = e->data;
    const CubeConfig *cfg = e->instance->config;
    CompOutput *o = output_by_id(d->output_id);

    if (!o || d->closing)
        return;
    if (d->held_at != 0.0 && now - d->held_at < HOLD_RENEW_MS)
        return;
    d->held_at = now;

    /* The other desktops' own wallpapers, asked for again and again
     * rather than once when the cube opened.
     *
     * kiwm raises a desktop layer for 700 ms, on the reasoning that a
     * compositor will have named its pixmap within that -- which is true
     * of the expo grid, where every cell is drawn from the first frame.
     * A cube culls the faces turned away from the viewer, so a wallpaper
     * whose face is at the back is never drawn, never named, and is back
     * down before it ever comes round. Renewing means that whenever a
     * face does turn to the front its layer is either up right now or
     * was up moments ago, and either way there is a picture of it. */
    desktop_request_prime();

    CompLiveWindows live = comp_live_windows_resolve(cfg->live);
    if (live == COMP_LIVE_DESKTOP)
        return;

    for (CompWindow *w = comp.stack; w; w = w->next) {
        if (w->input_only || w->zombie || w->wm_layer[0])
            continue;

        int face = face_of_window(d, o, w);
        if (face <= 0)
            continue;               /* every face, none, or the one in front */

        if (live == COMP_LIVE_ACTIVE && w != last_used_on(d, o, face))
            continue;

        desktop_request_hold(w, HOLD_MS);
    }
}

static void cube_update(CompEffect *e, double now)
{
    CubeData *d = e->data;

    hold_live_windows(e, now);

    /* Something on this output already damaged itself this tick -- a
     * window's own content, most likely, collected by damage_collect()
     * before update() runs. Left as just that window's rectangle, a
     * partial repaint of a scene under a 3D transform is only correct
     * when the transform itself is not also changing that same frame;
     * escalating it to the whole output whenever there is *any* damage
     * to begin with costs nothing extra on a tick with none. */
    CompOutput *ov = output_by_id(d->output_id);
    bool already_dirty = ov && ov->dirty;

    float phase = d->phase_from +
                  (d->phase_to - d->phase_from) * eased(e, d->phase_time, now);
    float angle = d->angle;
    float tilt = d->tilt;
    if (d->dragging && !d->settling) {
        /* Chase the drag's raw target by a fraction of the remaining
         * distance set by how much real time has passed, not by how many
         * motion events happened to arrive -- see DRAG_SMOOTH_MS. */
        double dt = now - d->drag_tick;
        if (dt < 0.0) dt = 0.0;
        d->drag_tick = now;
        float k = 1.0f - expf((float)(-dt / DRAG_SMOOTH_MS));
        angle = d->angle + (d->angle_target - d->angle) * k;
        tilt = d->tilt + (d->tilt_target - d->tilt) * k;
    } else if (d->settling) {
        float p = eased(e, d->angle_time, now);
        angle = d->angle_from + (d->angle_to - d->angle_from) * p;
        /* Level again as well as square on: the cube lines up with the
         * desktop in both directions on the way out, or it lies back
         * down still leaning and the desktop appears to drop into
         * place. */
        tilt = d->tilt_from * (1.0f - p);
    }

    /* The wheel's own chase, independent of dragging/settling -- it runs
     * whenever the target it set is not yet where the eye is drawn,
     * closing or not (closing still eases zoom back is not needed since
     * cube_open resets it fresh next time, but there is no reason to
     * freeze it either). */
    float zoom = d->zoom;
    if (zoom != d->zoom_target) {
        double zdt = now - d->zoom_tick;
        if (zdt < 0.0) zdt = 0.0;
        float zk = 1.0f - expf((float)(-zdt / ZOOM_SMOOTH_MS));
        zoom = d->zoom + (d->zoom_target - d->zoom) * zk;
    }
    d->zoom_tick = now;

    bool changed = phase != d->phase || angle != d->angle || tilt != d->tilt ||
                   zoom != d->zoom;
    d->phase = phase;
    d->angle = angle;
    d->tilt = tilt;
    d->zoom = zoom;

    /* Either this turned the cube itself, which always needs the whole
     * output repainted, or something else already did and that repaint
     * needs widening to the whole output too (the comment above). */
    if (changed || already_dirty)
        mark_dirty(d);

    /* A flick has no button to let go of: it is done when it has turned
     * as far as it was asked to, and closes itself. */
    if (d->flick && !d->closing && d->settling && angle == d->angle_to)
        close_mode(e);
}

static bool cube_finished(const CompEffect *e, double now)
{
    const CubeData *d = e->data;
    (void)now;
    return d->closing && d->phase <= 0.0f;
}

/* ------------------------------------------------------------------ */
/* drawing                                                             */
/* ------------------------------------------------------------------ */

/* Which face a window belongs on: the desktop it is on, counted round
 * from the one that was in front when the cube opened. -1 for a window
 * that has no face here -- one on another output, or on no desktop of
 * this one.
 *
 * Which output a window counts as being on is output_of()'s answer, the
 * same one show-windows, expo and the cover switcher use. Worth knowing
 * that kiwm decides it differently, by the output containing the
 * window's centre; the two disagree only for a window straddling two
 * monitors, and if that reads badly it is this one function to change. */
static int face_of_window(const CubeData *d, const CompOutput *o, CompWindow *w)
{
    CompRect r = window_rect(w);
    if (r.w <= 0 || r.h <= 0)
        return -1;

    CompOutput *ow = output_of(&r);
    if (!ow || ow->id != o->id)
        return -1;

    int desktop = 0, index = 0;
    if (!desktop_of_window(w, &desktop, &index))
        return -1;

    /* On every desktop, so it belongs to every face: the panels, and a
     * wallpaper that is published once for the screen rather than once
     * per desktop (plasmashell's is one window; xisback publishes a
     * layer per desktop, which lands in the ordinary case below). */
    if (desktop == COMP_DESKTOP_ALL)
        return CUBE_EVERY_FACE;
    if (desktop < 0 || desktop >= d->faces)
        return -1;

    return (desktop - d->first_desktop + d->faces) % d->faces;
}

static void cube_apply(CompEffect *e, CompScene *s, CompOutput *o)
{
    CubeData *d = e->data;
    const CubeConfig *cfg = e->instance->config;

    if (o->id != d->output_id) {
        /* The other monitors. A window of the cube's screen can hang
         * over the edge onto a neighbour, and that strip has nowhere to
         * go: the window is on a face of the cube now, turning with it,
         * and the piece left behind on the next monitor is a sliver of
         * something visibly somewhere else. So it fades out where it is
         * as the cube opens, and comes back as it closes. The other
         * monitor's own windows are not touched. */
        CompOutput *home = output_by_id(d->output_id);
        if (!home)
            return;
        for (int i = 0; i < s->count; i++) {
            CompSceneNode *n = &s->nodes[i];
            if (n->win->wm_layer[0])
                continue;
            if (face_of_window(d, home, n->win) == -1)
                continue;
            n->opacity *= 1.0f - d->phase;
        }
        return;
    }

    if (cfg->background > 0.0f)
        scene_set_backdrop(s, &o->rect, cfg->back_r, cfg->back_g, cfg->back_b,
                           cfg->background * d->phase);

    /* See-through while a *mouse* mode turns the cube: the shell fades
     * (veil is what its opacity is multiplied by, 1 solid, 0 gone), and
     * the faces turned away have their windows drawn too, so a drag
     * shows every desktop's windows at once. A keyed turn (flick) never
     * does this -- it is on its way to one face and back.
     *
     * Tied to `phase` rather than snapped on for as long as `dragging`
     * holds: the cube opens and closes on the same eased curve either
     * way, and riding it here means the shell fades in as the prism
     * stands up and fades back to solid as it lies back down, instead of
     * jumping straight to spin_transparency the instant the button goes
     * down and popping back to solid the instant it comes up. */
    /* The wheel has walked the eye past zero: it is inside the prism
     * now, among the faces rather than in front of them, so every face
     * has to carry its windows the way a turned-away one does while
     * spinning -- there is no "outside" left to be looking in from. */
    bool inside = d->zoom * d->phase < 0.0f;
    bool spin = (!d->flick && cfg->spin_transparency > 0.0f) || inside;
    float veil = spin ? 1.0f - cfg->spin_transparency * d->phase : 1.0f;

    /* Every face placed and ordered back to front -- there is no depth
     * buffer, so the order is the depth. Sorted by the w the projection
     * divides by, which is the distance the matrix itself reports. The
     * ones turned away carry no solid (you never see the back of a face),
     * but while the cube is see-through they still carry their windows --
     * `front` is which. A face the eye has already reached (cube_past_eye)
     * is dropped instead: its w has fallen through zero and there is no
     * projection left to draw, only a mirror of one. */
    struct { CompTransform t; CompRect rect; float depth; float near; int i; bool front; }
        vis[MAX_FACES + 2];
    int count = 0;

    for (int i = 0; i < d->faces && count < MAX_FACES; i++) {
        CompTransform t;
        face_transform(&t, o, cfg, d, i);
        if (cube_past_eye(&t, o))
            continue;
        bool front = face_faces_us(&t, &o->rect);
        if (!front && !spin)
            continue;
        vis[count].t = t;
        vis[count].rect = o->rect;
        vis[count].depth = face_depth(&t, o);
        vis[count].near = cube_near_solid(&t, o);
        vis[count].i = i;
        vis[count].front = front;
        count++;
    }

    /* The lid and the floor, in the same list so they sort by depth with
     * the sides rather than beside them. i < 0 marks them: they carry no
     * windows. Turned-away caps are never drawn, see-through or not. */
    if (cfg->shell && cfg->cap_a > 0.0f) {
        CompRect cr = cap_rect(o, d);
        for (int up = 1; up >= -1; up -= 2) {
            CompTransform t;
            cap_transform(&t, o, cfg, d, up);
            if (cube_past_eye(&t, o))
                continue;
            if (!face_faces_us(&t, &cr))
                continue;
            vis[count].t = t;
            vis[count].rect = cr;
            vis[count].depth = face_depth(&t, o);
            vis[count].near = cube_near_solid(&t, o);
            vis[count].i = -1;
            vis[count].front = true;
            count++;
        }
    }

    for (int a = 1; a < count; a++) {
        for (int b = a; b > 0 && vis[b].depth > vis[b - 1].depth; b--) {
            typeof(vis[0]) tmp = vis[b];
            vis[b] = vis[b - 1];
            vis[b - 1] = tmp;
        }
    }

    /* The prism first, then the windows: every face back to front --
     * the wallpaper and the panels lying on it, and the caps between
     * them by depth -- and only then every face's windows, again by the
     * depth of the face they float above.
     *
     * Not each face with its own windows immediately after it, which is
     * how this was first written, on the reasoning that a window floats
     * above its face and under every nearer one. What that got wrong is
     * that a window is not confined to its face: it floats *off* it
     * (window_gap), and it may hang past the desktop's edge, and both
     * of those reach into the neighbouring face's part of the screen --
     * where the nearer face, drawn afterwards, painted over them. A
     * window on the face turning away lost its edge to the face turning
     * in, and a window hanging past the desktop lost the part that hung,
     * at exactly the moment the turn made the next face the near one.
     * The faces of a convex prism never overlap on screen, so what
     * remains to order is the windows among themselves, and depth of
     * face does that: the near face's windows are drawn last and lie
     * over anything a far face's window reaches across.
     *
     * One window, several nodes. The scene is a list of things to draw
     * rather than a list of windows, so the panels and a wallpaper that
     * belongs to the screen instead of to one desktop -- plasmashell
     * publishes one such window, where xisback publishes a layer per
     * desktop -- are copied onto every side. Without that they appear on
     * one face and the rest of the cube is bare, which is exactly what
     * it looked like.
     *
     * The list is rebuilt rather than reordered: a node has to appear
     * more than once, and the order within a face has to be the
     * wallpaper, then the panels, then that face's windows -- except a
     * face turned away, where it is the windows first and the wallpaper
     * over them, since there we are looking at that face's back and its
     * own windows stand off the far side of it, away from us -- whatever
     * order the window manager stacked them in overall. */
    static CompSceneNode rebuilt[MAX_SCENE_NODES];
    static CompSceneNode faceset[MAX_SCENE_NODES];
    int n = 0;

    /* Pass -1 is a turned-away face's own windows -- drawn before even
     * the wallpaper, because what we are looking at is the *back* of
     * that face: the window stands off it on the far side, away from us,
     * and the wallpaper (its front) is what should cover it here, the
     * same way the wallpaper covers a front face's own windows from
     * behind. Pass 0 is what lies on the faces -- the wallpaper on every
     * face, front or back, and, unless told otherwise, the panels on the
     * front ones -- with the solids among them. Pass 1 is a front face's
     * windows, standing above all of that. */
    for (int pass = -1; pass < 2; pass++) {
        for (int k = 0; k < count; k++) {
            int face = vis[k].i;

            /* `veil` pulled towards solid as this particular face nears
             * the eye (cube_near_solid), so a face does not sit at a
             * constant spin_transparency right up to the frame it is
             * dropped -- it solidifies over CUBE_NEAR_FADE and only then
             * goes, which is what makes crossing into `inside` a fade
             * instead of the whole shell popping opaque in one tick. */
            float veil_k = veil + (1.0f - veil) * vis[k].near;

            /* The face's own backing quad, and the caps -- the shell.
             * A face turned away has none (you would be seeing its
             * inside), and while the cube is see-through what is drawn
             * fades by `veil`. */
            if (pass == 0 && vis[k].front && cfg->shell)
                scene_add_solid(s, &vis[k].rect, &vis[k].t,
                                cfg->cap_r, cfg->cap_g, cfg->cap_b,
                                cfg->cap_a * d->phase * veil_k, (float)n - 0.5f);

            if (face < 0)
                continue;               /* a cap carries nothing */

            int depth = 0;
            int fn = 0;
            for (int i = 0; i < s->count && fn < MAX_SCENE_NODES; i++) {
                CompSceneNode node = s->nodes[i];
                CompWindow *w = node.win;

                int owner = face_of_window(d, o, w);
                if (owner != face && owner != CUBE_EVERY_FACE)
                    continue;

                /* A minimized window is not on its desktop, and a face is
                 * that desktop as it would look. Its picture is in the
                 * scene (comp.h's keep_stowed, for the effects that are
                 * *about* what is put away), but drawn back onto the
                 * desktop it was taken off it shows a desktop nobody
                 * has. */
                if (w->state & COMP_STATE_MINIMIZED)
                    continue;

                bool flat = w->type == COMP_WINDOW_DESKTOP ||
                            (cfg->flat_docks && w->type == COMP_WINDOW_DOCK);
                if (flat) {
                    if (pass != 0)
                        continue;
                } else if (pass != (vis[k].front ? 1 : -1)) {
                    continue;
                }

                /* The panels are the shell too: they fade with it, and a
                 * turned-away face shows none, only its windows floating
                 * where the turn puts them. The wallpaper is not the
                 * shell, though drawn in the same pass as one -- it is
                 * what a face *is*, shell or no shell, so a turned-away
                 * face carries it as well (reached at all only because
                 * `spin` put that face in `vis[]` to begin with). The
                 * windows themselves stay solid whatever `veil` is. */
                bool wallpaper = w->type == COMP_WINDOW_DESKTOP;
                if (flat) {
                    if (!vis[k].front && !wallpaper)
                        continue;
                    node.opacity *= veil_k;
                }

                float off = flat ? 0.0f
                                 : d->window_gap +
                                   d->window_spacing * (float)depth;
                if (!flat)
                    depth++;

                CompTransform t;
                face_transform_at(&t, o, cfg, d, face, off * d->phase);
                if (cube_past_eye(&t, o))
                    continue;       /* this window's own plane, not just its face's, has been reached */
                node.transform = t;
                comp_transform_bbox(&t, &node.geometry, &node.visible_rect);
                faceset[fn++] = node;
            }

            /* Stacked bottom to top, `off` grows with height in the
             * stack, and that offset runs out along the face's own
             * normal -- towards whoever is looking at that face head
             * on. Drawn in that same order (topmost, biggest off,
             * last) is right when the face points at us: the nearest
             * window is painted last and so covers the rest, which is
             * what a painter's algorithm needs.
             *
             * A face turned away is only up at all because the shell
             * is see-through, and its own normal now points *away*
             * from the camera -- so the biggest off is the farthest
             * window here, not the nearest. Painting it last would
             * still draw it over the others. Run the same list back
             * to front instead: farthest (top of stack) first, then
             * nearer ones over it, which is the occlusion a viewer on
             * this side of the shell actually sees. */
            if (vis[k].front) {
                for (int j = 0; j < fn && n < MAX_SCENE_NODES; j++)
                    rebuilt[n++] = faceset[j];
            } else {
                for (int j = fn - 1; j >= 0 && n < MAX_SCENE_NODES; j--)
                    rebuilt[n++] = faceset[j];
            }
        }
    }

    /* Anything with no face at all -- another output's window, one whose
     * desktop is not among these -- is simply not in the new list, and
     * so is not drawn. */
    memcpy(s->nodes, rebuilt, sizeof(CompSceneNode) * (size_t)n);
    s->count = n;
}

static void cube_destroy(CompEffect *e)
{
    /* The other desktops go back to being invisible the moment this
     * stops drawing them; the holds themselves lapse on their own. */
    if (e->data)
        effects_show_stowed(((CubeData *)e->data)->output_id, false);
    input_cursor_hide(false);
    free(e->data);
    e->data = NULL;
}

/* The cube replaces the scene for its output: while it is up, nothing
 * else may draw there (effect.h). */
static int cube_owns_output(const CompEffect *e)
{
    const CubeData *d = e->data;
    return d ? d->output_id : COMP_NO_OUTPUT;
}

static const CompEffectOps cube_ops = {
    .name     = "cube",
    .owns_output = cube_owns_output,
    .rebuilds_scene = true,
    .update   = cube_update,
    .apply    = cube_apply,
    .finished = cube_finished,
    .destroy  = cube_destroy,
};

/* ------------------------------------------------------------------ */
/* the trigger                                                         */
/* ------------------------------------------------------------------ */

/* Opens the mode. `flick` is the keyboard's way in: it turns one face
 * and shuts itself, so it takes no grab and leaves the pointer alone --
 * there is no release to wait for, and a grab with nothing to end it is
 * a session that cannot be clicked. */
static void cube_open(const CompEffectInstance *self, bool flick)
{
    const CubeConfig *cfg = self->config;

    if (active)
        return;

    if (!renderer_is_projective()) {
        comp_log("cube: this renderer cannot draw perspective; not opening");
        return;
    }

    /* Asked afresh rather than taken from what was last seen. Which
     * desktop an output is on reaches the compositor as a property
     * change, so a cube opened in the moment after a switch -- which is
     * exactly what a second press of the turn key is -- would otherwise
     * build its faces starting from the desktop that has just been
     * left, and come up facing the wrong one. Before the count as well
     * as before the current one: both come from the same reading. */
    CompRect at0 = { 0, 0, 1, 1 };
    {
        int qx = 0, qy = 0;
        input_pointer_position(&qx, &qy);
        at0 = (CompRect){ qx, qy, 1, 1 };
    }
    CompOutput *o0 = output_of(&at0);
    /* One thing at a time takes the screen over: a cube, an expo grid, a
     * row of covers, a wall. Two of those at once is not a picture of
     * anything, so this simply does not open and the key does nothing
     * (effect.h). */
    if (o0 && effects_mode_running(o0->id, &cube_ops))
        return;

    desktop_refresh();

    int faces = desktop_count();
    if (faces < 2 || faces > MAX_FACES)
        return;                 /* one desktop is not a prism */

    int px = 0, py = 0;
    if (!input_pointer_position(&px, &py))
        return;
    CompRect at = { px, py, 1, 1 };
    CompOutput *o = output_of(&at);
    if (!o)
        return;

    CompEffect *e = calloc(1, sizeof(*e));
    CubeData *d = calloc(1, sizeof(*d));
    if (!e || !d) {
        free(e);
        free(d);
        return;
    }

    d->output_id = o->id;
    d->faces = faces;
    d->first_desktop = desktop_current_for_output(o);
    if (d->first_desktop < 0)
        d->first_desktop = 0;

    d->drag_x = px;
    d->drag_y = py;
    d->dragging = !flick;
    d->drag_tick = comp_now_ms();
    d->flick = flick;
    /* A keyed turn is the desktops sweeping past at full size, so the
     * windows lie on their faces for it: standing them off the surface
     * only means something when the cube has backed away far enough to
     * see that it has depth. */
    d->zoom = flick ? cfg->flick_zoom : cfg->zoom;
    d->zoom_target = d->zoom;
    d->zoom_tick = comp_now_ms();
    d->window_gap = flick ? 0.0f : cfg->window_gap;
    d->window_spacing = flick ? 0.0f : cfg->window_spacing;

    e->ops = &cube_ops;
    e->instance = self;
    e->window = NULL;
    e->start_time = comp_now_ms();
    e->duration = 0.0;
    e->data = d;

    phase_to(d, 1.0f, comp_now_ms());

    /* And say that this output is the one showing them.
     *
     * A window held up for a photograph, or stowed with the desktop it
     * belongs to, is deliberately kept out of the scene -- otherwise it
     * would appear on a desktop it is not on for as long as the hold
     * outlives whatever asked for it (scene.c). An effect that means to
     * draw those windows has to say so, and this is how: without it the
     * holds above are granted, the windows really are mapped again, and
     * the cube still draws empty faces, because the scene they would
     * have joined never let them in. */
    effects_show_stowed(o->id, true);

    /* Nothing here is pointed at -- the cube is turned by how far the
     * pointer has moved, never by what it is over -- so the arrow is
     * only something in the way. */
    if (!flick) {
        input_cursor_hide(true);
        if (!input_grab(&cube_input, e)) {
            effects_show_stowed(COMP_NO_OUTPUT, false);
            input_cursor_hide(false);
            free(d);
            free(e);
            return;
        }
    }

    (void)cfg;
    active = e;
    effects_add(e);
    mark_dirty(d);
}

/* One face over, with no drag: the cube opens, turns, and shuts itself.
 *
 * The same mode as the drag, not a second one -- so it holds the other
 * desktops up, draws their windows and lands on a face exactly as a
 * turned cube does, and a user who reaches for the key gets the same
 * picture as one who reaches for the mouse. While a drag is already
 * running it just aims the turn one face further, which is what pressing
 * the key mid-turn should obviously do.
 *
 * `by` is +1 for the next desktop and -1 for the previous. The cube
 * turns the other way from the desktop it is moving to: bringing the
 * next face round to the front means swinging the prism backwards. */
static void cube_step(const CompEffectInstance *self, int by)
{
    float step;

    if (!active) {
        cube_open(self, true);
        if (!active)
            return;
    }

    CubeData *d = active->data;
    if (d->closing)
        return;

    step = 2.0f * (float)M_PI / (float)d->faces;
    d->angle_from = d->angle;
    d->angle_to = (d->settling ? d->angle_to : d->angle) - step * (float)by;
    d->angle_time = comp_now_ms();
    d->tilt_from = d->tilt;
    d->settling = true;
    mark_dirty(d);
}

static void cube_next(void *data) { cube_step(data, +1); }
static void cube_prev(void *data) { cube_step(data, -1); }

/* A comma-separated list of specs, because the same action reached from
 * more than one combination is an ordinary thing to want. */
static void bind_keys(const char *spec, void (*fn)(void *),
                      const CompEffectInstance *self)
{
    if (!spec || !spec[0])
        return;

    char buf[128];
    snprintf(buf, sizeof(buf), "%s", spec);

    char *save = NULL;
    for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        while (*tok == ' ' || *tok == '\t')
            tok++;
        char *end = tok + strlen(tok);
        while (end > tok && (end[-1] == ' ' || end[-1] == '\t'))
            *--end = '\0';
        if (*tok)
            input_bind_hotkey(tok, fn, (void *)self);
    }
}

static void cube_press(void *data)
{
    cube_open(data, false);
}

static void cube_init(const CompEffectInstance *self)
{
    const CubeConfig *cfg = self->config;
    bound_instance = self;

    bind_keys(cfg->hotkey, cube_press, self);
    bind_keys(cfg->hotkey_next, cube_next, self);
    bind_keys(cfg->hotkey_prev, cube_prev, self);
}

/* ------------------------------------------------------------------ */
/* configuration                                                       */
/* ------------------------------------------------------------------ */

/* #rrggbb or #rrggbbaa. The alpha is optional because most colours here
 * are opaque and writing "ff" every time to say so is noise. */
static bool parse_colour(const char *v, float *r, float *g, float *b, float *a)
{
    unsigned rr, gg, bb, aa = 255;
    int n = sscanf(v, "#%2x%2x%2x%2x", &rr, &gg, &bb, &aa);
    if (n < 3)
        return false;
    *r = (float)rr / 255.0f;
    *g = (float)gg / 255.0f;
    *b = (float)bb / 255.0f;
    *a = (float)aa / 255.0f;
    return true;
}

static void cube_defaults(void *config)
{
    CubeConfig *c = config;
    snprintf(c->hotkey, sizeof(c->hotkey), "%s", "Ctrl+Meta+Button1");
    snprintf(c->hotkey_next, sizeof(c->hotkey_next), "%s", "Ctrl+Meta+Right");
    snprintf(c->hotkey_prev, sizeof(c->hotkey_prev), "%s", "Ctrl+Meta+Left");
    c->zoom = 0.55f;
    c->flick_zoom = 0.0f;
    c->perspective = 0.7f;
    c->zoom_step = 0.1f;
    c->zoom_min = -2.0f;
    c->zoom_max = 1.6f;
    c->turns = 1.0f;
    c->tilt_max = 90.0f;
    c->window_gap = 40.0f;
    c->window_spacing = 26.0f;
    c->cap_r = c->cap_g = c->cap_b = 0.22f;
    c->cap_a = 1.0f;
    c->back_r = c->back_g = c->back_b = 0.0f;
    c->background = 0.9f;
    c->live = COMP_LIVE_INHERIT;
    c->flat_docks = true;
    c->spin_transparency = 0.0f;
    c->shell = true;
}

static bool cube_config_key(void *config, const char *key, const char *value)
{
    CubeConfig *c = config;

    if (!strcmp(key, "hotkey")) {
        snprintf(c->hotkey, sizeof(c->hotkey), "%s", value);
        return true;
    }
    if (!strcmp(key, "hotkey_next")) {
        snprintf(c->hotkey_next, sizeof(c->hotkey_next), "%s", value);
        return true;
    }
    if (!strcmp(key, "hotkey_prev")) {
        snprintf(c->hotkey_prev, sizeof(c->hotkey_prev), "%s", value);
        return true;
    }
    if (!strcmp(key, "zoom"))        { c->zoom = (float)atof(value); return true; }
    if (!strcmp(key, "flick_zoom"))  { c->flick_zoom = (float)atof(value); return true; }
    if (!strcmp(key, "perspective")) { c->perspective = (float)atof(value); return true; }
    if (!strcmp(key, "zoom_step"))   { c->zoom_step = (float)atof(value); return true; }
    if (!strcmp(key, "zoom_min"))    { c->zoom_min = (float)atof(value); return true; }
    if (!strcmp(key, "zoom_max"))    { c->zoom_max = (float)atof(value); return true; }
    if (!strcmp(key, "turns"))       { c->turns = (float)atof(value); return true; }
    if (!strcmp(key, "tilt_max"))    { c->tilt_max = (float)atof(value); return true; }
    if (!strcmp(key, "window_gap"))     { c->window_gap = (float)atof(value); return true; }
    if (!strcmp(key, "window_spacing")) { c->window_spacing = (float)atof(value); return true; }
    if (!strcmp(key, "background"))  { c->background = (float)atof(value); return true; }
    if (!strcmp(key, "spin_transparency")) {
        float t = (float)atof(value);
        c->spin_transparency = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        return true;
    }
    if (!strcmp(key, "live_windows")) {
        int live = comp_live_windows_parse(value);
        if (live < 0) {
            fprintf(stderr, "kicomp: config: unknown live_windows '%s'\n", value);
            return false;
        }
        c->live = (CompLiveWindows)live;
        return true;
    }
    if (!strcmp(key, "flat_docks")) { c->flat_docks = atoi(value) != 0; return true; }
    if (!strcmp(key, "shell"))      { c->shell = atoi(value) != 0; return true; }
    if (!strcmp(key, "cap_color"))
        return parse_colour(value, &c->cap_r, &c->cap_g, &c->cap_b, &c->cap_a);
    if (!strcmp(key, "background_color")) {
        float ignored = 1.0f;
        return parse_colour(value, &c->back_r, &c->back_g, &c->back_b, &ignored);
    }
    return false;
}

const CompEffectModule effect_cube = {
    .name             = "cube",
    .default_enabled  = true,
    .default_duration = 1.2,
    .default_easing   = COMP_EASE_OUT,
    .default_events   = 0,
    .default_windows  = COMP_WINDOW_BIT(COMP_WINDOW_UNKNOWN) |
                        COMP_WINDOW_BIT(COMP_WINDOW_NORMAL) |
                        COMP_WINDOW_BIT(COMP_WINDOW_DIALOG) |
                        COMP_WINDOW_BIT(COMP_WINDOW_UTILITY),

    .init             = cube_init,
    .config_size      = sizeof(CubeConfig),
    .config_defaults  = cube_defaults,
    .config_key       = cube_config_key,
};
