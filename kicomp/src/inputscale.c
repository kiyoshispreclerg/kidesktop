/* See inputscale.h. Raw wire requests: this protocol is still a draft in
 * the server fork and has no client binding, so the structs below are the
 * contract, copied from the fork's Xext/inputscale/inputscaleproto.h.
 *
 * XCB fills in byte 0 (the major opcode) and the length field itself when
 * a request is sent with ext == NULL and opcode = major; what has to be
 * written here is the minor opcode and the payload. */
#include "inputscale.h"
#include "presenter.h"
#include "region.h"
#include "renderer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/xcbext.h>
#include <xcb/render.h>
#include <sys/uio.h>

#define XIS_EXTENSION_NAME          "X-INPUT-SCALE"
#define XIS_MAJOR_VERSION           2
#define XIS_MINOR_VERSION           0

#define X_XISQueryVersion           0
#define X_XISGetCrtcScanout         5
#define X_XISSetCrtcScanoutSource   6

#define XISScanoutCapable           (1 << 0)

static uint8_t xis_opcode;   /* 0 = extension absent */

typedef struct {
    uint8_t reqType, xisReqType;
    uint16_t length;
    uint32_t clientMajorVersion;
    uint32_t clientMinorVersion;
} XISQueryVersionReq;

typedef struct {
    uint8_t reqType, xisReqType;
    uint16_t length;
    uint32_t crtc;
} XISGetCrtcScanoutReq;

typedef struct {
    uint8_t reqType, xisReqType;
    uint16_t length;
    uint32_t crtc;
    uint32_t drawable;
    int16_t srcX, srcY;
} XISSetCrtcScanoutSourceReq;

/* One request out. `isvoid` is 0 for the ones with a reply. */
static unsigned xis_send(const void *req, size_t len, int isvoid)
{
    struct iovec parts[4];
    xcb_protocol_request_t r = {
        .count = 2, .ext = NULL, .opcode = xis_opcode, .isvoid = isvoid
    };

    parts[2].iov_base = (void *)req;
    parts[2].iov_len = len;
    parts[3].iov_base = NULL;
    parts[3].iov_len = (size_t)(-(ssize_t)len & 3);

    return xcb_send_request(comp.conn, 0, parts + 2, &r);
}

void inputscale_init(void)
{
    comp.caps.input_scale = false;
    xis_opcode = 0;

    /* An older kicomp published the confined logical boxes here, and kiwm
     * shrinks its outputs to whatever it finds. Nothing is confined any
     * more -- RandR's own geometry is the logical box now -- so a leftover
     * would only squeeze the WM's idea of a monitor for nothing. */
    if (comp.atoms.xis_confined_area != XCB_NONE)
        xcb_delete_property(comp.conn, comp.root, comp.atoms.xis_confined_area);

    xcb_query_extension_reply_t *q = xcb_query_extension_reply(comp.conn,
        xcb_query_extension(comp.conn, (uint16_t)strlen(XIS_EXTENSION_NAME),
                            XIS_EXTENSION_NAME), NULL);
    if (!q)
        return;

    if (!q->present) {
        free(q);
        return;
    }

    xis_opcode = q->major_opcode;
    free(q);

    /* The version handshake is the extension's own precondition, and it
     * doubles as proof that the opcode really answers. */
    XISQueryVersionReq req = { 0 };
    req.xisReqType = X_XISQueryVersion;
    req.clientMajorVersion = XIS_MAJOR_VERSION;
    req.clientMinorVersion = XIS_MINOR_VERSION;

    unsigned seq = xis_send(&req, sizeof(req), 0);
    xcb_generic_error_t *err = NULL;
    uint8_t *reply = xcb_wait_for_reply(comp.conn, seq, &err);

    if (err || !reply) {
        free(err);
        free(reply);
        xis_opcode = 0;
        return;
    }

    /* xXISQueryVersionReply: major at byte 8, minor at byte 12. */
    uint32_t major = *(uint32_t *)(void *)(reply + 8);
    uint32_t minor = *(uint32_t *)(void *)(reply + 12);
    free(reply);

    /* 1.x is the pointer-confinement protocol, which nothing here speaks
     * any more: without scanout sources the outputs are simply left for
     * the server to magnify. */
    if (major < 2) {
        comp_info("X-INPUT-SCALE %u.%u is too old (scanout sources need 2.0); "
                  "scaled outputs will be magnified by the server", major, minor);
        xis_opcode = 0;
        return;
    }

    comp.caps.input_scale = true;
    comp_log("X-INPUT-SCALE %u.%u", major, minor);
}

