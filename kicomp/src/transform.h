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

#include <stdbool.h>

typedef struct CompTransform {
    float m[4][4];
} CompTransform;

void comp_transform_identity(CompTransform *t);
bool comp_transform_is_identity(const CompTransform *t);

/* out = a * b (apply b first, then a). Aliasing-safe. */
void comp_transform_multiply(CompTransform *out, const CompTransform *a, const CompTransform *b);

/* In-place: t = t composed with the new operation applied *after* it. */
void comp_transform_translate(CompTransform *t, float dx, float dy);
void comp_transform_scale(CompTransform *t, float sx, float sy);

void comp_transform_point(const CompTransform *t, float x, float y, float *ox, float *oy);

/* Inverse of the affine 2D part (the only part XRender can express as a
 * picture transform, and enough for translate/scale/rotate/shear).
 * Returns false for a singular or non-affine matrix, in which case the
 * caller must fall back to drawing the node untransformed rather than
 * drawing garbage. */
bool comp_transform_invert_affine(const CompTransform *t, CompTransform *out);

#endif /* KICOMP_TRANSFORM_H */
