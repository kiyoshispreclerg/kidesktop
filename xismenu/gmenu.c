/*
 * gmenu.c - GMenuModel -> DBusMenu translator.
 *
 * GTK applications never register with the AppMenu registrar. What they
 * do -- GTK3 natively, GTK2 through appmenu-gtk-module (GIMP 2.10 is the
 * reference case) -- is export their menu as a GMenuModel over the bus
 * and say so on the window:
 *
 *     _GTK_UNIQUE_BUS_NAME           the application's bus name
 *     _GTK_MENUBAR_OBJECT_PATH       an org.gtk.Menus (+ org.gtk.Actions) object
 *     _GTK_APPLICATION_OBJECT_PATH   org.gtk.Actions for "app." actions (native GTK3)
 *     _GTK_WINDOW_OBJECT_PATH        org.gtk.Actions for "win." actions (native GTK3)
 *
 * That is a different protocol from com.canonical.dbusmenu, which is the
 * only thing any consumer in this desktop speaks (xispanel's globalmenu
 * widget, xisserve's search plugin and --menu, and through it kiwm's
 * appmenu button). Under Plasma, gmenudbusmenuproxy bridges the two. This
 * is that bridge: for every window carrying those properties it serves a
 * DBusMenu object of its own at /MenuBar/<window> and points the window's
 * _KDE_NET_WM_APPMENU_* at itself, so every consumer works with GTK
 * applications unchanged.
 *
 * Discovery is by X11, not by the bus: the registrar only ever hears from
 * appmenu-gtk-module windows, native GTK3 ones tell nobody. So this
 * watches the WM's _NET_CLIENT_LIST for new clients and each client's
 * _GTK_* properties, which covers both -- and a gmenu-path registration
 * arriving at the registrar is used as one more hint to look at that
 * window now (gmenu_rescan_window()).
 *
 * The menu itself is rebuilt from the application on every GetLayout of
 * the root (with a short grace period, so the cascade of subtree requests
 * a consumer makes right after reuses the same build): org.gtk.Menus.
 * Start() for the root group, then for every group a :submenu/:section
 * refers to until closure, in as few round-trips as possible, plus
 * org.gtk.Actions.DescribeAll() on each action object for enabled/checked
 * state. Every call is local IPC; a GIMP-sized menu is a few milliseconds.
 * An item's "clicked" Event becomes org.gtk.Actions.Activate() on the
 * object its prefix names. Icons are not translated yet.
 */
#define _POSIX_C_SOURCE 200809L

#include "xismenu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DBUSMENU_IFACE "com.canonical.dbusmenu"
#define GTK_MENUS_IFACE "org.gtk.Menus"
#define GTK_ACTIONS_IFACE "org.gtk.Actions"

#define CALL_TIMEOUT_MS 500

/* Sizing. A translated menu is one application's menubar; GIMP's, the
 * biggest around here, is ~40 groups and ~600 items. */
#define MAX_SOURCES 64
#define MAX_NODES 2048
#define MAX_GROUPS 128
#define MAX_ACTIONS 1024
#define LABEL_MAX 128
#define ACTION_MAX 128

/* Only checked on a *root* GetLayout (parent==0): past this age, treat a
 * reopen of the menu from the top as a cue to refresh from the
 * application rather than serve a build the app may have long since
 * changed. Never applied to a subtree request (parent!=0): those must
 * keep reusing the build that handed out the id they're asking about, no
 * matter how long the menu has been sitting open -- rebuilding mid-
 * navigation reassigns every id (g_next_id restarts at 1 in
 * build_source()), so a submenu opened after this much idle time would
 * ask for an id that no longer exists in the fresh tree and get
 * DBUS_ERROR_INVALID_ARGS back, which looked like "submenus don't work"
 * from the consumer's side. */
#define BUILD_REUSE_MS 2000

typedef struct {
    int id;              /* DBusMenu item id, 1-based; 0 is the root */
    int parent;          /* index into nodes, -1 for top-level */
    char label[LABEL_MAX];
    char action[ACTION_MAX];   /* "prefix.name", "" for separators/submenu heads without one */
    bool separator;
    bool submenu;        /* has children */
    bool enabled;
    int toggle;          /* 0 none, 1 checkmark, 2 radio */
    bool checked;
} Node;

typedef struct {
    char name[ACTION_MAX];
    bool enabled;
    int state_kind;      /* 0 none, 1 boolean, 2 string */
    bool state_bool;
    char state_str[64];
} ActionInfo;

typedef struct {
    uint32_t window;
    char bus[BUSNAME_MAX];
    char menubar_path[PATH_MAX_LEN];
    char app_path[PATH_MAX_LEN];
    char win_path[PATH_MAX_LEN];
    char export_path[40];   /* "/MenuBar/<window>" */
    uint32_t revision;

    Node *nodes;
    int n_nodes;
    struct timespec built_at;
    bool built;
} Source;

static Source g_sources[MAX_SOURCES];
static int g_source_count;

static char g_unique[BUSNAME_MAX];

static xcb_atom_t a_client_list, a_utf8, a_gtk_bus, a_gtk_menubar, a_gtk_app, a_gtk_win;

/* Windows we have selected events on -- clients seen in _NET_CLIENT_LIST,
 * whether or not they turned out to have a menu, so a later property
 * change on them is noticed. */
static uint32_t g_watched[MAX_SOURCES * 8];
static int g_watched_count;

/* ---- small helpers --------------------------------------------------- */

static double ms_since(const struct timespec *t)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - t->tv_sec) * 1000.0 + (now.tv_nsec - t->tv_nsec) / 1e6;
}

static Source *source_for_window(uint32_t window)
{
    for (int i = 0; i < g_source_count; i++)
        if (g_sources[i].window == window)
            return &g_sources[i];
    return NULL;
}

