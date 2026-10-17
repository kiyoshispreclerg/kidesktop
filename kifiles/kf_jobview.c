/* kf_jobview.c - shows a running operation's progress through the desktop's
 * job server (org.kde.JobViewServerV2: xispanel's jobviewd.c, plasmashell)
 * instead of kifiles' own dialog, the way Dolphin does. kf_ops.c falls back
 * to its dialog when there's no server, or when it goes away mid-job. */
#include "kf.h"
#include "../shared/xis_i18n.h"
#include <string.h>

#define SERVER_NAME "org.kde.JobViewServer"
#define SERVER_PATH "/JobViewServer"
#define SERVER_IFACE "org.kde.JobViewServerV2"
#define VIEW_IFACE "org.kde.JobViewV3"
#define UPDATE_MIN_US (250 * 1000)      /* updates are throttled to this */
#define SPEED_WINDOW_US (1000 * 1000)   /* speed measured over about this */

#define CAP_KILLABLE 0x1
#define CAP_SUSPENDABLE 0x2

struct KfJobView {
    int refs;                           /* the owner + each update in flight */
    GDBusConnection *bus;
    char *path;
    guint sub;
    KfJobViewSignal on_signal;
    gpointer data;
    gboolean lost;
    gint64 last_update;
    gboolean last_suspended;
    int last_percent;
    char *last_info;
    gint64 speed_at;                    /* start of the current speed window */
    guint64 speed_bytes;                /* bytes done at speed_at */
    guint64 speed;
};

static void on_view_signal(GDBusConnection *c, const char *sender, const char *path, const char *iface,
                           const char *member, GVariant *params, gpointer data)
{
    (void)c; (void)sender; (void)path; (void)iface; (void)params;
    KfJobView *v = data;
    if (!v->on_signal)
        return;
    if (!strcmp(member, "cancelRequested"))
        v->on_signal(KF_JOBVIEW_CANCEL, v->data);
    else if (!strcmp(member, "suspendRequested"))
        v->on_signal(KF_JOBVIEW_SUSPEND, v->data);
    else if (!strcmp(member, "resumeRequested"))
        v->on_signal(KF_JOBVIEW_RESUME, v->data);
}

KfJobView *kf_jobview_new(const char *title, KfJobViewSignal on_signal, gpointer data)
{
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    if (!bus)
        return NULL;
    GVariantBuilder hints;
    g_variant_builder_init(&hints, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&hints, "{sv}", "application-display-name", g_variant_new_string("kifiles"));
    g_variant_builder_add(&hints, "{sv}", "application-icon-name", g_variant_new_string("system-file-manager"));
    /* No auto-start: kuiserver's .service file would otherwise pop up a
     * second, unrelated progress window. */
    GVariant *ret = g_dbus_connection_call_sync(bus, SERVER_NAME, SERVER_PATH, SERVER_IFACE, "requestView",
                                                g_variant_new("(sia{sv})", "kifiles", CAP_KILLABLE | CAP_SUSPENDABLE,
                                                              &hints),
                                                G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NO_AUTO_START, 1000, NULL,
                                                NULL);
    if (!ret) {
        g_object_unref(bus);
        return NULL;
    }
    KfJobView *v = g_new0(KfJobView, 1);
    v->refs = 1;
    v->bus = bus;
    g_variant_get(ret, "(o)", &v->path);
    g_variant_unref(ret);
    v->on_signal = on_signal;
    v->data = data;
    v->last_percent = -1;
    v->speed_at = g_get_monotonic_time();
    v->sub = g_dbus_connection_signal_subscribe(bus, NULL, VIEW_IFACE, NULL, v->path, NULL,
                                                G_DBUS_SIGNAL_FLAGS_NONE, on_view_signal, v, NULL);
    GVariantBuilder props;
    g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&props, "{sv}", "title", g_variant_new_string(title));
    g_dbus_connection_call(bus, SERVER_NAME, v->path, VIEW_IFACE, "update", g_variant_new("(a{sv})", &props), NULL,
                           G_DBUS_CALL_FLAGS_NO_AUTO_START, -1, NULL, NULL, NULL);
    return v;
}

