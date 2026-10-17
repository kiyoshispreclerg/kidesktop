/*
 * jobviewd_stub.c - drop-in replacement for jobviewd.c when the Makefile
 * couldn't find dbus-1 via pkg-config (see notifd_stub.c): no job server,
 * so apps keep showing their own progress dialogs.
 */
#include "xispanel.h"

int jobviewd_fd(void)
{
    return -1;
}

void jobviewd_wake(void)
{
}

void jobviewd_poll(uint64_t now)
{
    (void)now;
}

int jobviewd_count(void)
{
    return 0;
}

const JobEntry *jobviewd_get(int idx)
{
    (void)idx;
    return NULL;
}

int jobviewd_action(unsigned int id, const char *action)
{
    (void)id;
    (void)action;
    return 0;
}
