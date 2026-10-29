/* See xis_inputscale.h. Wire layouts are the fork's
 * Xext/inputscale/inputscaleproto.h, the same as xispanel/xisback/kicomp use. */
#include "xis_inputscale.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/xcbext.h>

#define X_XISGetCrtcConfine 2
#define X_XISSelectInput 4
#define XISConfineNotify 0
#define XISConfineNotifyMask (1u << XISConfineNotify)

static xcb_extension_t xis_ext = { "X-INPUT-SCALE", 0 };

static const xcb_query_extension_reply_t *ext_data(xcb_connection_t *c)
{
    if (!c || xcb_connection_has_error(c))
        return NULL;
    const xcb_query_extension_reply_t *e = xcb_get_extension_data(c, &xis_ext);
    return e && e->present ? e : NULL;
}

/* xcb fills in byte 0 (the major opcode) and the length field; byte 1 is
 * ours. The pad entry is what rounds the request to four bytes. */
static unsigned send_req(xcb_connection_t *c, uint8_t minor, void *req, size_t len, int isvoid)
{
    struct iovec parts[4] = { { 0 } };
    xcb_protocol_request_t r = { .count = 2, .ext = &xis_ext, .opcode = minor, .isvoid = isvoid };
    parts[2].iov_base = req;
    parts[2].iov_len = len;
    parts[3].iov_base = NULL;
    parts[3].iov_len = (size_t)(-(ssize_t)len & 3);
    return xcb_send_request(c, 0, parts + 2, &r);
}

int xis_inputscale_present(xcb_connection_t *c)
{
    return ext_data(c) != NULL;
}

int xis_inputscale_get_confine(xcb_connection_t *c, xcb_randr_crtc_t crtc, int *x, int *y, int *w, int *h)
{
    if (!ext_data(c))
        return 0;
    struct {
        uint8_t reqType, xisReqType;
        uint16_t length;
        uint32_t crtc;
    } req = { 0 };
    req.crtc = crtc;
    unsigned seq = send_req(c, X_XISGetCrtcConfine, &req, sizeof(req), 0);
    xcb_generic_error_t *err = NULL;
    uint8_t *rep = xcb_wait_for_reply(c, seq, &err);
    if (err) {
        free(err);
        free(rep);
        return 0;
    }
    if (!rep)
        return 0;
    /* xXISGetCrtcConfineReply: byte 1 = active, then x/y int16 and
     * width/height uint16 at bytes 8, 10, 12, 14. */
    int active = rep[1];
    if (active) {
        int16_t rx, ry;
        uint16_t rw, rh;
        memcpy(&rx, rep + 8, 2);
        memcpy(&ry, rep + 10, 2);
        memcpy(&rw, rep + 12, 2);
        memcpy(&rh, rep + 14, 2);
        if (x) *x = rx;
        if (y) *y = ry;
        if (w) *w = rw;
        if (h) *h = rh;
    }
    free(rep);
    return active;
}

/* first_event below 64 (0 in practice) means this build never allocated an
 * event number: event 0 is the wire's "this is an error", never select on
 * it. */
static int event_number(xcb_connection_t *c)
{
    const xcb_query_extension_reply_t *e = ext_data(c);
    return e && e->first_event >= 64 ? e->first_event + XISConfineNotify : 0;
}

int xis_inputscale_select_events(xcb_connection_t *c, xcb_window_t window)
{
    if (!event_number(c))
        return 0;
    struct {
        uint8_t reqType, xisReqType;
        uint16_t length;
        uint32_t window;
        uint32_t eventMask;
    } req = { 0 };
    req.window = window;
    req.eventMask = XISConfineNotifyMask;
    send_req(c, X_XISSelectInput, &req, sizeof(req), 1);
    xcb_flush(c);
    return 1;
}

int xis_inputscale_is_event(xcb_connection_t *c, const xcb_generic_event_t *ev)
{
    int n = event_number(c);
    return n && (ev->response_type & 0x7f) == n;
}

/* ---- the map -------------------------------------------------------------- */

int xis_scalemap_load(xcb_connection_t *c, xcb_window_t root, XisScaleMap *m)
{
    m->n = 0;
    if (!ext_data(c))
        return 0;
    xcb_randr_get_screen_resources_current_reply_t *res = xcb_randr_get_screen_resources_current_reply(
        c, xcb_randr_get_screen_resources_current(c, root), NULL);
    if (!res)
        return 0;
    const xcb_randr_crtc_t *crtcs = xcb_randr_get_screen_resources_current_crtcs(res);
    int ncrtc = xcb_randr_get_screen_resources_current_crtcs_length(res);
    const xcb_randr_mode_info_t *modes = xcb_randr_get_screen_resources_current_modes(res);
    int nmodes = xcb_randr_get_screen_resources_current_modes_length(res);

    for (int i = 0; i < ncrtc && m->n < XIS_SCALEMAP_MAX; i++) {
        XisConfinedCrtc k = { 0 };
        if (!xis_inputscale_get_confine(c, crtcs[i], &k.cx, &k.cy, &k.cw, &k.ch) || k.cw <= 0 || k.ch <= 0)
            continue;
        xcb_randr_get_crtc_info_reply_t *ci =
            xcb_randr_get_crtc_info_reply(c, xcb_randr_get_crtc_info(c, crtcs[i], XCB_CURRENT_TIME), NULL);
        if (!ci)
            continue;
        k.crtc = crtcs[i];
        k.x = ci->x;
        k.y = ci->y;
        for (int j = 0; j < nmodes; j++)
            if (modes[j].id == ci->mode) {
                int rotated = ci->rotation & (XCB_RANDR_ROTATION_ROTATE_90 | XCB_RANDR_ROTATION_ROTATE_270);
                k.pw = rotated ? modes[j].height : modes[j].width;
                k.ph = rotated ? modes[j].width : modes[j].height;
            }
        free(ci);
        if (k.pw <= 0 || k.ph <= 0)
            continue;
        k.sx = (double)k.pw / k.cw;
        k.sy = (double)k.ph / k.ch;
        m->crtc[m->n++] = k;
    }
    free(res);
    return m->n;
}

