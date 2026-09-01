/* See region.h -- rectangles in plain memory, so every backend can have
 * them without a round trip. */
#include "region.h"

#include <string.h>

void region_clear(CompRegion *r)
{
    r->count = 0;
    r->full = false;
}

void region_set_full(CompRegion *r)
{
    r->count = 0;
    r->full = true;
}

bool region_is_empty(const CompRegion *r)
{
    return !r->full && r->count == 0;
}

bool region_is_full(const CompRegion *r)
{
    return r->full;
}

static bool contains(const CompRect *outer, const CompRect *inner)
{
    return inner->x >= outer->x && inner->y >= outer->y &&
           inner->x + inner->w <= outer->x + outer->w &&
           inner->y + inner->h <= outer->y + outer->h;
}

static CompRect union_of(const CompRect *a, const CompRect *b)
{
    int x1 = a->x < b->x ? a->x : b->x;
    int y1 = a->y < b->y ? a->y : b->y;
    int x2 = (a->x + a->w) > (b->x + b->w) ? (a->x + a->w) : (b->x + b->w);
    int y2 = (a->y + a->h) > (b->y + b->h) ? (a->y + a->h) : (b->y + b->h);
    return (CompRect){ x1, y1, x2 - x1, y2 - y1 };
}

void region_add(CompRegion *r, const CompRect *rect)
{
    if (r->full)
        return;
    if (rect->w <= 0 || rect->h <= 0)
        return;

    for (int i = 0; i < r->count; i++) {
        if (contains(&r->rects[i], rect))
            return;                       /* already covered */
        if (contains(rect, &r->rects[i])) {
            r->rects[i] = *rect;          /* swallows the old one */
            return;
        }
    }

    if (r->count < COMP_REGION_MAX) {
        r->rects[r->count++] = *rect;
        return;
    }

    /* Out of slots: everything becomes the box around everything. Past a
     * point the rectangles cost more to carry than the pixels they save. */
    CompRect all = r->rects[0];
    for (int i = 1; i < r->count; i++)
        all = union_of(&all, &r->rects[i]);
    all = union_of(&all, rect);

    r->rects[0] = all;
    r->count = 1;
}

void region_grow(CompRegion *r, int px)
{
    if (px <= 0)
        return;

    for (int i = 0; i < r->count; i++) {
        r->rects[i].x -= px;
        r->rects[i].y -= px;
        r->rects[i].w += px * 2;
        r->rects[i].h += px * 2;
    }
}

CompRect region_bounds(const CompRegion *r)
{
    if (r->count == 0)
        return (CompRect){ 0, 0, 0, 0 };

    CompRect all = r->rects[0];
    for (int i = 1; i < r->count; i++)
        all = union_of(&all, &r->rects[i]);
    return all;
}

bool region_hits(const CompRegion *r, const CompRect *rect)
{
    if (r->full)
        return true;

    CompRect ignored;
    for (int i = 0; i < r->count; i++)
        if (rect_intersect(&r->rects[i], rect, &ignored))
            return true;
    return false;
}