static Source *source_for_path(const char *path)
{
    for (int i = 0; i < g_source_count; i++)
        if (strcmp(g_sources[i].export_path, path) == 0)
            return &g_sources[i];
    return NULL;
}

static Source *source_for_bus(const char *bus)
{
    for (int i = 0; i < g_source_count; i++)
        if (strcmp(g_sources[i].bus, bus) == 0)
            return &g_sources[i];
    return NULL;
}

static xcb_atom_t intern(const char *name)
{
    xcb_intern_atom_reply_t *r =
        xcb_intern_atom_reply(g_xcb, xcb_intern_atom(g_xcb, 0, (uint16_t)strlen(name), name), NULL);
    xcb_atom_t a = r ? r->atom : XCB_ATOM_NONE;
    free(r);
    return a;
}

/* A UTF8_STRING (or STRING -- Electron writes these as plain STRING)
 * property into `out`; false when absent. */
static bool read_string_prop(uint32_t window, xcb_atom_t atom, char *out, size_t outsz)
{
    out[0] = '\0';
    xcb_get_property_reply_t *r = xcb_get_property_reply(g_xcb,
        xcb_get_property(g_xcb, 0, window, atom, XCB_GET_PROPERTY_TYPE_ANY, 0, 256), NULL);
    if (!r)
        return false;
    int len = xcb_get_property_value_length(r);
    bool ok = r->format == 8 && len > 0 && (r->type == a_utf8 || r->type == XCB_ATOM_STRING);
    if (ok) {
        if ((size_t)len >= outsz)
            len = (int)outsz - 1;
        memcpy(out, xcb_get_property_value(r), (size_t)len);
        out[len] = '\0';
    }
    free(r);
    return ok;
}

/* ---- bus-side signal matches ----------------------------------------- */

static void watch_app_changes(const char *bus, bool on)
{
    const char *ifaces[] = { GTK_MENUS_IFACE, GTK_ACTIONS_IFACE };
    for (int i = 0; i < 2; i++) {
        char rule[320];
        snprintf(rule, sizeof(rule), "type='signal',sender='%s',interface='%s',member='Changed'",
                 bus, ifaces[i]);
        if (on)
            dbus_bus_add_match(g_bus, rule, NULL);
        else
            dbus_bus_remove_match(g_bus, rule, NULL);
    }
}

static void emit_layout_updated(Source *s)
{
    DBusMessage *sig = dbus_message_new_signal(s->export_path, DBUSMENU_IFACE, "LayoutUpdated");
    if (!sig)
        return;
    int32_t parent = 0;
    dbus_message_append_args(sig, DBUS_TYPE_UINT32, &s->revision, DBUS_TYPE_INT32, &parent,
                             DBUS_TYPE_INVALID);
    dbus_connection_send(g_bus, sig, NULL);
    dbus_message_unref(sig);
}

/* ---- sources: add / drop --------------------------------------------- */

static void drop_source(Source *s)
{
    logmsg("window 0x%x: GMenuModel export gone, dropping %s", s->window, s->export_path);
    clear_props(s->window);
    watch_app_changes(s->bus, false);
    free(s->nodes);
    *s = g_sources[--g_source_count];
}

static void add_or_update_source(uint32_t window, const char *bus, const char *menubar,
                                 const char *app, const char *win)
{
    Source *s = source_for_window(window);
    bool fresh = !s;
    if (!s) {
        if (g_source_count >= MAX_SOURCES) {
            logmsg("too many GMenuModel windows (%d), ignoring 0x%x", MAX_SOURCES, window);
            return;
        }
        s = &g_sources[g_source_count++];
        memset(s, 0, sizeof(*s));
        s->window = window;
        snprintf(s->export_path, sizeof(s->export_path), "/MenuBar/%u", window);
    } else if (strcmp(s->bus, bus) != 0) {
        watch_app_changes(s->bus, false);
        fresh = true;
    }
    snprintf(s->bus, sizeof(s->bus), "%s", bus);
    snprintf(s->menubar_path, sizeof(s->menubar_path), "%s", menubar);
    snprintf(s->app_path, sizeof(s->app_path), "%s", app);
    snprintf(s->win_path, sizeof(s->win_path), "%s", win);
    s->built = false;
    s->revision++;

    if (fresh) {
        watch_app_changes(bus, true);
        logmsg("window 0x%x: GMenuModel at %s %s -> serving %s", window, bus, menubar, s->export_path);
    }
    /* Ours, not the application's: consumers must reach *this* object. */
    write_props(window, g_unique, s->export_path);
}

/* Looks at one client window and reconciles: a menu appearing creates or
 * updates a source, one disappearing drops it. */
static void check_window(uint32_t window)
{
    char bus[BUSNAME_MAX], menubar[PATH_MAX_LEN], app[PATH_MAX_LEN], win[PATH_MAX_LEN];
    bool has = read_string_prop(window, a_gtk_bus, bus, sizeof(bus)) &&
               read_string_prop(window, a_gtk_menubar, menubar, sizeof(menubar));
    if (has) {
        read_string_prop(window, a_gtk_app, app, sizeof(app));
        read_string_prop(window, a_gtk_win, win, sizeof(win));
        add_or_update_source(window, bus, menubar, app, win);
    } else {
        Source *s = source_for_window(window);
        if (s)
            drop_source(s);
    }
}

static bool is_watched(uint32_t window)
{
    for (int i = 0; i < g_watched_count; i++)
        if (g_watched[i] == window)
            return true;
    return false;
}

