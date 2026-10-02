#ifndef KIWM_A11Y_H
#define KIWM_A11Y_H

#include "osd.h"

#include <poll.h>

/* Alt+Tab for screen readers -- see a11y.c (a11y_stub.c without atk-bridge). */
void a11y_init(void);
void a11y_tabbox_open(const TabBoxState *state);
void a11y_tabbox_step(const TabBoxState *state); /* selection moved (also right after open) */
void a11y_tabbox_close(void);
/* main.c's poll(): adds up to `max` entries at `out`, returns how many,
 * may lower *timeout; a11y_dispatch() right after poll() with the same
 * entries (NULL after an interrupted poll). */
int a11y_pollfds(struct pollfd *out, int max, int *timeout);
void a11y_dispatch(const struct pollfd *fds, int n);

#endif
