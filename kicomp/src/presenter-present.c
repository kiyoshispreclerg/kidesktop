/*
 * PRESENT presenter: each output's frame handed to the server through the
 * Present extension, aimed at that output's own CRTC (sections 15/16/19).
 *
 * What this buys over the COPY presenter, which composites the target
 * straight onto the overlay:
 *
 *   - the copy happens at vblank, so a frame is never torn across the
 *     scanout it is landing in;
 *   - the server says *when* it landed -- PresentCompleteNotify carries
 *     the MSC (the monitor's frame counter) and the UST (when that frame
 *     was scanned out). That is the measurement the per-output frame clock
 *     has been missing: today's period is computed from the RandR mode,
 *     which is what the monitor claims rather than what it does;
 *   - and it is aimed per CRTC. A frame for the 144 Hz monitor is timed
 *     against that monitor's vblank, not against whichever one the server
 *     would have picked for a screen-spanning window.
 *
 * Each output gets a child window of the Composite overlay, exactly
 * covering it, and its pixmap is presented into that window. One window
 * per CRTC is not an implementation detail: it is the shape a per-CRTC
 * page flip needs later (Fase 8), where a window covering exactly one
 * CRTC can have its buffer scanned out directly instead of copied.
 *
 * Frames do not flip *yet*, and the reason is not the presenter: an
 * XRender pixmap is not allocated as a scanout buffer, so the server
 * copies it (PRESENT_COMPLETE_MODE_COPY) at the right moment rather than
 * handing it to the display engine. Flipping is what a GL/GBM renderer
 * unlocks -- with this presenter already in place.
 *
 * Throttling: an output that still has a frame in flight is not painted
 * again. Queueing a second present before the first has landed does not
 * make anything appear sooner; it builds a backlog, and every frame in it
 * is one more frame of latency between what the user did and what they
 * see. The output stays dirty and is painted as soon as the completion
 * arrives, which makes the loop vblank-driven for free.
 */
#include "presenter.h"
#include "renderer.h"
#include "region.h"
#include "animation.h"

#include <xcb/present.h>
#include <xcb/shape.h>

#include <stdlib.h>
#include <string.h>

/* If a completion never arrives -- an output nothing is scanning out, a
 * server that decided not to answer -- present again anyway rather than
 * leaving that output frozen. Deliberately much longer than any real
 * frame, so it never fires on a working output. */
#define COMPLETE_TIMEOUT_MS 250.0

typedef struct {
    xcb_window_t window;        /* CRTC-covering child of the overlay */
    xcb_present_event_t eid;

    uint32_t serial;
    bool pending;               /* a frame is in flight */
    double pending_since;

    uint64_t msc;               /* last completed frame's counter */
    uint64_t ust;               /* ...and when it was scanned out (us) */
    uint8_t mode;               /* how it landed: copy, flip, skip */
} PresentOutput;

static bool present_init(CompOutput *o)
{
    if (comp.overlay == XCB_NONE || o->rect.w <= 0 || o->rect.h <= 0)
        return false;

    PresentOutput *po = calloc(1, sizeof(*po));
    if (!po)
        return false;

    /* Same depth and visual as the target pixmap the renderer draws into
     * (the root's), because Present refuses a pixmap whose depth differs
     * from the window's. */
    po->window = xcb_generate_id(comp.conn);

    uint32_t mask = XCB_CW_BACK_PIXMAP | XCB_CW_EVENT_MASK;
    uint32_t values[] = { XCB_BACK_PIXMAP_NONE, 0 };

    xcb_create_window(comp.conn, XCB_COPY_FROM_PARENT, po->window, comp.overlay,
                      (int16_t)o->rect.x, (int16_t)o->rect.y,
                      (uint16_t)o->rect.w, (uint16_t)o->rect.h, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, XCB_COPY_FROM_PARENT,
                      mask, values);

    /* Transparent to input, exactly as the overlay itself is: the
     * compositor's windows must never eat a click meant for the desktop
     * underneath them. */
    if (comp.caps.xfixes) {
        xcb_xfixes_region_t empty = xcb_generate_id(comp.conn);
        xcb_xfixes_create_region(comp.conn, empty, 0, NULL);
        xcb_xfixes_set_window_shape_region(comp.conn, po->window,
                                           XCB_SHAPE_SK_INPUT, 0, 0, empty);
        xcb_xfixes_destroy_region(comp.conn, empty);
    }

    /* Completions only. IdleNotify would say when the pixmap is reusable,
     * which matters to a backend that rotates buffers; this one draws into
     * the same target every frame and waits for the completion anyway. */
    po->eid = xcb_generate_id(comp.conn);
    xcb_present_select_input(comp.conn, po->eid, po->window,
                             XCB_PRESENT_EVENT_MASK_COMPLETE_NOTIFY);

    xcb_map_window(comp.conn, po->window);

    o->present_data = po;
    return true;
}

static void present_destroy(CompOutput *o)
{
    PresentOutput *po = o->present_data;
    if (!po)
        return;

    if (po->eid != XCB_NONE)
        xcb_present_select_input(comp.conn, po->eid, po->window, 0);
    if (po->window != XCB_NONE)
        xcb_destroy_window(comp.conn, po->window);

    free(po);
    o->present_data = NULL;
}

/* The damaged part of the output, in the presented window's own
 * coordinates, as a region the server can clip the update to. XCB_NONE
 * means "all of it", which is what a full region turns into. */
