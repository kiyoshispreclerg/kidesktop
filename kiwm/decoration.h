#ifndef KIWM_DECORATION_H
#define KIWM_DECORATION_H

#include "wm.h"

void load_decoration(void);
bool client_deco_visible(Client *c);
void draw_decoration(Client *c);

#endif /* KIWM_DECORATION_H */
