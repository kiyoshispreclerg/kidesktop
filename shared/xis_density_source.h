/* xis_density_source - a window of ours inside a GTK2 widget that something
 * else draws into (a plugin, a video player) at the density X-DENSITY asks
 * for. See TESTS/X-DENSITY.md section 3.1.2.
 *
 * Without density the window just fills the widget and the drawer paints it
 * directly. At a density the toolkit (gtk2-ng) announces on the toplevel,
 * _X_DENSITY_SCALE [num, den], the window is redirected (Composite, manual)
 * and made density times the widget's size, so the drawer renders that many
 * pixels, and
 *
 *   _X_DENSITY_SOURCES  CARDINAL[5n]  [x, y, w, h, window]...
 *
 * on the toplevel tells the compositor to show it in the widget's place
 * (written once the window was drawn at its new size, deleted when the
 * density goes away or the widget is hidden). What the widget itself shows
 * is a scaled-down 1x copy, for recorders and screenshots, refreshed on
 * Damage.
 *
 * One source per process. It selects no input, so clicks and the wheel go
 * on to the widget. The X server destroys the window with the widget's. */
#ifndef XIS_DENSITY_SOURCE_H
#define XIS_DENSITY_SOURCE_H

#include <stdbool.h>

#include <gtk/gtk.h>

/* Called after the window's size or the density changed (and the server has
 * the new size): the drawer should render again, now at xis_density_source_
 * ratio(). */
typedef void (*XisDensityChanged)(gpointer user_data);

/* Creates the window in `area`, which must be realized and have its own
 * GdkWindow (a GtkDrawingArea). `toplevel` is the window that carries the
 * density. `force_env` names an environment variable holding a density to
 * use whatever the toplevel says, to try this without a compositor (NULL:
 * none). Returns the window's XID. */
unsigned long xis_density_source_create(GtkWidget *area, GtkWindow *toplevel,
                                        const char *force_env, XisDensityChanged changed,
                                        gpointer user_data);

unsigned long xis_density_source_window(void);

/* The density as num/den (1/1 when the window isn't dense). */
void xis_density_source_ratio(long *num, long *den);

/* Whether the drawer has something to show: false takes the window off the
 * screen and withdraws the announcement (a message is shown instead). */
void xis_density_source_set_shown(gboolean shown);

/* Call from the widget's expose handler when it paints nothing over the
 * 1x copy. */
void xis_density_source_expose(void);

#endif
