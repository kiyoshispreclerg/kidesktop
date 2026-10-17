/*
 * jobviewd.c - org.kde.JobViewServerV2 server: the progress of long
 * operations (kifiles' and KIO's copies/moves, say) that the app hands to
 * the desktop instead of drawing its own dialog. Same dlopen'd-libdbus-1,
 * private-connection, poll-and-drain design as notifd.c (see its header
 * comment for why each of those), and the same single-owner rule: if
 * something already owns org.kde.JobViewServer (plasmashell, kuiserver),
 * this stays dormant and that owner keeps showing the jobs.
 *
 * Protocol, as plasma-workspace's libnotificationmanager implements it:
 * requestView(desktopEntry, capabilities, hints) on /JobViewServer returns
 * one object path per job; the app then calls update(a{sv}) on it as the
 * job advances and terminate(errorCode, errorMessage, hints) once at the
 * end, and listens there for cancelRequested/suspendRequested/
 * resumeRequested. Only the V2 server interface: KF5's KIO already uses
 * KUiServerV2JobTracker, and the V1 one (per-field setters) has no other
 * client here.
 *
 * Jobs are live state, kept apart from notifd.c's history ring: what a
 * finished job leaves behind (if anything) is a separate notification.
 */
#include "xispanel.h"

#include <dbus/dbus.h>

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

#define JOBVIEWD_POLL_MS 500
#define JOBVIEWD_NAME "org.kde.JobViewServer"
#define JOBVIEWD_PATH "/JobViewServer"
#define JOBVIEWD_IFACE "org.kde.JobViewServerV2"
#define JOBVIEW_IFACE "org.kde.JobViewV3"
#define JOBVIEW_PATH_FMT "/xispanel/jobs/JobView_%u"
/* Same grace plasmashell gives an app to answer a cancel before the job
 * is dropped anyway, so an unresponsive app can't leave it stuck. */
#define JOBVIEWD_CANCEL_GRACE_MS 2000

typedef struct {
    JobEntry e;
    int in_use;
    char path[48];
    char sender[64];        /* unique bus name of the app that asked for it */
    uint64_t cancel_deadline; /* 0 = no cancel pending */
} Job;

static Job g_jobs[JOBVIEWD_MAX];
static unsigned int g_next_id = 1;

static void *g_libdbus = NULL;
static int g_load_attempted = 0;
static DBusConnection *g_conn = NULL;
static int g_is_owner = 0;
static uint64_t g_next_poll_ms = 0;

/* ---- dlopen'd libdbus-1 symbols ---------------------------------- */
static void (*p_dbus_error_init)(DBusError *);
static void (*p_dbus_error_free)(DBusError *);
static dbus_bool_t (*p_dbus_error_is_set)(const DBusError *);
static DBusConnection *(*p_dbus_bus_get_private)(DBusBusType, DBusError *);
static int (*p_dbus_bus_request_name)(DBusConnection *, const char *, unsigned int, DBusError *);
static dbus_bool_t (*p_dbus_bus_name_has_owner)(DBusConnection *, const char *, DBusError *);
static void (*p_dbus_bus_add_match)(DBusConnection *, const char *, DBusError *);
static DBusMessage *(*p_dbus_message_new_method_return)(DBusMessage *);
static DBusMessage *(*p_dbus_message_new_error)(DBusMessage *, const char *, const char *);
static DBusMessage *(*p_dbus_message_new_signal)(const char *, const char *, const char *);
static void (*p_dbus_connection_flush)(DBusConnection *);
static void (*p_dbus_message_unref)(DBusMessage *);
static dbus_bool_t (*p_dbus_connection_send)(DBusConnection *, DBusMessage *, dbus_uint32_t *);
static dbus_bool_t (*p_dbus_connection_read_write)(DBusConnection *, int);
static dbus_bool_t (*p_dbus_connection_get_unix_fd)(DBusConnection *, int *);
static DBusMessage *(*p_dbus_connection_pop_message)(DBusConnection *);
static dbus_bool_t (*p_dbus_message_iter_init)(DBusMessage *, DBusMessageIter *);
static dbus_bool_t (*p_dbus_message_iter_next)(DBusMessageIter *);
static void (*p_dbus_message_iter_recurse)(DBusMessageIter *, DBusMessageIter *);
static int (*p_dbus_message_iter_get_arg_type)(DBusMessageIter *);
static void (*p_dbus_message_iter_get_basic)(DBusMessageIter *, void *);
static void (*p_dbus_message_iter_init_append)(DBusMessage *, DBusMessageIter *);
static dbus_bool_t (*p_dbus_message_iter_append_basic)(DBusMessageIter *, int, const void *);
static int (*p_dbus_message_get_type)(DBusMessage *);
static const char *(*p_dbus_message_get_interface)(DBusMessage *);
static const char *(*p_dbus_message_get_member)(DBusMessage *);
static const char *(*p_dbus_message_get_path)(DBusMessage *);
static const char *(*p_dbus_message_get_sender)(DBusMessage *);
static dbus_bool_t (*p_dbus_message_get_no_reply)(DBusMessage *);

