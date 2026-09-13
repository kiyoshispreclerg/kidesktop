/*
 * magic-lamp: a window minimizing is sucked into its taskbar button like
 * a genie into a lamp -- the edge nearest the button narrows first and
 * pulls the rest of the window down a curving neck after it, and the
 * whole thing plays backwards on restore.
 *
 * The shape is not affine: every row across the window is a different
 * width and sits at a different place along the neck, so no single matrix
 * can say it. It is drawn as a deformed grid instead (scene.h's
 * CompSceneMesh) -- a column of quads, each one a piece of the window's
 * own pixmap, placed where this frame's neck puts it. Only the GL backend
 * draws the mesh; the node also carries the plain shrink-toward-the-button
 * that minimize does, which is what a backend without mesh support falls
 * back to.
 *
 * The model is Compiz's (animation plugin, magiclamp.c): the window and
 * the button define a funnel, a sigmoid gives the neck its curve, and the
 * animation runs in phases -- a window shapes into the neck, then stretches
 * down it, then the last of it is pulled through. Optional waves make the
 * neck ripple as it goes, off by default, the same as Compiz.
 *
 * Where the button is comes from `_NET_WM_ICON_GEOMETRY`, which a taskbar
 * publishes on each window it lists (xispanel's tasklist does); with none,
 * the window funnels to the bottom edge of its output, where a taskbar
 * most likely was -- the same guess minimize makes.
 *
 * kicomp.conf:
 *
 *   [effect:magic-lamp]
 *   enabled   = 1
 *   duration  = 2.0       # multiple of the global animation unit
 *   events    = minimize,restore
 *   grid_res  = 32        # rows the neck is cut into (4..48): more is
 *                         # smoother and a little dearer
 *   waves     = 0         # how many ripples travel the neck (0..12)
 *   wave_amp  = 0.4       # how far they push it, as a fraction of the
 *                         # window's width
 *
 * If fade-out or minimize also answer to `minimize`, turn them off for it
 * -- they would animate the same disappearance on top of this one.
 */
#include "../effect.h"
#include "../animation.h"
#include "../output.h"
#include "../window.h"
#include "../renderer.h"
#include "../transform.h"
#include "../scene.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

#define MAX_WAVES 12

typedef struct {
    int grid_res;
    int waves;
    float wave_amp;
} MagicLampConfig;

typedef struct {
    float amp;          /* how far it pushes, in window-width fractions */
    float pos;          /* where along the neck it sits, 0..1 */
    float half_width;   /* how much of the neck it spans */
} Wave;

typedef struct {
    const MagicLampConfig *cfg;
    bool going_away;            /* minimize, as opposed to restore */

    CompRect window;           /* where the window really is */
    CompRect icon;             /* the taskbar button it funnels into */
    bool to_top;               /* the button is above the window */

    int waves;
    Wave wave[MAX_WAVES];

    CompSceneMesh mesh;        /* filled every frame, handed to the scene */
    CompRect bbox;             /* what the mesh covers, for damage + clip */
    float opacity;

    CompRect covered;          /* last frame's bbox, for damage */
} MagicLampData;

static float frand(void)
{
    return (float)rand() / (float)RAND_MAX;
}

/* Compiz's sigmoid: a soft S from 0 to 1 across 0..1, steep in the
 * middle. The neck's curve is this, normalised to hit 0 and 1 exactly. */
static float sigmoid(float x)
{
    return 1.0f / (1.0f + expf(-10.0f * (x - 0.5f)));
}

/* Compiz's decelerate: eases the shaping phase so the window settles into
 * the neck rather than snapping into it. */
static float decelerate(float p)
{
    float x = 1.0f - p;
    float s = 8.0f;
    float a = 1.0f / (1.0f + expf(-s * 2.0f * ((0.5f + x * 0.25f) - 0.5f)));
    float b = 1.0f / (1.0f + expf(-s * 2.0f * (0.5f - 0.5f)));
    float c = 1.0f / (1.0f + expf(-s * 2.0f * (0.75f - 0.5f)));
    return 1.0f - (a - b) / (c - b);
}

static void config_defaults(void *config)
{
    MagicLampConfig *c = config;
    c->grid_res = 32;
    c->waves = 0;
    c->wave_amp = 0.4f;
}

