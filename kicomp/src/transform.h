/*
 * kicomp - transforms (section 22).
 *
 * A 4x4 matrix from the start, even though the first renderer only needs
 * the 2D part of it: the point of the plan is that zoom, rotation, the
 * cube and the desktop wall don't get to redefine what a transform is
 * when they arrive. XRender consumes the affine 2D part
 * (comp_transform_invert_affine); a GL renderer would use the whole
 * thing unchanged.
 *
 * Convention: transforms map *root coordinates to root coordinates*, and
 * are applied to a scene node's geometry (see scene.h). Composition is
 * left-multiplication, so building "scale about a point" reads in the
 * usual order:
 *
 *     identity, then translate(-cx,-cy), scale(s,s), translate(cx,cy)
 */
#ifndef KICOMP_TRANSFORM_H
#define KICOMP_TRANSFORM_H

#include "comp.h"   /* CompRect */

#include <stdbool.h>

typedef struct CompTransform {
    float m[4][4];
} CompTransform;

void comp_transform_identity(CompTransform *t);
bool comp_transform_is_identity(const CompTransform *t);

/* Is this transform nothing but a move? Fills dx/dy when it is.
 *
 * Worth asking separately because a translation is the one transform that
 * a *region* survives: XFixes can translate a region but cannot scale one,
 * so a window being slid across the screen can keep its shape clip --
 * rounded corners, a shaped window's real silhouette -- while a window
 * being scaled cannot. Most of what the effects do is exactly this: dodge,
 * the desktop wall, smooth-move and the tail of a geometry change are all
 * pure moves. */
bool comp_transform_is_translation(const CompTransform *t, float *dx, float *dy);

/* Where a rectangle lands once the transform is applied to it.
 *
 * Only meaningful for the affine transforms the effects here build --
 * moves and scales -- which is why it maps two corners rather than four.
 * What it is for: a window being scaled loses its shape clip, because
 * neither XFixes nor a scissor box can scale a region, and a window whose
 * rectangle is far larger than what it draws then appears as its whole
 * rectangle full of whatever its pixmap happens to hold. Its *extents*
 * can be carried across, and that is the difference between VirtualBox's
 * mini-toolbar shrinking into an expo cell as a small bar and as a
 * screen-sized ghost. */
CompRect comp_transform_rect(const CompTransform *t, const CompRect *r);

/* out = a * b (apply b first, then a). Aliasing-safe. */
void comp_transform_multiply(CompTransform *out, const CompTransform *a, const CompTransform *b);

/* In-place: t = t composed with the new operation applied *after* it. */
void comp_transform_translate(CompTransform *t, float dx, float dy);
void comp_transform_scale(CompTransform *t, float sx, float sy);

void comp_transform_point(const CompTransform *t, float x, float y, float *ox, float *oy);

/* Axis-aligned bounding box of a transformed rectangle -- what an effect
 * needs to say "this is the area I now cover" (a scene node's
 * visible_rect, and the damage that goes with it). */
struct CompRect;
void comp_transform_bbox(const CompTransform *t, const struct CompRect *in,
                         struct CompRect *out);

/* Inverse of the affine 2D part (the only part XRender can express as a
 * picture transform, and enough for translate/scale/rotate/shear).
 * Returns false for a singular or non-affine matrix, in which case the
 * caller must fall back to drawing the node untransformed rather than
 * drawing garbage. */
bool comp_transform_invert_affine(const CompTransform *t, CompTransform *out);

#endif /* KICOMP_TRANSFORM_H */