static void watch_window(uint32_t window)
{
    if (is_watched(window))
        return;
    if (g_watched_count >= (int)(sizeof(g_watched) / sizeof(g_watched[0]))) {
        /* Forget the oldest; a window that old has long since been
         * checked, and its destruction will drop its source anyway. */
        memmove(g_watched, g_watched + 1, sizeof(g_watched) - sizeof(g_watched[0]));
        g_watched_count--;
    }
    g_watched[g_watched_count++] = window;
    /* PropertyChange for the _GTK_* properties (GTK sets them at realize,
     * appmenu-gtk-module a little later), StructureNotify for the
     * DestroyNotify that ends it. Event selection is per client, so this
     * doesn't interfere with the WM's own. */
    uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_STRUCTURE_NOTIFY;
    xcb_change_window_attributes(g_xcb, window, XCB_CW_EVENT_MASK, &mask);
}

static void unwatch_window(uint32_t window)
{
    for (int i = 0; i < g_watched_count; i++) {
        if (g_watched[i] == window) {
            g_watched[i] = g_watched[--g_watched_count];
            return;
        }
    }
}

static void scan_client_list(void)
{
    xcb_get_property_reply_t *r = xcb_get_property_reply(g_xcb,
        xcb_get_property(g_xcb, 0, g_root, a_client_list, XCB_ATOM_WINDOW, 0, 4096), NULL);
    if (!r)
        return;
    if (r->type == XCB_ATOM_WINDOW && r->format == 32) {
        uint32_t *wins = xcb_get_property_value(r);
        int n = xcb_get_property_value_length(r) / 4;
        for (int i = 0; i < n; i++) {
            if (!is_watched(wins[i])) {
                watch_window(wins[i]);
                check_window(wins[i]);
            }
        }
    }
    free(r);
    xcb_flush(g_xcb);
}

/* ---- public: discovery ----------------------------------------------- */

void gmenu_init(const char *unique_name)
{
    snprintf(g_unique, sizeof(g_unique), "%s", unique_name);
    if (!g_xcb)
        return;

    a_client_list = intern("_NET_CLIENT_LIST");
    a_utf8 = intern("UTF8_STRING");
    a_gtk_bus = intern("_GTK_UNIQUE_BUS_NAME");
    a_gtk_menubar = intern("_GTK_MENUBAR_OBJECT_PATH");
    a_gtk_app = intern("_GTK_APPLICATION_OBJECT_PATH");
    a_gtk_win = intern("_GTK_WINDOW_OBJECT_PATH");

    uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
    xcb_change_window_attributes(g_xcb, g_root, XCB_CW_EVENT_MASK, &mask);
    scan_client_list();
}

void gmenu_handle_x_event(xcb_generic_event_t *ev)
{
    switch (ev->response_type & ~0x80) {
    case XCB_PROPERTY_NOTIFY: {
        xcb_property_notify_event_t *p = (xcb_property_notify_event_t *)ev;
        if (p->window == g_root) {
            if (p->atom == a_client_list)
                scan_client_list();
        } else if (p->atom == a_gtk_bus || p->atom == a_gtk_menubar ||
                   p->atom == a_gtk_app || p->atom == a_gtk_win) {
            check_window(p->window);
            xcb_flush(g_xcb);
        }
        break;
    }
    case XCB_DESTROY_NOTIFY: {
        xcb_destroy_notify_event_t *d = (xcb_destroy_notify_event_t *)ev;
        unwatch_window(d->window);
        Source *s = source_for_window(d->window);
        if (s) {
            /* The window is gone; nothing to clear on it. */
            watch_app_changes(s->bus, false);
            free(s->nodes);
            *s = g_sources[--g_source_count];
            if (g_verbose)
                logmsg("window 0x%x destroyed, dropped its menu", d->window);
        }
        break;
    }
    default:
        break;
    }
}

bool gmenu_window_translated(uint32_t window)
{
    return source_for_window(window) != NULL;
}

void gmenu_rescan_window(uint32_t window)
{
    if (!g_xcb)
        return;
    watch_window(window);
    check_window(window);
    xcb_flush(g_xcb);
}

/* ---- fetching from the application ----------------------------------- */

/* One org.gtk.Menus item as fetched: the few keys that matter. */
typedef struct {
    char label[LABEL_MAX];
    char action[ACTION_MAX];
    bool has_submenu, has_section;
    uint32_t sub_group, sub_menu;   /* :submenu or :section target */
} GItem;

typedef struct {
    uint32_t group, menu;
    int first, count;               /* into g_items */
} GMenu;

static GMenu g_menus[MAX_GROUPS * 8];
static int g_menu_count;
static GItem g_items[MAX_NODES * 2];
static int g_item_count;

static ActionInfo g_actions[MAX_ACTIONS];
static int g_action_count;

static bool group_fetched[MAX_GROUPS];

static void parse_uu_variant(DBusMessageIter *variant, uint32_t *g, uint32_t *m)
{
    DBusMessageIter st;
    dbus_message_iter_recurse(variant, &st);
    if (dbus_message_iter_get_arg_type(&st) != DBUS_TYPE_STRUCT)
        return;
    DBusMessageIter f;
    dbus_message_iter_recurse(&st, &f);
    if (dbus_message_iter_get_arg_type(&f) == DBUS_TYPE_UINT32) {
        dbus_message_iter_get_basic(&f, g);
        dbus_message_iter_next(&f);
        if (dbus_message_iter_get_arg_type(&f) == DBUS_TYPE_UINT32)
            dbus_message_iter_get_basic(&f, m);
    }
}

/* Start(groups) -> appends every returned (group, menu, items) to
 * g_menus/g_items. Returns false on a failed call. */
