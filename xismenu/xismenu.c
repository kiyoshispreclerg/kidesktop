/*
 * xismenu - application menu registrar for KiDesktop.
 *
 * Qt/KF5 applications don't export their menubar over DBus unless they
 * believe a global menu exists, and the way they decide is by looking for
 * `com.canonical.AppMenu.Registrar` on the session bus. Under Plasma that
 * name belongs to kded5's appmenu module (kf5/kded/appmenu.so -- a kded
 * *plugin*, so there is no standalone binary to borrow). In a kisession
 * with no kded, nothing owns it, nothing exports, and everything
 * downstream goes quiet: kiwm's appmenu titlebar button never appears,
 * `xisserve --menu` has nothing to pop, and the globalmenu widget/search
 * plugin find nothing.
 *
 * So this owns the name, keeps a window -> (bus name, menu path) table,
 * and -- the part everything downstream actually reads -- writes two X11
 * properties on each window that registers:
 *
 *     _KDE_NET_WM_APPMENU_SERVICE_NAME   the registering client's bus name
 *     _KDE_NET_WM_APPMENU_OBJECT_PATH    the menu object path it registered
 *
 * It never speaks DBusMenu itself. Consumers read those properties and
 * talk to the application directly, which is exactly what lets kiwm offer
 * an application-menu button without becoming a DBus client (see
 * ../kiwm's appmenu_command= and ../xisserve/appmenu.c).
 *
 * Started early by kisession, before the panel: an application only
 * creates its exporter if the registrar is already on the bus when the
 * application starts, so anything launched before this daemon never
 * exports at all -- not even later.
 *
 * GTK applications don't register: GTK3 natively, and GTK2 through
 * appmenu-gtk-module, export a *GMenuModel* instead -- a different
 * protocol, which is what gmenu.c translates into DBusMenu objects served
 * by this same process (see that file). The one place the two meet here
 * is appmenu-gtk-module, which *does* call RegisterWindow, with its
 * GMenuModel path: that registration is kept in the table (the module
 * expects it) but never advertised as DBusMenu -- it's handed to the
 * translator as a hint instead.
 */
#define _POSIX_C_SOURCE 200809L

#include "xismenu.h"

#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define REGISTRAR_NAME "com.canonical.AppMenu.Registrar"
#define REGISTRAR_PATH "/com/canonical/AppMenu/Registrar"
#define REGISTRAR_IFACE "com.canonical.AppMenu.Registrar"

/* The KDE-specific name kded's module owns alongside the registrar.
 * Owning it is how KF5 applications are told a global menu is *showing*
 * their menubar, which makes them hide their own in-window one -- so it
 * is opt-in (--hide-app-menubar) rather than default: turning it on
 * before the menu is actually reachable somewhere leaves an application
 * with no menu at all. */
#define KAPPMENU_NAME "org.kde.kappmenu"

#define PROP_SERVICE "_KDE_NET_WM_APPMENU_SERVICE_NAME"
#define PROP_PATH "_KDE_NET_WM_APPMENU_OBJECT_PATH"

/* Registrations are one per top-level window of one running application;
 * a few hundred covers a session with everything open several times over,
 * and a fixed table keeps this daemon allocation-free after startup. */
#define MAX_MENUS 512

typedef struct {
    uint32_t window;
    char service[BUSNAME_MAX];  /* the application's unique bus name */
    char path[PATH_MAX_LEN];    /* its menu object -- DBusMenu, or GMenuModel (see below) */
    bool gmenu;                 /* a GMenuModel export: registered, not advertised */
} MenuEntry;

/* appmenu-gtk-module's object paths. Anything registered under here is an
 * org.gtk.Menus object, which no consumer of the window properties can
 * read, so it must not be advertised as if it were DBusMenu. */
#define GMENU_PATH_PREFIX "/org/appmenu/gtk/"

