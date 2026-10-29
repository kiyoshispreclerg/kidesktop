/* ks_tray.c - the recording indicator in the system tray.
 *
 * A StatusNotifierItem over GDBus: the tray of this desktop (xispanel) is
 * StatusNotifierItem-only, and GtkStatusIcon speaks XEmbed
 * (_NET_SYSTEM_TRAY_Sn), which nothing here owns unless a bridge such as
 * xembedsniproxy happens to be running -- so the icon never showed up. When
 * no StatusNotifierWatcher is on the bus (another desktop, no panel yet) the
 * GtkStatusIcon is still created, for whatever XEmbed tray there is; if a
 * watcher shows up while recording, the item registers with it and the
 * status icon goes away. */
#include "ks.h"
#include <gio/gio.h>
#include <unistd.h>

#define WATCHER_NAME "org.kde.StatusNotifierWatcher"
#define WATCHER_PATH "/StatusNotifierWatcher"
#define ITEM_PATH "/StatusNotifierItem"
#define ITEM_IFACE "org.kde.StatusNotifierItem"

static const char item_xml[] =
    "<node>"
    " <interface name='" ITEM_IFACE "'>"
    "  <method name='Activate'><arg type='i' direction='in'/><arg type='i' direction='in'/></method>"
    "  <method name='SecondaryActivate'><arg type='i' direction='in'/><arg type='i' direction='in'/></method>"
    "  <method name='ContextMenu'><arg type='i' direction='in'/><arg type='i' direction='in'/></method>"
    "  <method name='Scroll'><arg type='i' direction='in'/><arg type='s' direction='in'/></method>"
    "  <property name='Category' type='s' access='read'/>"
    "  <property name='Id' type='s' access='read'/>"
    "  <property name='Title' type='s' access='read'/>"
    "  <property name='Status' type='s' access='read'/>"
    "  <property name='IconName' type='s' access='read'/>"
    "  <property name='ItemIsMenu' type='b' access='read'/>"
    "  <property name='Menu' type='o' access='read'/>"
    "  <property name='ToolTip' type='(sa(iiay)ss)' access='read'/>"
    "  <signal name='NewToolTip'/>"
    "  <signal name='NewStatus'><arg type='s'/></signal>"
    " </interface>"
    "</node>";

static struct {
    gboolean shown;
    KsTrayActivate cb;
    void *data;
    char *tooltip;
    guint own_id, watch_id, reg_id;
    GDBusConnection *conn;
    GDBusNodeInfo *info;
    gboolean registered;        /* with a watcher, this round */
    GtkStatusIcon *fallback;
} t;

static void on_fallback_activate(GtkStatusIcon *icon, gpointer unused)
{
    (void)icon;
    (void)unused;
    if (t.cb)
        t.cb(t.data);
}

static void method_call(GDBusConnection *c, const char *sender, const char *path, const char *iface,
                        const char *method, GVariant *params, GDBusMethodInvocation *inv, gpointer ud)
{
    (void)c; (void)sender; (void)path; (void)iface; (void)params; (void)ud;
    /* a click on the icon is the whole interface: stop the recording */
    if (!g_strcmp0(method, "Activate") || !g_strcmp0(method, "SecondaryActivate") ||
        !g_strcmp0(method, "ContextMenu")) {
        if (t.cb)
            t.cb(t.data);
    }
    g_dbus_method_invocation_return_value(inv, NULL);
}

static GVariant *get_property(GDBusConnection *c, const char *sender, const char *path, const char *iface,
                              const char *prop, GError **err, gpointer ud)
{
    (void)c; (void)sender; (void)path; (void)iface; (void)err; (void)ud;
    if (!g_strcmp0(prop, "Category")) return g_variant_new_string("ApplicationStatus");
    if (!g_strcmp0(prop, "Id")) return g_variant_new_string("kishot");
    if (!g_strcmp0(prop, "Title")) return g_variant_new_string("kishot");
    if (!g_strcmp0(prop, "Status")) return g_variant_new_string("Active");
    if (!g_strcmp0(prop, "IconName")) return g_variant_new_string("media-record");
    if (!g_strcmp0(prop, "ItemIsMenu")) return g_variant_new_boolean(FALSE);
    if (!g_strcmp0(prop, "Menu")) return g_variant_new_object_path("/NO_DMENU");
    if (!g_strcmp0(prop, "ToolTip"))
        return g_variant_new("(s@a(iiay)ss)", "media-record", g_variant_new_array(G_VARIANT_TYPE("(iiay)"), NULL, 0),
                             "kishot", t.tooltip ? t.tooltip : "");
    return NULL;
}

