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

typedef struct {
    char hotkey[128];

    /* How far the cube stands off from the eye once it is open, and how
     * strong the projection is -- both as fractions of the output's
     * width, so one setting looks the same on every monitor. */
    float zoom;
    float perspective;

    /* A drag across the whole output turns the cube this many times. */
    float turns;

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

    float cap_r, cap_g, cap_b;          /* the top and bottom */
    float back_r, back_g, back_b;       /* behind the cube */
    float background;                   /* how solid that is */
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

    /* The point the pointer is pinned to while the drag lasts: every
     * motion is measured from it and the pointer put back on it. */
    int drag_x, drag_y;
    bool dragging;

    float phase;                /* 0 the plain desktop, 1 the open cube */
    float phase_from, phase_to;
    double phase_time;
    bool closing;
} CubeData;

static const CompEffectOps cube_ops;
static CompEffect *active;
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
    float back = (float)o->rect.w * cfg->zoom * d->phase;

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
    (void)d;
    /* The cube covers the output and every frame of a turn changes all of
     * it; working out a smaller region would cost more than it saves. */
    output_damage_all();
}

static void phase_to(CubeData *d, float to, double now)
{
    d->phase_from = d->phase;
    d->phase_to = to;
    d->phase_time = now;
}

/* The face the cube is nearest to, and the turn that would centre it. */
static int nearest_face(const CubeData *d)
{
    float step = 2.0f * (float)M_PI / (float)d->faces;
    int k = (int)lrintf(-d->angle / step);
    k %= d->faces;
    if (k < 0)
        k += d->faces;
    return k;
}

static void close_mode(CompEffect *e)
{
    CubeData *d = e->data;
    if (d->closing)
        return;

    input_release();

    /* The face it landed on, asked for rather than done: a compositor
     * does not switch desktops, it says which one the user chose and
     * the window manager decides what that means. */
    CompOutput *o = output_by_id(d->output_id);
    int face = nearest_face(d);
    if (o && face != 0) {
        int want = (d->first_desktop + face) % d->faces;
        desktop_request_switch(o, want);
    }

    /* Settle onto that face on the way out, so the picture that lies back
     * down is the one being switched to. */
    float step = 2.0f * (float)M_PI / (float)d->faces;
    d->angle_from = d->angle;
    d->angle_to = -step * (float)face;
    d->angle_time = comp_now_ms();
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
     * distance of zero from it, so this does not feed back on itself. */
    int dx = root_x - d->drag_x;
    int dy = root_y - d->drag_y;
    if (dx == 0 && dy == 0)
        return;
    input_pointer_warp(d->drag_x, d->drag_y);

    d->angle += (float)dx / (float)o->rect.w * cfg->turns * 2.0f * (float)M_PI;

    /* Pulling down leans the cube back, the way pulling the near edge of
     * a box towards you tips its top into view. The tilt stops at the
     * poles: past looking straight down there is nothing further to see,
     * only the cube upside down. */
    float limit = cfg->tilt_max * (float)M_PI / 180.0f;
    d->tilt -= (float)dy / (float)o->rect.h * limit * 2.0f;
    if (d->tilt > limit) d->tilt = limit;
    if (d->tilt < -limit) d->tilt = -limit;

    mark_dirty(d);
}

