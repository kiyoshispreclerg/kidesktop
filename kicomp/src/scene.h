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

    /* Where this node can actually be seen: visible_rect grown by the
     * shadow's reach, minus every opaque window drawn over it
     * (scene_cull_occluded). Root coordinates. A renderer clips the node
     * to this ∩ the frame's damage and draws nothing else -- the part of
     * a window behind an opaque one costs no pixels at all, which is the
     * difference between a compositor that costs what is on screen and
     * one that costs what is *visible*. Never empty for a node that is
     * still in the scene: an empty one is culled. */
    CompRegion clip;

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

/* A label an effect wants drawn over the scene: the filter box
 * show-windows types into, the name under each window in its grid.
 *
 * Deliberately not a node. A node is a *window* -- something the WM owns,
 * that the compositor is only redrawing -- and chrome is the opposite: it
 * exists only inside the effect, for as long as the effect does. Keeping
 * them apart means nothing in window.c, damage.c or the stacking code
 * ever has to know that text is a thing that can be on screen.
 *
 * The image is rendered once and kept by the effect (text.h); the scene
 * carries only where to put it. Always drawn over every window, in the
 * order the effect adds them, and never scaled -- text scaled by a
 * fraction is text nobody can read. */
#define MAX_SCENE_CHROME 96

typedef struct CompSceneChrome {
    struct CompTextImage *image;
    CompRect rect;          /* root coordinates; the image's own size */
    float opacity;
} CompSceneChrome;

/* An effect's own ground: a rectangle of colour under every window it is
 * drawing.
 *
 * For the modes that *replace* the desktop rather than rearrange it. The
 * expo grid is four little desktops with the real one still lying behind
 * them, which reads as a mess; on its own ground it reads as a place you
 * have gone to. And a ground is a layer, not a colour: it is where
 * anything belonging to the mode rather than to a window goes -- the
 * names of the desktops, the button to add one -- as those arrive.
 *
 * Like chrome (above), deliberately not a node: no window owns it, and
 * nothing outside the effect that asked for it needs to know it exists.
 * An empty rectangle means no ground at all, which is every frame of
 * every other effect. */
typedef struct CompSceneBackdrop {
    CompRect rect;          /* root coordinates; empty for none */
    float r, g, b;          /* straight, 0..1 -- not premultiplied */
    float opacity;
} CompSceneBackdrop;

typedef struct CompScene {
    CompOutput *output;
    CompSceneNode nodes[MAX_SCENE_NODES];
    int count;

    CompSceneChrome chrome[MAX_SCENE_CHROME];
    int chrome_count;

    CompSceneBackdrop backdrop;
} CompScene;

/* Adds one, ignoring the request when the scene is full or the image
 * never rendered -- a label is worth nothing to fail a frame over. */
void scene_add_chrome(CompScene *s, struct CompTextImage *image,
                      const CompRect *rect, float opacity);

/* Sets the ground for this frame. Cleared again by the next
 * scene_build(), like everything else an effect puts here. */
void scene_set_backdrop(CompScene *s, const CompRect *rect,
                        float r, float g, float b, float opacity);

/* Rebuilds `s` from the current window stack, keeping only what is
 * visible on `o`. Cheap enough to redo per frame at this stage;
 * incremental updates are a later optimization (section 47.7). */
void scene_build(CompScene *s, CompOutput *o);

/* Drops the nodes that something opaque completely covers, gives each
 * surviving node its `clip` (the part of it not under anything opaque),
 * records on each window what covers it (comp.h's cover/occluded) and on
 * the output what is covered at all (CompOutput's covered).
 *
 * Separate from scene_build, and called after the effects have run: they
 * move what covers what, and occlusion decided before that is occlusion
 * of a scene nobody is drawing. */
void scene_cull_occluded(CompScene *s, CompOutput *o);

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
