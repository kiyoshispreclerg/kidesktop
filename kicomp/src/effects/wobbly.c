/*
 * wobbly: a window dragged around behaves like a sheet of something soft
 * -- the point you are holding follows the pointer, the rest of it lags
 * and catches up, and when you let go it swings past itself once or twice
 * before going rigid again.
 *
 * The model is kwin's (effects/wobblywindows), which is worth following
 * exactly rather than approximating, because what makes a wobble read as
 * cloth rather than as rubber is not the springs -- it is the two things
 * around them:
 *
 *   - the physics is coarse: a 4x4 grid of masses and no more. Sixteen
 *     points are cheap enough to integrate at a fixed step and stable at
 *     stiffnesses anyone will ask for.
 *   - what is *drawn* is not that grid. The sixteen points are the
 *     control net of a bicubic Bezier surface, and the mesh handed to the
 *     scene is that surface finely tessellated (scene.h's CompSceneMesh,
 *     the same one the magic lamp draws through). A simulated dense grid
 *     drawn straight looks like a trampoline; a coarse net drawn as a
 *     Bezier looks like a sheet.
 *
 * Each mass is pulled by its four neighbours toward keeping the spacing
 * it had when the window was rigid -- kwin writes that out per corner,
 * per edge and per inner point; here it is one loop over whichever
 * neighbours a point has, which comes to the same arithmetic. A point
 * marked `pinned` is pulled to where the window says it should be
 * instead, and that is the whole trick: while you drag, the point you
 * grabbed is pinned and so tracks the window exactly, and every other
 * point only hears about the move through the springs.
 *
 * Between the two passes of springs there is a smoothing pass over the
 * net (kwin's heightRingLinearMean): each value is averaged with its
 * eight neighbours, weighted to itself. Without it the corners ring
 * against each other and the sheet creases.
 *
 * Time, not frames (section 20). The integration runs in fixed 10 ms
 * steps however long the frame took, because a spring integrated with a
 * variable step is a spring whose stiffness depends on the frame rate.
 *
 * kicomp.conf:
 *
 *   [effect:wobbly]
 *   enabled      = 1
 *   events       = move
 *   windows      = windows
 *   stiffness    = 0.06     # how hard it pulls back to rigid
 *   drag         = 0.90     # how much speed survives each step
 *   move_factor  = 0.10     # how much of the speed becomes movement
 *   tessellation = 12       # cells per side of the drawn mesh (2..16)
 *   resize       = 0        # also wobble on resize drags, not just moves
 *
 * Only the GL backend draws a mesh; on XRender the node carries the plain
 * rectangle the mesh spans instead, so the window is drawn where it is
 * and simply does not bend.
 *
 * A resize can wobble too (resize=1), off by default since it is more
 * likely than a move to fight with whatever the window itself is doing
 * while being resized. The same single point is pinned (the one nearest
 * the pointer) exactly as for a move -- but that alone would let the
 * *whole* net drift on the still side, since nothing else anchors it
 * there. So resize mode adds a second mechanism on top: each side of the
 * window (top/bottom/left/right) starts the drag locked rigid, and stays
 * that way -- its rows or columns forced back to the window's own
 * rectangle every step, overriding the springs outright -- until that
 * side has actually moved away from where the grab found it, at which
 * point it latches free for the rest of the drag and is left to the
 * springs like everything else. This is kwin's own
 * can_wobble_top/bottom/left/right, copied rather than approximated: it
 * is what keeps the corner opposite the drag dead still while the one
 * under the pointer, and the side leading up to it, wobble.
 */
#include "../effect.h"
#include "../animation.h"
#include "../output.h"
#include "../window.h"
#include "../input.h"
#include "../transform.h"
#include "../scene.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* The control net. Four by four is kwin's, and it is also exactly what a
 * bicubic Bezier surface takes -- the net is the control net, with no
 * conversion in between. */
#define NET 4
#define NET_COUNT (NET * NET)

/* One integration step, in milliseconds. Fixed so the springs behave the
 * same whatever the frame rate, and small enough to stay stable at the
 * stiffnesses the config allows. kwin's is the same 10. */
#define STEP_MS 10.0

/* How long after the last drag step the window counts as let go. The same
 * gap window.c uses to decide a stream of configures is one interactive
 * drag, so the two agree about when a drag has ended. */
#define RELEASE_GAP_MS 140.0

