/*
 * kicomp - handing one output over to the window that fills it.
 *
 * A compositor's whole job is to get between an application and the
 * screen: the window draws into a pixmap and the compositor draws that
 * pixmap onto the display. For a game running at the monitor's refresh
 * rate that middle step is pure cost -- a full-screen copy per frame,
 * and a frame of latency -- which is why every compositor learns, sooner
 * or later, to step out of the way when a window covers the whole screen
 * and nothing else is visible over it: it *unredirects* the window, and
 * the server can then scan the window's own buffer out directly (Present
 * reports FLIP instead of COPY).
 *
 * The usual version of that trick is all or nothing, because a page flip
 * has traditionally meant "the whole screen": a window covering one of
 * two monitors was never eligible, so a game on one screen paid the
 * compositor's price for as long as the other screen existed. This fork
 * of the X server flips per CRTC (comp.h's caps.flip_per_crtc), which
 * makes the useful case possible -- hand *one output* over, leave every
 * other output composited, and let the game flip on its own monitor
 * while the desktop next to it keeps its effects.
 *
 * Two things have to happen together for that to work, and getting one
 * without the other is a black screen:
 *
 *   - the window is unredirected, so it draws to the screen again rather
 *     than into a pixmap only kicomp reads;
 *   - and the Composite overlay window, which covers the whole screen
 *     above every application window, has that output's rectangle cut
 *     out of its bounding shape. Otherwise the compositor's own output
 *     is still lying on top of the window that was just handed the
 *     screen, and all the unredirect achieves is that the game's pixels
 *     go somewhere nobody can see.
 *
 * kicomp then stops painting that output entirely. It has nothing to
 * paint there: the one window on it is drawing itself.
 *
 * The conditions are deliberately strict, and re-checked every frame,
 * because the failure mode of getting them wrong is a window that is
 * simply not on screen. Anything at all that kicomp would have to draw
 * over that output -- a notification, the WM's alt-tab overlay, an
 * effect, a menu, a second window, a shaped or translucent window, a
 * scaled output, a magnifier -- takes the output straight back.
 */
#ifndef KICOMP_UNREDIRECT_H
#define KICOMP_UNREDIRECT_H

#include "comp.h"

/* Re-decides, for every output, whether it should be handed over, and
 * carries out whatever changed. Called once per frame, after the window
 * events for this batch have been processed and before anything is
 * painted -- an output handed over in this call must not then be painted
 * in the same frame. */
void unredirect_update(void);

/* Is this output currently the window's rather than kicomp's? */
bool unredirect_holds(const CompOutput *o);

/* Which window it was handed to, or NULL if it wasn't. For anything that
 * wants to say so -- the stats effect above all, where an output that
 * isn't being presented at all is a fact its own frame-sync line would
 * otherwise misreport as "stuck". */
CompWindow *unredirect_holder(const CompOutput *o);

/* Gives every output back, redirecting whatever was handed over and
 * restoring the overlay. Called on shutdown, and by anything that needs
 * the compositor to be in charge of the whole screen again. */
void unredirect_release_all(void);

#endif /* KICOMP_UNREDIRECT_H */
