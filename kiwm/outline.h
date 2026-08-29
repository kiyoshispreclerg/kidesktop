#ifndef KIWM_OUTLINE_H
#define KIWM_OUTLINE_H

#include "wm.h"

/* The outline: a hollow rectangle drawn *around* where a window is or is
 * about to be, in the focused decoration's own color, with nothing inside
 * it -- the classic "wireframe" preview xfwm and every other WM of that
 * lineage draws.
 *
 * Two things use it, both cases where kiwm needs to point at a window
 * without moving or resizing anything yet:
 *
 *   - the window switcher with osd_live_preview=0 (osd.c), which doesn't
 *     raise or focus anything until the modifier is released, so the
 *     outline is the only thing saying *where* the highlighted window
 *     actually is;
 *   - dragging a window to a screen edge with live_snap_resize=0
 *     (events.c), where the outline shows the size the window will take
 *     when the button is released instead of resizing it there and back
 *     mid-drag.
 *
 * It lives in its own stacking layer (wm.h's LAYER_OUTLINE) directly below
 * LAYER_OSD: above every client, including an active fullscreen one, but
 * still under the switcher overlay that's driving it. */

/* One rectangle to outline -- a *frame* rect, the same coordinate space
 * Client::x/y/frame_width/frame_height use. */
typedef struct {
    int x, y, w, h;
} OutlineRect;

/* Shows an outline around each of `n` rectangles at once, replacing
 * whatever was showing. More than one is what a linked resize needs
 * (kiwm.conf's link_resize_neighbors=): the window being resized and every
 * neighbor moving with it have to be previewed together, or the preview
 * lies about what releasing the button will do. They cost nothing extra --
 * the outline is a single shaped window either way, and this is still one
 * request. */
void outline_show_rects(const OutlineRect *rects, int n);

/* Shows the outline around the rectangle (x, y, w, h) -- a *frame* rect,
 * the same coordinate space Client::x/y/frame_width/frame_height use. The
 * band straddles that rectangle's edges, half of it outside and half in.
 * Moves/resizes the existing outline when one is already up. */
void outline_show(int x, int y, int w, int h);

/* Hides it. No-op when nothing is showing. */
void outline_hide(void);

/* Whether the outline is currently up. */
bool outline_visible(void);

/* Whether `window` is the outline's own window, currently up -- how
 * client.c's restack_all() recognizes it (it's override-redirect and never
 * a Client, same as the switcher overlay and the window menu). */
bool outline_owns_window(xcb_window_t window);

/* Repaints after an Expose on the outline's window; a no-op for any other
 * window. */
void outline_handle_expose(xcb_window_t window);

#endif /* KIWM_OUTLINE_H */