static bool fetch_groups(Source *s, const uint32_t *groups, int n_groups)
{
    DBusMessage *msg = dbus_message_new_method_call(s->bus, s->menubar_path, GTK_MENUS_IFACE, "Start");
    if (!msg)
        return false;
    DBusMessageIter it, arr;
    dbus_message_iter_init_append(msg, &it);
    dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "u", &arr);
    for (int i = 0; i < n_groups; i++)
        dbus_message_iter_append_basic(&arr, DBUS_TYPE_UINT32, &groups[i]);
    dbus_message_iter_close_container(&it, &arr);

    DBusMessage *reply = dbus_connection_send_with_reply_and_block(g_bus, msg, CALL_TIMEOUT_MS, NULL);
    dbus_message_unref(msg);
    if (!reply)
        return false;

    DBusMessageIter rit, menus;
    if (!dbus_message_iter_init(reply, &rit) || dbus_message_iter_get_arg_type(&rit) != DBUS_TYPE_ARRAY) {
        dbus_message_unref(reply);
        return false;
    }
    dbus_message_iter_recurse(&rit, &menus);
    while (dbus_message_iter_get_arg_type(&menus) == DBUS_TYPE_STRUCT &&
           g_menu_count < (int)(sizeof(g_menus) / sizeof(g_menus[0]))) {
        DBusMessageIter m;
        dbus_message_iter_recurse(&menus, &m);
        GMenu *gm = &g_menus[g_menu_count];
        gm->group = gm->menu = 0;
        gm->first = g_item_count;
        gm->count = 0;
        if (dbus_message_iter_get_arg_type(&m) == DBUS_TYPE_UINT32) {
            dbus_message_iter_get_basic(&m, &gm->group);
            dbus_message_iter_next(&m);
        }
        if (dbus_message_iter_get_arg_type(&m) == DBUS_TYPE_UINT32) {
            dbus_message_iter_get_basic(&m, &gm->menu);
            dbus_message_iter_next(&m);
        }
        if (dbus_message_iter_get_arg_type(&m) == DBUS_TYPE_ARRAY) {
            DBusMessageIter items;
            dbus_message_iter_recurse(&m, &items);
            while (dbus_message_iter_get_arg_type(&items) == DBUS_TYPE_ARRAY &&
                   g_item_count < (int)(sizeof(g_items) / sizeof(g_items[0]))) {
                GItem *gi = &g_items[g_item_count];
                memset(gi, 0, sizeof(*gi));
                DBusMessageIter dict;
                dbus_message_iter_recurse(&items, &dict);
                while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
                    DBusMessageIter e;
                    dbus_message_iter_recurse(&dict, &e);
                    const char *key = NULL;
                    if (dbus_message_iter_get_arg_type(&e) == DBUS_TYPE_STRING)
                        dbus_message_iter_get_basic(&e, &key);
                    dbus_message_iter_next(&e);
                    if (key && dbus_message_iter_get_arg_type(&e) == DBUS_TYPE_VARIANT) {
                        DBusMessageIter v;
                        dbus_message_iter_recurse(&e, &v);
                        int vt = dbus_message_iter_get_arg_type(&v);
                        if (strcmp(key, "label") == 0 && vt == DBUS_TYPE_STRING) {
                            const char *str;
                            dbus_message_iter_get_basic(&v, &str);
                            snprintf(gi->label, sizeof(gi->label), "%s", str);
                        } else if (strcmp(key, "action") == 0 && vt == DBUS_TYPE_STRING) {
                            const char *str;
                            dbus_message_iter_get_basic(&v, &str);
                            snprintf(gi->action, sizeof(gi->action), "%s", str);
                        } else if (strcmp(key, ":submenu") == 0) {
                            gi->has_submenu = true;
                            parse_uu_variant(&e, &gi->sub_group, &gi->sub_menu);
                        } else if (strcmp(key, ":section") == 0) {
                            gi->has_section = true;
                            parse_uu_variant(&e, &gi->sub_group, &gi->sub_menu);
                        }
                    }
                    dbus_message_iter_next(&dict);
                }
                g_item_count++;
                gm->count++;
                dbus_message_iter_next(&items);
            }
        }
        g_menu_count++;
        dbus_message_iter_next(&menus);
    }
    dbus_message_unref(reply);

    for (int i = 0; i < n_groups; i++)
        if (groups[i] < MAX_GROUPS)
            group_fetched[groups[i]] = true;
    return true;
}

/* DescribeAll on one org.gtk.Actions object -> appended to g_actions
 * with names prefixed "<prefix>." so lookups use the same spelling the
 * menu items do. */