#define LOAD_SYM(name)                                                                                               \
    do {                                                                                                             \
        *(void **)(&p_##name) = dlsym(g_libdbus, #name);                                                            \
        if (!p_##name) {                                                                                             \
            fprintf(stderr, "xispanel: jobviewd: symbol '%s' missing from libdbus-1, disabling job progress\n",     \
                    #name);                                                                                          \
            return 0;                                                                                                \
        }                                                                                                            \
    } while (0)

static int load_symbols(void)
{
    LOAD_SYM(dbus_error_init);
    LOAD_SYM(dbus_error_free);
    LOAD_SYM(dbus_error_is_set);
    LOAD_SYM(dbus_bus_get_private);
    LOAD_SYM(dbus_bus_request_name);
    LOAD_SYM(dbus_bus_name_has_owner);
    LOAD_SYM(dbus_bus_add_match);
    LOAD_SYM(dbus_message_new_method_return);
    LOAD_SYM(dbus_message_new_error);
    LOAD_SYM(dbus_message_new_signal);
    LOAD_SYM(dbus_connection_flush);
    LOAD_SYM(dbus_message_unref);
    LOAD_SYM(dbus_connection_send);
    LOAD_SYM(dbus_connection_read_write);
    LOAD_SYM(dbus_connection_get_unix_fd);
    LOAD_SYM(dbus_connection_pop_message);
    LOAD_SYM(dbus_message_iter_init);
    LOAD_SYM(dbus_message_iter_next);
    LOAD_SYM(dbus_message_iter_recurse);
    LOAD_SYM(dbus_message_iter_get_arg_type);
    LOAD_SYM(dbus_message_iter_get_basic);
    LOAD_SYM(dbus_message_iter_init_append);
    LOAD_SYM(dbus_message_iter_append_basic);
    LOAD_SYM(dbus_message_get_type);
    LOAD_SYM(dbus_message_get_interface);
    LOAD_SYM(dbus_message_get_member);
    LOAD_SYM(dbus_message_get_path);
    LOAD_SYM(dbus_message_get_sender);
    LOAD_SYM(dbus_message_get_no_reply);
    return 1;
}

static int ensure_connected(void)
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
    if (!load_symbols()) {
        dlclose(g_libdbus);
        g_libdbus = NULL;
        return 0;
    }

    /* Private for the same reason as notifd.c's: a shared connection's
     * queue would be drained by whichever module polls it first. */
    DBusError err;
    p_dbus_error_init(&err);
    g_conn = p_dbus_bus_get_private(DBUS_BUS_SESSION, &err);
    if (p_dbus_error_is_set(&err)) {
        fprintf(stderr, "xispanel: jobviewd: could not connect to session bus (%s)\n", err.message);
        p_dbus_error_free(&err);
    }
    if (!g_conn) {
        return 0;
    }

    p_dbus_error_init(&err);
    dbus_bool_t owned = p_dbus_bus_name_has_owner(g_conn, JOBVIEWD_NAME, &err);
    p_dbus_error_free(&err);
    if (owned) {
        fprintf(stderr, "xispanel: jobviewd: %s is already owned by another process -- its owner shows job "
                        "progress instead\n",
                JOBVIEWD_NAME);
        return 1;
    }
    p_dbus_error_init(&err);
    int ret = p_dbus_bus_request_name(g_conn, JOBVIEWD_NAME, 4 /* DO_NOT_QUEUE */, &err);
    p_dbus_error_free(&err);
    g_is_owner = (ret == 1);
    if (!g_is_owner) {
        fprintf(stderr, "xispanel: jobviewd: could not claim %s, job progress disabled\n", JOBVIEWD_NAME);
        return 1;
    }
    /* An app that crashes or quits mid-job never calls terminate(). */
    p_dbus_error_init(&err);
    p_dbus_bus_add_match(g_conn,
                         "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',"
                         "member='NameOwnerChanged'",
                         &err);
    p_dbus_error_free(&err);
    fprintf(stderr, "xispanel: jobviewd: registered as %s\n", JOBVIEWD_NAME);
    return 1;
}

/* ---- jobs ------------------------------------------------------------- */

static Job *job_by_id(unsigned int id)
{
    for (int i = 0; id && i < JOBVIEWD_MAX; i++) {
        if (g_jobs[i].in_use && g_jobs[i].e.id == id) {
            return &g_jobs[i];
        }
    }
    return NULL;
}

static Job *job_by_path(const char *path)
{
    for (int i = 0; path && i < JOBVIEWD_MAX; i++) {
        if (g_jobs[i].in_use && !strcmp(g_jobs[i].path, path)) {
            return &g_jobs[i];
        }
    }
    return NULL;
}

static void job_drop(Job *j)
{
    memset(j, 0, sizeof(*j));
}

/* KJob::KilledJobError: the user cancelled it, nothing to report. */
#define JOB_ERR_CANCELLED 1

/* What a finished job leaves in the history. */
static void job_finished(Job *j, unsigned int error, const char *message)
{
    if (error == JOB_ERR_CANCELLED) {
        return;
    }
    const char *summary = j->e.title[0] ? j->e.title : j->e.app_name;
    const char *body = !error ? _("Concluído") : message && message[0] ? message : _("Falhou");
    notifd_post(j->e.app_name, j->e.app_icon, summary, body);
}

static void emit(Job *j, const char *signal)
{
    DBusMessage *sig = p_dbus_message_new_signal(j->path, JOBVIEW_IFACE, signal);
    if (!sig) {
        return;
    }
    p_dbus_connection_send(g_conn, sig, NULL);
    p_dbus_message_unref(sig);
    p_dbus_connection_flush(g_conn);
}

/* ---- a{sv} decoding --------------------------------------------------- */

static int variant_u64(DBusMessageIter *var, uint64_t *out)
{
    DBusMessageIter v;
    p_dbus_message_iter_recurse(var, &v);
    DBusBasicValue b;
    switch (p_dbus_message_iter_get_arg_type(&v)) {
    case DBUS_TYPE_BYTE:
        p_dbus_message_iter_get_basic(&v, &b);
        *out = b.byt;
        return 1;
    case DBUS_TYPE_BOOLEAN:
        p_dbus_message_iter_get_basic(&v, &b);
        *out = b.bool_val ? 1 : 0;
        return 1;
    case DBUS_TYPE_INT16:
    case DBUS_TYPE_UINT16:
        p_dbus_message_iter_get_basic(&v, &b);
        *out = b.u16;
        return 1;
    case DBUS_TYPE_INT32:
        p_dbus_message_iter_get_basic(&v, &b);
        *out = b.i32 < 0 ? 0 : (uint64_t)b.i32;
        return 1;
    case DBUS_TYPE_UINT32:
        p_dbus_message_iter_get_basic(&v, &b);
        *out = b.u32;
        return 1;
    case DBUS_TYPE_INT64:
        p_dbus_message_iter_get_basic(&v, &b);
        *out = b.i64 < 0 ? 0 : (uint64_t)b.i64;
        return 1;
    case DBUS_TYPE_UINT64:
        p_dbus_message_iter_get_basic(&v, &b);
        *out = b.u64;
        return 1;
    case DBUS_TYPE_DOUBLE:
        p_dbus_message_iter_get_basic(&v, &b);
        *out = b.dbl < 0 ? 0 : (uint64_t)b.dbl;
        return 1;
    default:
        return 0;
    }
}

static void variant_str(DBusMessageIter *var, char *dst, size_t dst_sz)
{
    DBusMessageIter v;
    p_dbus_message_iter_recurse(var, &v);
    int t = p_dbus_message_iter_get_arg_type(&v);
    if (t == DBUS_TYPE_STRING || t == DBUS_TYPE_OBJECT_PATH) {
        const char *s = NULL;
        p_dbus_message_iter_get_basic(&v, &s);
        snprintf(dst, dst_sz, "%s", s ? s : "");
    }
}

/* Calls fn(key, variant-iter) for each entry of the a{sv} at `it`. */
static void for_each_property(DBusMessageIter *it, void (*fn)(void *, const char *, DBusMessageIter *), void *data)
{
    if (p_dbus_message_iter_get_arg_type(it) != DBUS_TYPE_ARRAY) {
        return;
    }
    DBusMessageIter arr;
    p_dbus_message_iter_recurse(it, &arr);
    while (p_dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry;
        p_dbus_message_iter_recurse(&arr, &entry);
        const char *key = NULL;
        if (p_dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_STRING) {
            p_dbus_message_iter_get_basic(&entry, &key);
            if (p_dbus_message_iter_next(&entry) && p_dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_VARIANT) {
                fn(data, key, &entry);
            }
        }
        if (!p_dbus_message_iter_next(&arr)) {
            break;
        }
    }
}

static void apply_hint(void *data, const char *key, DBusMessageIter *var)
{
    JobEntry *e = data;
    if (!strcmp(key, "application-display-name")) {
        variant_str(var, e->app_name, sizeof(e->app_name));
    } else if (!strcmp(key, "application-icon-name")) {
        variant_str(var, e->app_icon, sizeof(e->app_icon));
    }
}

static void apply_property(void *data, const char *key, DBusMessageIter *var)
{
    JobEntry *e = data;
    uint64_t n;
    if (!strcmp(key, "title")) {
        variant_str(var, e->title, sizeof(e->title));
    } else if (!strcmp(key, "infoMessage")) {
        variant_str(var, e->info, sizeof(e->info));
    } else if (!strcmp(key, "descriptionLabel1")) {
        variant_str(var, e->label1, sizeof(e->label1));
    } else if (!strcmp(key, "descriptionValue1")) {
        variant_str(var, e->value1, sizeof(e->value1));
    } else if (!strcmp(key, "descriptionLabel2")) {
        variant_str(var, e->label2, sizeof(e->label2));
    } else if (!strcmp(key, "descriptionValue2")) {
        variant_str(var, e->value2, sizeof(e->value2));
    } else if (!variant_u64(var, &n)) {
        return;
    } else if (!strcmp(key, "percent")) {
        e->percent = n > 100 ? 100 : (int)n;
    } else if (!strcmp(key, "speed")) {
        e->speed = n;
    } else if (!strcmp(key, "processedBytes")) {
        e->processed_bytes = n;
    } else if (!strcmp(key, "totalBytes")) {
        e->total_bytes = n;
    } else if (!strcmp(key, "processedFiles")) {
        e->processed_files = n;
    } else if (!strcmp(key, "totalFiles")) {
        e->total_files = n;
    } else if (!strcmp(key, "suspended")) {
        e->suspended = n != 0;
    }
}

/* ---- method calls ----------------------------------------------------- */

static DBusMessage *handle_request_view(DBusMessage *msg)
{
    Job *j = NULL;
    for (int i = 0; i < JOBVIEWD_MAX; i++) {
        if (!g_jobs[i].in_use) {
            j = &g_jobs[i];
            break;
        }
    }
    if (!j) {
        return p_dbus_message_new_error(msg, "org.freedesktop.DBus.Error.LimitsExceeded", "too many jobs");
    }
    memset(j, 0, sizeof(*j));
    j->in_use = 1;
    j->e.id = g_next_id++;
    j->e.percent = -1;
    j->e.created_ms = now_ms();
    j->e.serial = 1;
    snprintf(j->path, sizeof(j->path), JOBVIEW_PATH_FMT, j->e.id);
    const char *sender = p_dbus_message_get_sender(msg);
    snprintf(j->sender, sizeof(j->sender), "%s", sender ? sender : "");

    DBusMessageIter it;
    if (p_dbus_message_iter_init(msg, &it)) {
        if (p_dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_STRING) {
            const char *s = NULL;
            p_dbus_message_iter_get_basic(&it, &s);
            snprintf(j->e.desktop_entry, sizeof(j->e.desktop_entry), "%s", s ? s : "");
        }
        if (p_dbus_message_iter_next(&it) && p_dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_INT32) {
            dbus_int32_t caps = 0;
            p_dbus_message_iter_get_basic(&it, &caps);
            j->e.killable = (caps & 0x1) != 0;
            j->e.suspendable = (caps & 0x2) != 0;
        }
        if (p_dbus_message_iter_next(&it)) {
            for_each_property(&it, apply_hint, &j->e);
        }
    }
    if (!j->e.app_name[0]) {
        snprintf(j->e.app_name, sizeof(j->e.app_name), "%s", j->e.desktop_entry);
    }

    DBusMessage *reply = p_dbus_message_new_method_return(msg);
    if (reply) {
        DBusMessageIter rit;
        const char *path = j->path;
        p_dbus_message_iter_init_append(reply, &rit);
        p_dbus_message_iter_append_basic(&rit, DBUS_TYPE_OBJECT_PATH, &path);
    }
    return reply;
}

static void handle_update(Job *j, DBusMessage *msg)
{
    DBusMessageIter it;
    if (p_dbus_message_iter_init(msg, &it)) {
        for_each_property(&it, apply_property, &j->e);
    }
    j->e.serial++;
}

static const char *const g_server_xml =
    "<node><interface name=\"" JOBVIEWD_IFACE "\"><method name=\"requestView\">"
    "<arg name=\"desktopEntry\" type=\"s\" direction=\"in\"/><arg name=\"capabilities\" type=\"i\" direction=\"in\"/>"
    "<arg name=\"hints\" type=\"a{sv}\" direction=\"in\"/><arg name=\"trackerPath\" type=\"o\" direction=\"out\"/>"
    "</method></interface></node>";
static const char *const g_job_xml =
    "<node><interface name=\"" JOBVIEW_IFACE "\"><method name=\"terminate\">"
    "<arg name=\"errorCode\" type=\"u\" direction=\"in\"/><arg name=\"errorMessage\" type=\"s\" direction=\"in\"/>"
    "<arg name=\"hints\" type=\"a{sv}\" direction=\"in\"/></method><method name=\"update\">"
    "<arg name=\"properties\" type=\"a{sv}\" direction=\"in\"/></method><signal name=\"updateRequested\"/>"
    "<signal name=\"suspendRequested\"/><signal name=\"resumeRequested\"/><signal name=\"cancelRequested\"/>"
    "</interface></node>";

static int is(const char *a, const char *b)
{
    return a && !strcmp(a, b);
}

static void handle_method_call(DBusMessage *msg)
{
    const char *iface = p_dbus_message_get_interface(msg);
    const char *member = p_dbus_message_get_member(msg);
    const char *path = p_dbus_message_get_path(msg);
    DBusMessage *reply = NULL;
    Job *j;

    if (is(path, JOBVIEWD_PATH) && (!iface || is(iface, JOBVIEWD_IFACE)) && is(member, "requestView")) {
        reply = handle_request_view(msg);
    } else if ((j = job_by_path(path)) && (!iface || is(iface, JOBVIEW_IFACE)) && is(member, "update")) {
        handle_update(j, msg);
        if (!p_dbus_message_get_no_reply(msg)) {
            reply = p_dbus_message_new_method_return(msg);
        }
    } else if ((j = job_by_path(path)) && (!iface || is(iface, JOBVIEW_IFACE)) && is(member, "terminate")) {
        DBusMessageIter it;
        dbus_uint32_t error = 0;
        const char *message = NULL;
        if (p_dbus_message_iter_init(msg, &it) && p_dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_UINT32) {
            p_dbus_message_iter_get_basic(&it, &error);
            if (p_dbus_message_iter_next(&it) && p_dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_STRING) {
                p_dbus_message_iter_get_basic(&it, &message);
            }
        }
        job_finished(j, error, message);
        job_drop(j);
        if (!p_dbus_message_get_no_reply(msg)) {
            reply = p_dbus_message_new_method_return(msg);
        }
    } else if (is(iface, "org.freedesktop.DBus.Introspectable") && is(member, "Introspect")) {
        const char *xml = is(path, JOBVIEWD_PATH) ? g_server_xml : job_by_path(path) ? g_job_xml : "<node/>";
        reply = p_dbus_message_new_method_return(msg);
        if (reply) {
            DBusMessageIter rit;
            p_dbus_message_iter_init_append(reply, &rit);
            p_dbus_message_iter_append_basic(&rit, DBUS_TYPE_STRING, &xml);
        }
    } else if (!p_dbus_message_get_no_reply(msg)) {
        reply = p_dbus_message_new_error(msg, "org.freedesktop.DBus.Error.UnknownMethod", "not implemented");
    }
    if (reply) {
        p_dbus_connection_send(g_conn, reply, NULL);
        p_dbus_message_unref(reply);
    }
}

/* NameOwnerChanged(name, old, new) with an empty `new`: that app is gone. */
static void handle_name_owner_changed(DBusMessage *msg)
{
    DBusMessageIter it;
    const char *name = NULL, *new_owner = NULL;
    if (!p_dbus_message_iter_init(msg, &it) || p_dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_STRING) {
        return;
    }
    p_dbus_message_iter_get_basic(&it, &name);
    if (!p_dbus_message_iter_next(&it) || !p_dbus_message_iter_next(&it) ||
        p_dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_STRING) {
        return;
    }
    p_dbus_message_iter_get_basic(&it, &new_owner);
    if (!name || !new_owner || new_owner[0]) {
        return;
    }
    for (int i = 0; i < JOBVIEWD_MAX; i++) {
        if (g_jobs[i].in_use && !strcmp(g_jobs[i].sender, name)) {
            job_finished(&g_jobs[i], 2, _("O aplicativo fechou inesperadamente"));
            job_drop(&g_jobs[i]);
        }
    }
}

static void handle_incoming(void)
{
    p_dbus_connection_read_write(g_conn, 0);
    DBusMessage *msg;
    while ((msg = p_dbus_connection_pop_message(g_conn)) != NULL) {
        int type = p_dbus_message_get_type(msg);
        if (type == DBUS_MESSAGE_TYPE_METHOD_CALL) {
            handle_method_call(msg);
        } else if (type == DBUS_MESSAGE_TYPE_SIGNAL && is(p_dbus_message_get_interface(msg), "org.freedesktop.DBus") &&
                   is(p_dbus_message_get_member(msg), "NameOwnerChanged")) {
            handle_name_owner_changed(msg);
        }
        p_dbus_message_unref(msg);
    }
    p_dbus_connection_flush(g_conn);
}

/* ---- public API ------------------------------------------------------- */

int jobviewd_fd(void)
{
    int fd = -1;
    if (!g_conn || !g_is_owner || !p_dbus_connection_get_unix_fd(g_conn, &fd)) {
        return -1;
    }
    return fd;
}

void jobviewd_wake(void)
{
    g_next_poll_ms = 0;
}

void jobviewd_poll(uint64_t now)
{
    if (now < g_next_poll_ms) {
        return;
    }
    g_next_poll_ms = now + JOBVIEWD_POLL_MS;
    if (!ensure_connected() || !g_is_owner) {
        return;
    }
    handle_incoming();
    for (int i = 0; i < JOBVIEWD_MAX; i++) {
        if (g_jobs[i].in_use && g_jobs[i].cancel_deadline && now >= g_jobs[i].cancel_deadline) {
            fprintf(stderr, "xispanel: jobviewd: %s did not answer a cancel in time, dropping its job\n",
                    g_jobs[i].e.app_name);
            job_drop(&g_jobs[i]);
        }
    }
}

int jobviewd_count(void)
{
    int n = 0;
    for (int i = 0; i < JOBVIEWD_MAX; i++) {
        n += g_jobs[i].in_use;
    }
    return n;
}

const JobEntry *jobviewd_get(int idx)
{
    for (int i = 0; i < JOBVIEWD_MAX; i++) {
        if (g_jobs[i].in_use && idx-- == 0) {
            return &g_jobs[i].e;
        }
    }
    return NULL;
}

int jobviewd_action(unsigned int id, const char *action)
{
    Job *j = job_by_id(id);
    if (!j || !g_conn) {
        return 0;
    }
    if (!strcmp(action, "cancel") && j->e.killable) {
        emit(j, "cancelRequested");
        if (!j->cancel_deadline) {
            j->cancel_deadline = now_ms() + JOBVIEWD_CANCEL_GRACE_MS;
        }
        return 1;
    }
    if (!strcmp(action, "suspend") && j->e.suspendable) {
        emit(j, "suspendRequested");
        return 1;
    }
    if (!strcmp(action, "resume") && j->e.suspendable) {
        emit(j, "resumeRequested");
        return 1;
    }
    return 0;
}
