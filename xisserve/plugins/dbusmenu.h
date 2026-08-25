/*
 * dbusmenu.h - generic com.canonical.dbusmenu client, ported from
 * xispanel/dbusmenu.c for plugins/globalmenu.c's use (see that file's
 * header comment for the full protocol notes -- GetLayout(parent_id,
 * depth, []) with depth=-1 returns a whole subtree in one call, which is
 * all globalmenu.c needs: everything at once, flattened, so it can
 * search across every submenu level at once). Only real difference from
 * the xispanel original: icons decode to GdkPixbuf (this is a standalone
 * GTK2 binary, no Imlib2 dependency to reuse), straight from the
 * DBusMenu reply's in-memory bytes via GdkPixbufLoader -- no temp file
 * needed at all, unlike Imlib2's load-from-path-only API.
 */
#ifndef XISSERVE_DBUSMENU_H
#define XISSERVE_DBUSMENU_H

#include <gtk/gtk.h>
#include <stdint.h>

#define DBUSMENU_MAX_ITEMS 512

typedef struct {
    char label[128];
    GdkPixbuf *icon; /* owned; NULL = no icon for this item */
    int is_separator;
    int enabled;
} DbusMenuItem;

/* Flattens parent_id's subtree into parallel out_items[]/out_ids[]/
 * out_depth[] arrays (out_depth[i] = item i's nesting depth relative to
 * parent_id). Returns the item count, or -1 on any failure (no dbus, no
 * reply, timeout, no such busname/path). */
int dbusmenu_fetch(const char *busname, const char *path, int32_t parent_id, int32_t depth, DbusMenuItem *out_items,
                    int *out_ids, int *out_depth, int max_items);

/* Frees every item's icon (if any) -- call once done with an array
 * dbusmenu_fetch() populated. */
void dbusmenu_free_item_icons(DbusMenuItem *items, int n);

/* Fire-and-forget DBusMenu Event(id, "clicked", <int32 0>, 0) -- the
 * same effect as activating that item for real in the exporting app's
 * own menu. */
void dbusmenu_send_event(const char *busname, const char *path, int32_t id);

#endif