static void fetch_actions(const char *bus, const char *path, const char *prefix)
{
    if (!path[0])
        return;
    DBusMessage *msg = dbus_message_new_method_call(bus, path, GTK_ACTIONS_IFACE, "DescribeAll");
    if (!msg)
        return;
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(g_bus, msg, CALL_TIMEOUT_MS, NULL);
    dbus_message_unref(msg);
    if (!reply)
        return;

    DBusMessageIter rit, dict;
    if (dbus_message_iter_init(reply, &rit) && dbus_message_iter_get_arg_type(&rit) == DBUS_TYPE_ARRAY) {
        dbus_message_iter_recurse(&rit, &dict);
        while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY && g_action_count < MAX_ACTIONS) {
            DBusMessageIter e;
            dbus_message_iter_recurse(&dict, &e);
            const char *name = NULL;
            if (dbus_message_iter_get_arg_type(&e) == DBUS_TYPE_STRING)
                dbus_message_iter_get_basic(&e, &name);
            dbus_message_iter_next(&e);
            if (name && dbus_message_iter_get_arg_type(&e) == DBUS_TYPE_STRUCT) {
                ActionInfo *a = &g_actions[g_action_count];
                memset(a, 0, sizeof(*a));
                snprintf(a->name, sizeof(a->name), "%s.%s", prefix, name);
                DBusMessageIter f;
                dbus_message_iter_recurse(&e, &f);
                dbus_bool_t en = FALSE;
                if (dbus_message_iter_get_arg_type(&f) == DBUS_TYPE_BOOLEAN) {
                    dbus_message_iter_get_basic(&f, &en);
                    dbus_message_iter_next(&f);
                }
                a->enabled = en;
                if (dbus_message_iter_get_arg_type(&f) == DBUS_TYPE_SIGNATURE)
                    dbus_message_iter_next(&f);   /* parameter type, unused */
                if (dbus_message_iter_get_arg_type(&f) == DBUS_TYPE_ARRAY) {
                    DBusMessageIter st;
                    dbus_message_iter_recurse(&f, &st);
                    if (dbus_message_iter_get_arg_type(&st) == DBUS_TYPE_VARIANT) {
                        DBusMessageIter v;
                        dbus_message_iter_recurse(&st, &v);
                        int vt = dbus_message_iter_get_arg_type(&v);
                        if (vt == DBUS_TYPE_BOOLEAN) {
                            dbus_bool_t b;
                            dbus_message_iter_get_basic(&v, &b);
                            a->state_kind = 1;
                            a->state_bool = b;
                        } else if (vt == DBUS_TYPE_STRING) {
                            const char *str;
                            dbus_message_iter_get_basic(&v, &str);
                            a->state_kind = 2;
                            snprintf(a->state_str, sizeof(a->state_str), "%s", str);
                        }
                    }
                }
                g_action_count++;
            }
            dbus_message_iter_next(&dict);
        }
    }
    dbus_message_unref(reply);
}

static const ActionInfo *find_action(const char *name)
{
    for (int i = 0; i < g_action_count; i++)
        if (strcmp(g_actions[i].name, name) == 0)
            return &g_actions[i];
    return NULL;
}

static GMenu *find_gmenu(uint32_t group, uint32_t menu)
{
    for (int i = 0; i < g_menu_count; i++)
        if (g_menus[i].group == group && g_menus[i].menu == menu)
            return &g_menus[i];
    return NULL;
}

/* ---- composing the DBusMenu tree ------------------------------------- */

static int g_next_id;

static Node *new_node(Source *s, int parent)
{
    if (s->n_nodes >= MAX_NODES)
        return NULL;
    Node *n = &s->nodes[s->n_nodes++];
    memset(n, 0, sizeof(*n));
    n->id = g_next_id++;
    n->parent = parent;
    n->enabled = true;
    return n;
}

static bool last_child_is_separator(Source *s, int parent)
{
    for (int i = s->n_nodes - 1; i >= 0; i--)
        if (s->nodes[i].parent == parent)
            return s->nodes[i].separator;
    return true;   /* no children yet: a separator here would be leading */
}

/* Sections become runs of items with a separator between runs; submenus
 * become items carrying children. Both recurse through the same
 * fetched groups. */
static void compose_menu(Source *s, uint32_t group, uint32_t menu, int parent, int depth)
{
    GMenu *gm = find_gmenu(group, menu);
    if (!gm || depth > 12)
        return;

    for (int i = 0; i < gm->count; i++) {
        GItem *gi = &g_items[gm->first + i];

        if (gi->has_section) {
            if (!last_child_is_separator(s, parent)) {
                Node *sep = new_node(s, parent);
                if (!sep)
                    return;
                sep->separator = true;
            }
            compose_menu(s, gi->sub_group, gi->sub_menu, parent, depth + 1);
            continue;
        }

        Node *n = new_node(s, parent);
        if (!n)
            return;
        snprintf(n->label, sizeof(n->label), "%s", gi->label);
        snprintf(n->action, sizeof(n->action), "%s", gi->action);

        if (gi->has_submenu) {
            n->submenu = true;
            int idx = (int)(n - s->nodes);
            compose_menu(s, gi->sub_group, gi->sub_menu, idx, depth + 1);
            /* A submenu that produced nothing is still a submenu head;
             * consumers show it empty, which is what the app would too. */
            continue;
        }

        if (gi->action[0]) {
            const ActionInfo *a = find_action(gi->action);
            /* GTK semantics: an item whose action doesn't exist is
             * insensitive. */
            n->enabled = a && a->enabled;
            if (a && a->state_kind == 1) {
                n->toggle = 1;
                n->checked = a->state_bool;
            }
        } else if (!gi->label[0]) {
            n->separator = true;
        }
    }

    /* Trailing separator from a final empty section: drop it. */
    if (s->n_nodes > 0) {
        Node *last = &s->nodes[s->n_nodes - 1];
        if (last->parent == parent && last->separator)
            s->n_nodes--;
    }
}

static bool build_source(Source *s)
{
    if (s->built)
        return true;

    g_menu_count = g_item_count = g_action_count = 0;
    memset(group_fetched, 0, sizeof(group_fetched));

    /* Root group, then whatever it refers to, then whatever *that* refers
     * to -- one Start() per round, all of a round's new groups together. */
    uint32_t want[MAX_GROUPS];
    int n_want = 1;
    want[0] = 0;
    for (int round = 0; round < 16 && n_want > 0; round++) {
        if (!fetch_groups(s, want, n_want))
            return false;
        n_want = 0;
        for (int i = 0; i < g_item_count && n_want < MAX_GROUPS; i++) {
            GItem *gi = &g_items[i];
            if (!(gi->has_submenu || gi->has_section) || gi->sub_group >= MAX_GROUPS)
                continue;
            if (group_fetched[gi->sub_group])
                continue;
            bool queued = false;
            for (int j = 0; j < n_want; j++)
                if (want[j] == gi->sub_group)
                    queued = true;
            if (!queued)
                want[n_want++] = gi->sub_group;
        }
    }

    /* Actions live wherever their prefix says. appmenu-gtk-module puts
     * its "unity." ones on the menubar object itself; native GTK3 splits
     * "app." and "win." across the two other objects. Fetching the
     * menubar's object under every prefix seen there costs nothing extra
     * (one call) and keeps unusual prefixes working. */
    fetch_actions(s->bus, s->menubar_path, "unity");
    fetch_actions(s->bus, s->app_path, "app");
    fetch_actions(s->bus, s->win_path, "win");

    if (!s->nodes)
        s->nodes = calloc(MAX_NODES, sizeof(Node));
    if (!s->nodes)
        return false;
    s->n_nodes = 0;
    g_next_id = 1;
    compose_menu(s, 0, 0, -1, 0);

    s->built = true;
    clock_gettime(CLOCK_MONOTONIC, &s->built_at);
    if (g_verbose)
        logmsg("window 0x%x: built %d items from %d menus, %d actions", s->window, s->n_nodes,
               g_menu_count, g_action_count);
    return true;
}

