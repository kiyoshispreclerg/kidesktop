#ifndef KIWM_CLIENT_H
#define KIWM_CLIENT_H

#include "wm.h"

Client *find_client_window(xcb_window_t window);

void configure_frame(Client *c);
void focus_client(Client *c);
void cycle_focus(int direction);

void close_client(Client *c);
void toggle_maximize(Client *c, int want /* -1=toggle 0=unmax 1=max */);
void minimize_client(Client *c);
void restore_client(Client *c);
void activate_client(Client *c);
void set_client_desktop(Client *c, int desktop);

void manage(xcb_window_t window);
void manage_existing_windows(void);
void unmanage(Client *c);

#endif /* KIWM_CLIENT_H */
