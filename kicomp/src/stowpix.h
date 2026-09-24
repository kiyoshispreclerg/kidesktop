/*
 * kicomp - the kept picture of a hidden window, offered to others
 * (_KICOMP_STOWED_PIXMAP).
 *
 * X frees the contents of a window that is not on screen, so the only
 * picture there will ever be of a minimized window is one taken while it
 * was still up. kiwm's _KIWM_HOLD_WINDOW exists for exactly that -- map
 * the frame for a moment, let whoever wants it take a live picture -- and
 * for a window that is merely away with its desktop it works: the
 * application has no idea anything happened and repaints on the Expose.
 *
 * A *minimized* window is a different matter. The application is told,
 * and some of them answer by shutting their rendering down: Firefox
 * suspends its compositor the moment GTK reports the window iconified,
 * so the frame comes back up mapped, marked, and entirely black -- and
 * the hold has destroyed the one good picture there was, since mapping
 * the window names a new pixmap for it (window.c's window_map).
 *
 * The compositor is the only one that can keep that picture: it holds
 * the pixmap it last bound, and comp.keep_stowed is it deciding not to
 * let go (comp.h's stowed) precisely so the expo grid has something to
 * draw. This publishes the XID of that pixmap on the window, so anything
 * else that wants a thumbnail of a window it cannot see -- xispanel's
 * tasklist, with live_thumbs=no -- can draw the same picture the grid
 * draws, with no hold, no flash, and no dependence on whether that
 * particular application still paints while minimized.
 *
 * The one rule for a reader: the pixmap is *ours*. Draw from it, never
 * free it. It is freed here, whenever the window comes back or the
 * contents are dropped for any other reason, and the property is
 * withdrawn first -- so a reader that finds a stale XID (it read the
 * property just before the window came back) is one race away, which is
 * why a reader must also tolerate the X error that names it.
 */
#ifndef KICOMP_STOWPIX_H
#define KICOMP_STOWPIX_H

#include "comp.h"

/* The window's contents are being kept: publish the pixmap holding them,
 * if the backend has one to offer. A no-op when it has none (a window
 * that was never painted), and when something is already published for
 * this window. */
void stowpix_publish(CompWindow *w);

/* The pixmap is about to stop being the window's contents -- it is being
 * freed, or replaced by a fresh one. Withdraws the property first, so
 * the XID is never advertised for longer than it is valid. */
void stowpix_drop(CompWindow *w);

/* The X window itself is gone: forget what was published without asking
 * the server to delete a property from a window that no longer exists.
 * The property died with the window. */
void stowpix_gone(CompWindow *w);

#endif /* KICOMP_STOWPIX_H */
