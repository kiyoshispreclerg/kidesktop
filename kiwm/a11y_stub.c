/* a11y_stub.c - a11y.c's API without atk-bridge: Alt+Tab isn't announced. */
#include "a11y.h"

void a11y_init(void)
{
}

void a11y_tabbox_open(const TabBoxState *state)
{
    (void)state;
}

void a11y_tabbox_step(const TabBoxState *state)
{
    (void)state;
}

void a11y_tabbox_close(void)
{
}

int a11y_pollfds(struct pollfd *out, int max, int *timeout)
{
    (void)out;
    (void)max;
    (void)timeout;
    return 0;
}

void a11y_dispatch(const struct pollfd *fds, int n)
{
    (void)fds;
    (void)n;
}
