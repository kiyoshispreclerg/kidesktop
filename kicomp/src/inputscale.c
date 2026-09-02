/* See inputscale.h. Raw wire requests: this protocol is still a draft in
 * the server fork and has no client binding, so the structs below are the
 * contract, copied from the fork's Xext/inputscale/inputscaleproto.h.
 *
 * XCB fills in byte 0 (the major opcode) and the length field itself when
 * a request is sent with ext == NULL and opcode = major; what has to be
 * written here is the minor opcode and the payload. */
#include "inputscale.h"

#include <stdlib.h>
#include <string.h>
#include <xcb/xcbext.h>
#include <sys/uio.h>

#define XIS_EXTENSION_NAME    "X-INPUT-SCALE"
#define XIS_MAJOR_VERSION     1
#define XIS_MINOR_VERSION     0

#define X_XISQueryVersion     0
#define X_XISSetCrtcConfine   1
#define X_XISGetCrtcConfine   2
#define X_XISResetCrtcConfine 3

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
    int16_t x, y;
    uint16_t width, height;
} XISSetCrtcConfineReq;

typedef struct {
    uint8_t reqType, xisReqType;
    uint16_t length;
    uint32_t crtc;
} XISCrtcReq;

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
     * doubles as proof that the opcode really answers: a server that
     * merely reports the name would fail here rather than silently
     * ignoring every confinement. */
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

    comp.caps.input_scale = true;
    comp_log("X-INPUT-SCALE %u.%u", major, minor);
}

static void set_confine(const CompOutput *o)
{
    XISSetCrtcConfineReq req = { 0 };
    req.xisReqType = X_XISSetCrtcConfine;
    req.crtc = o->crtc;
    req.x = (int16_t)o->rect.x;
    req.y = (int16_t)o->rect.y;
    req.width = (uint16_t)o->rect.w;
    req.height = (uint16_t)o->rect.h;

    xis_send(&req, sizeof(req), 1);

    comp_log("confine %s to %dx%d+%d+%d (scanout %dx%d)", o->name,
             o->rect.w, o->rect.h, o->rect.x, o->rect.y,
             o->physical.w, o->physical.h);
}

static void reset_confine(const CompOutput *o)
{
    XISCrtcReq req = { 0 };
    req.xisReqType = X_XISResetCrtcConfine;
    req.crtc = o->crtc;

    xis_send(&req, sizeof(req), 1);
}

void inputscale_apply(void)
{
    if (!comp.caps.input_scale)
        return;

    for (int i = 0; i < comp.output_count; i++) {
        CompOutput *o = &comp.outputs[i];
        if (o->crtc == XCB_NONE)
            continue;

        /* Reset, not "leave alone", for an unscaled output: this
         * compositor may have scaled it a moment ago, and a confinement
         * nobody is drawing for is a pointer that can't reach part of its
         * own screen. */
        if (o->scale > 1.0f)
            set_confine(o);
        else
            reset_confine(o);
    }

    xcb_flush(comp.conn);
}

void inputscale_shutdown(void)
{
    if (!comp.caps.input_scale)
        return;

    for (int i = 0; i < comp.output_count; i++)
        if (comp.outputs[i].crtc != XCB_NONE)
            reset_confine(&comp.outputs[i]);

    xcb_flush(comp.conn);
}
