/*
 * kicomp - X-SCALE: handing a scaled output's sharp picture to the
 * server.
 *
 * An output is scaled the ordinary RandR way: a CRTC transform (`xrandr
 * --scale-from`, or kicomp applying the fork's DPI property, output.c)
 * makes its footprint in the root -- its logical box, comp.h's `rect` --
 * smaller than its mode. The root, the WM, input and the pointer's travel
 * between outputs all follow from that by themselves, on any server. What
 * a transform alone shows is a *blurry* picture: the server fills the
 * CRTC by magnifying the logical root.
 *
 * X-SCALE (the XiS fork's extension, doc/x-scale.md there) lets a
 * compositor that drew the output at physical
 * size give that picture to the CRTC instead: a *scanout source*, any
 * drawable of the CRTC's physical size, which the server watches with
 * Damage and copies onto the CRTC by itself. So when the server has it and
 * says a CRTC is capable, that output is drawn magnified into a physical-
 * sized target (`physical`, `scale` > 1) and handed over; everywhere else
 * -- a stock server, another DDX, a backend that can't -- the output is
 * drawn at its logical size like any other and the server magnifies it.
 * Correct either way, sharp where it can be. The capability decides,
 * never a guess about which server this is (section 17/30).
 *
 * The source is a drawable whose every change is a finished frame, because
 * the server copies whenever it happens to go idle: the GLX backend's own
 * window (a swap lands whole), or, for XRender, a pixmap of our own the
 * finished target is composited into (xscale_present), so a half-drawn
 * frame is never what gets copied.
 *
 * No client binding exists for this protocol yet, so the requests are
 * written on the wire directly, mirroring the server tree's
 * Xext/xscale/xscaleproto.h and its xscale-smoke-test.c.
 */
#ifndef KICOMP_XSCALE_H
#define KICOMP_XSCALE_H

#include "comp.h"

/* Probes the extension and sets comp.caps.xscale.
 * Also deletes the `_XIS_CONFINED_AREA` root property an older kicomp
 * (the X-INPUT-SCALE experiment, pointer confinement) may have left behind. */
void xscale_init(void);

/* Can `crtc` take a source right now, and at what size? False on a server
 * without the extension, or a CRTC without a scale-only transform its DDX
 * can be fed for. */
bool xscale_crtc_capable(xcb_randr_crtc_t crtc, int *width, int *height);

/* Gets a sourced output ready (o->sourced, after renderer->init): the
 * XRender path's own source pixmap. The source itself is only handed to
 * the server after the first frame (xscale_frame_done), so the CRTC
 * never shows a picture nothing has been drawn into. False if it can't
 * be set up -- the caller then draws the output unscaled. */
bool xscale_attach(CompOutput *o);

/* Gives the CRTC back to the server and frees what attach made. */
void xscale_detach(CompOutput *o);

/* The XRender path's present: the repainted part of the finished target
 * into the source pixmap. Only for outputs with o->source_picture. */
void xscale_present(CompOutput *o, const CompRegion *damage);

/* After each painted frame: hands the source over the first time. */
void xscale_frame_done(CompOutput *o);

/* While a fullscreen window is unredirected on this output it draws into
 * the root, so the CRTC must show the (magnified) root again: suspend
 * gives the CRTC back without forgetting the source, resume hands it over
 * again with the next frame. */
void xscale_suspend(CompOutput *o);
void xscale_resume(CompOutput *o);

/* Presents (o->source_presented): hands the server `pixmap`, a whole
 * finished frame of the output's physical size, to show from the next
 * vblank on -- flipped onto the CRTC where the server can, copied
 * otherwise, and the output's source either way. `damage` is what changed
 * (logical root coordinates; NULL or full: all of it), which the server
 * only needs for a copy. Completion and idle come back as events
 * (xscale_decode_event). False when nothing was sent. */
bool xscale_present_pixmap(CompOutput *o, xcb_pixmap_t pixmap,
                           uint32_t serial, const CompRegion *damage);

/* An X-SCALE presentation event, decoded. */
typedef struct {
    bool complete;          /* else idle */
    xcb_randr_crtc_t crtc;
    uint32_t serial;
    uint8_t mode;           /* complete: XSCALE_MODE_* */
    uint64_t msc, ust;      /* complete: 0 when unknown */
    xcb_pixmap_t pixmap;    /* idle */
} XScaleEvent;

#define XSCALE_MODE_COPY 0
#define XSCALE_MODE_FLIP 1
#define XSCALE_MODE_SKIP 2

/* Is `ev` an X-SCALE completion or idle event? Fills `out` if so. */
bool xscale_decode_event(const xcb_generic_event_t *ev, XScaleEvent *out);

/* Is this one of the windows kicomp draws sourced outputs into (the GLX
 * backend's)? They are top-level, so the window tracker must skip them. */
bool xscale_is_own_window(xcb_window_t id);

/* Releases every source. Closing the connection would do it too -- the
 * server drops a source with the client that set it -- but shutting down
 * cleanly says so explicitly. */
void xscale_shutdown(void);

#endif /* KICOMP_XSCALE_H */