static void unref(KfJobView *v)
{
    if (--v->refs > 0)
        return;
    g_dbus_connection_signal_unsubscribe(v->bus, v->sub);
    g_free(v->path);
    g_free(v->last_info);
    g_object_unref(v->bus);
    g_free(v);
}

/* An update the server didn't take means it went away (xispanel
 * restarted, say): kf_ops.c then brings its own dialog up. Each update
 * holds a reference, so a reply arriving after the job ended is harmless. */
static void on_update_reply(GObject *src, GAsyncResult *res, gpointer data)
{
    KfJobView *v = data;
    GVariant *ret = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, NULL);
    if (ret)
        g_variant_unref(ret);
    else
        v->lost = TRUE;
    unref(v);
}

void kf_jobview_update(KfJobView *v, const char *info, int percent, guint64 done, guint64 total, gboolean bytes,
                       gboolean suspended, const char *source, const char *dest)
{
    gint64 now = g_get_monotonic_time();
    if (bytes && now - v->speed_at >= SPEED_WINDOW_US) {
        v->speed = done > v->speed_bytes ? (done - v->speed_bytes) * G_USEC_PER_SEC / (guint64)(now - v->speed_at) : 0;
        v->speed_at = now;
        v->speed_bytes = done;
    }
    gboolean changed = percent != v->last_percent || suspended != v->last_suspended ||
                       g_strcmp0(info, v->last_info) != 0;
    /* A pause shows at once; everything else waits for the throttle. */
    if (!changed || (suspended == v->last_suspended && now - v->last_update < UPDATE_MIN_US))
        return;
    v->last_update = now;
    v->last_percent = percent;
    v->last_suspended = suspended;
    g_free(v->last_info);
    v->last_info = g_strdup(info);

    GVariantBuilder props;
    g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&props, "{sv}", "infoMessage", g_variant_new_string(info ? info : ""));
    if (percent >= 0)
        g_variant_builder_add(&props, "{sv}", "percent", g_variant_new_uint32((guint32)percent));
    if (bytes) {
        g_variant_builder_add(&props, "{sv}", "processedBytes", g_variant_new_uint64(done));
        g_variant_builder_add(&props, "{sv}", "totalBytes", g_variant_new_uint64(total));
        g_variant_builder_add(&props, "{sv}", "speed", g_variant_new_uint64(suspended ? 0 : v->speed));
    }
    g_variant_builder_add(&props, "{sv}", "suspended", g_variant_new_boolean(suspended));
    if (source) {
        g_variant_builder_add(&props, "{sv}", "descriptionLabel1", g_variant_new_string(_("Source")));
        g_variant_builder_add(&props, "{sv}", "descriptionValue1", g_variant_new_string(source));
    }
    if (dest) {
        g_variant_builder_add(&props, "{sv}", "descriptionLabel2", g_variant_new_string(_("Destination")));
        g_variant_builder_add(&props, "{sv}", "descriptionValue2", g_variant_new_string(dest));
    }
    g_dbus_connection_call(v->bus, SERVER_NAME, v->path, VIEW_IFACE, "update", g_variant_new("(a{sv})", &props),
                           NULL, G_DBUS_CALL_FLAGS_NO_AUTO_START, 2000, NULL, on_update_reply, v);
    v->refs++;
}

gboolean kf_jobview_lost(KfJobView *v)
{
    return v->lost;
}

void kf_jobview_finish(KfJobView *v, guint error, const char *message)
{
    if (!v->lost) {
        /* Synchronous: the process may be about to exit (window already
         * closed), and an unsent terminate would read as a crash. */
        GVariant *ret = g_dbus_connection_call_sync(
            v->bus, SERVER_NAME, v->path, VIEW_IFACE, "terminate",
            g_variant_new("(us@a{sv})", error, message ? message : "", g_variant_new_array(G_VARIANT_TYPE("{sv}"), NULL, 0)),
            NULL, G_DBUS_CALL_FLAGS_NO_AUTO_START, 1000, NULL, NULL);
        if (ret)
            g_variant_unref(ret);
    }
    kf_jobview_free(v);
}

void kf_jobview_free(KfJobView *v)
{
    v->on_signal = NULL;                /* the job is gone: no more callbacks into it */
    unref(v);
}