static bool config_key(void *config, const char *key, const char *value)
{
    MagicLampConfig *c = config;

    if (strcmp(key, "grid_res") == 0) {
        int r = atoi(value);
        if (r < 4) r = 4;
        if (r > MESH_MAX_ROWS) r = MESH_MAX_ROWS;
        c->grid_res = r;
        return true;
    }
    if (strcmp(key, "waves") == 0) {
        int n = atoi(value);
        if (n < 0) n = 0;
        if (n > MAX_WAVES) n = MAX_WAVES;
        c->waves = n;
        return true;
    }
    if (strcmp(key, "wave_amp") == 0) {
        c->wave_amp = (float)atof(value);
        if (c->wave_amp < 0.0f) c->wave_amp = 0.0f;
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
 * is on, under its own centre -- the same fallback minimize uses. */
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

/* One step of the model: fills the mesh for progress `p`, where 0 is the
 * window whole and 1 is the window gone into the button. Compiz's
 * fxMagicLampModelStep, in kicomp's coordinates -- the node's geometry is
 * the framed window and its pixmap covers it, so a grid point at
 * (gx/cols, gy/rows) sits at window.x + gridx*window.w and so on, with no
 * decoration insets to unpick. */
static void model_step(MagicLampData *d, float p)
{
    CompSceneMesh *m = &d->mesh;
    int cols = m->cols, rows = m->rows;

    const CompRect *W = &d->window;
    const CompRect *I = &d->icon;

    float iconFarEndY, iconCloseEndY, winFarEndY, winVisibleCloseEndY;
    if (d->to_top) {
        iconFarEndY   = (float)I->y;
        iconCloseEndY = (float)(I->y + I->h);
        winFarEndY    = (float)(W->y + W->h);
        winVisibleCloseEndY = (float)W->y;
        if (winVisibleCloseEndY < iconCloseEndY)
            winVisibleCloseEndY = iconCloseEndY;
    } else {
        iconFarEndY   = (float)(I->y + I->h);
        iconCloseEndY = (float)I->y;
        winFarEndY    = (float)W->y;
        winVisibleCloseEndY = (float)(W->y + W->h);
        if (winVisibleCloseEndY > iconCloseEndY)
            winVisibleCloseEndY = iconCloseEndY;
    }

    const float preShapePhaseEnd = 0.22f;
    float stretchPhaseEnd =
        preShapePhaseEnd + (1.0f - preShapePhaseEnd) *
        (iconCloseEndY - winVisibleCloseEndY) /
        ((iconCloseEndY - winFarEndY) + (iconCloseEndY - winVisibleCloseEndY));
    if (stretchPhaseEnd < preShapePhaseEnd + 0.1f)
        stretchPhaseEnd = preShapePhaseEnd + 0.1f;

    float preShapeProgress = 0.0f, stretchProgress = 0.0f, postStretchProgress = 0.0f;

    if (p < preShapePhaseEnd) {
        preShapeProgress = p / preShapePhaseEnd;
        preShapeProgress = 1.0f - decelerate(1.0f - preShapeProgress);
        stretchProgress = p / stretchPhaseEnd;
    } else if (p < stretchPhaseEnd) {
        preShapeProgress = 1.0f;
        stretchProgress = p / stretchPhaseEnd;
    } else {
        preShapeProgress = 1.0f;
        stretchProgress = 1.0f;
        postStretchProgress = (p - stretchPhaseEnd) / (1.0f - stretchPhaseEnd);
    }

    float sig0 = sigmoid(0.0f), sig1 = sigmoid(1.0f);

    float minx = 0.0f, miny = 0.0f, maxx = 0.0f, maxy = 0.0f;
    bool first = true;

    for (int gy = 0; gy <= rows; gy++) {
        float gposy = (float)gy / (float)rows;
        for (int gx = 0; gx <= cols; gx++) {
            float gposx = (float)gx / (float)cols;
            int idx = gy * (cols + 1) + gx;

            float origx = (float)W->x + gposx * (float)W->w;
            float origy = (float)W->y + gposy * (float)W->h;
            float iconx = (float)I->x + gposx * (float)I->w;
            float icony = (float)I->y + gposy * (float)I->h;

            float stretchedPos = d->to_top
                ? gposy * origy + (1.0f - gposy) * icony
                : (1.0f - gposy) * origy + gposy * icony;

            float posy;
            if (p < stretchPhaseEnd) {
                posy = (1.0f - stretchProgress) * origy + stretchProgress * stretchedPos;
            } else {
                posy = (1.0f - postStretchProgress) * stretchedPos +
                       postStretchProgress * (stretchedPos + (iconCloseEndY - winFarEndY));
            }

            /* The neck's curve: how far down the funnel this row has got
             * decides how narrow it is, through the sigmoid. */
            float denom = (iconCloseEndY - winFarEndY);
            float fx = denom != 0.0f ? (iconCloseEndY - posy) / denom : 0.0f;
            float fy = (sigmoid(fx) - sig0) / (sig1 - sig0);
            float targetx = fy * (origx - iconx) + iconx;

            for (int wv = 0; wv < d->waves; wv++) {
                float cosfx = (fx - d->wave[wv].pos) / d->wave[wv].half_width;
                if (cosfx < -1.0f || cosfx > 1.0f)
                    continue;
                targetx += d->wave[wv].amp * (float)W->w *
                           (cosf(cosfx * (float)M_PI) + 1.0f) / 2.0f;
            }

            float posx = (p < preShapePhaseEnd)
                ? (1.0f - preShapeProgress) * origx + preShapeProgress * targetx
                : targetx;

            if (d->to_top) {
                if (posy < iconFarEndY) posy = iconFarEndY;
            } else {
                if (posy > iconFarEndY) posy = iconFarEndY;
            }

            m->x[idx] = posx;
            m->y[idx] = posy;

            if (first || posx < minx) minx = posx;
            if (first || posx > maxx) maxx = posx;
            if (first || posy < miny) miny = posy;
            if (first || posy > maxy) maxy = posy;
            first = false;
        }
    }

    d->bbox = (CompRect){ (int)minx - 1, (int)miny - 1,
                          (int)(maxx - minx) + 3, (int)(maxy - miny) + 3 };
}

/* The raw 0..1 of the animation, where 0 is the window whole and 1 is it
 * gone. Restore is the same shape played backwards. Linear on purpose:
 * the model does its own phase shaping (model_step), so easing it here
 * would ease it twice. */
static float lamp_progress(CompEffect *e)
{
    float raw = comp_progress(comp_now_ms(), e->start_time, e->duration);
    MagicLampData *d = e->data;
    return d->going_away ? raw : 1.0f - raw;
}

static void magic_lamp_update(CompEffect *e, double now)
{
    (void)now;
    MagicLampData *d = e->data;

    model_step(d, lamp_progress(e));

    output_damage_rect(&d->covered);
    output_damage_rect(&d->bbox);
    d->covered = d->bbox;
}

static void magic_lamp_apply(CompEffect *e, CompScene *s, CompOutput *o)
{
    MagicLampData *d = e->data;

    for (int i = 0; i < s->count; i++) {
        CompSceneNode *n = &s->nodes[i];
        if (n->win != e->window)
            continue;

        n->mesh = &d->mesh;

        /* The plain shrink toward the button, for a backend that cannot
         * draw the mesh -- and, just as much, so the scene sees a
         * non-identity transform and does not mistake the window's real
         * rectangle (where nothing is being drawn) for something that
         * covers what is behind it (scene_cull_occluded). */
        float sx = (float)d->bbox.w / (float)(n->geometry.w > 0 ? n->geometry.w : 1);
        float sy = (float)d->bbox.h / (float)(n->geometry.h > 0 ? n->geometry.h : 1);
        comp_transform_identity(&n->transform);
        comp_transform_translate(&n->transform, (float)-n->geometry.x, (float)-n->geometry.y);
        comp_transform_scale(&n->transform, sx, sy);
        comp_transform_translate(&n->transform, (float)d->bbox.x, (float)d->bbox.y);

        n->opacity *= d->opacity;

        if (!rect_intersect(&d->bbox, &o->rect, &n->visible_rect))
            n->visible_rect = (CompRect){ 0, 0, 0, 0 };
        return;
    }
}

static bool magic_lamp_finished(const CompEffect *e, double now)
{
    return now >= e->start_time + e->duration;
}

static void magic_lamp_destroy(CompEffect *e)
{
    MagicLampData *d = e->data;
    if (d && d->going_away)
        window_release(e->window);
    free(e->data);
    e->data = NULL;
}

static const CompEffectOps magic_lamp_ops = {
    .name     = "magic-lamp",
    .update   = magic_lamp_update,
    .apply    = magic_lamp_apply,
    .finished = magic_lamp_finished,
    .destroy  = magic_lamp_destroy,
};

static void init_waves(MagicLampData *d)
{
    d->waves = d->cfg->waves;
    if (d->waves <= 0)
        return;

    /* Compiz's waves: each a raised cosine over a slice of the neck, of
     * alternating sign so the neck snakes rather than bulging one way. */
    int dir = frand() < 0.5f ? 1 : -1;
    const float min_hw = 0.22f, max_hw = 0.38f;
    for (int i = 0; i < d->waves; i++) {
        d->wave[i].amp = dir * d->cfg->wave_amp * (0.5f + 0.5f * frand());
        d->wave[i].half_width = min_hw + frand() * (max_hw - min_hw);

        float avail = 1.0f - 2.0f * d->wave[i].half_width;
        float in_seg = (i > 0) ? (avail / d->waves) * frand() : 0.0f;
        d->wave[i].pos = in_seg + i * avail / d->waves + d->wave[i].half_width;
        dir *= -1;
    }
}

static void on_event(CompWindow *w, const CompEvent *event,
                     const CompEffectInstance *self)
{
    bool going_away = (event->kind == COMP_EVENT_MINIMIZE);

    /* Minimizing draws a window X has already unmapped, so the contents
     * bound before it went away are all there will ever be. Restoring is
     * the opposite -- the pixmap is deliberately unbound, to be named
     * fresh -- so asking for contents there would refuse every restore. */
    if (going_away && !renderer_window_has_content(w))
        return;

    double duration = effect_instance_duration(self);
    if (duration <= 0.0)
        return;

    CompRect win = window_rect(w);
    if (win.w <= 0 || win.h <= 0)
        return;

    CompEffect *e = calloc(1, sizeof(*e));
    MagicLampData *d = calloc(1, sizeof(*d));
    if (!e || !d) {
        free(e);
        free(d);
        return;
    }

    const MagicLampConfig *cfg = self->config;

    d->cfg = cfg;
    d->going_away = going_away;
    d->window = win;
    if (!icon_geometry(w, &d->icon))
        d->icon = fallback_target(&win);
    d->to_top = (win.y + win.h / 2) > (d->icon.y + d->icon.h / 2);
    d->opacity = 1.0f;

    d->mesh.cols = 2;
    d->mesh.rows = cfg->grid_res;
    if (d->mesh.rows > MESH_MAX_ROWS) d->mesh.rows = MESH_MAX_ROWS;
    if (d->mesh.rows < 4) d->mesh.rows = 4;

    init_waves(d);

    /* The neck as it starts, so the first frame is not a blank one. */
    model_step(d, going_away ? 0.0f : 1.0f);
    d->covered = d->bbox;

    e->ops = &magic_lamp_ops;
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

const CompEffectModule effect_magic_lamp = {
    .name             = "magic-lamp",
    /* Off by default: `minimize` is on, and the two answer the same
     * events -- turning this on means turning that off (README). */
    .default_enabled  = false,
    .default_duration = 2.0,
    .default_easing   = COMP_EASE_LINEAR,
    .default_events   = COMP_EVENT_BIT(COMP_EVENT_MINIMIZE) |
                        COMP_EVENT_BIT(COMP_EVENT_RESTORE),
    .default_windows  = COMP_WINDOW_BIT(COMP_WINDOW_UNKNOWN) |
                        COMP_WINDOW_BIT(COMP_WINDOW_NORMAL) |
                        COMP_WINDOW_BIT(COMP_WINDOW_DIALOG) |
                        COMP_WINDOW_BIT(COMP_WINDOW_UTILITY) |
                        COMP_WINDOW_BIT(COMP_WINDOW_TOOLBAR),
    .config_size      = sizeof(MagicLampConfig),
    .config_defaults  = config_defaults,
    .config_key       = config_key,
    .window_event     = on_event,
};