int xis_scalemap_select_changes(xcb_connection_t *c, xcb_window_t root)
{
    const xcb_query_extension_reply_t *rr = xcb_get_extension_data(c, &xcb_randr_id);
    if (rr && rr->present)
        xcb_randr_select_input(c, root,
                               XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE | XCB_RANDR_NOTIFY_MASK_CRTC_CHANGE);
    return xis_inputscale_select_events(c, root);
}

int xis_scalemap_is_change_event(xcb_connection_t *c, const xcb_generic_event_t *ev)
{
    if (xis_inputscale_is_event(c, ev))
        return 1;
    const xcb_query_extension_reply_t *rr = xcb_get_extension_data(c, &xcb_randr_id);
    if (!rr || !rr->present)
        return 0;
    int t = ev->response_type & 0x7f;
    return t == rr->first_event + XCB_RANDR_SCREEN_CHANGE_NOTIFY || t == rr->first_event + XCB_RANDR_NOTIFY;
}

const XisConfinedCrtc *xis_scalemap_find(const XisScaleMap *m, int x, int y)
{
    for (int i = 0; i < m->n; i++) {
        const XisConfinedCrtc *k = &m->crtc[i];
        if (x >= k->x && x < k->x + k->pw && y >= k->y && y < k->y + k->ph)
            return k;
    }
    return NULL;
}

void xis_scalemap_to_physical(const XisConfinedCrtc *k, int x, int y, int *px, int *py)
{
    *px = k->x + (int)lround((x - k->x) * k->sx);
    *py = k->y + (int)lround((y - k->y) * k->sy);
}

#define RECT_PIECES 32

static int rect_inter(XisRect a, XisRect b, XisRect *o)
{
    int x1 = a.x > b.x ? a.x : b.x, y1 = a.y > b.y ? a.y : b.y;
    int x2 = a.x + a.w < b.x + b.w ? a.x + a.w : b.x + b.w;
    int y2 = a.y + a.h < b.y + b.h ? a.y + a.h : b.y + b.h;
    if (x2 <= x1 || y2 <= y1)
        return 0;
    *o = (XisRect){ x1, y1, x2 - x1, y2 - y1 };
    return 1;
}

int xis_scalemap_rect(const XisScaleMap *m, XisRect in, XisRect *out, int max)
{
    XisRect rest[RECT_PIECES] = { { 0 } }, next[RECT_PIECES] = { { 0 } };
    int nrest = 1, nout = 0;
    rest[0] = in;
    for (int i = 0; i < m->n; i++) {
        const XisConfinedCrtc *k = &m->crtc[i];
        XisRect box = { k->cx, k->cy, k->cw, k->ch };
        int nnext = 0;
        for (int j = 0; j < nrest; j++) {
            XisRect p = rest[j], in_box;
            if (!rect_inter(p, box, &in_box)) {
                if (nnext < RECT_PIECES)
                    next[nnext++] = p;
                continue;
            }
            if (nout < max) {
                int x1 = k->x + (int)floor((in_box.x - k->x) * k->sx);
                int y1 = k->y + (int)floor((in_box.y - k->y) * k->sy);
                int x2 = k->x + (int)ceil((in_box.x + in_box.w - k->x) * k->sx);
                int y2 = k->y + (int)ceil((in_box.y + in_box.h - k->y) * k->sy);
                out[nout++] = (XisRect){ x1, y1, x2 - x1, y2 - y1 };
            }
            /* what is left of p around the box: above, below, left, right */
            XisRect left[4] = {
                { p.x, p.y, p.w, in_box.y - p.y },
                { p.x, in_box.y + in_box.h, p.w, p.y + p.h - (in_box.y + in_box.h) },
                { p.x, in_box.y, in_box.x - p.x, in_box.h },
                { in_box.x + in_box.w, in_box.y, p.x + p.w - (in_box.x + in_box.w), in_box.h },
            };
            for (int s = 0; s < 4; s++)
                if (left[s].w > 0 && left[s].h > 0 && nnext < RECT_PIECES)
                    next[nnext++] = left[s];
        }
        for (int j = 0; j < nnext; j++)
            rest[j] = next[j];
        nrest = nnext;
    }
    for (int j = 0; j < nrest && nout < max; j++)
        out[nout++] = rest[j];
    return nout;
}
