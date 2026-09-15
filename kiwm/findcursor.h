#ifndef KIWM_FINDCURSOR_H
#define KIWM_FINDCURSOR_H

#include "wm.h"

/* "Find the cursor": the classic Windows-XP trick of a shrinking wireframe
 * that collapses onto the pointer, for locating it on a large or
 * multi-monitor desktop. Bound to a key (kiwm.conf's key_find_cursor=,
 * default ModKey+F7 -- see keybind.c) rather than a Ctrl-tap gesture like
 * XP's own: kiwm has no bare-modifier grab machinery, and a normal
 * key_* binding is both easier to implement correctly and, like every
 * other key_*, trivially disabled by clearing it. See findcursor.c. */

/* Starts the animation, centered on the pointer's current position.
 * Retriggering while one is already in flight restarts it from there. */
void findcursor_trigger(void);

/* Same shape as client.h's client_pending_expose_timeout_ms()/
 * client_run_pending_expose(): how long until the animation's next step is
 * due (-1 = nothing in flight, block as usual), and running that step when
 * it's due. Call both from the main loop every time round. */
int findcursor_timeout_ms(void);
void findcursor_run(void);

/* Whether `window` is the animation's own window -- how client.c's
 * restack_all() recognizes it (override-redirect, never a Client), same
 * as outline.c's outline_owns_window(). */
bool findcursor_owns_window(xcb_window_t window);

/* Repaints after an Expose on that window; a no-op for any other window. */
void findcursor_handle_expose(xcb_window_t window);

#endif /* KIWM_FINDCURSOR_H */