static void on_button(void *data, int root_x, int root_y, uint8_t button,
                      bool pressed)
{
    CompEffect *e = data;
    (void)root_x;
    (void)root_y;
    (void)button;

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

static void cube_update(CompEffect *e, double now)
{
    CubeData *d = e->data;

    float phase = d->phase_from +
                  (d->phase_to - d->phase_from) * eased(e, d->phase_time, now);
    float angle = d->angle;
    if (d->settling)
        angle = d->angle_from +
                (d->angle_to - d->angle_from) * eased(e, d->angle_time, now);

    if (phase != d->phase || angle != d->angle) {
        d->phase = phase;
        d->angle = angle;
        mark_dirty(d);
    }
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

    /* Sticky: on every desktop, so it rides the face in front. */
    if (desktop == COMP_DESKTOP_ALL)
        return 0;
    if (desktop < 0 || desktop >= d->faces)
        return -1;

    return (desktop - d->first_desktop + d->faces) % d->faces;
}

static void cube_apply(CompEffect *e, CompScene *s, CompOutput *o)
{
    CubeData *d = e->data;
    const CubeConfig *cfg = e->instance->config;
    if (o->id != d->output_id)
        return;

    if (cfg->background > 0.0f)
        scene_set_backdrop(s, &o->rect, cfg->back_r, cfg->back_g, cfg->back_b,
                           cfg->background * d->phase);

    /* Every face placed, the ones turned away dropped, and the rest
     * ordered back to front -- there is no depth buffer, so the order is
     * the depth. Sorted by the w the projection divides by, which is the
     * distance the matrix itself reports. */
    struct { CompTransform t; float depth; int i; } vis[MAX_FACES];
    int count = 0;

    for (int i = 0; i < d->faces && count < MAX_FACES; i++) {
        CompTransform t;
        face_transform(&t, o, cfg, d, i);
        if (!face_faces_us(&t, &o->rect))
            continue;
        vis[count].t = t;
        vis[count].depth = face_depth(&t, o);
        vis[count].i = i;
        count++;
    }

    for (int a = 1; a < count; a++) {
        for (int b = a; b > 0 && vis[b].depth > vis[b - 1].depth; b--) {
            typeof(vis[0]) tmp = vis[b];
            vis[b] = vis[b - 1];
            vis[b - 1] = tmp;
        }
    }

    /* Faces back to front, and each one's windows immediately after it:
     * a window floats above its own face and so is drawn over it, and
     * under every face that is nearer than the one it belongs to. That
     * interleaving is the whole reason a solid carries a position in the
     * node order (scene.h) -- drawing all the faces and then all the
     * windows gets it exactly wrong the moment they overlap.
     *
     * The nodes are reordered so each face's windows sit together, and
     * the solids are given a z just before the first of them. */
    int placed = 0;                     /* nodes settled at the front */

    for (int k = 0; k < count; k++) {
        int face = vis[k].i;

        scene_add_solid(s, &o->rect, &vis[k].t,
                        cfg->cap_r, cfg->cap_g, cfg->cap_b, d->phase,
                        (float)placed - 0.5f);

        /* This face's windows, bottom of the stack first, each one a
         * little further off the surface than the last. */
        int depth = 0;
        for (int n = placed; n < s->count; n++) {
            CompSceneNode *node = &s->nodes[n];
            if (face_of_window(d, o, node->win) != face)
                continue;

            float off = cfg->window_gap + cfg->window_spacing * (float)depth;
            CompTransform t;
            face_transform_at(&t, o, cfg, d, face, off * d->phase);

            node->transform = t;
            comp_transform_bbox(&t, &node->geometry, &node->visible_rect);
            depth++;

            if (n != placed)
                scene_move_node(s, n, placed);
            placed++;
        }
    }

    /* Anything with no face on this cube -- another output's window, or
     * one whose desktop is not among these -- is not drawn. */
    for (int n = placed; n < s->count; n++)
        s->nodes[n].visible_rect = (CompRect){ 0, 0, 0, 0 };
}

static void cube_destroy(CompEffect *e)
{
    free(e->data);
    e->data = NULL;
}

static const CompEffectOps cube_ops = {
    .name     = "cube",
    .update   = cube_update,
    .apply    = cube_apply,
    .finished = cube_finished,
    .destroy  = cube_destroy,
};

/* ------------------------------------------------------------------ */
/* the trigger                                                         */
/* ------------------------------------------------------------------ */

static void cube_press(void *data)
{
    const CompEffectInstance *self = data;
    const CubeConfig *cfg = self->config;

    if (active)
        return;

    if (!renderer_is_projective()) {
        comp_log("cube: this renderer cannot draw perspective; not opening");
        return;
    }

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
    d->dragging = true;

    e->ops = &cube_ops;
    e->instance = self;
    e->window = NULL;
    e->start_time = comp_now_ms();
    e->duration = 0.0;
    e->data = d;

    phase_to(d, 1.0f, comp_now_ms());

    if (!input_grab(&cube_input, e)) {
        free(d);
        free(e);
        return;
    }

    (void)cfg;
    active = e;
    effects_add(e);
    mark_dirty(d);
}

static void cube_init(const CompEffectInstance *self)
{
    const CubeConfig *cfg = self->config;
    bound_instance = self;
    if (!cfg->hotkey[0])
        return;

    char buf[sizeof(cfg->hotkey)];
    snprintf(buf, sizeof(buf), "%s", cfg->hotkey);

    char *save = NULL;
    for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        while (*tok == ' ' || *tok == '\t')
            tok++;
        char *end = tok + strlen(tok);
        while (end > tok && (end[-1] == ' ' || end[-1] == '\t'))
            *--end = '\0';
        if (*tok)
            input_bind_hotkey(tok, cube_press, (void *)self);
    }
}

/* ------------------------------------------------------------------ */
/* configuration                                                       */
/* ------------------------------------------------------------------ */

static bool parse_colour(const char *v, float *r, float *g, float *b)
{
    unsigned rr, gg, bb;
    if (sscanf(v, "#%2x%2x%2x", &rr, &gg, &bb) != 3)
        return false;
    *r = (float)rr / 255.0f;
    *g = (float)gg / 255.0f;
    *b = (float)bb / 255.0f;
    return true;
}

static void cube_defaults(void *config)
{
    CubeConfig *c = config;
    snprintf(c->hotkey, sizeof(c->hotkey), "%s", "Ctrl+Meta+Button1");
    c->zoom = 0.9f;
    c->perspective = 1.4f;
    c->turns = 1.0f;
    c->tilt_max = 90.0f;
    c->window_gap = 40.0f;
    c->window_spacing = 26.0f;
    c->cap_r = c->cap_g = c->cap_b = 0.22f;
    c->back_r = c->back_g = c->back_b = 0.0f;
    c->background = 0.9f;
}

static bool cube_config_key(void *config, const char *key, const char *value)
{
    CubeConfig *c = config;

    if (!strcmp(key, "hotkey")) {
        snprintf(c->hotkey, sizeof(c->hotkey), "%s", value);
        return true;
    }
    if (!strcmp(key, "zoom"))        { c->zoom = (float)atof(value); return true; }
    if (!strcmp(key, "perspective")) { c->perspective = (float)atof(value); return true; }
    if (!strcmp(key, "turns"))       { c->turns = (float)atof(value); return true; }
    if (!strcmp(key, "tilt_max"))    { c->tilt_max = (float)atof(value); return true; }
    if (!strcmp(key, "window_gap"))     { c->window_gap = (float)atof(value); return true; }
    if (!strcmp(key, "window_spacing")) { c->window_spacing = (float)atof(value); return true; }
    if (!strcmp(key, "background"))  { c->background = (float)atof(value); return true; }
    if (!strcmp(key, "cap_color"))
        return parse_colour(value, &c->cap_r, &c->cap_g, &c->cap_b);
    if (!strcmp(key, "background_color"))
        return parse_colour(value, &c->back_r, &c->back_g, &c->back_b);
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
