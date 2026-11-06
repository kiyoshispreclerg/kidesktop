/* xis_launchfx_gtk - see xis_launchfx_gtk.h. */
#include "xis_launchfx_gtk.h"
#include "xis_launchfx.h"

#include <gdk/gdkx.h>

static guint g_timer;

static gboolean on_tick(gpointer data)
{
    (void)data;
    xis_launchfx_tick(xis_launchfx_now_ms());
    if (xis_launchfx_next_wake_ms())
        return TRUE;
    g_timer = 0;
    return FALSE;
}

void xis_launchfx_gtk_play(GdkWindow *win, int x, int y, int w, int h, GdkPixbuf *icon)
{
    if (!win || !icon)
        return;
    int iw = gdk_pixbuf_get_width(icon), ih = gdk_pixbuf_get_height(icon);
    if (iw <= 0 || ih <= 0)
        return;
    int ox, oy;
    gdk_window_get_origin(win, &ox, &oy);

    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, iw, ih);
    cairo_t *cr = cairo_create(s);
    gdk_cairo_set_source_pixbuf(cr, icon, 0, 0);
    cairo_paint(cr);
    cairo_destroy(cr);

    GdkScreen *scr = gdk_window_get_screen(win);
    xis_launchfx_trigger(GDK_SCREEN_XDISPLAY(scr), gdk_x11_screen_get_screen_number(scr), s, ox + x + w / 2,
                         oy + y + h / 2, iw > ih ? iw : ih);
    cairo_surface_destroy(s);   /* the animation keeps its own reference */
    if (xis_launchfx_next_wake_ms() && !g_timer)
        g_timer = g_timeout_add(33, on_tick, NULL);
}
