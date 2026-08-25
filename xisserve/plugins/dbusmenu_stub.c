/*
 * dbusmenu_stub.c - drop-in replacement for dbusmenu.c when the Makefile
 * couldn't find dbus-1 via pkg-config at build time (see Makefile and
 * xispanel/dbusmenu_stub.c for the identical reasoning). Every function
 * reports "no menu, ever" -- plugins/globalmenu.c still builds and
 * registers normally, its search just never finds anything.
 */
#include "dbusmenu.h"

int dbusmenu_fetch(const char *busname, const char *path, int32_t parent_id, int32_t depth, DbusMenuItem *out_items,
                    int *out_ids, int *out_depth, int max_items)
{
    (void)busname;
    (void)path;
    (void)parent_id;
    (void)depth;
    (void)out_items;
    (void)out_ids;
    (void)out_depth;
    (void)max_items;
    return -1;
}

void dbusmenu_free_item_icons(DbusMenuItem *items, int n)
{
    (void)items;
    (void)n;
}

void dbusmenu_send_event(const char *busname, const char *path, int32_t id)
{
    (void)busname;
    (void)path;
    (void)id;
}
