/*
 * a11y_stub.c - a11y.c's API without screen reader support, linked when
 * atk/atk-bridge-2.0 weren't found at build time. See a11y.c.
 */
#include "xispanel.h"

void a11y_init(void)
{
}

int a11y_fds(fd_set *rfds, int maxfd, long *timeout_ms)
{
    (void)rfds;
    (void)timeout_ms;
    return maxfd;
}

void a11y_dispatch(const fd_set *rfds)
{
    (void)rfds;
}

void a11y_focus_changed(Panel *p, int item_index)
{
    (void)p;
    (void)item_index;
}

void a11y_focus_cleared(void)
{
}

void a11y_reset(void)
{
}