/* kwin's bounds, in its units (dt in ms). Below the minimum a value is
 * taken as zero, above the maximum it is clamped; and once the whole net
 * is quieter than the stop values with nothing holding it, it is rigid
 * again and the effect is done. */
#define MIN_VEL 0.0f
#define MAX_VEL 1000.0f
#define STOP_VEL 0.5f
#define MIN_ACC 0.0f
#define MAX_ACC 1000.0f
#define STOP_ACC 0.5f

typedef struct {
    float stiffness;
    float drag;
    float move_factor;
    int tessellation;
    bool resize;    /* off by default: see kicomp.conf's resize= below */
} WobblyConfig;

typedef struct {
    float x, y;
} Vec;

typedef struct {
    const WobblyConfig *cfg;

    /* The net: where each mass is, where the window says it should be,
     * and how it is moving. */
    Vec pos[NET_COUNT];
    Vec prev[NET_COUNT];        /* pos before the last step, to draw between */
    Vec origin[NET_COUNT];
    Vec vel[NET_COUNT];
    Vec acc[NET_COUNT];
    Vec buffer[NET_COUNT];      /* the smoothing pass writes here */
    bool pinned[NET_COUNT];

    bool dragging;              /* still being moved, as opposed to settling */
    double last_move_ms;        /* when the last drag step arrived */
    double clock;               /* how far the integration has got */
    bool rigid;                 /* settled: nothing left to draw */

    /* Resize only: which sides are allowed to wobble. A move sets all
     * four true straight away; a resize starts with all four false and
     * `drag_rect` holding the rectangle as it was at the grab, and each
     * one latches true, permanently for the rest of the drag, the first
     * time that side's coordinate differs from `drag_rect` -- kwin's
     * can_wobble_top/bottom/left/right. Until a side latches, its rows or
     * columns are held rigid every step regardless of what the springs
     * computed (see integrate()), which is what keeps the still corner
     * of a resize dead still while the dragged one wobbles. */
    bool wobble_top, wobble_bottom, wobble_left, wobble_right;
    CompRect drag_rect;

    CompSceneMesh mesh;         /* the Bezier surface, handed to the scene */
    CompRect bbox;              /* what it covers, for damage and the clip */
    CompRect covered;           /* and what it covered last frame */
} WobblyData;

/* The windows wobbling right now. A drag is one window at a time, but a
 * window let go is still settling when the next one is grabbed, so there
 * has to be room for more than one. */
#define MAX_WOBBLY 8
static CompEffect *actives[MAX_WOBBLY];

static CompEffect *active_for(const CompWindow *w)
{
    for (int i = 0; i < MAX_WOBBLY; i++)
        if (actives[i] && actives[i]->window == w)
            return actives[i];
    return NULL;
}

static bool active_add(CompEffect *e)
{
    for (int i = 0; i < MAX_WOBBLY; i++) {
        if (!actives[i]) {
            actives[i] = e;
            return true;
        }
    }
    return false;
}

static void active_remove(const CompEffect *e)
{
    for (int i = 0; i < MAX_WOBBLY; i++)
        if (actives[i] == e)
            actives[i] = NULL;
}

static void config_defaults(void *config)
{
    WobblyConfig *c = config;
    /* kwin's middle preset: soft enough to see, stiff enough to settle
     * in well under a second. */
    c->stiffness = 0.06f;
    c->drag = 0.90f;
    c->move_factor = 0.10f;
    c->tessellation = 12;
    c->resize = false;
}

static bool config_key(void *config, const char *key, const char *value)
{
    WobblyConfig *c = config;

    if (strcmp(key, "stiffness") == 0) {
        float f = (float)atof(value);
        c->stiffness = f < 0.001f ? 0.001f : (f > 1.0f ? 1.0f : f);
        return true;
    }
    if (strcmp(key, "drag") == 0) {
        float f = (float)atof(value);
        c->drag = f < 0.0f ? 0.0f : (f > 0.999f ? 0.999f : f);
        return true;
    }
    if (strcmp(key, "move_factor") == 0) {
        float f = (float)atof(value);
        c->move_factor = f < 0.001f ? 0.001f : (f > 1.0f ? 1.0f : f);
        return true;
    }
    if (strcmp(key, "tessellation") == 0) {
        int t = atoi(value);
        if (t < 2) t = 2;
        if (t > MESH_MAX_COLS) t = MESH_MAX_COLS;
        c->tessellation = t;
        return true;
    }
    if (strcmp(key, "resize") == 0) {
        c->resize = atoi(value) != 0;
        return true;
    }
    return false;
}