static bool is_gmenu_path(const char *path)
{
    return strncmp(path, GMENU_PATH_PREFIX, strlen(GMENU_PATH_PREFIX)) == 0;
}

static MenuEntry g_menus[MAX_MENUS];
static int g_menu_count;

DBusConnection *g_bus;
xcb_connection_t *g_xcb;
xcb_window_t g_root;
static xcb_atom_t g_atom_service, g_atom_path;

bool g_verbose;
static bool g_write_props = true;
static volatile sig_atomic_t g_running = 1;

static const char *INTROSPECT_XML =
    "<node>"
    " <interface name='org.freedesktop.DBus.Introspectable'>"
    "  <method name='Introspect'><arg name='xml' type='s' direction='out'/></method>"
    " </interface>"
    " <interface name='" REGISTRAR_IFACE "'>"
    "  <method name='RegisterWindow'>"
    "   <arg name='windowId' type='u' direction='in'/>"
    "   <arg name='menuObjectPath' type='o' direction='in'/>"
    "  </method>"
    "  <method name='UnregisterWindow'>"
    "   <arg name='windowId' type='u' direction='in'/>"
    "  </method>"
    "  <method name='GetMenuForWindow'>"
    "   <arg name='windowId' type='u' direction='in'/>"
    "   <arg name='service' type='s' direction='out'/>"
    "   <arg name='menuObjectPath' type='o' direction='out'/>"
    "  </method>"
    "  <method name='GetMenus'>"
    "   <arg name='menus' type='a(uso)' direction='out'/>"
    "  </method>"
    "  <signal name='WindowRegistered'>"
    "   <arg name='windowId' type='u'/><arg name='service' type='s'/>"
    "   <arg name='menuObjectPath' type='o'/>"
    "  </signal>"
    "  <signal name='WindowUnregistered'><arg name='windowId' type='u'/></signal>"
    " </interface>"
    "</node>";

void logmsg(const char *fmt, ...)
{
    char stamp[16];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(stamp, sizeof(stamp), "%H:%M:%S", &tm);

    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%s xismenu: ", stamp);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    fflush(stderr);
}

static void on_term(int sig)
{
    (void)sig;
    g_running = 0;
}

/* ---- X11 side -------------------------------------------------------- */

static xcb_atom_t intern(const char *name)
{
    xcb_intern_atom_reply_t *r =
        xcb_intern_atom_reply(g_xcb, xcb_intern_atom(g_xcb, 0, (uint16_t)strlen(name), name), NULL);
    xcb_atom_t a = r ? r->atom : XCB_ATOM_NONE;
    free(r);
    return a;
}

/* Both properties are plain STRING, not UTF8_STRING -- that is what every
 * reader of them expects (xispanel's globalmenu widget, xisserve's
 * plugin, kiwm), and what Plasma writes. */
void write_props(uint32_t window, const char *service, const char *path)
{
    if (!g_write_props || !g_xcb)
        return;
    xcb_change_property(g_xcb, XCB_PROP_MODE_REPLACE, window, g_atom_service, XCB_ATOM_STRING, 8,
                        (uint32_t)strlen(service), service);
    xcb_change_property(g_xcb, XCB_PROP_MODE_REPLACE, window, g_atom_path, XCB_ATOM_STRING, 8,
                        (uint32_t)strlen(path), path);
    xcb_flush(g_xcb);
}

void clear_props(uint32_t window)
{
    if (!g_write_props || !g_xcb)
        return;
    /* A window that is already gone makes these BadWindow, which is
     * expected and harmless: the requests are never checked, and the
     * connection carries on. Deleting the properties matters only for the
     * case where the window outlives its menu. */
    xcb_delete_property(g_xcb, window, g_atom_service);
    xcb_delete_property(g_xcb, window, g_atom_path);
    xcb_flush(g_xcb);
}

/* ---- the table ------------------------------------------------------- */

