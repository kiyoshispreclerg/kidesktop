/*
 * launchfx.c - xispanel's side of the shared launch-feedback animation
 * (../shared/xis_launchfx.c): binds it to the panel's display and screen,
 * so the tasklist and the main loop keep calling launchfx_*().
 */
#include "xispanel.h"
#include "../shared/xis_launchfx.h"

void launchfx_trigger(cairo_surface_t *icon, int cx, int cy, int icon_px)
{
    xis_launchfx_trigger(g_dpy, g_screen, icon, cx, cy, icon_px);
}

void launchfx_tick(uint64_t now) { xis_launchfx_tick(now); }

uint64_t launchfx_next_wake_ms(void) { return xis_launchfx_next_wake_ms(); }

int launchfx_handle_event(const XEvent *ev) { return xis_launchfx_handle_event(ev); }