/* Where the net's point (col, row) belongs when the window is rigid: its
 * own place in the window's rectangle. */
static Vec rest_at(const CompRect *r, int col, int row)
{
    Vec v;
    v.x = (float)r->x + (float)r->w * (float)col / (float)(NET - 1);
    v.y = (float)r->y + (float)r->h * (float)row / (float)(NET - 1);
    return v;
}

static void net_reset(WobblyData *d, const CompRect *r)
{
    for (int row = 0; row < NET; row++) {
        for (int col = 0; col < NET; col++) {
            int i = row * NET + col;
            d->pos[i] = d->prev[i] = d->origin[i] = rest_at(r, col, row);
            d->vel[i] = (Vec){ 0.0f, 0.0f };
            d->acc[i] = (Vec){ 0.0f, 0.0f };
            d->pinned[i] = false;
        }
    }
}

/* Below `min` is nothing, above `max` is `max` -- kwin's fixVectorBounds,
 * which is what stops a spring that has been given an impossible step
 * from throwing the net off screen. */
static void clamp_vec(Vec *v, float min, float max)
{
    if (fabsf(v->x) < min) v->x = 0.0f;
    else if (fabsf(v->x) > max) v->x = v->x > 0.0f ? max : -max;

    if (fabsf(v->y) < min) v->y = 0.0f;
    else if (fabsf(v->y) > max) v->y = v->y > 0.0f ? max : -max;
}

/* kwin's heightRingLinearMean: each value averaged with the neighbours it
 * has -- the eight around an inner point, fewer at an edge -- weighted to
 * itself by the neighbour count. The net rings without it. */
static void smooth(Vec *data, Vec *buffer)
{
    for (int row = 0; row < NET; row++) {
        for (int col = 0; col < NET; col++) {
            int i = row * NET + col;
            Vec sum = { 0.0f, 0.0f };
            int n = 0;

            for (int dr = -1; dr <= 1; dr++) {
                for (int dc = -1; dc <= 1; dc++) {
                    if (dr == 0 && dc == 0)
                        continue;
                    int r = row + dr, c = col + dc;
                    if (r < 0 || r >= NET || c < 0 || c >= NET)
                        continue;
                    sum.x += data[r * NET + c].x;
                    sum.y += data[r * NET + c].y;
                    n++;
                }
            }

            /* Weighted to itself by exactly the neighbour count, so the
             * mean is over 2n samples and the point keeps half the say --
             * the ratio kwin uses at every one of its cases (3/6 at a
             * corner, 5/10 at an edge, 8/16 inside). */
            buffer[i].x = (sum.x + (float)n * data[i].x) / (float)(2 * n);
            buffer[i].y = (sum.y + (float)n * data[i].y) / (float)(2 * n);
        }
    }
    memcpy(data, buffer, sizeof(Vec) * NET_COUNT);
}

/* One step of the model, `dt` milliseconds long, with the window where it
 * is now. Returns whether anything is still moving.
 *
 * The springs: every point is pulled by each neighbour it has toward
 * standing the rigid distance from it -- so the sum is zero exactly when
 * the net is the window's own grid, wherever that grid happens to be.
 * That is why a pinned point is what ties the sheet to the window: on its
 * own the springs only care about each other. */