static MenuEntry *find_menu(uint32_t window)
{
    for (int i = 0; i < g_menu_count; i++)
        if (g_menus[i].window == window)
            return &g_menus[i];
    return NULL;
}

static void forget_at(int i)
{
    g_menus[i] = g_menus[--g_menu_count];
}

/* Whether any *other* entry still belongs to this application, which
 * decides if its NameOwnerChanged match is still worth keeping. */
static bool service_still_used(const char *service)
{
    for (int i = 0; i < g_menu_count; i++)
        if (strcmp(g_menus[i].service, service) == 0)
            return true;
    return false;
}

static void watch_service(const char *service, bool on)
{
    char rule[320];
    snprintf(rule, sizeof(rule),
             "type='signal',sender='org.freedesktop.DBus',"
             "interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='%s'", service);
    if (on)
        dbus_bus_add_match(g_bus, rule, NULL);
    else
        dbus_bus_remove_match(g_bus, rule, NULL);
}

static void emit_unregistered(uint32_t window);

/* An application exited: nothing calls UnregisterWindow on a crash, and a
 * stale entry pointing at a dead bus name is worse than no entry -- a
 * consumer would sit waiting for a reply that can never come. */
static void drop_service(const char *service)
{
    for (int i = 0; i < g_menu_count;) {
        if (strcmp(g_menus[i].service, service) != 0) {
            i++;
            continue;
        }
        uint32_t w = g_menus[i].window;
        logmsg("window 0x%x: %s vanished, dropping", w, service);
        if (!g_menus[i].gmenu && !gmenu_window_translated(w))
            clear_props(w);
        forget_at(i);
        emit_unregistered(w);
    }
    watch_service(service, false);
}

/* ---- DBus ------------------------------------------------------------ */

static void send_signal(const char *member, uint32_t window, const char *service, const char *path)
{
    DBusMessage *sig = dbus_message_new_signal(REGISTRAR_PATH, REGISTRAR_IFACE, member);
    if (!sig)
        return;
    if (service)
        dbus_message_append_args(sig, DBUS_TYPE_UINT32, &window, DBUS_TYPE_STRING, &service,
                                 DBUS_TYPE_OBJECT_PATH, &path, DBUS_TYPE_INVALID);
    else
        dbus_message_append_args(sig, DBUS_TYPE_UINT32, &window, DBUS_TYPE_INVALID);
    dbus_connection_send(g_bus, sig, NULL);
    dbus_message_unref(sig);
}

static void emit_unregistered(uint32_t window)
{
    send_signal("WindowUnregistered", window, NULL, NULL);
}

static void reply_empty(DBusMessage *msg)
{
    DBusMessage *r = dbus_message_new_method_return(msg);
    if (r) {
        dbus_connection_send(g_bus, r, NULL);
        dbus_message_unref(r);
    }
}

static void reply_error(DBusMessage *msg, const char *name, const char *text)
{
    DBusMessage *r = dbus_message_new_error(msg, name, text);
    if (r) {
        dbus_connection_send(g_bus, r, NULL);
        dbus_message_unref(r);
    }
}

