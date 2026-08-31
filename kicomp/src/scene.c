/* Per-output scene assembly (section 21).
 *
 * The rule this file exists to enforce: a window is never "on" an output,
 * it *intersects* one (section 26). A window straddling two monitors
 * produces one node in each output's scene, each with its own visible
 * rectangle -- and later its own transform and animation state.
 */
#include "scene.h"
#include "window.h"
#include "effect.h"

void scene_build(CompScene *s, CompOutput *o)
{
    s->output = o;
    s->count = 0;

    for (CompWindow *w = comp.stack; w; w = w->next) {
        /* An unmapped window is still drawn while an effect holds it --
         * that is the whole point of the retain (window.h): a fade-out
         * has to keep painting something X has already taken away. */
        if ((!w->mapped && w->retain_count == 0 && !w->pending_disappear) ||
            w->input_only)
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
        n->opacity = (float)w->opacity;
        n->z = s->count;
        s->count++;
    }
}
