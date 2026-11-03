/* See xscale.h. Raw wire requests: this protocol is still a draft in
 * the server fork and has no client binding, so the structs below are the
 * contract, copied from the fork's Xext/xscale/xscaleproto.h.
 *
 * XCB fills in byte 0 (the major opcode) and the length field itself when
 * a request is sent with ext == NULL and opcode = major; what has to be
 * written here is the minor opcode and the payload. */
#include "xscale.h"
#include "presenter.h"
#include "region.h"
#include "renderer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/xcbext.h>
#include <xcb/render.h>
#include <sys/uio.h>

#define XSCALE_EXTENSION_NAME           "X-SCALE"
#define XSCALE_MAJOR_VERSION            1
#define XSCALE_MINOR_VERSION            0

#define X_XScaleQueryVersion            0
#define X_XScaleGetCrtcScanout          2
#define X_XScaleSetCrtcScanoutSource    3
#define X_XScalePresentCrtcScanout      5

#define XScaleScanoutComplete           1
#define XScaleScanoutIdle               2

#define XScaleScanoutCapable            (1 << 0)

static uint8_t xscale_opcode;   /* 0 = extension absent */
static uint8_t xscale_first_event;

typedef struct {
    uint8_t reqType, xscaleReqType;
    uint16_t length;
    uint32_t clientMajorVersion;
    uint32_t clientMinorVersion;
} XScaleQueryVersionReq;

typedef struct {
    uint8_t reqType, xscaleReqType;
    uint16_t length;
    uint32_t crtc;
} XScaleGetCrtcScanoutReq;

typedef struct {
    uint8_t reqType, xscaleReqType;
    uint16_t length;
    uint32_t crtc;
    uint32_t drawable;
    int16_t srcX, srcY;
} XScaleSetCrtcScanoutSourceReq;

typedef struct {
    uint8_t reqType, xscaleReqType;
    uint16_t length;
    uint32_t crtc;
    uint32_t pixmap;
    uint32_t serial;
    uint32_t update;            /* XFixes region, scanout coordinates */
} XScalePresentCrtcScanoutReq;

/* One request out. `isvoid` is 0 for the ones with a reply. */
static unsigned xscale_send(const void *req, size_t len, int isvoid)
{
    struct iovec parts[4];
    xcb_protocol_request_t r = {
        .count = 2, .ext = NULL, .opcode = xscale_opcode, .isvoid = isvoid
    };

    parts[2].iov_base = (void *)req;
    parts[2].iov_len = len;
    parts[3].iov_base = NULL;
    parts[3].iov_len = (size_t)(-(ssize_t)len & 3);

    /* Void requests go out checked, so set_source() gets their errors
     * through xcb_request_check() instead of the event loop. */
    return xcb_send_request(comp.conn, isvoid ? XCB_REQUEST_CHECKED : 0,
                            parts + 2, &r);
}

void xscale_init(void)
{
    comp.caps.xscale = false;
    xscale_opcode = 0;

    /* An older kicomp, speaking the X-INPUT-SCALE experiment this replaced,
     * published the confined logical boxes here, and kiwm
     * shrinks its outputs to whatever it finds. Nothing is confined any
     * more -- RandR's own geometry is the logical box now -- so a leftover
     * would only squeeze the WM's idea of a monitor for nothing. */
    if (comp.atoms.xis_confined_area != XCB_NONE)
        xcb_delete_property(comp.conn, comp.root, comp.atoms.xis_confined_area);

    xcb_query_extension_reply_t *q = xcb_query_extension_reply(comp.conn,
        xcb_query_extension(comp.conn, (uint16_t)strlen(XSCALE_EXTENSION_NAME),
                            XSCALE_EXTENSION_NAME), NULL);
    if (!q)
        return;

    if (!q->present) {
        free(q);
        return;
    }

    xscale_opcode = q->major_opcode;
    xscale_first_event = q->first_event;
    free(q);

    /* The version handshake is the extension's own precondition, and it
     * doubles as proof that the opcode really answers. */
    XScaleQueryVersionReq req = { 0 };
    req.xscaleReqType = X_XScaleQueryVersion;
    req.clientMajorVersion = XSCALE_MAJOR_VERSION;
    req.clientMinorVersion = XSCALE_MINOR_VERSION;

    unsigned seq = xscale_send(&req, sizeof(req), 0);
    xcb_generic_error_t *err = NULL;
    uint8_t *reply = xcb_wait_for_reply(comp.conn, seq, &err);

    if (err || !reply) {
        free(err);
        free(reply);
        xscale_opcode = 0;
        return;
    }

    /* xXScaleQueryVersionReply: major at byte 8, minor at byte 12. */
    uint32_t major = *(uint32_t *)(void *)(reply + 8);
    uint32_t minor = *(uint32_t *)(void *)(reply + 12);
    free(reply);

    comp.caps.xscale = true;
    comp_log("X-SCALE %u.%u", major, minor);
}

