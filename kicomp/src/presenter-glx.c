/*
 * GLX presenter: the buffer swap.
 *
 * With GL there is no separate presentation step to speak of -- the
 * renderer draws into the drawable's back buffer and presenting is
 * glXSwapBuffers on it. So this presenter is deliberately thin, and it is
 * paired with renderer-glx.c rather than being independent of it: the
 * drawable belongs to the renderer, which is the one that has a context
 * current on it.
 *
 * What it adds of its own is the measurement. glXSwapBuffers honours the
 * driver's swap interval -- normally 1, so the swap really does wait for
 * the next vblank on most setups -- but "normally" and "most" are
 * beliefs, and a compositor whose frame pacing rests on a belief cannot
 * tell the stats effect anything true. GLX_OML_sync_control is the
 * drawable's own vblank counter; read after every swap, it says how many
 * vblanks each frame took and how long a vblank is, which is the
 * difference between "synced to the monitor" and "synced to whatever the
 * driver felt like".
 *
 * The split still earns its keep, because it is where the *next* step
 * goes: presenting a GL buffer through Present (and, on the XiS server,
 * flipping it per CRTC, Fase 8) replaces this file and nothing else.
 */
#include "presenter.h"
#include "renderer.h"

#include <stdio.h>
#include <stdlib.h>

/* From renderer-glx.c: swapping is the renderer's drawable, not ours, and
 * so is the counter behind it. */
void renderer_glx_swap(CompOutput *o);
bool renderer_glx_ready(void);
bool renderer_glx_sync_values(CompOutput *o, int64_t *ust, int64_t *msc,
                              int64_t *sbc);

typedef struct {
    bool counted;         /* the counter has answered at least once */
    int64_t msc;          /* vblank count at the last swap */
    int64_t ust;          /* ...and its time (us) */

    /* Over the frames that came in a run (measure): how many vblanks the
     * swaps have spanned and how many swaps there were, so the ratio is
     * "vblanks per frame" -- 1.0 when every frame lands on the next
     * vblank, more when frames are being skipped, under 1 only if the
     * swap isn't waiting at all. And the vblank period, from the
     * counter's own clock. */
    int64_t frames;
    int64_t vblanks;
    double period_us;
} GlxPresentOutput;

/* A swap this many vblanks after the previous one is the compositor
 * having had nothing to paint, not a frame that took that long. */
#define IDLE_VBLANKS 4

static bool glx_present_init(CompOutput *o)
{
    /* The renderer made the drawable; if it never came up, there is
     * nothing to swap and saying so here is better than swapping into a
     * drawable that doesn't exist. */
    if (!renderer_glx_ready())
        return false;

    GlxPresentOutput *po = calloc(1, sizeof(*po));
    if (!po)
        return false;
    o->present_data = po;
    return true;
}

static void glx_present_destroy(CompOutput *o)
{
    /* The drawable is the renderer's to destroy; only the bookkeeping is
     * ours. */
    free(o->present_data);
    o->present_data = NULL;
}

/* Reads the counter after a swap and folds it into the running measure.
 * Nothing is waited for: GetSyncValues answers with the vblank the
 * drawable has *reached*, and a swap that is still queued behind the
 * next one shows up as the following frame spanning two. */
static void measure(CompOutput *o)
{
    GlxPresentOutput *po = o->present_data;
    if (!po)
        return;

    int64_t ust, msc, sbc;
    if (!renderer_glx_sync_values(o, &ust, &msc, &sbc))
        return;

    if (po->counted && msc > po->msc) {
        int64_t steps = msc - po->msc;
        /* One vblank's length, as this pair of readings has it. Smoothed
         * rather than averaged from the start, so a mode change shows up
         * within a few frames instead of being diluted by every frame
         * before it. */
        double period = (double)(ust - po->ust) / (double)steps;
        if (period > 0.0)
            po->period_us = po->period_us > 0.0
                          ? po->period_us * 0.9 + period * 0.1
                          : period;

        /* Only a frame that followed another closely says anything about
         * pacing. The compositor paints when something changed, and a
         * gap of many vblanks is the screen standing still, not a frame
         * that took that long -- counted in, an idle desktop reads as
         * dropping five frames out of six. */
        if (steps <= IDLE_VBLANKS) {
            po->frames++;
            po->vblanks += steps;
        }
    }

    po->counted = true;
    po->msc = msc;
    po->ust = ust;
}

static bool glx_present(CompOutput *o, CompPresentMode mode,
                        const CompRegion *damage)
{
    /* The damage went into the frame as a scissor box; the swap itself is
     * all or nothing. GLX_EXT_buffer_age plus a partial swap
     * (GLX_MESA_copy_sub_buffer / EXT_swap_buffers_with_damage) is the
     * follow-up that makes this region mean something here too. */
    (void)damage;

    if (mode == COMP_PRESENT_FLIP)
        return false;

    renderer_glx_swap(o);
    measure(o);
    return true;
}

static uint64_t glx_present_msc(CompOutput *o)
{
    GlxPresentOutput *po = o->present_data;
    if (!po || !po->counted || po->msc < 0)
        return 0;
    return (uint64_t)po->msc;
}

static void glx_sync_info(CompOutput *o, char *buf, size_t n)
{
    GlxPresentOutput *po = o->present_data;

    if (!po || !po->counted) {
        /* No counter: the swap is believed to wait for vblank, and that
         * is all that can honestly be said. */
        snprintf(buf, n, "vblank via GLX swap (driver interval, unmeasured)");
        return;
    }
    if (po->frames == 0) {
        snprintf(buf, n, "vblank msc %lld via GLX swap (measuring)",
                 (long long)po->msc);
        return;
    }

    snprintf(buf, n, "vblank msc %lld via GLX swap (%.2f vblanks/frame, %.2f ms)",
             (long long)po->msc,
             (double)po->vblanks / (double)po->frames,
             po->period_us / 1000.0);
}

static const CompPresenter glx_presenter = {
    .name      = "glx",
    .init      = glx_present_init,
    .destroy   = glx_present_destroy,
    .present   = glx_present,
    .get_msc   = glx_present_msc,
    .sync_info = glx_sync_info,
};

const CompPresenter *presenter_glx(void)
{
    return &glx_presenter;
}
