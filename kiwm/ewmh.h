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

void ewmh_init_supported(void);
void ewmh_init_supporting_wm_check(void);

#endif /* KIWM_EWMH_H */
