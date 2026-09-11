#ifndef KIWM_OUTPUT_H
#define KIWM_OUTPUT_H

#include <xcb/xcb.h>
#include <stdbool.h>

int primary_output_index(void);
int output_index_for_point(int x, int y);
int output_index_containing_point(int x, int y);
int output_for_pointer(void);
int largest_output_index(void);
int output_for_new_window(void);
/* Which output the switcher overlays / direct desktop jumps act on -- see
 * output.c, and kiwm.conf's osd_output_follows_pointer=. */
int output_for_effects(void);

void outputs_refresh(void);

/* Publishes _NET_DESKTOP_GEOMETRY/_NET_DESKTOP_VIEWPORT for the current
 * root window size -- see output.c. Called at startup and on every RandR
 * screen change. */
void ewmh_set_desktop_geometry(void);

/* Publishes _NET_DESKTOP_LAYOUT (the columns x rows shape kiwm.conf's
 * desktop_columns=/desktop_rows= ask for) so an outside pager -- xispanel's
 * reads exactly this property -- lays its squares out the same way kiwm's
 * own Meta+Tab grid does. Called at startup, next to the other one-shot
 * desktop properties. */
void ewmh_set_desktop_layout(void);

/* The desktop grid: the single place kiwm.conf's desktop_columns=/
 * desktop_rows= (either of which may be 0, "as many as needed") plus
 * wm.num_desktops turn into a real cols x rows, and the row/column
 * arithmetic that goes with it. Row-major from the top-left, matching what
 * ewmh_set_desktop_layout() publishes.
 *
 * desktop_at_cell() returns -1 for a cell the desktop count doesn't reach
 * (the last row of a 3x2 grid holding 5 desktops) -- callers should skip
 * those rather than treat it as desktop -1. */
void desktop_grid(int *out_cols, int *out_rows);
int desktop_at_cell(int row, int col);
void desktop_cell(int desktop, int *out_row, int *out_col);

/* Which direction a desktop switch moves in: through the desktops in
 * plain index order (kiwm's original Tab cycling, and what it still does
 * by default), or one column / one row at a time through the grid above.
 * Each is offered in both directions via desktop_step()'s +1/-1. */
typedef enum {
    DESKTOP_AXIS_LINEAR = 0,
    DESKTOP_AXIS_HORZ,
    DESKTOP_AXIS_VERT
} DesktopAxis;

/* The desktop `direction` (+1/-1) steps to along `axis`, wrapping around
 * (the row, the column, or the whole list) and skipping empty grid cells.
 * Returns `from` unchanged if there is nowhere else to go. */
int desktop_step(int from, int direction, DesktopAxis axis);

void ewmh_set_current_desktop(int desktop);
void switch_workspace(int output_idx, int desktop);
void cycle_output_desktop(int direction, DesktopAxis axis);

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

/* Wallpaper layers (wm.h's DesktopLayer). Tracked when one is mapped,
 * forgotten when it goes away, and shown or hidden by the desktop each
 * one says it belongs to -- which is the half xisback leaves to the WM
 * and kiwm was not doing, so every desktop's wallpaper stayed mapped and
 * whichever was on top was the one you saw everywhere. */
void desktop_layer_track(xcb_window_t window);
bool desktop_layer_forget(xcb_window_t window);
/* Its _NET_WM_DESKTOP changed. */
bool desktop_layer_refresh(xcb_window_t window);
/* The topmost layer currently on screen -- what a new one (a crossfade
 * window) has to be stacked above to be seen at all. */
xcb_window_t desktop_layer_topmost_mapped(void);

/* Map or unmap this output's layers for the desktop it is now showing. */
void desktop_layers_apply(int output_idx);

/* Holds every hidden wallpaper up for a moment, on screen but directly
 * under the one that belongs there, and hides it again.
 *
 * For a compositor: X frees an unmapped window's contents, so the only
 * picture there will ever be of a wallpaper is one taken while it was
 * mapped. A desktop nobody has visited yet has never been mapped, which
 * is why an expo grid opens with three empty cells and a real one. This
 * is the WM doing the one part only it can do -- putting a window on
 * screen -- somewhere it cannot be seen, since the wallpaper of the
 * desktop you are on covers that monitor completely.
 *
 * Asked for with _KIWM_PRIME_DESKTOP_LAYERS (PROTOCOL.md). Nothing here
 * knows or cares whether anyone is composited: without a compositor the
 * whole thing is a window mapped under an opaque one for half a second,
 * which is nothing at all.
 *
 * The two below are the delayed half, driven from the main loop the same
 * way client.c's pending expose rounds are. */
void desktop_layers_prime(void);
int  desktop_layers_prime_timeout_ms(void);
void desktop_layers_run_prime(void);
void compute_output_workarea(int output_idx, int *x, int *y, int *w, int *h);

#endif /* KIWM_OUTPUT_H */
