#ifndef KIWM_CLIENT_H
#define KIWM_CLIENT_H

#include "wm.h"

Client *find_client_window(xcb_window_t window);

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
void toggle_fullscreen(Client *c, int want /* -1=toggle 0=unfullscreen 1=fullscreen */);
void toggle_shade(Client *c, int want /* -1=toggle 0=unshade 1=shade */);
void unshade_now(Client *c);
void toggle_keep_above(Client *c, int want /* -1=toggle 0=off 1=on */);
void toggle_keep_below(Client *c, int want /* -1=toggle 0=off 1=on */);
/* Rebuilds the real X stacking order from every managed client's current
 * WmLayer (see wm.h), preserving each client's relative order within its
 * own layer from whatever xcb_query_tree() reports right now -- so calling
 * this doesn't reorder anything except across layer boundaries. Call after
 * anything that raises/lowers a client (an explicit xcb_configure_window()
 * STACK_MODE_ABOVE/BELOW to move it to the very top/bottom of the whole
 * stack *first*, then restack_all(), is how focus_client()/
 * toggle_keep_above()/toggle_keep_below()/toggle_fullscreen() put a client
 * at the top/bottom of its own layer specifically -- see client.c). */
void restack_all(void);
void toggle_sticky(Client *c, int want /* -1=toggle 0=off 1=on */);
void snap_client_to_side(Client *c, SnapSide side /* SNAP_LEFT or SNAP_RIGHT */);
void unsnap_client(Client *c, int x, int y, int width, int height);
void detile_for_drag(Client *c, int press_root_x, int press_root_y);
void minimize_client(Client *c);
void restore_client(Client *c);
void activate_client(Client *c);
void set_client_desktop(Client *c, int desktop);

void manage(xcb_window_t window);
void manage_existing_windows(void);
void unmanage(Client *c);

#endif /* KIWM_CLIENT_H */
