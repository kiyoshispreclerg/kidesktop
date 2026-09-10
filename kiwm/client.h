#ifndef KIWM_CLIENT_H
#define KIWM_CLIENT_H

#include "wm.h"

Client *find_client_window(xcb_window_t window);

/* How much frame space the titlebar (top) and the flat side/bottom border
 * currently take up -- both 0 together when the decoration is hidden (see
 * decoration.c's client_deco_visible()). Exported so events.c's resize-drag
 * magnet snapping (handle_motion()) can convert the client's content-space
 * new_w/new_h into the same frame-space coordinates every other magnet
 * candidate (other clients' c->x/frame_width, docks, screen edges) is
 * already expressed in. */
void deco_insets(Client *c, int *bt, int *th);

void configure_frame(Client *c);
/* Cheap subset of configure_frame() -- geometry + synthetic ConfigureNotify
 * only, no XShape re-clip or decoration repaint. See client.c's comment;
 * used by events.c's handle_motion() on every move/resize motion event,
 * uncapped, with the full configure_frame() still throttled to the
 * output's refresh rate for the expensive paint/shape part. */
void apply_frame_geometry(Client *c);
void focus_client(Client *c);
void cycle_focus(int direction);

void close_client(Client *c);
void toggle_maximize(Client *c, int want /* -1=toggle 0=unmax 1=max */);
/* The single-axis maximizations EWMH has always had as separate states
 * (_NET_WM_STATE_MAXIMIZED_HORZ / _VERT): fill the output's usable width
 * or height, leaving the other axis exactly where the user put it. Bound
 * to the maximize button's right and middle click (events.c), the same
 * places kwin puts them. Going from either of these to a full maximize
 * and back restores the geometry from before *any* of it -- see
 * client.c's set_maximized(). */
void toggle_maximize_horz(Client *c, int want /* -1=toggle 0=off 1=on */);
void toggle_maximize_vert(Client *c, int want /* -1=toggle 0=off 1=on */);
/* Sets both axes at once -- what the three toggles above are written in
 * terms of, and what a _NET_WM_STATE client message naming one or both of
 * the maximize atoms resolves to (events.c). Captures the floating
 * geometry when the window first leaves it and hands back exactly the
 * axes being given up. */
void client_set_maximized(Client *c, bool horz, bool vert);
void toggle_fullscreen(Client *c, int want /* -1=toggle 0=unfullscreen 1=fullscreen */);
void toggle_shade(Client *c, int want /* -1=toggle 0=unshade 1=shade */);
void unshade_now(Client *c);
void toggle_keep_above(Client *c, int want /* -1=toggle 0=off 1=on */);
void toggle_keep_below(Client *c, int want /* -1=toggle 0=off 1=on */);
/* Rebuilds the real X stacking order from every managed client's current
 * WmLayer (see wm.h), preserving each client's relative order within its
 * own layer from whatever xcb_query_tree() reports right now -- so calling
 * this doesn't reorder anything except across layer boundaries. */
void restack_all(void);
/* The same, with one client first moved to the top (or the bottom) of its
 * own layer -- which is what focus_client(), toggle_keep_above/below() and
 * toggle_fullscreen() actually want.
 *
 * This used to be done by sending that client an xcb_configure_window()
 * STACK_MODE_ABOVE to put it at the top of the *whole* stack and letting
 * restack_all() pull it back down into its layer. The result was right and
 * the intermediate state was visible: restack_all() opens with an
 * xcb_query_tree() round trip, which flushes that raise, so the server got
 * a frame's worth of time to display the stack with the client above
 * everything -- including the windows its own layer rules keep on top of
 * it. That is the VirtualBox mini-toolbar disappearing under the VM window
 * for one frame on every single click.
 *
 * Reordering the bucket in memory instead means the whole reorder leaves
 * as one batch of ConfigureWindow requests with no round trip in the
 * middle, so there is no intermediate stack for the server to present. */
void restack_all_raising(Client *c);
void restack_all_lowering(Client *c);
void toggle_sticky(Client *c, int want /* -1=toggle 0=off 1=on */);
void snap_client_to_side(Client *c, SnapSide side /* SNAP_LEFT or SNAP_RIGHT */);
/* Keyboard half-screen tiling: snaps c to `side`, or restores it if it's
 * already tiled there -- captures/restores the floating geometry and
 * applies everything itself, unlike snap_client_to_side() above. See
 * client.c, and kiwm.conf's key_tile_left=/key_tile_right=. */
void toggle_snap_side(Client *c, SnapSide side /* SNAP_LEFT or SNAP_RIGHT */);
void unsnap_client(Client *c, int x, int y, int width, int height);
void detile_for_drag(Client *c, int press_root_x, int press_root_y);
void minimize_client(Client *c);
void restore_client(Client *c);
/* Re-reads whether the window exports an application menu
 * (_KDE_NET_WM_APPMENU_SERVICE_NAME/_OBJECT_PATH), which is what decides
 * whether the appmenu titlebar element is shown for it. Called at manage
 * time and whenever either property changes -- apps typically set them
 * just after mapping, so the button appears a moment later rather than
 * never. */
void client_refresh_appmenu(Client *c);

void activate_client(Client *c);