static void handle_register(DBusMessage *msg)
{
    uint32_t window = 0;
    const char *path = NULL;
    DBusError err;
    dbus_error_init(&err);
    if (!dbus_message_get_args(msg, &err, DBUS_TYPE_UINT32, &window,
                               DBUS_TYPE_OBJECT_PATH, &path, DBUS_TYPE_INVALID)) {
        reply_error(msg, DBUS_ERROR_INVALID_ARGS, err.message ? err.message : "expected (u, o)");
        dbus_error_free(&err);
        return;
    }
    const char *sender = dbus_message_get_sender(msg);
    if (!sender || !window) {
        reply_error(msg, DBUS_ERROR_INVALID_ARGS, "no sender or null window");
        return;
    }

    MenuEntry *e = find_menu(window);
    if (!e) {
        if (g_menu_count >= MAX_MENUS) {
            logmsg("table full (%d entries), refusing window 0x%x", MAX_MENUS, window);
            reply_error(msg, DBUS_ERROR_LIMITS_EXCEEDED, "too many registered windows");
            return;
        }
        e = &g_menus[g_menu_count++];
        e->window = window;
    }
    snprintf(e->service, sizeof(e->service), "%s", sender);
    snprintf(e->path, sizeof(e->path), "%s", path);
    e->gmenu = is_gmenu_path(path);

    if (e->gmenu) {
        logmsg("RegisterWindow 0x%x -> %s %s (GMenuModel -- handing to the translator)",
               window, sender, path);
        /* appmenu-gtk-module registering is the surest sign this window
         * now carries the _GTK_* properties: have the translator look at
         * it right away rather than wait for a PropertyNotify. If it
         * takes the window, the properties point at us; if not, nothing
         * must advertise a GMenuModel object as DBusMenu. */
        gmenu_rescan_window(window);
        if (!gmenu_window_translated(window))
            clear_props(window);
    } else {
        logmsg("RegisterWindow 0x%x -> %s %s", window, sender, path);
        write_props(window, sender, path);
    }
    watch_service(sender, true);
    send_signal("WindowRegistered", window, sender, path);
    reply_empty(msg);
}

static void handle_unregister(DBusMessage *msg)
{
    uint32_t window = 0;
    if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_UINT32, &window, DBUS_TYPE_INVALID)) {
        reply_error(msg, DBUS_ERROR_INVALID_ARGS, "expected (u)");
        return;
    }

    for (int i = 0; i < g_menu_count; i++) {
        if (g_menus[i].window != window)
            continue;
        char service[BUSNAME_MAX];
        snprintf(service, sizeof(service), "%s", g_menus[i].service);
        bool gmenu = g_menus[i].gmenu;
        forget_at(i);
        if (!gmenu && !gmenu_window_translated(window))
            clear_props(window);
        if (!service_still_used(service))
            watch_service(service, false);
        break;
    }
    logmsg("UnregisterWindow 0x%x", window);
    emit_unregistered(window);
    reply_empty(msg);
}

static void handle_get_menu(DBusMessage *msg)
{
    uint32_t window = 0;
    if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_UINT32, &window, DBUS_TYPE_INVALID)) {
        reply_error(msg, DBUS_ERROR_INVALID_ARGS, "expected (u)");
        return;
    }
    MenuEntry *e = find_menu(window);
    /* Unknown window answers ("", "/") rather than an error: that's what
     * the Unity registrar this interface comes from does, and callers
     * check for an empty service name. */
    const char *service = e ? e->service : "";
    const char *path = e ? e->path : "/";
    if (g_verbose)
        logmsg("GetMenuForWindow 0x%x -> %s %s", window, e ? service : "(none)", path);

    DBusMessage *r = dbus_message_new_method_return(msg);
    if (!r)
        return;
    dbus_message_append_args(r, DBUS_TYPE_STRING, &service, DBUS_TYPE_OBJECT_PATH, &path,
                             DBUS_TYPE_INVALID);
    dbus_connection_send(g_bus, r, NULL);
    dbus_message_unref(r);
}

static void handle_get_menus(DBusMessage *msg)
{
    DBusMessage *r = dbus_message_new_method_return(msg);
    if (!r)
        return;

    DBusMessageIter it, array, entry;
    dbus_message_iter_init_append(r, &it);
    dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "(uso)", &array);
    for (int i = 0; i < g_menu_count; i++) {
        const char *service = g_menus[i].service;
        const char *path = g_menus[i].path;
        dbus_message_iter_open_container(&array, DBUS_TYPE_STRUCT, NULL, &entry);
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_UINT32, &g_menus[i].window);
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &service);
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_OBJECT_PATH, &path);
        dbus_message_iter_close_container(&array, &entry);
    }
    dbus_message_iter_close_container(&it, &array);

    dbus_connection_send(g_bus, r, NULL);
    dbus_message_unref(r);
}

