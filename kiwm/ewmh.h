#ifndef KIWM_EWMH_H
#define KIWM_EWMH_H

#include "wm.h"

void ewmh_update_client_list(void);
void ewmh_update_active_window(void);
void set_icccm_wm_state(Client *c, uint32_t state);
void ewmh_update_wm_state(Client *c);
void ewmh_update_wm_desktop(Client *c);
void ewmh_update_wm_output(Client *c);
void ewmh_update_frame_extents(Client *c);
void get_title(Client *c);
void get_size_hints(Client *c);
/* Whether WM_NORMAL_HINTS carries USPosition or PPosition -- see ewmh.c.
 * For windows that never become a Client (manage()'s unframed path). */
bool window_hints_have_position(xcb_window_t window);
/* Publishes _NET_WM_ALLOWED_ACTIONS from Client::allow_* -- see ewmh.c.
 * Called whenever those change (manage(), WM_NORMAL_HINTS changes). */
void ewmh_update_allowed_actions(Client *c);

void ewmh_init_supported(void);
void ewmh_init_supporting_wm_check(void);

#endif /* KIWM_EWMH_H */
