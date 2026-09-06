/* COPY presenter: composites each output's target onto the Composite
 * overlay window (section 15/16).
 *
 * This is the path every X server can do. The XiS per-CRTC FLIP presenter
 * (Fase 8) implements the same three ops and is selected by capability
 * detection alone -- no caller changes, no scene changes.
 *
 * The overlay is one window covering the whole screen, so "one drawable
 * per output" survives here as one *composite per output* into disjoint
 * rectangles of it: outputs still never wait for each other, and a clean
 * output is never touched.
 */
#include "presenter.h"
#include "renderer.h"
#include "region.h"

#include <stdio.h>
#include <stdlib.h>

const CompPresenter *presenter;

static xcb_render_picture_t overlay_picture;
static int overlay_w, overlay_h;

static void overlay_release(void)
{
    if (overlay_picture) {
        xcb_render_free_picture(comp.conn, overlay_picture);
        overlay_picture = 0;
    }
}

static bool overlay_ensure(void)
{
    if (overlay_picture && overlay_w == comp.root_w && overlay_h == comp.root_h)
        return true;

    overlay_release();

    if (comp.overlay == XCB_NONE)
        return false;

    xcb_render_pictformat_t fmt = comp_root_pictformat();
    if (!fmt)
        return false;

    overlay_picture = xcb_generate_id(comp.conn);
    /* IncludeInferiors matters here: the overlay has a child window in
     * some setups, and we want to draw over the whole thing. */
    uint32_t sub = XCB_SUBWINDOW_MODE_INCLUDE_INFERIORS;
    xcb_generic_error_t *err = xcb_request_check(comp.conn,
        xcb_render_create_picture_checked(comp.conn, overlay_picture,
                                          comp.overlay, fmt,
                                          XCB_RENDER_CP_SUBWINDOW_MODE, &sub));
    if (err) {
        free(err);
        overlay_picture = 0;
        return false;
    }

    overlay_w = comp.root_w;
    overlay_h = comp.root_h;
    return true;
}

static bool copy_init(CompOutput *o)
{
    (void)o;
    return overlay_ensure();
}

static void copy_destroy(CompOutput *o)
{
    (void)o;
    /* The overlay picture is shared by every output; it goes away in
     * presenter_shutdown(), not with an individual output. */
}

static bool copy_present(CompOutput *o, CompPresentMode mode,
                         const CompRegion *damage)
{
    if (mode != COMP_PRESENT_COPY)
        return false;
    if (!o->target || !overlay_ensure())
        return false;

    /* Only what was repainted. The overlay keeps its pixels between
     * frames just as the target does, so copying the untouched parts
     * again would be copying a rectangle onto its own contents -- the
     * most expensive way there is to change nothing. */
    /* The target is already in physical pixels (the renderer magnified
     * the logical scene into it), so this stays a straight copy however
     * the output is scaled -- only the rectangles have to be converted
     * from the logical coordinates damage is tracked in. */
    if (damage && !region_is_full(damage) && damage->count > 0) {
        for (int i = 0; i < damage->count; i++) {
            CompRect d;
            if (!present_physical_rect(o, &damage->rects[i], &d))
                continue;
            xcb_render_composite(comp.conn, XCB_RENDER_PICT_OP_SRC,
                                 o->target, XCB_NONE, overlay_picture,
                                 (int16_t)(d.x - o->physical.x),
                                 (int16_t)(d.y - o->physical.y), 0, 0,
                                 (int16_t)d.x, (int16_t)d.y,
                                 (uint16_t)d.w, (uint16_t)d.h);
        }
        return true;
    }

    xcb_render_composite(comp.conn, XCB_RENDER_PICT_OP_SRC,
                         o->target, XCB_NONE, overlay_picture,
                         0, 0, 0, 0,
                         (int16_t)o->physical.x, (int16_t)o->physical.y,
                         (uint16_t)o->physical.w, (uint16_t)o->physical.h);
    return true;
}

static uint64_t copy_msc(CompOutput *o)
{
    (void)o;
    return 0;   /* no MSC without Present -- Fase 7 */
}

/* Nothing here ever asks the server when the next vblank is: the copy
 * lands the moment scheduler.c's software clock says the output's next
 * frame is due, and that clock is paced from the RandR mode's advertised
 * refresh rate, not from anything the monitor actually did. Which is
 * exactly what makes this presenter portable -- every X server can do
 * it -- and exactly why it can tear. */
static void copy_sync_info(CompOutput *o, char *buf, size_t n)
{
    snprintf(buf, n, "sw clock, randr %.0f Hz (no vblank)", o->refresh_hz);
}

void presenter_shutdown(void)
{
    overlay_release();
}

static const CompPresenter copy_presenter = {
    .name      = "copy",
    .init      = copy_init,
    .destroy   = copy_destroy,
    .present   = copy_present,
    .get_msc   = copy_msc,
    .sync_info = copy_sync_info,
};

const CompPresenter *presenter_copy(void)
{
    return &copy_presenter;
}

bool present_physical_rect(const CompOutput *o, const CompRect *logical,
                           CompRect *out)
{
    if (logical->w <= 0 || logical->h <= 0)
        return false;

    if (o->scale == 1.0f) {
        *out = *logical;
        return true;
    }

    /* Rounded outwards: half a physical pixel of damage still has to be
     * copied, and a rectangle that came back a pixel short would leave a
     * seam of last frame's content along the edge of everything that
     * moves. */
    int x0 = o->physical.x + (int)((float)(logical->x - o->rect.x) * o->scale);
    int y0 = o->physical.y + (int)((float)(logical->y - o->rect.y) * o->scale);
    int x1 = o->physical.x +
             (int)((float)(logical->x + logical->w - o->rect.x) * o->scale + 0.999f);
    int y1 = o->physical.y +
             (int)((float)(logical->y + logical->h - o->rect.y) * o->scale + 0.999f);

    if (x0 < o->physical.x) x0 = o->physical.x;
    if (y0 < o->physical.y) y0 = o->physical.y;
    if (x1 > o->physical.x + o->physical.w) x1 = o->physical.x + o->physical.w;
    if (y1 > o->physical.y + o->physical.h) y1 = o->physical.y + o->physical.h;

    if (x1 <= x0 || y1 <= y0)
        return false;

    out->x = x0;
    out->y = y0;
    out->w = x1 - x0;
    out->h = y1 - y0;
    return true;
}
