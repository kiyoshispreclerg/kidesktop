/* kicomp - publishing the kept picture of a hidden window. See stowpix.h. */
#include "stowpix.h"
#include "renderer.h"

void stowpix_publish(CompWindow *w)
{
    if (w->stow_published != XCB_NONE || w->zombie)
        return;
    if (comp.atoms.kicomp_stowed_pixmap == XCB_ATOM_NONE)
        return;

    /* Whatever the backend has bound right now -- which, for a window
     * being stowed, is the picture it had when it left the screen: the
     * stow is taken before anything invalidates it (window.c), and the
     * pixmap named for a window goes on holding its last contents after
     * the window is unmapped. That is the whole trick being offered. */
    xcb_pixmap_t pm = renderer_window_pixmap(w);
    if (pm == XCB_NONE)
        return;

    xcb_change_property(comp.conn, XCB_PROP_MODE_REPLACE, w->id,
                        comp.atoms.kicomp_stowed_pixmap, XCB_ATOM_PIXMAP, 32,
                        1, &pm);
    w->stow_published = pm;
    comp_log("stowed pixmap 0x%x published for 0x%x", pm, w->id);
}

void stowpix_drop(CompWindow *w)
{
    if (w->stow_published == XCB_NONE)
        return;

    w->stow_published = XCB_NONE;

    /* A zombie's window is already destroyed -- the property went with
     * it, and asking the server to delete it is a BadWindow for nothing.
     * stowpix_gone() is the same answer for the window that is gone
     * without being a zombie yet. */
    if (w->zombie)
        return;

    xcb_delete_property(comp.conn, w->id, comp.atoms.kicomp_stowed_pixmap);
    /* Flushed here rather than with the next frame: what follows this
     * call is the pixmap being freed, and the whole point of withdrawing
     * the property is that it stops being advertised *first*. */
    xcb_flush(comp.conn);
}

void stowpix_gone(CompWindow *w)
{
    w->stow_published = XCB_NONE;
}
