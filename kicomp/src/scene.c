/* Per-output scene assembly (section 21).
 *
 * The rule this file exists to enforce: a window is never "on" an output,
 * it *intersects* one (section 26). A window straddling two monitors
 * produces one node in each output's scene, each with its own visible
 * rectangle -- and later its own transform and animation state.
 */
#include "scene.h"
#include "shadow.h"
#include "window.h"
#include "window.h"
#include "effect.h"

#include <string.h>

/* How many covering rectangles are carried while culling. Small on
 * purpose: the case worth catching is one big opaque window over the
 * others, and every extra rectangle is another containment test per
 * window per frame. */
#define MAX_COVERS 4

/* Is `inner` entirely inside `outer`? */
static bool rect_contains(const CompRect *outer, const CompRect *inner)
{
    return inner->x >= outer->x && inner->y >= outer->y &&
           inner->x + inner->w <= outer->x + outer->w &&
           inner->y + inner->h <= outer->y + outer->h;
}

void scene_build(CompScene *s, CompOutput *o)
{
    s->output = o;
    s->count = 0;

    /* The lens starts clean every frame; an effect that wants one sets
     * it in apply(), which runs before anything is drawn. */
    comp_transform_identity(&o->view);
    s->chrome_count = 0;

    for (CompWindow *w = comp.stack; w; w = w->next) {
        /* An unmapped window is still drawn while an effect holds it --
         * that is the whole point of the retain (window.h): a fade-out
         * has to keep painting something X has already taken away. */
        if ((!w->mapped && w->retain_count == 0 && !w->pending_disappear) ||
            w->input_only)
            continue;

        /* A window whose picture is merely being *kept* (comp.h's
         * keep_stowed: minimized, or on a desktop that isn't showing) is
         * not on screen and must not be drawn as though it were. It
         * joins the scene only while an effect is showing what isn't
         * there -- the expo grid -- and leaves it again the moment that
         * effect is done.
         *
         * "Merely" is the whole condition, and retain_count is what says
         * so: stowing takes exactly one retain of its own, so anything
         * above that is an effect still animating the window -- the
         * minimize shrinking it towards its taskbar button, a fade-out
         * seeing it off. Those have to keep being drawn, and leaving
         * this test at "is it stowed" is what stopped the minimize
         * animation from appearing at all: the window was marked the
         * instant it went, and the effect was left with nothing on
         * screen to animate. */
        if (w->stowed && !comp.show_stowed && w->retain_count <= 1)
            continue;
        if (w->opacity <= 0.0)
            continue;
        /* kiwm's own overlay layers (switcher, wireframe), left out when
         * the compositor means to draw its own version of them. kiwm is
         * never told: it keeps drawing them exactly as it does with no
         * compositor at all, they just never reach the scene. */
        if (comp.skip_wm_layers && w->wm_layer[0])
            continue;

        CompRect geom = window_rect(w);
        CompRect vis;
        if (!rect_intersect(&geom, &o->rect, &vis)) {
            /* Off this output -- unless something is animating, in which
             * case an effect may still be drawing it here (a window
             * sliding in from the neighbouring monitor). Keep the node
             * with an empty visible rect and let the effect fill it in;
             * the renderer skips nodes nothing claimed. */
            if (!effects_active())
                continue;
            vis = (CompRect){ 0, 0, 0, 0 };
        }

        if (s->count >= MAX_SCENE_NODES)
            break;

        CompSceneNode *n = &s->nodes[s->count];
        n->win = w;
        n->geometry = geom;
        n->visible_rect = vis;
        comp_transform_identity(&n->transform);
        n->use_stash = false;
        n->opacity = (float)w->opacity;
        n->z = s->count;
        s->count++;
    }

    /* And now leave out what nobody can see.
     *
     * Walking from the top down, each window that is certainly opaque
     * covers the ones below it; any of those whose whole visible
     * rectangle -- grown by the shadow's reach, since a window paints
     * outside itself -- falls inside one of those covers is not drawn at
     * all. That is the difference between a compositor that costs the
     * same whatever is on screen and one that costs what is visible, and
     * it is why a window behind another one gets cheaper on desktops
     * that do it.
     *
     * Deliberately conservative in three ways: only against a *single*
     * covering rectangle rather than the union of several (a window
     * hidden by two overlapping ones stays drawn), only for untransformed
     * fully opaque nodes, and only using the part of a window we are sure
     * about (window.h's opaque -- the client inside a frame, never the
     * frame itself, which under kiwm is translucent). A window wrongly
     * culled disappears; a window wrongly kept merely costs what it
     * costs today. */
    int reach = shadow_margin();
    CompRect cover[MAX_COVERS];
    int covers = 0;

    for (int i = s->count - 1; i >= 0; i--) {
        CompSceneNode *n = &s->nodes[i];

        CompRect probe = n->visible_rect;
        probe.x -= reach;
        probe.y -= reach;
        probe.w += reach * 2;
        probe.h += reach * 2;

        bool hidden = false;
        for (int c = 0; c < covers && !hidden; c++)
            hidden = rect_contains(&cover[c], &probe);

        if (hidden) {
            /* Out of the list entirely: the renderers never learn that a
             * window was left out, which is what keeps this in one
             * place. */
            memmove(&s->nodes[i], &s->nodes[i + 1],
                    sizeof(CompSceneNode) * (size_t)(s->count - i - 1));
            s->count--;
            continue;
        }

        if (covers < MAX_COVERS && n->opacity >= 1.0f &&
            comp_transform_is_identity(&n->transform)) {
            CompRect op = window_opaque_rect(n->win);
            CompRect vis;
            if (op.w > 0 && op.h > 0 && rect_intersect(&op, &o->rect, &vis))
                cover[covers++] = vis;
        }
    }
}

void scene_move_node(CompScene *s, int from, int to)
{
    if (from == to || from < 0 || to < 0 || from >= s->count || to >= s->count)
        return;

    CompSceneNode moved = s->nodes[from];

    if (to < from)
        memmove(&s->nodes[to + 1], &s->nodes[to],
                sizeof(CompSceneNode) * (size_t)(from - to));
    else
        memmove(&s->nodes[from], &s->nodes[from + 1],
                sizeof(CompSceneNode) * (size_t)(to - from));

    s->nodes[to] = moved;

    /* z is the node's own record of its depth; the array order is what
     * the renderer draws by. Keep the two saying the same thing. */
    for (int i = 0; i < s->count; i++)
        s->nodes[i].z = i;
}

void scene_add_chrome(CompScene *s, struct CompTextImage *image,
                      const CompRect *rect, float opacity)
{
    if (!s || !image || !rect || s->chrome_count >= MAX_SCENE_CHROME)
        return;
    if (rect->w <= 0 || rect->h <= 0 || opacity <= 0.0f)
        return;

    CompSceneChrome *c = &s->chrome[s->chrome_count++];
    c->image = image;
    c->rect = *rect;
    c->opacity = opacity;
}