bool xscale_present_pixmap(CompOutput *o, xcb_pixmap_t pixmap,
                           uint32_t serial, const CompRegion *damage)
{
    if (!o->source_presented || o->source_suspended || pixmap == XCB_NONE)
        return false;

    /* What changed, in the pixmap's own (scanout) coordinates. The server
     * reads it only when it has to copy; a flip shows the whole buffer. */
    xcb_xfixes_region_t update = XCB_NONE;
    if (comp.caps.xfixes && damage && !region_is_full(damage) &&
        damage->count > 0) {
        xcb_rectangle_t rects[COMP_REGION_MAX];
        int n = 0;
        for (int i = 0; i < damage->count && n < COMP_REGION_MAX; i++) {
            CompRect d;
            if (!present_physical_rect(o, &damage->rects[i], &d))
                continue;
            rects[n].x = (int16_t)(d.x - o->physical.x);
            rects[n].y = (int16_t)(d.y - o->physical.y);
            rects[n].width = (uint16_t)d.w;
            rects[n].height = (uint16_t)d.h;
            n++;
        }
        if (n > 0) {
            update = xcb_generate_id(comp.conn);
            xcb_xfixes_create_region(comp.conn, update, (uint32_t)n, rects);
        }
    }

    XScalePresentCrtcScanoutReq req = { 0 };
    req.xscaleReqType = X_XScalePresentCrtcScanout;
    req.crtc = o->crtc;
    req.pixmap = pixmap;
    req.serial = serial;
    req.update = update;

    /* Unchecked: one request per frame must not cost a round trip. A
     * refusal (the CRTC changed under us) arrives as an error, and the
     * RandR event behind it rebuilds the outputs. */
    xcb_protocol_request_t r = {
        .count = 2, .ext = NULL, .opcode = xscale_opcode, .isvoid = 1
    };
    struct iovec parts[4];
    parts[2].iov_base = &req;
    parts[2].iov_len = sizeof(req);
    parts[3].iov_base = NULL;
    parts[3].iov_len = 0;
    xcb_send_request(comp.conn, 0, parts + 2, &r);

    if (update != XCB_NONE)
        xcb_xfixes_destroy_region(comp.conn, update);

    if (!o->source_set) {
        o->source_set = true;
        comp_info("%s: sharp frames presented to the server (%dx%d for a "
                  "%dx%d logical box)", o->name, o->physical.w,
                  o->physical.h, o->rect.w, o->rect.h);
    }
    return true;
}

bool xscale_decode_event(const xcb_generic_event_t *ev, XScaleEvent *out)
{
    if (!comp.caps.xscale)
        return false;

    uint8_t type = ev->response_type & 0x7f;
    const uint8_t *b = (const uint8_t *)ev;
    const uint32_t *w = (const uint32_t *)(const void *)ev;

    if (type == (uint8_t)(xscale_first_event + XScaleScanoutComplete)) {
        /* type, mode, seq; crtc, serial, ustLo, ustHi, mscLo, mscHi */
        memset(out, 0, sizeof(*out));
        out->complete = true;
        out->mode = b[1];
        out->crtc = w[1];
        out->serial = w[2];
        out->ust = (uint64_t)w[3] | ((uint64_t)w[4] << 32);
        out->msc = (uint64_t)w[5] | ((uint64_t)w[6] << 32);
        return true;
    }
    if (type == (uint8_t)(xscale_first_event + XScaleScanoutIdle)) {
        /* type, pad, seq; crtc, serial, pixmap */
        memset(out, 0, sizeof(*out));
        out->crtc = w[1];
        out->serial = w[2];
        out->pixmap = w[3];
        return true;
    }
    return false;
}

bool xscale_crtc_capable(xcb_randr_crtc_t crtc, int *width, int *height)
{
    if (!comp.caps.xscale || crtc == XCB_NONE)
        return false;

    XScaleGetCrtcScanoutReq req = { 0 };
    req.xscaleReqType = X_XScaleGetCrtcScanout;
    req.crtc = crtc;

    unsigned seq = xscale_send(&req, sizeof(req), 0);
    xcb_generic_error_t *err = NULL;
    uint8_t *reply = xcb_wait_for_reply(comp.conn, seq, &err);
    if (err || !reply) {
        free(err);
        free(reply);
        return false;
    }

    /* xXScaleGetCrtcScanoutReply: flags at byte 1, scanoutWidth/Height at 8
     * and 10 (then the logical box, which RandR's monitors already give). */
    bool capable = (reply[1] & XScaleScanoutCapable) != 0;
    *width = *(uint16_t *)(void *)(reply + 8);
    *height = *(uint16_t *)(void *)(reply + 10);
    free(reply);
    return capable;
}

/* Hands `drawable` (or XCB_NONE: give back) to the output's CRTC. */
static bool set_source(const CompOutput *o, xcb_drawable_t drawable)
{
    XScaleSetCrtcScanoutSourceReq req = { 0 };
    req.xscaleReqType = X_XScaleSetCrtcScanoutSource;
    req.crtc = o->crtc;
    req.drawable = drawable;

    unsigned seq = xscale_send(&req, sizeof(req), 1);
    xcb_generic_error_t *err = xcb_request_check(comp.conn,
                                                 (xcb_void_cookie_t){ seq });
    if (err) {
        fprintf(stderr, "kicomp: %s: X-SCALE refused the scanout source "
                "(error %d)\n", o->name, err->error_code);
        free(err);
        return false;
    }
    return true;
}