bool inputscale_crtc_capable(xcb_randr_crtc_t crtc, int *width, int *height)
{
    if (!comp.caps.input_scale || crtc == XCB_NONE)
        return false;

    XISGetCrtcScanoutReq req = { 0 };
    req.xisReqType = X_XISGetCrtcScanout;
    req.crtc = crtc;

    unsigned seq = xis_send(&req, sizeof(req), 0);
    xcb_generic_error_t *err = NULL;
    uint8_t *reply = xcb_wait_for_reply(comp.conn, seq, &err);
    if (err || !reply) {
        free(err);
        free(reply);
        return false;
    }

    /* xXISGetCrtcScanoutReply: flags at byte 1, scanoutWidth/Height at 8
     * and 10 (then the logical box, which RandR's monitors already give). */
    bool capable = (reply[1] & XISScanoutCapable) != 0;
    *width = *(uint16_t *)(void *)(reply + 8);
    *height = *(uint16_t *)(void *)(reply + 10);
    free(reply);
    return capable;
}

/* Hands `drawable` (or XCB_NONE: give back) to the output's CRTC. */
static bool set_source(const CompOutput *o, xcb_drawable_t drawable)
{
    XISSetCrtcScanoutSourceReq req = { 0 };
    req.xisReqType = X_XISSetCrtcScanoutSource;
    req.crtc = o->crtc;
    req.drawable = drawable;

    unsigned seq = xis_send(&req, sizeof(req), 1);
    xcb_generic_error_t *err = xcb_request_check(comp.conn,
                                                 (xcb_void_cookie_t){ seq });
    if (err) {
        fprintf(stderr, "kicomp: %s: X-INPUT-SCALE refused the scanout source "
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

bool inputscale_attach(CompOutput *o)
{
    o->source_set = false;
    o->source_suspended = false;

    /* A backend with a drawable of its own that only ever changes by a
     * whole frame (GLX: its window, which a swap lands on whole) needs
     * nothing more. */
    if (renderer->output_source)
        return renderer->output_source(o) != XCB_NONE;

    /* XRender draws its frame into the target in many requests, and the
     * server copies the source whenever it goes idle -- possibly between
     * two of them. So the source is a pixmap of its own, which only ever
     * receives finished frames (inputscale_present). */
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

void inputscale_detach(CompOutput *o)
{
    if (o->source_set && comp.caps.input_scale)
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

void inputscale_present(CompOutput *o, const CompRegion *damage)
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

void inputscale_frame_done(CompOutput *o)
{
    if (!o->sourced || o->source_set || o->source_suspended)
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

void inputscale_suspend(CompOutput *o)
{
    if (!o->sourced || o->source_suspended)
        return;
    if (o->source_set)
        set_source(o, XCB_NONE);
    o->source_set = false;
    o->source_suspended = true;
}

void inputscale_resume(CompOutput *o)
{
    if (!o->sourced)
        return;
    o->source_suspended = false;
    /* handed over again by inputscale_frame_done after the next frame,
     * which the give-back already damaged the whole output for */
}

bool inputscale_is_own_window(xcb_window_t id)
{
    if (id == XCB_NONE || !renderer || !renderer->output_source)
        return false;
    for (int i = 0; i < comp.output_count; i++)
        if (comp.outputs[i].sourced &&
            renderer->output_source(&comp.outputs[i]) == id)
            return true;
    return false;
}

void inputscale_shutdown(void)
{
    for (int i = 0; i < comp.output_count; i++)
        inputscale_detach(&comp.outputs[i]);
    xcb_flush(comp.conn);
}