/* ---- serving DBusMenu ------------------------------------------------ */

static void append_variant_string(DBusMessageIter *dict, const char *key, const char *val)
{
    DBusMessageIter e, v;
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &e);
    dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&e, DBUS_TYPE_VARIANT, "s", &v);
    dbus_message_iter_append_basic(&v, DBUS_TYPE_STRING, &val);
    dbus_message_iter_close_container(&e, &v);
    dbus_message_iter_close_container(dict, &e);
}

static void append_variant_bool(DBusMessageIter *dict, const char *key, bool val)
{
    DBusMessageIter e, v;
    dbus_bool_t b = val;
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &e);
    dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&e, DBUS_TYPE_VARIANT, "b", &v);
    dbus_message_iter_append_basic(&v, DBUS_TYPE_BOOLEAN, &b);
    dbus_message_iter_close_container(&e, &v);
    dbus_message_iter_close_container(dict, &e);
}

static void append_variant_int(DBusMessageIter *dict, const char *key, int32_t val)
{
    DBusMessageIter e, v;
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &e);
    dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&e, DBUS_TYPE_VARIANT, "i", &v);
    dbus_message_iter_append_basic(&v, DBUS_TYPE_INT32, &val);
    dbus_message_iter_close_container(&e, &v);
    dbus_message_iter_close_container(dict, &e);
}

static void append_node_props(DBusMessageIter *dict, const Node *n)
{
    if (n->separator) {
        append_variant_string(dict, "type", "separator");
        return;
    }
    append_variant_string(dict, "label", n->label);
    append_variant_bool(dict, "enabled", n->enabled);
    append_variant_bool(dict, "visible", true);
    if (n->submenu)
        append_variant_string(dict, "children-display", "submenu");
    if (n->toggle) {
        append_variant_string(dict, "toggle-type", n->toggle == 1 ? "checkmark" : "radio");
        append_variant_int(dict, "toggle-state", n->checked ? 1 : 0);
    }
}

/* One (ia{sv}av) node; children to `depth` more levels (-1 = all). */
static void append_layout_node(DBusMessageIter *it, Source *s, int node_idx, int depth)
{
    DBusMessageIter st, dict, children;
    dbus_message_iter_open_container(it, DBUS_TYPE_STRUCT, NULL, &st);

    int32_t id = node_idx < 0 ? 0 : s->nodes[node_idx].id;
    dbus_message_iter_append_basic(&st, DBUS_TYPE_INT32, &id);

    dbus_message_iter_open_container(&st, DBUS_TYPE_ARRAY, "{sv}", &dict);
    if (node_idx < 0)
        append_variant_string(&dict, "children-display", "submenu");
    else
        append_node_props(&dict, &s->nodes[node_idx]);
    dbus_message_iter_close_container(&st, &dict);

    dbus_message_iter_open_container(&st, DBUS_TYPE_ARRAY, "v", &children);
    if (depth != 0) {
        for (int i = 0; i < s->n_nodes; i++) {
            if (s->nodes[i].parent != node_idx)
                continue;
            DBusMessageIter v;
            dbus_message_iter_open_container(&children, DBUS_TYPE_VARIANT, "(ia{sv}av)", &v);
            append_layout_node(&v, s, i, depth < 0 ? -1 : depth - 1);
            dbus_message_iter_close_container(&children, &v);
        }
    }
    dbus_message_iter_close_container(&st, &children);
    dbus_message_iter_close_container(it, &st);
}

static int node_index_for_id(Source *s, int32_t id)
{
    if (id == 0)
        return -1;
    for (int i = 0; i < s->n_nodes; i++)
        if (s->nodes[i].id == id)
            return i;
    return -2;
}

static void reply_error(DBusMessage *msg, const char *name, const char *text)
{
    DBusMessage *r = dbus_message_new_error(msg, name, text);
    if (r) {
        dbus_connection_send(g_bus, r, NULL);
        dbus_message_unref(r);
    }
}

static void handle_get_layout(Source *s, DBusMessage *msg)
{
    int32_t parent = 0, depth = -1;
    dbus_message_get_args(msg, NULL, DBUS_TYPE_INT32, &parent, DBUS_TYPE_INT32, &depth, DBUS_TYPE_INVALID);

    /* The root is (re)built from the application; a subtree request
     * reuses the build the root request just made. */
    if (parent == 0)
        s->built = s->built && ms_since(&s->built_at) < BUILD_REUSE_MS;
    if (!build_source(s)) {
        reply_error(msg, DBUS_ERROR_FAILED, "the application did not answer");
        return;
    }
    int idx = node_index_for_id(s, parent);
    if (idx == -2) {
        reply_error(msg, DBUS_ERROR_INVALID_ARGS, "no such item");
        return;
    }

    DBusMessage *r = dbus_message_new_method_return(msg);
    if (!r)
        return;
    DBusMessageIter it;
    dbus_message_iter_init_append(r, &it);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_UINT32, &s->revision);
    append_layout_node(&it, s, idx, depth);
    dbus_connection_send(g_bus, r, NULL);
    dbus_message_unref(r);
}