static const GDBusInterfaceVTable vtable = { method_call, get_property, NULL, { 0 } };

static void register_with_watcher(void)
{
    if (!t.conn || t.registered)
        return;
    const char *name = g_dbus_connection_get_unique_name(t.conn);
    char *svc = g_strdup_printf("org.kde.StatusNotifierItem-%d-1", (int)getpid());
    (void)name;
    g_dbus_connection_call(t.conn, WATCHER_NAME, WATCHER_PATH, WATCHER_NAME, "RegisterStatusNotifierItem",
                           g_variant_new("(s)", svc), NULL, G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL, NULL);
    g_free(svc);
    t.registered = TRUE;
}

static void drop_fallback(void)
{
    if (!t.fallback)
        return;
    gtk_status_icon_set_visible(t.fallback, FALSE);
    g_object_unref(t.fallback);
    t.fallback = NULL;
}

static void on_bus_acquired(GDBusConnection *c, const char *name, gpointer ud)
{
    (void)name; (void)ud;
    t.conn = c;
    t.reg_id = g_dbus_connection_register_object(c, ITEM_PATH, t.info->interfaces[0], &vtable, NULL, NULL, NULL);
}

static void on_name_acquired(GDBusConnection *c, const char *name, gpointer ud)
{
    (void)c; (void)name; (void)ud;
    /* registered when the watcher is there; its appearance does the rest */
}

static void on_watcher_appeared(GDBusConnection *c, const char *name, const char *owner, gpointer ud)
{
    (void)c; (void)name; (void)owner; (void)ud;
    t.registered = FALSE;
    drop_fallback();
    register_with_watcher();
}

static void on_watcher_vanished(GDBusConnection *c, const char *name, gpointer ud)
{
    (void)c; (void)name; (void)ud;
    t.registered = FALSE;
    if (t.shown && !t.fallback) {
        t.fallback = gtk_status_icon_new_from_icon_name("media-record");
        g_signal_connect(t.fallback, "activate", G_CALLBACK(on_fallback_activate), NULL);
        gtk_status_icon_set_visible(t.fallback, TRUE);
        if (t.tooltip)
            gtk_status_icon_set_tooltip_text(t.fallback, t.tooltip);
    }
}

void ks_tray_show(KsTrayActivate cb, void *data)
{
    if (t.shown)
        return;
    t.shown = TRUE;
    t.cb = cb;
    t.data = data;
    t.info = g_dbus_node_info_new_for_xml(item_xml, NULL);
    char *svc = g_strdup_printf("org.kde.StatusNotifierItem-%d-1", (int)getpid());
    t.own_id = g_bus_own_name(G_BUS_TYPE_SESSION, svc, G_BUS_NAME_OWNER_FLAGS_NONE, on_bus_acquired,
                              on_name_acquired, NULL, NULL, NULL);
    g_free(svc);
    t.watch_id = g_bus_watch_name(G_BUS_TYPE_SESSION, WATCHER_NAME, G_BUS_NAME_WATCHER_FLAGS_NONE,
                                  on_watcher_appeared, on_watcher_vanished, NULL, NULL);
}

void ks_tray_set_tooltip(const char *text)
{
    if (!t.shown)
        return;
    g_free(t.tooltip);
    t.tooltip = g_strdup(text);
    if (t.fallback)
        gtk_status_icon_set_tooltip_text(t.fallback, text);
    if (t.conn && t.registered)
        g_dbus_connection_emit_signal(t.conn, NULL, ITEM_PATH, ITEM_IFACE, "NewToolTip", NULL, NULL);
}

void ks_tray_hide(void)
{
    if (!t.shown)
        return;
    t.shown = FALSE;
    drop_fallback();
    if (t.watch_id)
        g_bus_unwatch_name(t.watch_id);
    if (t.conn && t.reg_id)
        g_dbus_connection_unregister_object(t.conn, t.reg_id);
    if (t.own_id)
        g_bus_unown_name(t.own_id);
    if (t.info)
        g_dbus_node_info_unref(t.info);
    g_free(t.tooltip);
    t = (typeof(t)){ 0 };
}
