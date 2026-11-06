/*
 * kicomp - X-DENSITY: asking a client to redraw its own contents densely.
 *
 * The other half of per-output scaling (comp.h). Scaling alone magnifies
 * whatever the client drew at logical size, which is exactly as sharp as
 * it sounds; this is how it stops being blurry. The compositor asks a
 * window to redraw at N times its logical size into an auxiliary pixmap,
 * and samples that pixmap instead of the window's own contents. The
 * window's real geometry never changes -- it stays the same size in the
 * WM's layout, keeps its decoration, its focus, its whole ICCCM/EWMH life.
 * Only the pixels are denser.
 *
 * Three ordinary window properties, no extension required
 * (TESTS/X-DENSITY.md):
 *
 *   _X_DENSITY_REQUESTED  compositor -> client, [num, den]: "redraw at
 *                         num/den". Deleted to mean 1/1.
 *   _X_DENSITY_SCALE      client -> compositor, [num, den]: what it is
 *                         *actually* rendering at, which may not be what
 *                         was asked for. This is the one to believe.
 *   _X_DENSITY_PIXMAP     client -> compositor, [xid]: the auxiliary
 *                         pixmap, logical size x effective density.
 *
 * And one selection, `_X_DENSITY_MANAGER_S<screen>`, owned exactly like
 * _NET_WM_CM_S<screen>: a well-behaved client picks a density other than
 * 1 only while somebody is holding it, so that killing the compositor
 * leaves every window drawing itself normally again rather than frozen at
 * a density nothing is sampling.
 *
 * A pixmap generates no Damage of its own, so "the contents changed" is
 * signalled by the client rewriting _X_DENSITY_PIXMAP with the same XID.
 * Every property change here is therefore also a repaint.
 *
 * And two optional ones for the holes in that pixmap, where a native
 * child (a video player's --wid) draws in its own window and the client
 * has nothing to put:
 *
 *   _X_DENSITY_PASSTHROUGH  [x, y, w, h]...: show the window's own
 *                           pixels there, magnified. The toolkit's.
 *   _X_DENSITY_SOURCES      [x, y, w, h, window]...: show that window
 *                           there instead -- redirected by the client
 *                           and rendered at _X_DENSITY_SCALE, its pixel
 *                           (0, 0) at the corner, clipped to the
 *                           rectangle. The app's.
 *
 * Client-relative logical rectangles. A source's pixmap is named here and
 * its Damage followed like a window's; the renderers draw both kinds over
 * the dense layer, which leaves them out.
 */
#ifndef KICOMP_DENSITY_H
#define KICOMP_DENSITY_H

#include "comp.h"

/* Acquires the manager selection. Called once at startup, after the
 * atoms. */
void density_init(void);

/* Asks this window for the density its output is scaled to (or for 1/1,
 * which deletes the request). Called when a window appears, when it
 * moves -- it may have crossed onto a differently scaled monitor -- and
 * for every window when the outputs change. */
void density_update_window(CompWindow *w);
void density_update_all(void);

/* _X_DENSITY_SCALE or _X_DENSITY_PIXMAP (`atom`) changed. `on` says on
 * which window: the client (the app's contents) or the frame (the WM's
 * decoration) -- two independent answers to two independent requests.
 * The same pixmap rewritten is a new frame, repainted from the pixmap's
 * own Damage once that has reported, and whole otherwise. */
void density_property_changed(CompWindow *w, xcb_window_t on, xcb_atom_t atom);

/* A DamageNotify on `drawable`: true if it is a dense pixmap someone
 * published, now marked for damage.c to collect. */
bool density_pixmap_damaged(xcb_drawable_t drawable);

/* _X_DENSITY_PASSTHROUGH or _X_DENSITY_SOURCES changed on the client. */
void density_holes_changed(CompWindow *w, xcb_atom_t atom);

/* Events on a window that is not one of ours, and might be a source: its
 * Damage reported, it was resized or mapped (its pixmap is renamed), it
 * was unmapped or destroyed (its pixmap is dropped). Each a short walk
 * when nothing publishes sources. */
void density_source_damaged(xcb_drawable_t drawable);
void density_source_configured(xcb_window_t window);
void density_source_gone(xcb_window_t window, bool destroyed);

/* Where a source is shown, in root coordinates, with the window at its
 * own place (damage.c's coordinates). False while it has no pixmap, and
 * so is not drawn. */
bool density_source_root_rect(const CompWindow *w, const CompDensitySource *s,
                              CompRect *out);

/* Is this passthrough area (one of w->density_through) entirely inside a
 * source that is drawn? Then that source fills it, and the window's own
 * pixels there are not drawn. */
bool density_through_filled(const CompWindow *w, const CompRect *through);

/* Is `r` (root coordinates, as damage.c has it) entirely where a source
 * is shown? Then the window's own pixels changing there reach no screen:
 * they are the client's 1x copy of its video, kept for recorders. */
bool density_source_hides(const CompWindow *w, const CompRect *r);

/* The window is going away, or its density is no longer wanted. */
void density_forget(CompWindow *w);

/* Whether this window currently has denser contents than its logical
 * size, and what the factor is -- asked separately for the client's
 * contents and for the frame's decoration. */
bool density_active(const CompWindow *w, float *factor);
bool deco_density_active(const CompWindow *w, float *factor);

void density_shutdown(void);

#endif /* KICOMP_DENSITY_H */
