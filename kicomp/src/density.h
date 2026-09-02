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

/* _X_DENSITY_SCALE or _X_DENSITY_PIXMAP changed. `on` says on which
 * window: the client (the app's contents) or the frame (the WM's
 * decoration) -- two independent answers to two independent requests. */
void density_property_changed(CompWindow *w, xcb_window_t on);

/* The window is going away, or its density is no longer wanted. */
void density_forget(CompWindow *w);

/* Whether this window currently has denser contents than its logical
 * size, and what the factor is -- asked separately for the client's
 * contents and for the frame's decoration. */
bool density_active(const CompWindow *w, float *factor);
bool deco_density_active(const CompWindow *w, float *factor);

void density_shutdown(void);

#endif /* KICOMP_DENSITY_H */
