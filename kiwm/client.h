#ifndef KIWM_CLIENT_H
#define KIWM_CLIENT_H

#include "wm.h"

Client *find_client_window(xcb_window_t window);

void configure_frame(Client *c);
void focus_client(Client *c);
void cycle_focus(int direction);

void close_client(Client *c);
void toggle_maximize(Client *c, int want /* -1=toggle 0=unmax 1=max */);
void toggle_shade(Client *c, int want /* -1=toggle 0=unshade 1=shade */);
void unshade_now(Client *c);
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
