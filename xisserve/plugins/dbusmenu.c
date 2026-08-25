/*
 * dbusmenu.c - see dbusmenu.h. Ported from xispanel/dbusmenu.c (that
 * file's own header comment has the full wire-protocol reasoning this
 * doesn't repeat) -- same dlopen'd-libdbus-1-independently philosophy
 * (own dlopen(), own symbol table, own session-bus connection, never
 * linked at build time; see the Makefile's HAVE_DBUS block and
 * dbusmenu_stub.c for what a system without libdbus-1-dev gets instead).
 */
#include "dbusmenu.h"
#include "../xisserve.h"

#include <dbus/dbus.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DBUSMENU_IFACE "com.canonical.dbusmenu"
#define DBUSMENU_CALL_TIMEOUT_MS 200
/* Cap on a single item's icon-data payload (raw embedded PNG bytes) --
 * generous for a menu icon (real ones are a few KB at most) while still
 * bounding how much a misbehaving app could make us buffer for one icon.
 * Buffered on the stack inside the (recursive) dbusmenu_parse_node(), so
 * this also has to stay small enough that deep nesting can't blow the
 * stack -- static, not stack-allocated, for exactly that reason (see
 * the xispanel original). */
#define DBUSMENU_ICON_DATA_MAX (32 * 1024)

static void *g_libdbus = NULL;
static int g_load_attempted = 0;
static DBusConnection *g_conn = NULL;

static void (*p_dbus_error_init)(DBusError *);
static void (*p_dbus_error_free)(DBusError *);
static dbus_bool_t (*p_dbus_error_is_set)(const DBusError *);
static DBusConnection *(*p_dbus_bus_get)(DBusBusType, DBusError *);
static DBusMessage *(*p_dbus_message_new_method_call)(const char *, const char *, const char *, const char *);
static void (*p_dbus_message_unref)(DBusMessage *);
static DBusMessage *(*p_dbus_connection_send_with_reply_and_block)(DBusConnection *, DBusMessage *, int, DBusError *);
static dbus_bool_t (*p_dbus_connection_send)(DBusConnection *, DBusMessage *, dbus_uint32_t *);
static dbus_bool_t (*p_dbus_message_iter_init)(DBusMessage *, DBusMessageIter *);
static dbus_bool_t (*p_dbus_message_iter_next)(DBusMessageIter *);
static int (*p_dbus_message_iter_get_arg_type)(DBusMessageIter *);
static void (*p_dbus_message_iter_recurse)(DBusMessageIter *, DBusMessageIter *);
static void (*p_dbus_message_iter_get_basic)(DBusMessageIter *, void *);
static void (*p_dbus_message_iter_init_append)(DBusMessage *, DBusMessageIter *);
static dbus_bool_t (*p_dbus_message_iter_append_basic)(DBusMessageIter *, int, const void *);
static dbus_bool_t (*p_dbus_message_iter_open_container)(DBusMessageIter *, int, const char *, DBusMessageIter *);
static dbus_bool_t (*p_dbus_message_iter_close_container)(DBusMessageIter *, DBusMessageIter *);

