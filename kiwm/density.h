/*
 * X-DENSITY, client side, for kiwm's own decorations -- see density.c.
 *
 * kiwm is a client of this protocol like any other: a compositor asks the
 * *frame* to redraw its decoration densely, and kiwm publishes a pixmap
 * for it to sample. Entirely optional -- with no compositor asking,
 * nothing here allocates or publishes anything.
 */
#ifndef KIWM_DENSITY_H
#define KIWM_DENSITY_H

#include "wm.h"

/* Watches _X_DENSITY_MANAGER_S<n> (XFixes): call once at startup, after
 * the screen number is known. handle_event returns true when it consumed
 * the event; losing the owner snaps every frame and menu back to 1x. */
void density_manager_init(void);
bool density_manager_handle_event(xcb_generic_event_t *event);
bool density_manager_present(void);

/* _X_DENSITY_REQUESTED changed on this client's frame. */
void deco_density_request_changed(Client *c);

/* Called at the start of every decoration repaint: renders the same
 * decoration into the density pixmap (announced only when new; later
 * frames reach the compositor as Damage on it). A no-op at density 1.
 * Returns true when the pixmap was already announced: the compositor shows
 * it instead of the frame's decoration, so the 1x paint can be skipped --
 * the frame is repainted when the density goes back to 1/1. */
bool deco_density_publish(Client *c, int w, int h, bool focused);

/* The decoration is not being drawn at all right now -- maximized with
 * hide_deco_on_maximize, fullscreen, a client that asked for no chrome.
 * Withdraws the published pixmap without forgetting the density that was
 * asked for, so the decoration comes back dense when it comes back at all.
 *
 * Necessary because the pixmap is a *separate drawable*: unlike the frame,
 * whose contents the server replaces when it is resized, a pixmap nobody
 * withdraws stays exactly as it was and the compositor keeps compositing
 * it -- which shows up as the old decoration, at the old size, painted
 * over a maximized window that is supposed to have none. */
void deco_density_hide(Client *c);

/* Density no longer wanted, or the client is going away. */
void deco_density_forget(Client *c);

/* The density manager went away (maybe without withdrawing its
 * requests): forgets every frame's density and repaints it at 1x. */
void deco_density_forget_all(void);

/* Frees a published density pixmap a second from now rather than at once
 * (see density.c) -- for any pixmap kiwm published, the window menu's
 * included. */
void density_retire_pixmap(xcb_pixmap_t pixmap);

/* Replaced density pixmaps are freed a second late (see
 * density_retire_pixmap()); the main loop runs this and sleeps no longer than
 * the timeout (-1: nothing pending). */
void deco_density_run_retired(void);
int  deco_density_retired_timeout_ms(void);

#endif /* KIWM_DENSITY_H */
