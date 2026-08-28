#ifndef KIWM_OUTPUT_H
#define KIWM_OUTPUT_H

#include <xcb/xcb.h>
#include <stdbool.h>

int primary_output_index(void);
int output_index_for_point(int x, int y);
int output_for_pointer(void);
/* Which output the switcher overlays / direct desktop jumps act on -- see
 * output.c, and kiwm.conf's osd_output_follows_pointer=. */
int output_for_effects(void);

void outputs_refresh(void);

/* Publishes _NET_DESKTOP_GEOMETRY/_NET_DESKTOP_VIEWPORT for the current
 * root window size -- see output.c. Called at startup and on every RandR
 * screen change. */
void ewmh_set_desktop_geometry(void);

void ewmh_set_current_desktop(int desktop);
void switch_workspace(int output_idx, int desktop);
void cycle_output_desktop(int direction);

/* Dock/panel struts (_NET_WM_STRUT/_NET_WM_STRUT_PARTIAL) -- see
 * DockWindow in wm.h. dock_refresh_strut()/dock_forget() return false if
 * `window` isn't a tracked dock, so callers (events.c) know whether to
 * also check it against wm.clients instead. */
void dock_track(xcb_window_t window);
/* Whether `window` is one of the tracked dock/panel windows -- client.c's
 * restack_all() asks, since a dock takes part in the stacking order
 * (LAYER_DOCK) without ever being a Client. */
bool dock_is_tracked(xcb_window_t window);
bool dock_refresh_strut(xcb_window_t window);
bool dock_forget(xcb_window_t window);
void compute_output_workarea(int output_idx, int *x, int *y, int *w, int *h);

#endif /* KIWM_OUTPUT_H */