static void handle_introspect(DBusMessage *msg)
{
    DBusMessage *r = dbus_message_new_method_return(msg);
    if (!r)
        return;
    dbus_message_append_args(r, DBUS_TYPE_STRING, &INTROSPECT_XML, DBUS_TYPE_INVALID);
    dbus_connection_send(g_bus, r, NULL);
    dbus_message_unref(r);
}

static DBusHandlerResult on_message(DBusConnection *conn, DBusMessage *msg, void *data)
{
    (void)conn;
    (void)data;

    if (dbus_message_is_signal(msg, DBUS_INTERFACE_DBUS, "NameOwnerChanged")) {
        const char *name = NULL, *old_owner = NULL, *new_owner = NULL;
        if (dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &name, DBUS_TYPE_STRING, &old_owner,
                                  DBUS_TYPE_STRING, &new_owner, DBUS_TYPE_INVALID) &&
            new_owner && !*new_owner)
            drop_service(name);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (dbus_message_get_type(msg) != DBUS_MESSAGE_TYPE_METHOD_CALL)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    const char *path = dbus_message_get_path(msg);
    if (!path || strcmp(path, REGISTRAR_PATH) != 0)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    if (dbus_message_is_method_call(msg, DBUS_INTERFACE_INTROSPECTABLE, "Introspect"))
        handle_introspect(msg);
    else if (dbus_message_is_method_call(msg, REGISTRAR_IFACE, "RegisterWindow"))
        handle_register(msg);
    else if (dbus_message_is_method_call(msg, REGISTRAR_IFACE, "UnregisterWindow"))
        handle_unregister(msg);
    else if (dbus_message_is_method_call(msg, REGISTRAR_IFACE, "GetMenuForWindow"))
        handle_get_menu(msg);
    else if (dbus_message_is_method_call(msg, REGISTRAR_IFACE, "GetMenus"))
        handle_get_menus(msg);
    else
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult on_message_all(DBusConnection *conn, DBusMessage *msg, void *data)
{
    /* The app's Changed signals and the /MenuBar/<n> objects belong to
     * the translator; everything else is the registrar's. */
    DBusHandlerResult r = gmenu_handle_message(msg);
    if (r == DBUS_HANDLER_RESULT_HANDLED)
        return r;
    return on_message(conn, msg, data);
}

/* ---- startup --------------------------------------------------------- */

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [-v] [--no-x-props] [--hide-app-menubar]\n"
            "       %s --version\n"
            "\n"
            "  -v, --verbose        log every call, not just registrations\n"
            "      --no-x-props     don't write %s /\n"
            "                       %s on registering windows\n"
            "      --hide-app-menubar\n"
            "                       also own %s, which makes KF5 apps hide their\n"
            "                       own in-window menubar. Only useful once the menu is\n"
            "                       reachable elsewhere (kiwm's appmenu button, the panel\n"
            "                       widget) -- otherwise the app ends up with no menu at all.\n",
            argv0, argv0, PROP_SERVICE, PROP_PATH, KAPPMENU_NAME);
}

/* Who owns a bus name right now, as "unique-name (pid comm)" -- the
 * detail that turns "already owned" into something actionable. The
 * usual culprit is a kded5 left over from a Plasma session: the systemd
 * user instance, and with it the session bus at $XDG_RUNTIME_DIR/bus,
 * outlives the X session that started it, so a kisession launched
 * afterwards inherits every KDE daemon still running on that bus. */
