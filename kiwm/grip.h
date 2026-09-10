/*
 * kiwm - the invisible resize grip around a window.
 *
 * The grip is the area where a plain click (no modifier) resizes the
 * window instead of going to the application. It used to be a strip
 * *inside* the frame, tested arithmetically against the pointer position
 * in events.c, which had two costs the user paid for: every pixel of it
 * was a pixel the application didn't get (a 12px grip shadows a scrollbar
 * sitting at the window's edge), and showing the right resize cursor there
 * meant taking an xcb_grab_pointer() with that cursor for as long as the
 * pointer hovered, because the area belonged to the *client's* window and
 * its cursor is the application's business.
 *
 * So the grip moved outside the frame, where nothing else wants those
 * pixels. Nothing can be tested arithmetically any more -- events outside
 * the frame do not reach it -- so the grip is windows now:
 *
 *      ring (InputOnly, root child, stacked just below the frame)
 *       |
 *       +-- 8 InputOnly children: N NE E SE S SW W NW
 *
 * The eight children are what earns the shape. Each one carries its own
 * resize cursor as a plain window attribute, which is the entire reason
 * the pointer grab is gone: X shows the cursor of the window under the
 * pointer, so hovering an edge just works, with no grab, no ungrab and no
 * hover state to keep in sync.
 *
 * The ring parent earns its keep in the stacking order. The children only
 * cover the margin, but the parent covers the frame *and* the margin, so
 * the whole grip rides in the stack as one window that restack_all() can
 * chain below its frame -- one request per client rather than eight. It
 * sits below the frame so the ring never wins over its own window, and
 * being at its frame's level is what makes a click in the margin between
 * two adjacent windows go to whichever of them is actually on top.
 *
 * The ring is InputOnly throughout: no pixels, no backing store, nothing
 * to draw and nothing for a compositor to composite.
 */
#ifndef KIWM_GRIP_H
#define KIWM_GRIP_H

#include "wm.h"

/* Which edge or corner of the ring a grip window is. The order matters
 * only in that GRIP_COUNT closes it. */
typedef enum {
    GRIP_N, GRIP_NE, GRIP_E, GRIP_SE,
    GRIP_S, GRIP_SW, GRIP_W, GRIP_NW,
    GRIP_COUNT
} GripEdge;

/* Creates the ring and its eight children for a client whose frame
 * already exists, and stacks the ring below that frame. A no-op when
 * kiwm.conf's resize_grip= is 0. */
void grip_create(Client *c);

/* Destroys the whole ring. Safe on a client that never had one. */
void grip_destroy(Client *c);

/* Follows the frame: repositions the ring and its children for the
 * client's current geometry, and maps or unmaps the ring according to
 * whether this window should have a grip at all right now (a maximized,
 * fullscreen, shaded, unmapped or fixed-size window should not).
 *
 * Skipped for the client currently being dragged -- during a drag the
 * pointer is grabbed by the drag itself, so the ring is unreachable, and
 * apply_frame_geometry() runs on every motion event. finish_drag() catches
 * it up once at the end. */
void grip_sync(Client *c);

/* Which client and edge a window belongs to, for the ButtonPress that
 * starts a resize. False when the window is not a grip. */
bool grip_lookup(xcb_window_t window, Client **out, GripEdge *edge);

/* Is this window part of some client's ring? Used by restack_all() to skip
 * ring windows when it walks the root's children -- they are placed
 * relative to their frame, not sorted into a layer. */
bool grip_owns_window(xcb_window_t window);

/* The ring window to chain directly below `c`'s frame, or XCB_NONE when
 * this client has no ring. For restack_all(). */
xcb_window_t grip_ring_window(const Client *c);

/* Translates an edge into the four arguments begin_drag_at() takes:
 * which corner grows (1/0, or -1 for "this axis isn't moving") and which
 * axes the drag may change at all. */
void grip_drag_params(GripEdge edge, int *right, int *bottom,
                      bool *axis_x, bool *axis_y);

/* The frame this ring belongs to was just mapped or unmapped, so the ring
 * follows it. Driven from the frame's own MapNotify/UnmapNotify (root has
 * SubstructureNotify selected), which is the only hook that catches every
 * path: minimize, restore, a desktop switch, an output reassignment and
 * set_client_desktop() all map and unmap the frame, and the last three do
 * it *without* touching Client::mapped. */
void grip_frame_mapped(Client *c, bool mapped);

#endif /* KIWM_GRIP_H */
