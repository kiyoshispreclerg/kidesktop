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

    /* Draw the window's stashed contents (renderer.h) rather than its
     * live ones, with `geometry` describing those instead. What lets
     * shade roll up a window whose real pixmap is already just a
     * titlebar. */
    bool use_stash;

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

/* Moves a node within the scene, shifting the ones in between to fill the
 * gap. Drawing order only: the WM's stacking is untouched and the window
 * is still where the WM put it, exactly as a transform doesn't move a
 * window (section 27) -- this is the same lie told about z instead of
 * about x and y.
 *
 * What it exists for: a raise that should appear to happen *after*
 * something else. The WM raises and focuses in one gesture, so by the
 * time an effect runs the window is already on top; drawing it at its old
 * depth for a moment is the only way to show the sequence the user
 * actually caused. */
void scene_move_node(CompScene *s, int from, int to);

#endif /* KICOMP_SCENE_H */