static bool integrate(WobblyData *d, const CompRect *rect, float dt)
{
    const WobblyConfig *cfg = d->cfg;

    float x_len = (float)rect->w / (float)(NET - 1);
    float y_len = (float)rect->h / (float)(NET - 1);

    memcpy(d->prev, d->pos, sizeof(d->prev));

    for (int row = 0; row < NET; row++)
        for (int col = 0; col < NET; col++)
            d->origin[row * NET + col] = rest_at(rect, col, row);

    for (int row = 0; row < NET; row++) {
        for (int col = 0; col < NET; col++) {
            int i = row * NET + col;

            if (d->pinned[i]) {
                /* Held to the window: this is the point the pointer has,
                 * or -- once let go -- the middle, which is what settles
                 * the sheet back onto the window rather than leaving it
                 * to drift wherever the springs balance. */
                d->acc[i].x = (d->origin[i].x - d->pos[i].x) * cfg->stiffness;
                d->acc[i].y = (d->origin[i].y - d->pos[i].y) * cfg->stiffness;
                continue;
            }

            Vec a = { 0.0f, 0.0f };
            int n = 0;
            static const int dcol[4] = { -1, 1, 0, 0 };
            static const int drow[4] = { 0, 0, -1, 1 };

            for (int k = 0; k < 4; k++) {
                int c = col + dcol[k], r = row + drow[k];
                if (c < 0 || c >= NET || r < 0 || r >= NET)
                    continue;
                int j = r * NET + c;

                /* Where that neighbour should sit relative to this point
                 * when the window is rigid. */
                float rest_dx = (float)dcol[k] * x_len;
                float rest_dy = (float)drow[k] * y_len;

                a.x += ((d->pos[j].x - d->pos[i].x) - rest_dx) * cfg->stiffness;
                a.y += ((d->pos[j].y - d->pos[i].y) - rest_dy) * cfg->stiffness;
                n++;
            }

            if (n > 0) {
                a.x /= (float)n;
                a.y /= (float)n;
            }
            d->acc[i] = a;
        }
    }

    smooth(d->acc, d->buffer);

    float acc_sum = 0.0f, vel_sum = 0.0f;

    for (int i = 0; i < NET_COUNT; i++) {
        Vec a = d->acc[i];
        clamp_vec(&a, MIN_ACC, MAX_ACC);
        d->vel[i].x = a.x * dt + d->vel[i].x * cfg->drag;
        d->vel[i].y = a.y * dt + d->vel[i].y * cfg->drag;
        acc_sum += fabsf(a.x) + fabsf(a.y);
    }

    smooth(d->vel, d->buffer);

    for (int i = 0; i < NET_COUNT; i++) {
        clamp_vec(&d->vel[i], MIN_VEL, MAX_VEL);
        d->pos[i].x += d->vel[i].x * dt * cfg->move_factor;
        d->pos[i].y += d->vel[i].y * dt * cfg->move_factor;
        vel_sum += fabsf(d->vel[i].x) + fabsf(d->vel[i].y);
    }

    /* The still sides of a resize, overriding whatever the springs just
     * computed -- kwin's own post-pass. Each axis is independent and each
     * rule covers all but the one row/column nearest the side that *is*
     * allowed to wobble, so with neither side of an axis wobbling yet
     * every row or column on it ends up locked and that axis holds
     * perfectly rigid, and as soon as one side latches the lock backs off
     * to leave only the strip nearest the other side free. */
    for (int row = 0; row < NET; row++) {
        for (int col = 0; col < NET; col++) {
            int i = row * NET + col;
            if ((!d->wobble_top && row < NET - 1) || (!d->wobble_bottom && row > 0))
                d->pos[i].y = d->origin[i].y;
            if ((!d->wobble_left && col < NET - 1) || (!d->wobble_right && col > 0))
                d->pos[i].x = d->origin[i].x;
        }
    }

    return !(acc_sum < STOP_ACC && vel_sum < STOP_VEL);
}

/* The net read as a bicubic Bezier surface: the sixteen masses are its
 * control points, so this is one Bernstein sum per drawn vertex and no
 * conversion at all. What makes the sheet smooth however coarse the
 * physics is. */
static Vec bezier_at(const Vec *pos, float u, float v)
{
    float bu[NET], bv[NET];
    float iu = 1.0f - u, iv = 1.0f - v;

    bu[0] = iu * iu * iu;
    bu[1] = 3.0f * iu * iu * u;
    bu[2] = 3.0f * iu * u * u;
    bu[3] = u * u * u;

    bv[0] = iv * iv * iv;
    bv[1] = 3.0f * iv * iv * v;
    bv[2] = 3.0f * iv * v * v;
    bv[3] = v * v * v;

    Vec out = { 0.0f, 0.0f };
    for (int row = 0; row < NET; row++) {
        for (int col = 0; col < NET; col++) {
            float weight = bu[col] * bv[row];
            out.x += weight * pos[row * NET + col].x;
            out.y += weight * pos[row * NET + col].y;
        }
    }
    return out;
}

/* `alpha` is how far into the next step the frame falls: the net is drawn
 * between where the last step left it and where the one before did, so
 * the picture advances by exactly the frame's time however the fixed
 * steps happen to straddle it. Without it, at 60 Hz, successive frames
 * showed the simulation 20, 20, 10, 20, 20, 10 ms further along -- a
 * spring that jumps unevenly reads as a low frame rate however many
 * frames are drawn. (The usual "fix your timestep" answer; the state a
 * step behind is a lag nobody can see.) */
