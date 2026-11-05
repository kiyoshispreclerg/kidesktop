/* xis_launchfx_gtk - xis_launchfx for GTK2 programs: runs the animation off
 * a GLib timeout, so a caller only says where the icon is. */
#ifndef XIS_LAUNCHFX_GTK_H
#define XIS_LAUNCHFX_GTK_H

#include <gtk/gtk.h>

/* Zooms+fades `icon` from the rectangle (x, y, w, h) given in `win`'s own
 * coordinates -- where the icon is drawn right now -- e.g. a tree view's
 * bin window and gtk_tree_view_get_cell_area(). The icon is drawn at its own
 * pixel size. No-op without a compositor. */
void xis_launchfx_gtk_play(GdkWindow *win, int x, int y, int w, int h, GdkPixbuf *icon, double zoom,
                           int duration_ms);

#endif
