/* xismenu.h - what xismenu.c (the registrar, the bus, the X connection)
 * shares with gmenu.c (the GMenuModel -> DBusMenu translator). Two files
 * on purpose: the registrar is a lookup table with an X11 side effect,
 * the translator is a protocol bridge, and neither needs to read the
 * other's internals. */
#ifndef XISMENU_H
#define XISMENU_H

#include <dbus/dbus.h>
#include <xcb/xcb.h>

#include <stdbool.h>
#include <stdint.h>

#define XISMENU_VERSION "0.1.2"

#define BUSNAME_MAX 128
#define PATH_MAX_LEN 256

extern DBusConnection *g_bus;
extern xcb_connection_t *g_xcb;   /* NULL when there is no display */
extern xcb_window_t g_root;
extern bool g_verbose;

void logmsg(const char *fmt, ...);

/* The two properties every consumer of the global menu reads off a
 * window (xispanel's globalmenu widget, xisserve's plugin and --menu,
 * kiwm's appmenu button): _KDE_NET_WM_APPMENU_SERVICE_NAME/_OBJECT_PATH.
 * Written by the registrar for DBusMenu exports, and by the translator
 * for the GMenuModel ones it serves itself. */
void write_props(uint32_t window, const char *service, const char *path);
void clear_props(uint32_t window);

/* ---- gmenu.c --------------------------------------------------------- */

/* Sets up discovery (selects the root events it needs, scans
 * _NET_CLIENT_LIST) and the bus matches for the applications' change
 * signals. `unique_name` is what the exported menus are advertised
 * under. No-op for the X side when g_xcb is NULL. */
void gmenu_init(const char *unique_name);

/* Discovery: _NET_CLIENT_LIST changes on the root, _GTK_* property
 * changes and destruction on client windows. */
void gmenu_handle_x_event(xcb_generic_event_t *ev);

/* The DBusMenu server side (method calls on /MenuBar/<n>) plus the
 * org.gtk.Menus/Actions Changed signals from the applications. Returns
 * NOT_YET_HANDLED for anything that isn't its business. */
DBusHandlerResult gmenu_handle_message(DBusMessage *msg);

/* Whether `window`'s menu is one the translator serves -- the registrar
 * asks before touching that window's properties. */
bool gmenu_window_translated(uint32_t window);

/* Re-checks one window's _GTK_* properties now -- the registrar calls it
 * when appmenu-gtk-module registers a window, since that is a reliable
 * "this window has a GMenuModel export" hint arriving over the bus. */
void gmenu_rescan_window(uint32_t window);

#endif