/* What the server is (to be) told this output's picture is in. */
static xcb_drawable_t source_drawable(const CompOutput *o)
{
    if (o->source_pixmap)
        return o->source_pixmap;
    if (renderer && renderer->output_source)
        return renderer->output_source(o);
    return XCB_NONE;
}

bool xscale_attach(CompOutput *o)
{
    o->source_set = false;
    o->source_suspended = false;

    /* Presented frames are their own source: the first one sets it. */
    if (o->source_presented)
        return true;

    /* A backend with a drawable of its own that only ever changes by a
     * whole frame (GLX: its window, which a swap lands on whole) needs
     * nothing more. */
    if (renderer->output_source)
        return renderer->output_source(o) != XCB_NONE;

    /* XRender draws its frame into the target in many requests, and the
     * server copies the source whenever it goes idle -- possibly between
     * two of them. So the source is a pixmap of its own, which only ever
     * receives finished frames (xscale_present). */
    if (!o->target)
        return false;

    xcb_render_pictformat_t fmt = comp_root_pictformat();
    if (!fmt)
        return false;

    o->source_pixmap = xcb_generate_id(comp.conn);
    xcb_create_pixmap(comp.conn, comp.screen->root_depth, o->source_pixmap,
                      comp.root, (uint16_t)o->physical.w, (uint16_t)o->physical.h);
    o->source_picture = xcb_generate_id(comp.conn);
    xcb_render_create_picture(comp.conn, o->source_picture, o->source_pixmap,
                              fmt, 0, NULL);
    return true;
}

void xscale_detach(CompOutput *o)
{
    if (o->source_set && comp.caps.xscale)
        set_source(o, XCB_NONE);
    o->source_set = false;
    o->source_suspended = false;

    if (o->source_picture) {
        xcb_render_free_picture(comp.conn, o->source_picture);
        o->source_picture = 0;
    }
    if (o->source_pixmap) {
        xcb_free_pixmap(comp.conn, o->source_pixmap);
        o->source_pixmap = 0;
    }
}

void xscale_present(CompOutput *o, const CompRegion *damage)
{
    if (!o->target || !o->source_picture)
        return;

    /* Only what was repainted, the copy presenter's rule -- into the
     * source at its origin rather than into the overlay at the output's
     * position. */
    if (damage && !region_is_full(damage) && damage->count > 0) {
        for (int i = 0; i < damage->count; i++) {
            CompRect d;
            if (!present_physical_rect(o, &damage->rects[i], &d))
                continue;
            int16_t x = (int16_t)(d.x - o->physical.x);
            int16_t y = (int16_t)(d.y - o->physical.y);
            xcb_render_composite(comp.conn, XCB_RENDER_PICT_OP_SRC,
                                 o->target, XCB_NONE, o->source_picture,
                                 x, y, 0, 0, x, y,
                                 (uint16_t)d.w, (uint16_t)d.h);
        }
        return;
    }

    xcb_render_composite(comp.conn, XCB_RENDER_PICT_OP_SRC,
                         o->target, XCB_NONE, o->source_picture,
                         0, 0, 0, 0, 0, 0,
                         (uint16_t)o->physical.w, (uint16_t)o->physical.h);
}

void xscale_frame_done(CompOutput *o)
{
    if (!o->sourced || o->source_presented || o->source_set ||
        o->source_suspended)
        return;

    xcb_drawable_t d = source_drawable(o);
    if (d == XCB_NONE)
        return;

    if (set_source(o, d)) {
        o->source_set = true;
        comp_info("%s: sharp scanout handed to the server (%dx%d for a "
                  "%dx%d logical box)", o->name, o->physical.w, o->physical.h,
                  o->rect.w, o->rect.h);
    } else {
        /* Refused (the CRTC changed under us, most likely): the server
         * keeps magnifying the root, which nobody is drawing for this
         * output. The RandR event behind the refusal rebuilds the outputs
         * soon; until then at least don't ask again every frame. */
        o->source_suspended = true;
    }
}

void xscale_suspend(CompOutput *o)
{
    if (!o->sourced || o->source_suspended)
        return;
    if (o->source_set)
        set_source(o, XCB_NONE);
    o->source_set = false;
    o->source_suspended = true;
}

void xscale_resume(CompOutput *o)
{
    if (!o->sourced)
        return;
    o->source_suspended = false;
    /* handed over again by xscale_frame_done after the next frame,
     * which the give-back already damaged the whole output for */
}

bool xscale_is_own_window(xcb_window_t id)
{
    if (id == XCB_NONE || !renderer || !renderer->output_source)
        return false;
    for (int i = 0; i < comp.output_count; i++)
        if (comp.outputs[i].sourced &&
            renderer->output_source(&comp.outputs[i]) == id)
            return true;
    return false;
}

void xscale_shutdown(void)
{
    for (int i = 0; i < comp.output_count; i++)
        xscale_detach(&comp.outputs[i]);
    xcb_flush(comp.conn);
}
