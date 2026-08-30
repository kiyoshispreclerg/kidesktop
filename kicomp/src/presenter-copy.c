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

static bool copy_present(CompOutput *o, CompPresentMode mode)
{
    if (mode != COMP_PRESENT_COPY)
        return false;
    if (!o->target || !overlay_ensure())
        return false;

    xcb_render_composite(comp.conn, XCB_RENDER_PICT_OP_SRC,
                         o->target, XCB_NONE, overlay_picture,
                         0, 0, 0, 0,
                         (int16_t)o->rect.x, (int16_t)o->rect.y,
                         (uint16_t)o->rect.w, (uint16_t)o->rect.h);
    return true;
}

static uint64_t copy_msc(CompOutput *o)
{
    (void)o;
    return 0;   /* no MSC without Present -- Fase 7 */
}

void presenter_shutdown(void)
{
    overlay_release();
}

static const CompPresenter copy_presenter = {
    .name    = "copy",
    .init    = copy_init,
    .destroy = copy_destroy,
    .present = copy_present,
    .get_msc = copy_msc,
};

const CompPresenter *presenter_copy(void)
{
    return &copy_presenter;
}
