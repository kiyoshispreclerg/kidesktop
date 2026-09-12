/* See damage.h -- X Damage in, the core's per-output regions out. */
#include "damage.h"
#include "output.h"
#include "window.h"
#include "effect.h"

#include <stdlib.h>

/* How many windows one frame collects precisely. Past this the rest fall
 * back to their whole rectangle, which is always correct and merely
 * repaints more -- a frame with hundreds of windows changing at once is
 * repainting most of the screen anyway. */
#define MAX_COLLECT 64

/* And how many rectangles are taken from one window before its own
 * bounding box is used instead. A window reporting twenty separate little
 * damaged areas is cheaper to repaint as one box than to carry twenty
 * rectangles through the region, the renderer and the presenter. */
#define MAX_RECTS_PER_WINDOW 8

void damage_window_reported(CompWindow *w)
{
    /* No request, no reply, nothing. The server keeps accumulating into
     * the Damage object and won't report again until the subtract at
     * frame time, which is also what fetches all of it at once. */
    w->damage_pending = true;
}

/* Is `inner` entirely within `outer`? */
static bool rect_inside(const CompRect *inner, const CompRect *outer)
{
    return inner->x >= outer->x && inner->y >= outer->y &&
           inner->x + inner->w <= outer->x + outer->w &&
           inner->y + inner->h <= outer->y + outer->h;
}

void damage_collect(void)
{
    if (!comp.caps.damage || !comp.caps.xfixes)
        return;

    CompWindow *windows[MAX_COLLECT];
    xcb_xfixes_region_t regions[MAX_COLLECT];
    xcb_xfixes_fetch_region_cookie_t cookies[MAX_COLLECT];
    int n = 0;

    for (CompWindow *w = comp.stack; w; w = w->next) {
        if (!w->damage_pending)
            continue;
        w->damage_pending = false;

        if (w->damage == XCB_NONE)
            continue;

        if (w->occluded) {
            /* Covered by something opaque (scene.c). The damage still has
             * to be *taken* -- the server stops reporting until it is --
             * but there is nothing to repaint: whatever is drawn over
             * this window is unchanged, and the moment something uncovers
             * it, that movement damages the area itself.
             *
             * This is where a video behind a maximized window stops
             * costing anything. */
            /* ...except where an effect is drawing this window somewhere
             * of its own: covered here is not covered there. Its whole
             * rectangle, since the pieces were never fetched. */
            CompRect whole = window_rect(w);
            effects_damage_window(w, &whole);

            xcb_discard_reply(comp.conn,
                xcb_damage_subtract_checked(comp.conn, w->damage,
                                            XCB_XFIXES_REGION_NONE,
                                            XCB_XFIXES_REGION_NONE).sequence);
            continue;
        }

        if (n >= MAX_COLLECT) {
            /* Out of slots: correct, just coarser. */
            CompRect r = window_rect(w);
            output_damage_window_rect(w, &r);
            effects_damage_window(w, &r);
            continue;
        }

        /* Subtract with a region to receive what was subtracted: that
         * both hands us everything damaged since the last frame and
         * re-arms the reporting for the next one. All asynchronous -- the
         * replies are read below, in one go.
         *
         * Checked, with the error discarded: the window may have been
         * destroyed between the DamageNotify that set damage_pending and
         * this request, taking its Damage with it, and the DestroyNotify
         * that would have told us is still on its way. The region then
         * simply comes back empty. */
        xcb_xfixes_region_t region = xcb_generate_id(comp.conn);
        xcb_xfixes_create_region(comp.conn, region, 0, NULL);
        xcb_discard_reply(comp.conn,
            xcb_damage_subtract_checked(comp.conn, w->damage,
                                        XCB_XFIXES_REGION_NONE, region).sequence);

        windows[n] = w;
        regions[n] = region;
        cookies[n] = xcb_xfixes_fetch_region(comp.conn, region);
        n++;
    }

    /* One round trip for the whole frame happens here, at the first
     * reply. */
    for (int i = 0; i < n; i++) {
        CompWindow *w = windows[i];
        xcb_xfixes_fetch_region_reply_t *r =
            xcb_xfixes_fetch_region_reply(comp.conn, cookies[i], NULL);

        xcb_xfixes_destroy_region(comp.conn, regions[i]);

        if (!r) {
            /* The window died between the event and the reply, or the
             * server refused: repaint where it was and move on. */
            CompRect whole = window_rect(w);
            output_damage_window_rect(w, &whole);
            effects_damage_window(w, &whole);
            continue;
        }

        int count = xcb_xfixes_fetch_region_rectangles_length(r);
        xcb_rectangle_t *rects = xcb_xfixes_fetch_region_rectangles(r);

        if (count <= 0 || count > MAX_RECTS_PER_WINDOW) {
            /* Nothing (the damage was already collected some other way)
             * or too many to be worth carrying separately: the window's
             * own rectangle covers both cases. */
            if (count > 0) {
                CompRect whole = window_rect(w);
                output_damage_window_rect(w, &whole);
                effects_damage_window(w, &whole);
            }
            free(r);
            continue;
        }

        for (int j = 0; j < count; j++) {
            /* Damage rectangles are relative to the window's origin --
             * not to its pixmap's, which starts at the border. */
            CompRect d = {
                w->x + rects[j].x,
                w->y + rects[j].y,
                rects[j].width,
                rects[j].height,
            };
            /* A piece of a window that something opaque is drawn over:
             * repainting it would compose the same pixels again
             * (comp.h's cover). This is what a video behind a text
             * editor costs, and it should be nothing. */
            bool unseen = false;
            for (int c = 0; c < w->cover_count && !unseen; c++)
                unseen = rect_inside(&d, &w->cover[c]);

            /* ...unless an effect is drawing this window somewhere else,
             * where nothing is covering it: what is hidden here is not
             * hidden there (effect.h's damage_map). */
            effects_damage_window(w, &d);

            if (unseen)
                continue;

            output_damage_window_rect(w, &d);
        }

        free(r);
    }
}