static void handle_get_group_properties(Source *s, DBusMessage *msg)
{
    if (!build_source(s)) {
        reply_error(msg, DBUS_ERROR_FAILED, "the application did not answer");
        return;
    }
    DBusMessageIter it, ids;
    int32_t want[MAX_NODES];
    int n_want = 0;
    if (dbus_message_iter_init(msg, &it) && dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_ARRAY) {
        dbus_message_iter_recurse(&it, &ids);
        while (dbus_message_iter_get_arg_type(&ids) == DBUS_TYPE_INT32 && n_want < MAX_NODES) {
            dbus_message_iter_get_basic(&ids, &want[n_want++]);
            dbus_message_iter_next(&ids);
        }
    }

    DBusMessage *r = dbus_message_new_method_return(msg);
    if (!r)
        return;
    DBusMessageIter rit, arr;
    dbus_message_iter_init_append(r, &rit);
    dbus_message_iter_open_container(&rit, DBUS_TYPE_ARRAY, "(ia{sv})", &arr);
    for (int i = 0; i < s->n_nodes; i++) {
        bool wanted = n_want == 0;
        for (int j = 0; j < n_want && !wanted; j++)
            wanted = want[j] == s->nodes[i].id;
        if (!wanted)
            continue;
        DBusMessageIter st, dict;
        dbus_message_iter_open_container(&arr, DBUS_TYPE_STRUCT, NULL, &st);
        dbus_message_iter_append_basic(&st, DBUS_TYPE_INT32, &s->nodes[i].id);
        dbus_message_iter_open_container(&st, DBUS_TYPE_ARRAY, "{sv}", &dict);
        append_node_props(&dict, &s->nodes[i]);
        dbus_message_iter_close_container(&st, &dict);
        dbus_message_iter_close_container(&arr, &st);
    }
    dbus_message_iter_close_container(&rit, &arr);
    dbus_connection_send(g_bus, r, NULL);
    dbus_message_unref(r);
}

/* "clicked" on an item -> org.gtk.Actions.Activate on the object its
 * action prefix names. Fire and forget, like the click in the app's own
 * menubar; the application is the one that acts on it. */
static void activate(Source *s, const Node *n)
{
    if (!n->action[0])
        return;
    const char *dot = strchr(n->action, '.');
    if (!dot)
        return;
    const char *path = s->menubar_path;
    if (strncmp(n->action, "app.", 4) == 0 && s->app_path[0])
        path = s->app_path;
    else if (strncmp(n->action, "win.", 4) == 0 && s->win_path[0])
        path = s->win_path;
    const char *name = dot + 1;

    DBusMessage *msg = dbus_message_new_method_call(s->bus, path, GTK_ACTIONS_IFACE, "Activate");
    if (!msg)
        return;
    DBusMessageIter it, params, platform;
    dbus_message_iter_init_append(msg, &it);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &name);
    dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "v", &params);
    dbus_message_iter_close_container(&it, &params);
    dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &platform);
    dbus_message_iter_close_container(&it, &platform);
    dbus_message_set_no_reply(msg, TRUE);
    dbus_connection_send(g_bus, msg, NULL);
    dbus_message_unref(msg);
    if (g_verbose)
        logmsg("window 0x%x: activated %s on %s", s->window, n->action, path);
}

static void handle_event(Source *s, DBusMessage *msg)
{
    int32_t id = 0;
    const char *event_id = NULL;
    DBusMessageIter it;
    if (dbus_message_iter_init(msg, &it) && dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_INT32) {
        dbus_message_iter_get_basic(&it, &id);
        dbus_message_iter_next(&it);
        if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_STRING)
            dbus_message_iter_get_basic(&it, &event_id);
    }
    if (event_id && strcmp(event_id, "clicked") == 0 && s->built) {
        int idx = node_index_for_id(s, id);
        if (idx >= 0)
            activate(s, &s->nodes[idx]);
    }
    if (!dbus_message_get_no_reply(msg)) {
        DBusMessage *r = dbus_message_new_method_return(msg);
        if (r) {
            dbus_connection_send(g_bus, r, NULL);
            dbus_message_unref(r);
        }
    }
}

static void handle_about_to_show(DBusMessage *msg)
{
    /* Nothing to prepare: the layout is rebuilt on GetLayout anyway. */
    DBusMessage *r = dbus_message_new_method_return(msg);
    if (!r)
        return;
    dbus_bool_t need = FALSE;
    dbus_message_append_args(r, DBUS_TYPE_BOOLEAN, &need, DBUS_TYPE_INVALID);
    dbus_connection_send(g_bus, r, NULL);
    dbus_message_unref(r);
}

/* org.freedesktop.DBus.Properties on the menu object: the four DBusMenu
 * properties, fixed values. */
static void append_dbusmenu_property(DBusMessageIter *it, const char *name)
{
    DBusMessageIter v;
    if (strcmp(name, "Version") == 0) {
        uint32_t ver = 3;
        dbus_message_iter_open_container(it, DBUS_TYPE_VARIANT, "u", &v);
        dbus_message_iter_append_basic(&v, DBUS_TYPE_UINT32, &ver);
        dbus_message_iter_close_container(it, &v);
    } else if (strcmp(name, "IconThemePath") == 0) {
        DBusMessageIter arr;
        dbus_message_iter_open_container(it, DBUS_TYPE_VARIANT, "as", &v);
        dbus_message_iter_open_container(&v, DBUS_TYPE_ARRAY, "s", &arr);
        dbus_message_iter_close_container(&v, &arr);
        dbus_message_iter_close_container(it, &v);
    } else {
        const char *val = strcmp(name, "TextDirection") == 0 ? "ltr" : "normal";
        dbus_message_iter_open_container(it, DBUS_TYPE_VARIANT, "s", &v);
        dbus_message_iter_append_basic(&v, DBUS_TYPE_STRING, &val);
        dbus_message_iter_close_container(it, &v);
    }
}

