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

/* The confined areas as a root property, for everything that has to lay
 * windows out inside them (see inputscale.h). */
static void publish_confined_area(void)
{
    if (comp.atoms.xis_confined_area == XCB_NONE)
        return;

    uint32_t rects[MAX_OUTPUTS * 4];
    int n = 0;

    for (int i = 0; i < comp.output_count && n < MAX_OUTPUTS * 4; i++) {
        const CompOutput *o = &comp.outputs[i];
        if (o->scale <= 1.0f)
            continue;
        rects[n++] = (uint32_t)o->rect.x;
        rects[n++] = (uint32_t)o->rect.y;
        rects[n++] = (uint32_t)o->rect.w;
        rects[n++] = (uint32_t)o->rect.h;
    }

    if (n == 0) {
        /* Deleted rather than written empty: absent means "no output is
         * confined", which is the state of almost every machine, and a
         * consumer that finds nothing has nothing to think about. */
        xcb_delete_property(comp.conn, comp.root, comp.atoms.xis_confined_area);
        return;
    }

    xcb_change_property(comp.conn, XCB_PROP_MODE_REPLACE, comp.root,
                        comp.atoms.xis_confined_area, XCB_ATOM_CARDINAL, 32,
                        (uint32_t)n, rects);
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

    publish_confined_area();
    xcb_flush(comp.conn);
}

void inputscale_release_all(void)
{
    if (!comp.caps.input_scale)
        return;

    for (int i = 0; i < comp.output_count; i++)
        if (comp.outputs[i].crtc != XCB_NONE)
            reset_confine(&comp.outputs[i]);

    /* Synchronously: the caller is about to ask RandR for the geometry
     * these confinements were changing the answer to, and a request still
     * sitting in the output buffer would not have changed it back yet. A
     * round trip on anything is enough to make the server have processed
     * them. */
    free(xcb_get_input_focus_reply(comp.conn,
                                   xcb_get_input_focus(comp.conn), NULL));
}

void inputscale_shutdown(void)
{
    inputscale_release_all();

    /* And the property with them: a WM that outlives this compositor must
     * not keep laying windows out inside a logical box nobody is
     * magnifying any more. */
    if (comp.atoms.xis_confined_area != XCB_NONE)
        xcb_delete_property(comp.conn, comp.root, comp.atoms.xis_confined_area);
    xcb_flush(comp.conn);
}
