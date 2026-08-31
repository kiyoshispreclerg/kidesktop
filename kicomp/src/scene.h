/*
 * kicomp - the per-output intermediate scene (section 21).
 *
 * Effects will operate on this, never on X windows directly, and always
 * with an output in hand (section 25) -- which is why a node carries both
 * the window's full geometry and the portion visible on *this* output
 * (section 26). Nothing in this prototype produces a non-identity
 * transform yet; the fields exist so effects can be added later without
 * touching the core.
 */
#ifndef KICOMP_SCENE_H
#define KICOMP_SCENE_H

#include "comp.h"
#include "transform.h"

typedef struct CompSceneNode {
    CompWindow *win;

    CompRect geometry;      /* whole window, root coordinates */
    CompRect visible_rect;  /* what to draw, root coordinates -- geometry ∩
                             * output for an untransformed node, and
                             * whatever area the effect actually covers
                             * once a transform is in play */

    /* Root-to-root, applied to `geometry` (transform.h). Identity for
     * every node the effects didn't touch, which is the fast path in the
     * renderer. */
    CompTransform transform;

    float opacity;
    int z;                  /* 0 = bottom-most */
} CompSceneNode;

typedef struct CompScene {
    CompOutput *output;
    CompSceneNode nodes[MAX_SCENE_NODES];
    int count;
} CompScene;

/* Rebuilds `s` from the current window stack, keeping only what is
 * visible on `o`. Cheap enough to redo per frame at this stage;
 * incremental updates are a later optimization (section 47.7). */
void scene_build(CompScene *s, CompOutput *o);

#endif /* KICOMP_SCENE_H */