static void describe_owner(const char *name, char *out, size_t outsz)
{
    snprintf(out, outsz, "unknown owner");

    DBusMessage *m = dbus_message_new_method_call(DBUS_SERVICE_DBUS, DBUS_PATH_DBUS,
                                                  DBUS_INTERFACE_DBUS, "GetNameOwner");
    if (!m)
        return;
    dbus_message_append_args(m, DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID);
    DBusMessage *r = dbus_connection_send_with_reply_and_block(g_bus, m, 1000, NULL);
    dbus_message_unref(m);
    if (!r)
        return;
    const char *owner = NULL;
    if (!dbus_message_get_args(r, NULL, DBUS_TYPE_STRING, &owner, DBUS_TYPE_INVALID)) {
        dbus_message_unref(r);
        return;
    }
    snprintf(out, outsz, "%s", owner);

    m = dbus_message_new_method_call(DBUS_SERVICE_DBUS, DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS,
                                     "GetConnectionUnixProcessID");
    if (m) {
        dbus_message_append_args(m, DBUS_TYPE_STRING, &owner, DBUS_TYPE_INVALID);
        DBusMessage *r2 = dbus_connection_send_with_reply_and_block(g_bus, m, 1000, NULL);
        dbus_message_unref(m);
        uint32_t pid = 0;
        if (r2 && dbus_message_get_args(r2, NULL, DBUS_TYPE_UINT32, &pid, DBUS_TYPE_INVALID)) {
            char comm[64] = "?", path[64];
            snprintf(path, sizeof(path), "/proc/%u/comm", pid);
            FILE *f = fopen(path, "r");
            if (f) {
                if (fgets(comm, sizeof(comm), f)) {
                    char *nl = strchr(comm, '\n');
                    if (nl) *nl = '\0';
                }
                fclose(f);
            }
            snprintf(out, outsz, "%s (pid %u, %s)", owner, pid, comm);
        }
        if (r2)
            dbus_message_unref(r2);
    }
    dbus_message_unref(r);
}

/* Requests a name and insists on actually owning it. Being queued behind
 * an existing registrar is not something to limp along with: applications
 * would register with the other one and this daemon would sit there
 * pretending to be the menu service. So it says exactly who has it, and
 * exits -- kisession restarts it with backoff, and the log then shows what
 * has to go away first. */
static bool own_name(const char *name)
{
    DBusError err;
    dbus_error_init(&err);
    int r = dbus_bus_request_name(g_bus, name, DBUS_NAME_FLAG_DO_NOT_QUEUE, &err);
    if (dbus_error_is_set(&err)) {
        logmsg("cannot own %s: %s", name, err.message);
        dbus_error_free(&err);
        return false;
    }
    if (r != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
        char who[160];
        describe_owner(name, who, sizeof(who));
        logmsg("%s is already owned by %s -- a registrar left over from another "
               "session (kded5's, usually)? nothing will register with this one until it is gone",
               name, who);
        return false;
    }
    logmsg("owning %s", name);
    return true;
}

