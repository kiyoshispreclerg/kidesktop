/*
 * kicomp - a small client-side region: what changed, as a handful of
 * rectangles.
 *
 * Deliberately not an XFixes region. A region kept on the server is the
 * natural thing for the XRender backend -- it clips a Picture directly and
 * never needs a round trip -- and exactly the wrong thing for every other
 * backend, since reading it back costs a round trip per frame. What both
 * need is the *answer*: which rectangles of this output changed. So the
 * core keeps that in plain memory, every effect and every event posts into
 * it, and each renderer turns it into whatever its API wants (an XFixes
 * region here, a scissor rectangle or a set of quads in GL) once per frame.
 *
 * The rectangle count is capped: past COMP_REGION_MAX the region collapses
 * to its bounding box. Tracking twenty separate little rectangles costs
 * more to carry through the renderer than repainting the box that contains
 * them, and a frame that damaged twenty places is usually one where nearly
 * everything moved anyway.
 */
#ifndef KICOMP_REGION_H
#define KICOMP_REGION_H

#include "comp.h"

/* CompRegion itself is in comp.h -- an output carries one. `full` is
 * "everything": what output_damage_all() means, and what a region that
 * overflowed its own bookkeeping falls back to. It stays a flag rather
 * than one big rectangle so a renderer can tell the two apart: a full
 * region is the cue to skip the clipping work altogether. */

void region_clear(CompRegion *r);
void region_set_full(CompRegion *r);

bool region_is_empty(const CompRegion *r);
bool region_is_full(const CompRegion *r);

/* Adds a rectangle. Empty rectangles are ignored, one already covered by
 * a rectangle in the region is dropped, and one that covers an existing
 * rectangle replaces it -- enough bookkeeping to keep the common cases
 * (the same window damaged twice in a frame, a window's old and new place
 * during a move) from filling the array. */
void region_add(CompRegion *r, const CompRect *rect);

/* Every rectangle, grown by `px` on all four sides. What the shadow
 * margin needs: a window's shadow is painted outside the window, so the
 * area a window's change dirties is bigger than the window. */
void region_grow(CompRegion *r, int px);

/* The smallest rectangle containing all of them (empty when the region
 * is). Undefined for a full region -- ask region_is_full() first. */
CompRect region_bounds(const CompRegion *r);

/* Does anything in the region touch this rectangle? The test a renderer
 * uses to skip a window that didn't change. Always true for a full
 * region. */
bool region_hits(const CompRegion *r, const CompRect *rect);

#endif /* KICOMP_REGION_H */
