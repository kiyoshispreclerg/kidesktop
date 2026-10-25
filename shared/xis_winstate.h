/* xis_winstate - remember a GTK2 toplevel's position, size and maximized
 * state between runs, the way KDE applications do.
 *
 * Stored per application in $XDG_CONFIG_HOME/kidesktop/winstate-<name>.conf
 * (fallback ~/.config). Call xis_winstate_restore() before the window is
 * shown and xis_winstate_track() once, any time after creating it; the
 * state is written shortly after the window stops moving, so it survives a
 * crash and a close that only hides the window. */
#ifndef XIS_WINSTATE_H
#define XIS_WINSTATE_H

#include <gtk/gtk.h>

enum {
    XIS_WINSTATE_POS = 1,  /* position and maximized state */
    XIS_WINSTATE_SIZE = 2, /* also the size (leave out when the app sizes itself) */
    XIS_WINSTATE_ALL = 3
};

/* Applies the saved state to `win` (not yet shown). A position that no
 * longer lands on any monitor is ignored, a partly off-screen one is pulled
 * back in. A no-op the first time. */
void xis_winstate_restore(GtkWindow *win, const char *name, int what);

/* Starts recording `win`'s state under `name`; `what` must match the call
 * to xis_winstate_restore(). */
void xis_winstate_track(GtkWindow *win, const char *name, int what);

#endif