static void handle_properties(DBusMessage *msg)
{
    const char *names[] = { "Version", "TextDirection", "Status", "IconThemePath" };
    DBusMessage *r = dbus_message_new_method_return(msg);
    if (!r)
        return;
    DBusMessageIter it;
    dbus_message_iter_init_append(r, &it);

    if (dbus_message_is_method_call(msg, DBUS_INTERFACE_PROPERTIES, "Get")) {
        const char *iface = NULL, *name = NULL;
        dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &iface, DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID);
        append_dbusmenu_property(&it, name ? name : "Status");
    } else {
        DBusMessageIter dict;
        dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &dict);
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
            DBusMessageIter e;
            dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &e);
            dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &names[i]);
            append_dbusmenu_property(&e, names[i]);
            dbus_message_iter_close_container(&dict, &e);
        }
        dbus_message_iter_close_container(&it, &dict);
    }
    dbus_connection_send(g_bus, r, NULL);
    dbus_message_unref(r);
}

static const char *MENU_INTROSPECT_XML =
    "<node>"
    " <interface name='org.freedesktop.DBus.Introspectable'>"
    "  <method name='Introspect'><arg name='xml' type='s' direction='out'/></method>"
    " </interface>"
    " <interface name='org.freedesktop.DBus.Properties'>"
    "  <method name='Get'><arg type='s' direction='in'/><arg type='s' direction='in'/>"
    "   <arg type='v' direction='out'/></method>"
    "  <method name='GetAll'><arg type='s' direction='in'/><arg type='a{sv}' direction='out'/></method>"
    " </interface>"
    " <interface name='" DBUSMENU_IFACE "'>"
    "  <property name='Version' type='u' access='read'/>"
    "  <property name='TextDirection' type='s' access='read'/>"
    "  <property name='Status' type='s' access='read'/>"
    "  <property name='IconThemePath' type='as' access='read'/>"
    "  <method name='GetLayout'><arg type='i' direction='in'/><arg type='i' direction='in'/>"
    "   <arg type='as' direction='in'/><arg type='u' direction='out'/>"
    "   <arg type='(ia{sv}av)' direction='out'/></method>"
    "  <method name='GetGroupProperties'><arg type='ai' direction='in'/><arg type='as' direction='in'/>"
    "   <arg type='a(ia{sv})' direction='out'/></method>"
    "  <method name='Event'><arg type='i' direction='in'/><arg type='s' direction='in'/>"
    "   <arg type='v' direction='in'/><arg type='u' direction='in'/></method>"
    "  <method name='AboutToShow'><arg type='i' direction='in'/><arg type='b' direction='out'/></method>"
    "  <signal name='LayoutUpdated'><arg type='u'/><arg type='i'/></signal>"
    " </interface>"
    "</node>";

DBusHandlerResult gmenu_handle_message(DBusMessage *msg)
{
    int type = dbus_message_get_type(msg);

    if (type == DBUS_MESSAGE_TYPE_SIGNAL && dbus_message_has_member(msg, "Changed") &&
        (dbus_message_has_interface(msg, GTK_MENUS_IFACE) || dbus_message_has_interface(msg, GTK_ACTIONS_IFACE))) {
        const char *sender = dbus_message_get_sender(msg);
        Source *s = sender ? source_for_bus(sender) : NULL;
        if (s) {
            /* The app changed its menu or an action's state: the next
             * GetLayout must not reuse the last build, and consumers that
             * keep a menu open get told. */
            s->built = false;
            s->revision++;
            emit_layout_updated(s);
        }
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (type != DBUS_MESSAGE_TYPE_METHOD_CALL)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    const char *path = dbus_message_get_path(msg);
    if (!path)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    Source *s = source_for_path(path);
    if (!s)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    if (dbus_message_is_method_call(msg, DBUS_INTERFACE_INTROSPECTABLE, "Introspect")) {
        DBusMessage *r = dbus_message_new_method_return(msg);
        if (r) {
            dbus_message_append_args(r, DBUS_TYPE_STRING, &MENU_INTROSPECT_XML, DBUS_TYPE_INVALID);
            dbus_connection_send(g_bus, r, NULL);
            dbus_message_unref(r);
        }
    } else if (dbus_message_is_method_call(msg, DBUS_INTERFACE_PROPERTIES, "Get") ||
               dbus_message_is_method_call(msg, DBUS_INTERFACE_PROPERTIES, "GetAll")) {
        handle_properties(msg);
    } else if (dbus_message_is_method_call(msg, DBUSMENU_IFACE, "GetLayout")) {
        handle_get_layout(s, msg);
    } else if (dbus_message_is_method_call(msg, DBUSMENU_IFACE, "GetGroupProperties")) {
        handle_get_group_properties(s, msg);
    } else if (dbus_message_is_method_call(msg, DBUSMENU_IFACE, "Event")) {
        handle_event(s, msg);
    } else if (dbus_message_is_method_call(msg, DBUSMENU_IFACE, "AboutToShow")) {
        handle_about_to_show(msg);
    } else {
        reply_error(msg, DBUS_ERROR_UNKNOWN_METHOD, dbus_message_get_member(msg));
    }
    return DBUS_HANDLER_RESULT_HANDLED;
}