#define LOAD_SYM(name)                                                                                               \
    do {                                                                                                             \
        *(void **)(&p_##name) = dlsym(g_libdbus, #name);                                                            \
        if (!p_##name) {                                                                                             \
            fprintf(stderr, "xisserve: dbusmenu: symbol '%s' missing from libdbus-1, disabling\n", #name);          \
            return 0;                                                                                                \
        }                                                                                                            \
    } while (0)

static int dbusmenu_load_symbols(void)
{
    LOAD_SYM(dbus_error_init);
    LOAD_SYM(dbus_error_free);
    LOAD_SYM(dbus_error_is_set);
    LOAD_SYM(dbus_bus_get);
    LOAD_SYM(dbus_message_new_method_call);
    LOAD_SYM(dbus_message_unref);
    LOAD_SYM(dbus_connection_send_with_reply_and_block);
    LOAD_SYM(dbus_connection_send);
    LOAD_SYM(dbus_message_iter_init);
    LOAD_SYM(dbus_message_iter_next);
    LOAD_SYM(dbus_message_iter_get_arg_type);
    LOAD_SYM(dbus_message_iter_recurse);
    LOAD_SYM(dbus_message_iter_get_basic);
    LOAD_SYM(dbus_message_iter_init_append);
    LOAD_SYM(dbus_message_iter_append_basic);
    LOAD_SYM(dbus_message_iter_open_container);
    LOAD_SYM(dbus_message_iter_close_container);
    return 1;
}

static int dbusmenu_ensure_connected(void)
{
    if (g_conn) {
        return 1;
    }
    if (g_load_attempted) {
        return 0;
    }
    g_load_attempted = 1;

    g_libdbus = dlopen("libdbus-1.so.3", RTLD_NOW | RTLD_GLOBAL);
    if (!g_libdbus) {
        g_libdbus = dlopen("libdbus-1.so", RTLD_NOW | RTLD_GLOBAL);
    }
    if (!g_libdbus) {
        return 0;
    }
    if (!dbusmenu_load_symbols()) {
        dlclose(g_libdbus);
        g_libdbus = NULL;
        return 0;
    }

    DBusError err;
    p_dbus_error_init(&err);
    g_conn = p_dbus_bus_get(DBUS_BUS_SESSION, &err);
    if (p_dbus_error_is_set(&err)) {
        fprintf(stderr, "xisserve: dbusmenu: could not connect to session bus (%s)\n", err.message);
        p_dbus_error_free(&err);
    }
    return g_conn != NULL;
}

/* icon-data is a raw embedded image file (PNG in every real
 * implementation seen), decoded straight from memory via GdkPixbufLoader
 * -- no temp file needed (unlike the Imlib2-based xispanel original,
 * which has no from-memory entry point). Scaled to XISSERVE_ICON_PX so
 * every result row's icon column lines up regardless of what size the
 * exporting app embedded. */
static GdkPixbuf *decode_icon_data(const unsigned char *data, int len)
{
    if (!data || len <= 0) {
        return NULL;
    }
    GdkPixbufLoader *loader = gdk_pixbuf_loader_new();
    GdkPixbuf *result = NULL;
    if (gdk_pixbuf_loader_write(loader, data, (gsize)len, NULL) && gdk_pixbuf_loader_close(loader, NULL)) {
        GdkPixbuf *decoded = gdk_pixbuf_loader_get_pixbuf(loader); /* owned by loader */
        if (decoded) {
            result = gdk_pixbuf_scale_simple(decoded, XISSERVE_ICON_PX, XISSERVE_ICON_PX, GDK_INTERP_BILINEAR);
        }
    }
    g_object_unref(loader);
    return result;
}

/* Recursively flattens one (ia{sv}av) node (already positioned at a
 * DBUS_TYPE_VARIANT wrapping that struct) into out_items[]/out_ids[]/
 * out_depth[], recording each item's nesting depth (relative to the
 * subtree's own root) rather than baking it into the label. Skips
 * invisible items and their entire subtree; disabled items are still
 * listed (globalmenu.c filters those out of search results itself). */
static void dbusmenu_parse_node(DBusMessageIter *variant_iter, int depth, DbusMenuItem *out_items, int *out_ids,
                                 int *out_depth, int max_items, int *n)
{
    if (*n >= max_items) {
        return;
    }
    /* Two recurses, not one: the first lands *at* the (ia{sv}av) struct
     * the variant wraps (its arg type, not inside its fields yet); the
     * second actually enters that struct to reach its fields (id, then
     * props, then children). */
    DBusMessageIter node_struct, node;
    p_dbus_message_iter_recurse(variant_iter, &node_struct);
    p_dbus_message_iter_recurse(&node_struct, &node);

    int32_t id = 0;
    if (p_dbus_message_iter_get_arg_type(&node) == DBUS_TYPE_INT32) {
        p_dbus_message_iter_get_basic(&node, &id);
    }
    p_dbus_message_iter_next(&node);

    char label[128] = "";
    char icon_name[256] = "";
    static unsigned char icon_data[DBUSMENU_ICON_DATA_MAX];
    int icon_data_len = 0;
    int is_separator = 0, enabled = 1, visible = 1;
    if (p_dbus_message_iter_get_arg_type(&node) == DBUS_TYPE_ARRAY) {
        DBusMessageIter props;
        p_dbus_message_iter_recurse(&node, &props);
        while (p_dbus_message_iter_get_arg_type(&props) == DBUS_TYPE_DICT_ENTRY) {
            DBusMessageIter entry;
            p_dbus_message_iter_recurse(&props, &entry);
            const char *key = NULL;
            if (p_dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_STRING) {
                p_dbus_message_iter_get_basic(&entry, &key);
            }
            p_dbus_message_iter_next(&entry);
            if (key && p_dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_VARIANT) {
                DBusMessageIter val;
                p_dbus_message_iter_recurse(&entry, &val);
                int vt = p_dbus_message_iter_get_arg_type(&val);
                if (!strcmp(key, "label") && vt == DBUS_TYPE_STRING) {
                    const char *s = NULL;
                    p_dbus_message_iter_get_basic(&val, &s);
                    /* DBusMenu labels use "_" for mnemonics (like GTK) --
                     * stripped, xisserve has no keyboard-mnemonic
                     * underline rendering to hook it up to anyway. */
                    if (s) {
                        size_t o = 0;
                        for (size_t j = 0; s[j] && o + 1 < sizeof(label); j++) {
                            if (s[j] != '_') {
                                label[o++] = s[j];
                            }
                        }
                        label[o] = 0;
                    }
                } else if (!strcmp(key, "type") && vt == DBUS_TYPE_STRING) {
                    const char *s = NULL;
                    p_dbus_message_iter_get_basic(&val, &s);
                    if (s && !strcmp(s, "separator")) {
                        is_separator = 1;
                    }
                } else if (!strcmp(key, "enabled") && vt == DBUS_TYPE_BOOLEAN) {
                    dbus_bool_t b = TRUE;
                    p_dbus_message_iter_get_basic(&val, &b);
                    enabled = b;
                } else if (!strcmp(key, "visible") && vt == DBUS_TYPE_BOOLEAN) {
                    dbus_bool_t b = TRUE;
                    p_dbus_message_iter_get_basic(&val, &b);
                    visible = b;
                } else if (!strcmp(key, "icon-name") && vt == DBUS_TYPE_STRING) {
                    const char *s = NULL;
                    p_dbus_message_iter_get_basic(&val, &s);
                    if (s) {
                        snprintf(icon_name, sizeof(icon_name), "%s", s);
                    }
                } else if (!strcmp(key, "icon-data") && vt == DBUS_TYPE_ARRAY) {
                    DBusMessageIter bytes;
                    p_dbus_message_iter_recurse(&val, &bytes);
                    icon_data_len = 0;
                    while (p_dbus_message_iter_get_arg_type(&bytes) == DBUS_TYPE_BYTE &&
                           icon_data_len < DBUSMENU_ICON_DATA_MAX) {
                        unsigned char b;
                        p_dbus_message_iter_get_basic(&bytes, &b);
                        icon_data[icon_data_len++] = b;
                        if (!p_dbus_message_iter_next(&bytes)) {
                            break;
                        }
                    }
                }
            }
            if (!p_dbus_message_iter_next(&props)) {
                break;
            }
        }
        p_dbus_message_iter_next(&node);
    }

    if (visible) {
        DbusMenuItem *mi = &out_items[*n];
        memset(mi, 0, sizeof(*mi));
        mi->is_separator = is_separator;
        mi->enabled = enabled;
        if (!is_separator) {
            snprintf(mi->label, sizeof(mi->label), "%s", label);
            /* icon-data (an embedded image) wins over icon-name (a
             * themed name to resolve) when an item somehow has both --
             * an app that bothered to embed actual pixel data
             * presumably wants exactly that rendered, not a theme's
             * substitute. */
            if (icon_data_len > 0) {
                mi->icon = decode_icon_data(icon_data, icon_data_len);
            }
            if (!mi->icon && icon_name[0]) {
                mi->icon = xisserve_resolve_icon(icon_name, XISSERVE_ICON_PX);
            }
        }
        out_ids[*n] = id;
        out_depth[*n] = depth;
        (*n)++;
    }

    /* Third field: av (array of variant), each wrapping a child node --
     * present (possibly empty) whether or not this item has visible
     * children; recurse only if actually visible, matching the skip
     * above. */
    if (visible && p_dbus_message_iter_get_arg_type(&node) == DBUS_TYPE_ARRAY) {
        DBusMessageIter children;
        p_dbus_message_iter_recurse(&node, &children);
        while (p_dbus_message_iter_get_arg_type(&children) == DBUS_TYPE_VARIANT && *n < max_items) {
            dbusmenu_parse_node(&children, depth + 1, out_items, out_ids, out_depth, max_items, n);
            if (!p_dbus_message_iter_next(&children)) {
                break;
            }
        }
    }
}

void dbusmenu_free_item_icons(DbusMenuItem *items, int n)
{
    for (int i = 0; i < n; i++) {
        if (items[i].icon) {
            g_object_unref(items[i].icon);
            items[i].icon = NULL;
        }
    }
}

int dbusmenu_fetch(const char *busname, const char *path, int32_t parent_id, int32_t depth, DbusMenuItem *out_items,
                    int *out_ids, int *out_depth, int max_items)
{
    if (!dbusmenu_ensure_connected()) {
        return -1;
    }

    DBusMessage *msg = p_dbus_message_new_method_call(busname, path, DBUSMENU_IFACE, "GetLayout");
    if (!msg) {
        return -1;
    }
    DBusMessageIter it, str_arr;
    p_dbus_message_iter_init_append(msg, &it);
    p_dbus_message_iter_append_basic(&it, DBUS_TYPE_INT32, &parent_id);
    p_dbus_message_iter_append_basic(&it, DBUS_TYPE_INT32, &depth);
    p_dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "s", &str_arr);
    p_dbus_message_iter_close_container(&it, &str_arr);
    DBusError err;
    p_dbus_error_init(&err);
    DBusMessage *reply = p_dbus_connection_send_with_reply_and_block(g_conn, msg, DBUSMENU_CALL_TIMEOUT_MS, &err);
    p_dbus_message_unref(msg);
    if (p_dbus_error_is_set(&err)) {
        p_dbus_error_free(&err);
        return -1;
    }
    if (!reply) {
        return -1;
    }

    int n = 0;
    DBusMessageIter rit;
    if (p_dbus_message_iter_init(reply, &rit) && p_dbus_message_iter_get_arg_type(&rit) == DBUS_TYPE_UINT32) {
        p_dbus_message_iter_next(&rit); /* skip revision */
        if (p_dbus_message_iter_get_arg_type(&rit) == DBUS_TYPE_STRUCT) {
            /* dbusmenu_parse_node() expects a variant-wrapped struct
             * (that's the shape every *child* comes in); the top-level
             * reply's struct isn't itself variant-wrapped, but its own
             * children (what we actually want -- the root item is just
             * an unlabeled container) are, via the same av field every
             * node has. Walk into the root struct by hand once to reach
             * them. */
            DBusMessageIter root;
            p_dbus_message_iter_recurse(&rit, &root); /* now at field 1: id */
            p_dbus_message_iter_next(&root);          /* now at field 2: props dict (a{sv}) */
            if (p_dbus_message_iter_get_arg_type(&root) == DBUS_TYPE_ARRAY) {
                p_dbus_message_iter_next(&root); /* now at field 3: children (av) */
            }
            if (p_dbus_message_iter_get_arg_type(&root) == DBUS_TYPE_ARRAY) {
                DBusMessageIter children;
                p_dbus_message_iter_recurse(&root, &children);
                while (p_dbus_message_iter_get_arg_type(&children) == DBUS_TYPE_VARIANT && n < max_items) {
                    dbusmenu_parse_node(&children, 0, out_items, out_ids, out_depth, max_items, &n);
                    if (!p_dbus_message_iter_next(&children)) {
                        break;
                    }
                }
            }
        }
    }
    p_dbus_message_unref(reply);
    return n;
}

/* Fire-and-forget DBusMenu Event(id, "clicked", <int32 0>, timestamp) --
 * nothing here waits for or cares about a reply. */
void dbusmenu_send_event(const char *busname, const char *path, int32_t id)
{
    if (!dbusmenu_ensure_connected()) {
        return;
    }
    DBusMessage *msg = p_dbus_message_new_method_call(busname, path, DBUSMENU_IFACE, "Event");
    if (!msg) {
        return;
    }
    DBusMessageIter it, variant;
    p_dbus_message_iter_init_append(msg, &it);
    p_dbus_message_iter_append_basic(&it, DBUS_TYPE_INT32, &id);
    const char *event_id = "clicked";
    p_dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &event_id);
    p_dbus_message_iter_open_container(&it, DBUS_TYPE_VARIANT, "i", &variant);
    int32_t dummy = 0;
    p_dbus_message_iter_append_basic(&variant, DBUS_TYPE_INT32, &dummy);
    p_dbus_message_iter_close_container(&it, &variant);
    dbus_uint32_t timestamp = 0;
    p_dbus_message_iter_append_basic(&it, DBUS_TYPE_UINT32, &timestamp);
    p_dbus_connection_send(g_conn, msg, NULL);
    p_dbus_message_unref(msg);
}
