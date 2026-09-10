#ifndef KIWM_SELECTION_H
#define KIWM_SELECTION_H

#include <stdbool.h>

bool acquire_wm_selection(int screen_nbr, bool replace);

/* Whether something owns _NET_WM_CM_Sn -- i.e. whether a compositor is
 * compositing this screen. Briefly cached, so this is cheap enough to ask
 * from a repaint; see selection.c for why that matters and why the answer
 * is re-asked rather than watched. Only for deciding how to *draw*; kiwm
 * never depends on a compositor being there. */
bool compositor_running(void);

#endif /* KIWM_SELECTION_H */