/* The same, for an activation kiwm was *asked* for rather than one it
 * decided on: an _NET_ACTIVE_WINDOW client message. `user_driven` is that
 * message's source indication (EWMH's 2 = a pager/taskbar acting on a
 * user's click, always honored; 1 or 0 = the application itself, which
 * kiwm.conf's focus_stealing_prevention= may refuse). A refused request
 * only marks the window as demanding attention. */
void activate_client_requested(Client *c, bool user_driven);

/* Whether a window that asks for focus on its own behalf may have it,
 * under kiwm.conf's focus_stealing_prevention= (FSP_NONE by default,
 * which is always yes -- kiwm's original behavior). `user_driven` short-
 * circuits it to true: anything the user directly did is never stealing.
 * Only the two paths a *client* can trigger consult this -- a window
 * mapping itself into focus and an _NET_ACTIVE_WINDOW message; clicking,
 * the switcher, the window menu and the keyboard shortcuts all focus
 * through focus_client() as they always have. */
bool focus_request_allowed(Client *c, bool user_driven);

/* Refuses that request the way EWMH says to: sets
 * _NET_WM_STATE_DEMANDS_ATTENTION so a taskbar can highlight the window,
 * and leaves it otherwise untouched. */
void deny_focus_request(Client *c);
/* Moves a client to another output, re-basing which desktop it is on to
 * that output's current one (desktop numbers are per-output, see
 * PROTOCOL.md -- keeping the old number across a move can strand a window
 * on a desktop the destination screen isn't showing) and syncing its
 * frame's mapped state to match. Sticky clients keep their desktop
 * number. A no-op for an out-of-range index or the output it is already
 * on. */
void client_reassign_output(Client *c, int output_idx);

void set_client_desktop(Client *c, int desktop);

/* Re-derives the geometry of every maximized/half-tiled/fullscreen client
 * from the current outputs and workarea -- call after anything that can
 * change either (a dock appearing/disappearing/changing its strut, an
 * output change, the initial adoption pass). Leaves floating windows
 * alone. See client.c. */
void refit_tiled_clients(void);

/* Sets `c`'s geometry to its output's full rectangle -- what being
 * fullscreen means (the whole output, docks included, unlike maximize's
 * workarea). See client.c. */
void client_apply_fullscreen_geometry(Client *c);

/* Repainting what a fullscreen window leaves behind when it loses focus
 * takes more than one round of exposes, and the later rounds are on a
 * timer -- see client.c's pending_expose (and why, which is the X server
 * page-flipping a fullscreen window straight to the scanout when there's
 * no compositor). main.c's event loop drives them: it caps its poll()
 * timeout with client_pending_expose_timeout_ms() (-1 = nothing pending,
 * block as usual) and calls client_run_pending_expose() every time round,
 * which is a cheap no-op until a round is actually due. */
int client_pending_expose_timeout_ms(void);
void client_run_pending_expose(void);

/* Holds one hidden window on screen for a moment, so that whoever asked
 * can have a live picture of it (PROTOCOL.md's _KIWM_HOLD_WINDOW).
 *
 * X keeps nothing of a window that is not on screen, and a window on a
 * desktop nobody is showing is not on screen: its last picture is all
 * anyone has, which is why an expo grid shows other desktops frozen at
 * the moment they were left. Mapping the frame again gives the picture
 * back, and only the frame -- the application's own window never
 * changed state when the desktop was left (see switch_workspace) and
 * does not change now, so nothing about this reaches the application.
 *
 * While held, the frame takes no input (an empty input shape) and sits
 * at the very bottom of the stack, which is how kwin's own "keep hidden
 * windows mapped for previews" avoids them interfering. It is also
 * marked with _KIWM_HELD, so a compositor can tell this map from a
 * window actually arriving and not animate it.
 *
 * The prize is that the *window manager* owns the undoing: whoever asked
 * can crash mid-picture and the window still goes back where it was,
 * within `ms`. Asking again before then simply extends it, which is how
 * a mode that lasts holds on -- there is no "release" request, on
 * purpose.
 *
 * Refused for anything that isn't merely away with its desktop:
 * minimized (the user put it away), shaded (whose *client* window really
 * is unmapped), sticky and already-visible windows have nothing to
 * hold. */
void client_hold(xcb_window_t window, int ms);
int  client_hold_timeout_ms(void);
void client_run_holds(void);

/* Puts a held window back now, whatever its prize said. For the paths
 * that are about to decide this window's visibility themselves. */
void client_release_hold(Client *c);

/* Recomputes Client::allow_* from the client's current WM_NORMAL_HINTS /
 * _MOTIF_WM_HINTS and republishes _NET_WM_ALLOWED_ACTIONS -- see
 * client.c. Call after get_size_hints(). */
void update_client_actions(Client *c);

/* Re-reads WM_TRANSIENT_FOR (which not every toolkit sets before mapping)
 * and restacks if it changed -- see client.c. */
void client_refresh_transient_for(Client *c);

/* An unframed popup that kiwm gave the keyboard to has gone away -- hands
 * focus back to the focused client. No-op for any other window. See
 * client.c and wm.h's KiWM::focused_popup. */
void popup_focus_released(xcb_window_t window);

/* Frames and takes over `window`. `map_requested` is true only when a
 * MapRequest brought us here (the client is asking to be shown); false
 * when adopting an already-existing window at startup, whose current map
 * state is then left exactly as it is. See client.c. */
void manage(xcb_window_t window, bool map_requested);
void manage_existing_windows(void);
void unmanage(Client *c);

#endif /* KIWM_CLIENT_H */