int main(int argc, char **argv)
{
    bool kde_name = false;

    static const struct option opts[] = {
        {"verbose", no_argument, 0, 'v'},
        {"no-x-props", no_argument, 0, 'n'},
        {"hide-app-menubar", no_argument, 0, 'k'},
        {"version", no_argument, 0, 'V'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0},
    };
    int c;
    while ((c = getopt_long(argc, argv, "vh", opts, NULL)) != -1) {
        switch (c) {
        case 'v': g_verbose = true; break;
        case 'n': g_write_props = false; break;
        case 'k': kde_name = true; break;
        case 'V': printf("xismenu %s\n", XISMENU_VERSION); return 0;
        case 'h': usage(argv[0]); return 0;
        default: usage(argv[0]); return 1;
        }
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_term;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    /* Started from a session leader whose terminal can go away. */
    signal(SIGHUP, SIG_IGN);

    DBusError err;
    dbus_error_init(&err);
    g_bus = dbus_bus_get(DBUS_BUS_SESSION, &err);
    if (!g_bus) {
        logmsg("no session bus: %s", err.message ? err.message : "unknown error");
        dbus_error_free(&err);
        return 1;
    }
    /* The bus outliving this process is the normal case (it's the
     * session's), so don't let libdbus abort the program if it drops. */
    dbus_connection_set_exit_on_disconnect(g_bus, FALSE);

    if (g_write_props) {
        int screen;
        g_xcb = xcb_connect(NULL, &screen);
        if (xcb_connection_has_error(g_xcb)) {
            logmsg("no X display -- window properties won't be written");
            xcb_disconnect(g_xcb);
            g_xcb = NULL;
        } else {
            g_atom_service = intern(PROP_SERVICE);
            g_atom_path = intern(PROP_PATH);
            xcb_screen_iterator_t it = xcb_setup_roots_iterator(xcb_get_setup(g_xcb));
            for (int i = 0; i < screen && it.rem; i++)
                xcb_screen_next(&it);
            g_root = it.data ? it.data->root : XCB_NONE;
        }
    }

    if (!dbus_connection_add_filter(g_bus, on_message_all, NULL, NULL)) {
        logmsg("out of memory adding message filter");
        return 1;
    }
    if (!own_name(REGISTRAR_NAME))
        return 1;
    if (kde_name && !own_name(KAPPMENU_NAME))
        logmsg("continuing without %s -- apps keep their own menubar", KAPPMENU_NAME);

    /* Which bus this is on, and as whom: the first thing to compare with
     * an application that "doesn't register" -- if its
     * DBUS_SESSION_BUS_ADDRESS differs, the two never meet. */
    const char *addr = getenv("DBUS_SESSION_BUS_ADDRESS");
    logmsg("on %s as %s", addr ? addr : "(autolaunched bus)", dbus_bus_get_unique_name(g_bus));

    /* The GMenuModel translator: discovers GTK windows through X and
     * serves their menus under this connection's unique name. */
    gmenu_init(dbus_bus_get_unique_name(g_bus));
    logmsg("waiting for applications to register");

    /* Two event sources -- the bus, and X for the translator's discovery
     * -- so poll() on both fds. libdbus is driven by hand here rather
     * than through its watch/timeout callbacks: after poll() wakes, a
     * zero-timeout read_write_dispatch() pulls in whatever arrived, and
     * dispatch() is drained. The 1s cap is only so a termination signal
     * that lands mid-wait is noticed promptly. */
    int dbus_fd = -1;
    dbus_connection_get_unix_fd(g_bus, &dbus_fd);
    while (g_running) {
        struct pollfd fds[2];
        int nfds = 0;
        fds[nfds++] = (struct pollfd){ .fd = dbus_fd, .events = POLLIN };
        if (g_xcb)
            fds[nfds++] = (struct pollfd){ .fd = xcb_get_file_descriptor(g_xcb), .events = POLLIN };

        /* Anything libdbus already queued (a reply that arrived while a
         * synchronous call was waiting) must be dispatched before
         * blocking, or it sits there until the next unrelated wakeup. */
        int timeout = dbus_connection_get_dispatch_status(g_bus) == DBUS_DISPATCH_DATA_REMAINS ? 0 : 1000;
        if (poll(fds, (nfds_t)nfds, timeout) < 0 && !g_running)
            break;

        if (!dbus_connection_read_write_dispatch(g_bus, 0))
            break; /* disconnected */
        while (dbus_connection_get_dispatch_status(g_bus) == DBUS_DISPATCH_DATA_REMAINS)
            dbus_connection_dispatch(g_bus);

        if (g_xcb) {
            xcb_generic_event_t *ev;
            while ((ev = xcb_poll_for_event(g_xcb))) {
                gmenu_handle_x_event(ev);
                free(ev);
            }
            if (xcb_connection_has_error(g_xcb)) {
                logmsg("X connection lost");
                break;
            }
        }
    }

    logmsg("exiting");
    if (g_xcb)
        xcb_disconnect(g_xcb);
    dbus_connection_unref(g_bus);
    return 0;
}