static void build_mesh(WobblyData *d, float alpha)
{
    CompSceneMesh *m = &d->mesh;
    int cols = m->cols, rows = m->rows;

    Vec pos[NET_COUNT];
    for (int i = 0; i < NET_COUNT; i++) {
        pos[i].x = d->prev[i].x + (d->pos[i].x - d->prev[i].x) * alpha;
        pos[i].y = d->prev[i].y + (d->pos[i].y - d->prev[i].y) * alpha;
    }

    float minx = 0.0f, miny = 0.0f, maxx = 0.0f, maxy = 0.0f;
    bool first = true;

    for (int gy = 0; gy <= rows; gy++) {
        float v = (float)gy / (float)rows;
        for (int gx = 0; gx <= cols; gx++) {
            float u = (float)gx / (float)cols;
            Vec p = bezier_at(pos, u, v);
            int i = gy * (cols + 1) + gx;
            m->x[i] = p.x;
            m->y[i] = p.y;

            if (first || p.x < minx) minx = p.x;
            if (first || p.x > maxx) maxx = p.x;
            if (first || p.y < miny) miny = p.y;
            if (first || p.y > maxy) maxy = p.y;
            first = false;
        }
    }

    d->bbox = (CompRect){ (int)minx - 1, (int)miny - 1,
                          (int)(maxx - minx) + 3, (int)(maxy - miny) + 3 };

    /* Marks this as new content, so the renderer uploads it once instead
     * of once per scissor piece it is drawn through (scene.h). */
    m->generation++;
}

/* Let go. Nothing about the springs changes: the point the pointer had
 * stays pinned, and since the window has stopped moving that point is
 * already sitting on its rest position, so it goes on anchoring the sheet
 * while everything still out of place swings back to it.
 *
 * kwin re-pins the middle of the net here instead, because it is told the
 * exact moment the button came up and the sheet is still fully stretched.
 * We only learn a drag ended by the moves stopping (RELEASE_GAP_MS), by
 * which time the sheet has partly settled -- and pinning four points that
 * are still a little displaced adds a pull that was not there a frame
 * earlier, which arrives as one late swing bigger than the wobble it is
 * supposed to be finishing. Leaving the pins alone is both simpler and
 * continuous: one anchored point is all it takes for the springs to bring
 * the whole net back onto the window's own grid. */
static void release(WobblyData *d)
{
    d->dragging = false;
}

static void wobbly_update(CompEffect *e, double now)
{
    WobblyData *d = e->data;
    CompRect rect = window_rect(e->window);

    if (d->dragging && now - d->last_move_ms > RELEASE_GAP_MS)
        release(d);

    /* Fixed steps, however long the frame was. A frame that took 40 ms
     * runs four of them; one that took 2 ms runs none and the net is
     * drawn between the last two states (build_mesh). The step is never
     * shortened to fit the remainder: this runs once per loop iteration,
     * which is many times a frame while events are arriving, and `drag`
     * is applied per step -- tiny steps would damp the sheet to a crawl
     * while events flow and let it jump whenever the loop sleeps. */
    bool moving = true;
    int guard = 0;
    while (now - d->clock >= STEP_MS && guard++ < 64) {
        d->clock += STEP_MS;
        moving = integrate(d, &rect, (float)STEP_MS);
    }
    if (guard >= 64)
        d->clock = now;          /* a long stall: don't try to catch up */

    /* Rigid again only once nothing is holding it: while the pointer has
     * it, a net that happens to be at rest is still a wobble waiting to
     * happen. */
    if (!d->dragging && !moving)
        d->rigid = true;

    float alpha = (float)((now - d->clock) / STEP_MS);
    if (alpha < 0.0f) alpha = 0.0f;
    if (alpha > 1.0f) alpha = 1.0f;
    build_mesh(d, alpha);

    /* One rect, not two: the old and new bbox overlap by most of their
     * area, and their union is one rectangle where two would be one plus
     * the slivers region_add cuts to keep them disjoint -- one scissor
     * pass for the renderer instead of several. */
    CompRect damage = {
        d->covered.x < d->bbox.x ? d->covered.x : d->bbox.x,
        d->covered.y < d->bbox.y ? d->covered.y : d->bbox.y,
        0, 0
    };
    int x1 = (d->covered.x + d->covered.w > d->bbox.x + d->bbox.w)
             ? d->covered.x + d->covered.w : d->bbox.x + d->bbox.w;
    int y1 = (d->covered.y + d->covered.h > d->bbox.y + d->bbox.h)
             ? d->covered.y + d->covered.h : d->bbox.y + d->bbox.h;
    damage.w = x1 - damage.x;
    damage.h = y1 - damage.y;
    output_damage_rect(&damage);
    d->covered = d->bbox;
}

