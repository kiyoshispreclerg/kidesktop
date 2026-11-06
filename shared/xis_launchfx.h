/* xis_launchfx - zoom+fade "launch feedback" over a clicked icon, shared by
 * xispanel, kifiles and xisserve. Compositor-only (a 32-bit ARGB popup
 * blended live by the compositor); silently does nothing without one.
 * Only one animation plays at a time: a new trigger replaces the old.
 *
 * (cx, cy) is the icon's on-screen center in root coordinates, icon_px its
 * normal on-screen size. Zoom and duration are global, read from
 * launchfx.conf (xis_launchfx_conf.h) on every trigger. `icon` is only referenced, never
 * modified. The caller advances it: call xis_launchfx_tick() about every
 * 33 ms until xis_launchfx_next_wake_ms() returns 0. GTK programs just use
 * xis_launchfx_gtk_play() (xis_launchfx_gtk.h), which does that itself. */
#ifndef XIS_LAUNCHFX_H
#define XIS_LAUNCHFX_H

#include <X11/Xlib.h>
#include <cairo/cairo.h>
#include <stdint.h>

void xis_launchfx_trigger(Display *dpy, int screen, cairo_surface_t *icon, int cx, int cy, int icon_px);
void xis_launchfx_tick(uint64_t now_ms);
uint64_t xis_launchfx_next_wake_ms(void);   /* 0 = nothing active */
int xis_launchfx_handle_event(const XEvent *ev);   /* 1 if `ev` (an Expose) was the popup's */
uint64_t xis_launchfx_now_ms(void);

#endif