static xcb_xfixes_region_t update_region(CompOutput *o, const CompRegion *damage)
{
    if (!comp.caps.xfixes || !damage || region_is_full(damage) || damage->count <= 0)
        return XCB_NONE;

    xcb_rectangle_t rects[COMP_REGION_MAX];
    int n = 0;

    for (int i = 0; i < damage->count && n < COMP_REGION_MAX; i++) {
        const CompRect *d = &damage->rects[i];
        if (d->w <= 0 || d->h <= 0)
            continue;
        rects[n].x = (int16_t)(d->x - o->rect.x);
        rects[n].y = (int16_t)(d->y - o->rect.y);
        rects[n].width = (uint16_t)d->w;
        rects[n].height = (uint16_t)d->h;
        n++;
    }

    if (n == 0)
        return XCB_NONE;

    xcb_xfixes_region_t region = xcb_generate_id(comp.conn);
    xcb_xfixes_create_region(comp.conn, region, (uint32_t)n, rects);
    return region;
}

static bool present_present(CompOutput *o, CompPresentMode mode,
                            const CompRegion *damage)
{
    PresentOutput *po = o->present_data;

    if (!po || mode == COMP_PRESENT_FLIP)
        return false;

    /* Present hands the server a *pixmap*, where the COPY presenter reads
     * a Picture -- both are the renderer's target seen from a different
     * side, and both are asked for rather than assumed. */
    xcb_pixmap_t pixmap = renderer_output_pixmap(o);
    if (pixmap == XCB_NONE)
        return false;

    xcb_xfixes_region_t update = update_region(o, damage);

    /* target_msc 0: at the next vblank, not at a particular frame number.
     * divisor/remainder 0 for the same reason -- this is "show it as soon
     * as it can be shown without tearing", which is what a compositor
     * driven by damage wants; a fixed cadence would be the wrong shape for
     * a screen that is idle most of the time. */
    xcb_present_pixmap(comp.conn, po->window, pixmap, ++po->serial,
                       XCB_NONE,          /* valid: the whole pixmap */
                       update,            /* update: only what changed */
                       0, 0,              /* no offset */
                       o->crtc,           /* timed against this monitor */
                       XCB_NONE, XCB_NONE,/* no fences */
                       XCB_PRESENT_OPTION_NONE,
                       0, 0, 0,           /* target_msc, divisor, remainder */
                       0, NULL);

    if (update != XCB_NONE)
        xcb_xfixes_destroy_region(comp.conn, update);

    po->pending = true;
    po->pending_since = comp_now_ms();
    return true;
}

/* Still waiting for the last frame to land: painting another one now
 * would only queue latency behind it. */
static bool present_busy(CompOutput *o)
{
    PresentOutput *po = o->present_data;
    if (!po || !po->pending)
        return false;

    if (comp_now_ms() - po->pending_since > COMPLETE_TIMEOUT_MS) {
        /* No completion is coming. Say so once and carry on unthrottled
         * rather than leaving the output frozen. */
        comp_log("present: no completion for %s in %.0f ms; presenting anyway",
                 o->name, COMPLETE_TIMEOUT_MS);
        po->pending = false;
        return false;
    }
    return true;
}

static const char *mode_name(uint8_t mode)
{
    switch (mode) {
    case XCB_PRESENT_COMPLETE_MODE_COPY:        return "copy";
    case XCB_PRESENT_COMPLETE_MODE_FLIP:        return "flip";
    case XCB_PRESENT_COMPLETE_MODE_SKIP:        return "skip";
    case XCB_PRESENT_COMPLETE_MODE_SUBOPTIMAL_COPY: return "suboptimal-copy";
    default:                                    return "?";
    }
}

static bool present_handle_event(xcb_generic_event_t *ev)
{
    if ((ev->response_type & 0x7f) != XCB_GE_GENERIC)
        return false;

    xcb_ge_generic_event_t *ge = (xcb_ge_generic_event_t *)ev;
    if (ge->extension != comp.present_opcode)
        return false;
    if (ge->event_type != XCB_PRESENT_COMPLETE_NOTIFY)
        return true;   /* ours, but not something this presenter uses */

    xcb_present_complete_notify_event_t *c =
        (xcb_present_complete_notify_event_t *)ev;

    for (int i = 0; i < comp.output_count; i++) {
        CompOutput *o = &comp.outputs[i];
        PresentOutput *po = o->present_data;
        if (!po || po->window != c->window)
            continue;

        po->pending = false;
        po->msc = c->msc;
        po->ust = c->ust;
        po->mode = c->mode;

        /* The output may still be dirty (something changed while that
         * frame was in flight); the main loop paints it on this same
         * wake-up, which is what makes the loop vblank-paced. */
        comp_log("present %s: msc %llu mode %s", o->name,
                 (unsigned long long)c->msc, mode_name(c->mode));
        break;
    }

    return true;
}

static uint64_t present_msc(CompOutput *o)
{
    PresentOutput *po = o->present_data;
    return po ? po->msc : 0;
}

static const CompPresenter present_presenter = {
    .name         = "present",
    .init         = present_init,
    .destroy      = present_destroy,
    .present      = present_present,
    .busy         = present_busy,
    .handle_event = present_handle_event,
    .get_msc      = present_msc,
};

const CompPresenter *presenter_present(void)
{
    return &present_presenter;
}