static void wobbly_apply(CompEffect *e, CompScene *s, CompOutput *o)
{
    WobblyData *d = e->data;

    for (int i = 0; i < s->count; i++) {
        CompSceneNode *n = &s->nodes[i];
        if (n->win != e->window)
            continue;

        n->mesh = &d->mesh;

        /* What a backend that cannot draw a mesh gets instead: the
         * rectangle the sheet spans. It also keeps the scene from taking
         * the window's own rectangle for something that covers what is
         * behind it -- that test is "is the transform the identity"
         * (scene_cull_occluded), and a bent window's is not. */
        float sx = (float)d->bbox.w / (float)(n->geometry.w > 0 ? n->geometry.w : 1);
        float sy = (float)d->bbox.h / (float)(n->geometry.h > 0 ? n->geometry.h : 1);
        comp_transform_identity(&n->transform);
        comp_transform_translate(&n->transform, (float)-n->geometry.x, (float)-n->geometry.y);
        comp_transform_scale(&n->transform, sx, sy);
        comp_transform_translate(&n->transform, (float)d->bbox.x, (float)d->bbox.y);

        if (!rect_intersect(&d->bbox, &o->rect, &n->visible_rect))
            n->visible_rect = (CompRect){ 0, 0, 0, 0 };
        return;
    }
}

static bool wobbly_finished(const CompEffect *e, double now)
{
    const WobblyData *d = e->data;
    (void)now;
    return d->rigid;
}

static void wobbly_destroy(CompEffect *e)
{
    active_remove(e);
    free(e->data);
    e->data = NULL;
}

static const CompEffectOps wobbly_ops = {
    .name     = "wobbly",
    .update   = wobbly_update,
    .apply    = wobbly_apply,
    .finished = wobbly_finished,
    .destroy  = wobbly_destroy,
};

/* Called once, at the moment a fresh drag grabs the window (not on every
 * step): sets up which sides start out allowed to wobble.
 *
 * A move has no still side to keep rigid -- the whole net is free from
 * the first step, exactly as before this effect knew about resizes at
 * all. A resize starts with every side locked and `rect` kept as the
 * reference: nothing may wobble until wobble_update() below has seen
 * that side actually move away from where the grab found it. */
static void wobble_start(WobblyData *d, const CompRect *rect, bool is_resize)
{
    d->wobble_top = d->wobble_bottom = d->wobble_left = d->wobble_right = !is_resize;
    d->drag_rect = *rect;
}

/* Called on every step of a drag: once a side has moved from where the
 * grab found it, it stays free to wobble for the rest of the drag, even
 * if -- corner resizes do this constantly -- the pointer wanders back
 * near the start for a moment. Latching rather than re-checking each
 * step is what keeps a side from snapping rigid again mid-drag. */
static void wobble_update(WobblyData *d, const CompRect *rect)
{
    if (!d->wobble_top && rect->y != d->drag_rect.y)
        d->wobble_top = true;
    if (!d->wobble_left && rect->x != d->drag_rect.x)
        d->wobble_left = true;
    if (!d->wobble_right && (rect->x + rect->w) != (d->drag_rect.x + d->drag_rect.w))
        d->wobble_right = true;
    if (!d->wobble_bottom && (rect->y + rect->h) != (d->drag_rect.y + d->drag_rect.h))
        d->wobble_bottom = true;
}

/* The point of the net nearest the pointer, which is the one the user is
 * holding: a titlebar drag grabs a top edge, and the sheet should hang
 * from there rather than from the middle. Falls back to the top-left
 * corner when there is no answer about the pointer. */
