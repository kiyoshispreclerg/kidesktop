/*
 * mpris.c - see mpris.h.
 */
#include "mpris.h"

#include <gio/gio.h>
#include <string.h>

#define MPRIS_PREFIX "org.mpris.MediaPlayer2."
#define MPRIS_TIMEOUT_MS 250

static GDBusConnection *session_bus(void)
{
    static GDBusConnection *conn;
    static gboolean tried;
    if (!tried) {
        tried = TRUE;
        conn = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    }
    return conn;
}

static GVariant *call(GDBusConnection *c, const char *dest, const char *path, const char *iface,
                      const char *method, GVariant *params)
{
    GVariant *r = g_dbus_connection_call_sync(c, dest, path, iface, method, params, NULL, G_DBUS_CALL_FLAGS_NONE,
                                              MPRIS_TIMEOUT_MS, NULL, NULL);
    return r;
}

/* Properties.Get -> the unboxed value, or NULL. */
static GVariant *get_prop(GDBusConnection *c, const char *bus, const char *iface, const char *prop)
{
    GVariant *r = call(c, bus, "/org/mpris/MediaPlayer2", "org.freedesktop.DBus.Properties", "Get",
                       g_variant_new("(ss)", iface, prop));
    if (!r) {
        return NULL;
    }
    GVariant *boxed = NULL;
    g_variant_get(r, "(v)", &boxed);
    g_variant_unref(r);
    return boxed;
}

static void get_string(GDBusConnection *c, const char *bus, const char *iface, const char *prop, char *out, size_t n)
{
    out[0] = 0;
    GVariant *v = get_prop(c, bus, iface, prop);
    if (v) {
        if (g_variant_is_of_type(v, G_VARIANT_TYPE_STRING)) {
            g_strlcpy(out, g_variant_get_string(v, NULL), n);
        }
        g_variant_unref(v);
    }
}

static gboolean get_bool(GDBusConnection *c, const char *bus, const char *prop)
{
    gboolean b = FALSE;
    GVariant *v = get_prop(c, bus, "org.mpris.MediaPlayer2.Player", prop);
    if (v) {
        if (g_variant_is_of_type(v, G_VARIANT_TYPE_BOOLEAN)) {
            b = g_variant_get_boolean(v);
        }
        g_variant_unref(v);
    }
    return b;
}

GPtrArray *mpris_list(void)
{
    GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
    GDBusConnection *c = session_bus();
    if (!c) {
        return out;
    }
    GVariant *names = call(c, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "ListNames", NULL);
    if (!names) {
        return out;
    }
    GVariantIter *it;
    g_variant_get(names, "(as)", &it);
    const char *name;
    while (g_variant_iter_next(it, "&s", &name)) {
        if (strncmp(name, MPRIS_PREFIX, strlen(MPRIS_PREFIX)) != 0) {
            continue;
        }
        MprisPlayer *p = g_new0(MprisPlayer, 1);
        g_strlcpy(p->bus, name, sizeof(p->bus));
        get_string(c, name, "org.mpris.MediaPlayer2", "Identity", p->identity, sizeof(p->identity));
        get_string(c, name, "org.mpris.MediaPlayer2", "DesktopEntry", p->desktop, sizeof(p->desktop));
        char status[32];
        get_string(c, name, "org.mpris.MediaPlayer2.Player", "PlaybackStatus", status, sizeof(status));
        p->playing = strcmp(status, "Playing") == 0;
        p->can_next = get_bool(c, name, "CanGoNext");
        p->can_prev = get_bool(c, name, "CanGoPrevious");
        GVariant *pid = call(c, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                             "GetConnectionUnixProcessID", g_variant_new("(s)", name));
        if (pid) {
            g_variant_get(pid, "(u)", &p->pid);
            g_variant_unref(pid);
        }
        g_ptr_array_add(out, p);
    }
    g_variant_iter_free(it);
    g_variant_unref(names);
    return out;
}

/* Two names refer to the same program when one contains the other
 * (ignoring case) and the shorter is at least 3 chars, so "firefox"
 * matches "Firefox Web Browser" but "a" matches nothing. */
static gboolean names_match(const char *a, const char *b)
{
    if (!a[0] || !b[0]) {
        return FALSE;
    }
    char *la = g_ascii_strdown(a, -1), *lb = g_ascii_strdown(b, -1);
    size_t na = strlen(la), nb = strlen(lb);
    gboolean m = (na >= 3 && nb >= 3) && (strstr(la, lb) || strstr(lb, la));
    g_free(la);
    g_free(lb);
    return m;
}

const MprisPlayer *mpris_match(GPtrArray *players, guint pid, const char *app, const char *binary, const char *icon)
{
    if (pid) {
        for (guint i = 0; i < players->len; i++) {
            const MprisPlayer *p = g_ptr_array_index(players, i);
            if (p->pid == pid) {
                return p;
            }
        }
    }
    const char *mine[3] = {app, binary, icon};
    for (guint i = 0; i < players->len; i++) {
        const MprisPlayer *p = g_ptr_array_index(players, i);
        const char *suffix = p->bus + strlen(MPRIS_PREFIX);
        char short_bus[128];
        g_strlcpy(short_bus, suffix, sizeof(short_bus));
        char *dot = strstr(short_bus, ".instance");
        if (dot) {
            *dot = 0;
        }
        const char *theirs[3] = {p->identity, p->desktop, short_bus};
        for (int a = 0; a < 3; a++) {
            for (int b = 0; b < 3; b++) {
                if (names_match(mine[a] ? mine[a] : "", theirs[b])) {
                    return p;
                }
            }
        }
    }
    return NULL;
}

void mpris_command(const char *bus, const char *method)
{
    GDBusConnection *c = session_bus();
    if (!c) {
        return;
    }
    g_dbus_connection_call(c, bus, "/org/mpris/MediaPlayer2", "org.mpris.MediaPlayer2.Player", method, NULL, NULL,
                           G_DBUS_CALL_FLAGS_NONE, MPRIS_TIMEOUT_MS, NULL, NULL, NULL);
}
