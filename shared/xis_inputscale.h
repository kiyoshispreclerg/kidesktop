/* xis_inputscale - client side of the fork's X-INPUT-SCALE extension (per-CRTC
 * cursor confinement; see the fork's doc/x11-per-output-scaling-extension.md),
 * shared by the programs that need to know whether an output is scaled.
 *
 * No client binding exists for the protocol, so the requests are written on
 * the wire over XCB. Everything takes the caller's xcb_connection_t; an Xlib
 * program passes XGetXCBConnection(dpy) (synchronous requests are safe to
 * interleave there) and, for the live events, a second dedicated
 * xcb_connect() instead -- sharing the Xlib connection's event queue made
 * xispanel spin at 100% CPU (see reference_xis_extensions.md).
 *
 * Two layers:
 *   - the protocol: present / get confine / select events;
 *   - XisScaleMap, a snapshot of the confined CRTCs with the two boxes the
 *     compositor's HiDPI scheme hinges on (the physical scanout box and the
 *     logical confine box) and the pure maths between them -- what a screen
 *     recorder needs to draw the pointer where, and as large as, the
 *     hardware cursor shows on the physical framebuffer.
 *
 * Not a setter: confining is a compositor's job (kicomp has its own). */
#ifndef XIS_INPUTSCALE_H
#define XIS_INPUTSCALE_H

#include <stdint.h>
#include <xcb/xcb.h>
#include <xcb/randr.h>

#define XIS_SCALEMAP_MAX 16

/* Whether the server has the extension at all. */
int xis_inputscale_present(xcb_connection_t *c);

/* XISGetCrtcConfine: 1 and the box (desktop coordinates) when the CRTC is
 * confined, 0 when it is not or the extension is missing. Any out pointer may
 * be NULL. */
int xis_inputscale_get_confine(xcb_connection_t *c, xcb_randr_crtc_t crtc, int *x, int *y, int *w, int *h);

/* XISSelectInput for XISConfineNotify on `window` (the root, normally).
 * Returns 1 when events are on, 0 when this server build has none (the
 * extension's first_event is still unallocated): the caller falls back to
 * polling. */
int xis_inputscale_select_events(xcb_connection_t *c, xcb_window_t window);

/* Whether `ev` is an XISConfineNotify on this connection. */
int xis_inputscale_is_event(xcb_connection_t *c, const xcb_generic_event_t *ev);

/* One confined CRTC. */
typedef struct {
    xcb_randr_crtc_t crtc;
    int x, y;       /* the CRTC's origin in root coordinates */
    int pw, ph;     /* physical scanout size (from the mode, rotation applied) */
    int cx, cy;     /* the confine box: the logical desktop the compositor ... */
    int cw, ch;     /* ... magnifies to fill the scanout */
    double sx, sy;  /* physical / logical */
} XisConfinedCrtc;

typedef struct {
    int n;
    XisConfinedCrtc crtc[XIS_SCALEMAP_MAX];
} XisScaleMap;

/* Reads every CRTC's confinement. Returns the number of confined CRTCs (also
 * m->n); 0 means nothing is scaled, which is also the answer without the
 * extension. While a CRTC is confined the fork's RandR reports the confine
 * box in place of the scanout box, so the physical size is taken from the
 * CRTC's mode, not from its reported geometry. */
int xis_scalemap_load(xcb_connection_t *c, xcb_window_t root, XisScaleMap *m);

/* Asks for what invalidates a map: XISConfineNotify (when the server has it)
 * and RandR screen/CRTC changes, as events on this connection. Returns 1 when
 * the confine events are on, as xis_inputscale_select_events(). */
int xis_scalemap_select_changes(xcb_connection_t *c, xcb_window_t root);

/* Whether `ev` is one of those. */
int xis_scalemap_is_change_event(xcb_connection_t *c, const xcb_generic_event_t *ev);

/* The confined CRTC whose physical box contains the root point (x, y), or
 * NULL. A logical point and its physical image are on the same CRTC (the
 * physical box is the bigger one and shares the origin), so either can be
 * looked up. */
const XisConfinedCrtc *xis_scalemap_find(const XisScaleMap *m, int x, int y);

/* Where the logical point (x, y) on `k` shows on the physical framebuffer,
 * the same arithmetic the server uses for the hardware cursor. */
void xis_scalemap_to_physical(const XisConfinedCrtc *k, int x, int y, int *px, int *py);

typedef struct { int x, y, w, h; } XisRect;

/* Where a rectangle in logical root coordinates (a window's geometry) shows
 * on the physical framebuffer, as up to `max` pieces: the part inside each
 * confined logical box is scaled onto its CRTC (rounded outwards, so a
 * censored window never leaves a sliver), the rest is left as it is. Returns
 * the number of pieces written to `out`. */
int xis_scalemap_rect(const XisScaleMap *m, XisRect in, XisRect *out, int max);

#endif /* XIS_INPUTSCALE_H */