static int pinned_for_pointer(const CompRect *r)
{
    int px = 0, py = 0;
    if (!input_pointer_position(&px, &py))
        return 0;

    float fx = r->w > 0 ? (float)(px - r->x) / (float)r->w : 0.0f;
    float fy = r->h > 0 ? (float)(py - r->y) / (float)r->h : 0.0f;

    int col = (int)(fx * (NET - 1) + 0.5f);
    int row = (int)(fy * (NET - 1) + 0.5f);
    if (col < 0) col = 0;
    if (col > NET - 1) col = NET - 1;
    if (row < 0) row = 0;
    if (row > NET - 1) row = NET - 1;

    return row * NET + col;
}

static void on_event(CompWindow *w, const CompEvent *event,
                     const CompEffectInstance *self)
{
    const WobblyConfig *cfg = self->config;

    /* A drag, either moving or resizing the window. A jump the WM made is
     * the geometry effect's business. Both pin the same single point
     * under the pointer; only a resize also arms the still-side lock
     * (wobble_start/wobble_update, integrate()). */
    if (!event->interactive || !w->mapped || w->input_only)
        return;

    CompRect rect = window_rect(w);
    if (rect.w <= 0 || rect.h <= 0)
        return;

    bool is_resize = event->to.w != event->from.w || event->to.h != event->from.h;
    if (is_resize && !cfg->resize)
        return;

    double now = comp_now_ms();

    CompEffect *e = active_for(w);
    if (e) {
        /* One more step of a drag already under way. Nothing to do but
         * say so: the window has moved, and the next integration reads
         * its new rectangle -- the pinned point follows it and the
         * springs drag the rest along, which *is* the wobble. A window
         * grabbed again while it was still settling picks up from
         * wherever the sheet had got to. */
        WobblyData *d = e->data;
        if (!d->dragging) {
            d->dragging = true;
            for (int i = 0; i < NET_COUNT; i++)
                d->pinned[i] = false;
            d->pinned[pinned_for_pointer(&rect)] = true;
            wobble_start(d, &rect, is_resize);
        }
        wobble_update(d, &rect);
        d->last_move_ms = now;
        return;
    }

    e = calloc(1, sizeof(*e));
    WobblyData *d = calloc(1, sizeof(*d));
    if (!e || !d) {
        free(e);
        free(d);
        return;
    }

    d->cfg = cfg;
    net_reset(d, &rect);
    d->pinned[pinned_for_pointer(&rect)] = true;
    wobble_start(d, &rect, is_resize);
    d->dragging = true;
    d->last_move_ms = now;
    d->clock = now;

    d->mesh.cols = cfg->tessellation;
    d->mesh.rows = cfg->tessellation;
    if (d->mesh.cols > MESH_MAX_COLS) d->mesh.cols = MESH_MAX_COLS;
    if (d->mesh.rows > MESH_MAX_ROWS) d->mesh.rows = MESH_MAX_ROWS;

    /* A wobbling window stays roughly rectangular, so its shadow can come
     * along under the rectangle the sheet spans (scene.h). */
    d->mesh.shadow = true;

    build_mesh(d, 1.0f);
    d->covered = d->bbox;

    e->ops = &wobbly_ops;
    e->instance = self;
    e->window = w;
    e->start_time = now;
    e->duration = 0.0;      /* the springs decide, not a stopwatch */
    e->data = d;

    if (!active_add(e)) {
        free(d);
        free(e);
        return;
    }

    effects_add(e);
}

const CompEffectModule effect_wobbly = {
    .name             = "wobbly",
    /* Off by default: it is the one effect that bends something the user
     * is actively holding, which is a taste rather than an improvement
     * everybody wants -- the same reason smooth-move is off. */
    .default_enabled  = false,
    .default_duration = 1.0,     /* logged only; the springs end it */
    .default_easing   = COMP_EASE_LINEAR,
    .default_events   = COMP_EVENT_BIT(COMP_EVENT_MOVE),
    .default_windows  = COMP_WINDOW_BIT(COMP_WINDOW_UNKNOWN) |
                        COMP_WINDOW_BIT(COMP_WINDOW_NORMAL) |
                        COMP_WINDOW_BIT(COMP_WINDOW_DIALOG) |
                        COMP_WINDOW_BIT(COMP_WINDOW_UTILITY) |
                        COMP_WINDOW_BIT(COMP_WINDOW_TOOLBAR),
    .config_size      = sizeof(WobblyConfig),
    .config_defaults  = config_defaults,
    .config_key       = config_key,
    .window_event     = on_event,
};
