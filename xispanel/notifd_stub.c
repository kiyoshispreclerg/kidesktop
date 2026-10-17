/*
 * notifd_stub.c - drop-in replacement for notifd.c when the Makefile
 * couldn't find dbus-1 via pkg-config at build time (see Makefile).
 * Every function is a harmless no-op reporting "no notifications" --
 * callers never need to know or care which of notifd.c/notifd_stub.c
 * actually got linked in. Exactly one of the two is ever compiled.
 */
#include "xispanel.h"

int notifd_fd(void)
{
    return -1;
}

void notifd_wake(void)
{
}

void notifd_poll(uint64_t now)
{
    (void)now;
}

int notifd_count(void)
{
    return 0;
}

const NotifEntry *notifd_get(int idx)
{
    (void)idx;
    return NULL;
}

int notifd_unread_count(void)
{
    return 0;
}

void notifd_mark_read(unsigned int id)
{
    (void)id;
}

void notifd_remove(unsigned int id)
{
    (void)id;
}

void notifd_clear(void)
{
}

void notifd_set_arrived_callback(NotifArrivedFn fn)
{
    (void)fn;
}

void notifd_post(const char *app_name, const char *app_icon, const char *summary, const char *body)
{
    (void)app_name;
    (void)app_icon;
    (void)summary;
    (void)body;
}

void notifd_set_default_expire_ms(int ms)
{
    (void)ms;
}

void notifd_action_invoked(unsigned int id, const char *key)
{
    (void)id;
    (void)key;
}

void notifd_closed(unsigned int id, int reason)
{
    (void)id;
    (void)reason;
}

void notifd_set_close_callback(NotifCloseFn fn)
{
    (void)fn;
}
