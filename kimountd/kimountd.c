/*
 * kimountd - automounts the disks the user asked for, through udisks2.
 *
 * Watches udisks2 on the system bus and keeps ~/.config/ki-mount.conf: one
 * line per filesystem it has seen that isn't part of the system (removable
 * media always; internal disks unless fstab or the system owns them), with
 * an `auto` flag kiconf's Discos tab sets. A filesystem marked auto is
 * mounted when it appears -- plugged in, or already there when the session
 * starts -- and right away when it gets marked while connected.
 *
 * Mounting goes through udisks2's own Filesystem.Mount, the same call
 * KDE, GNOME, XFCE and GIO make, so the mount point is wherever udisks puts
 * it on this distribution (/media/$USER/LABEL on Debian/Ubuntu/Devuan,
 * /run/media/$USER/LABEL on Fedora/Arch), and polkit decides whether a
 * password is needed (internal disks need one unless kiconf installed the
 * rule that waives it).
 *
 * Filesystems are keyed by their UUID, not by device name or udisks object
 * path: /dev/sdb is a different disk depending on what else is plugged in.
 *
 * ki-mount.conf, tab separated, rewritten whole:
 *   DEVICE <uuid> <auto 0|1> <removable 0|1> <last_seen> <fstype> <label> <description>
 * kiconf rewrites it to change `auto` and sends SIGHUP; kimountd re-reads
 * it before each write of its own, so neither loses the other's change.
 */
#include "../shared/xis_safefile.h"

#include <dbus/dbus.h>

#include <errno.h>
#include <stdarg.h>
#include <strings.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define KIMOUNTD_VERSION "0.1.0"

#define UD_NAME "org.freedesktop.UDisks2"
#define UD_ROOT "/org/freedesktop/UDisks2"
#define UD_BLOCK "org.freedesktop.UDisks2.Block"
#define UD_FS "org.freedesktop.UDisks2.Filesystem"
#define UD_PART "org.freedesktop.UDisks2.Partition"
#define UD_LOOP "org.freedesktop.UDisks2.Loop"
#define UD_DRIVE "org.freedesktop.UDisks2.Drive"
#define OM_IFACE "org.freedesktop.DBus.ObjectManager"
#define PROPS_IFACE "org.freedesktop.DBus.Properties"

#define MAX_KNOWN 256
#define MAX_MOUNTS 4

/* One line of ki-mount.conf. */
typedef struct {
    char uuid[64];
    int automount;
    int removable;
    long last_seen;
    char fstype[24];
    char label[128];
    char desc[160];
} Known;

/* What udisks says about one block device with a filesystem on it. */
typedef struct {
    char path[256];
    char drive[256];
    char uuid[64];
    char label[128];
    char fstype[24];
    char part_type[64];
    int has_fs, is_loop, hint_ignore, configured;
    int n_mounts;
    char mounts[MAX_MOUNTS][256];
} Dev;

static DBusConnection *g_bus;
static Known g_known[MAX_KNOWN];
static int g_n_known;
static char g_conf[PATH_MAX];
static int g_sigpipe[2] = {-1, -1};
static volatile sig_atomic_t g_hup, g_term;

static void log_msg(const char *fmt, const char *a)
{
    fprintf(stderr, "kimountd: ");
    fprintf(stderr, fmt, a ? a : "");
    fputc('\n', stderr);
}

/* ---- ki-mount.conf ---- */

static void conf_path(void)
{
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    if (xdg && xdg[0]) {
        snprintf(g_conf, sizeof(g_conf), "%s/ki-mount.conf", xdg);
    } else {
        snprintf(g_conf, sizeof(g_conf), "%s/.config/ki-mount.conf", home ? home : ".");
    }
}

static Known *known_find(const char *uuid)
{
    for (int i = 0; i < g_n_known; i++) {
        if (!strcmp(g_known[i].uuid, uuid)) {
            return &g_known[i];
        }
    }
    return NULL;
}

/* Splits the next tab-separated field off *p (in place). */
static char *field(char **p)
{
    char *s = *p;
    if (!s) {
        return (char *)"";
    }
    char *t = strchr(s, '\t');
    if (t) {
        *t = 0;
        *p = t + 1;
    } else {
        *p = NULL;
    }
    return s;
}

static void conf_load(void)
{
    g_n_known = 0;
    size_t len = 0;
    char *text = xis_read_file_or_backup(g_conf, xis_check_lines, &len);
    if (!text) {
        return;
    }
    char *save = NULL;
    for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (strncmp(line, "DEVICE\t", 7) != 0 || g_n_known >= MAX_KNOWN) {
            continue;
        }
        char *p = line + 7;
        Known *k = &g_known[g_n_known];
        memset(k, 0, sizeof(*k));
        snprintf(k->uuid, sizeof(k->uuid), "%s", field(&p));
        k->automount = atoi(field(&p)) != 0;
        k->removable = atoi(field(&p)) != 0;
        k->last_seen = atol(field(&p));
        snprintf(k->fstype, sizeof(k->fstype), "%s", field(&p));
        snprintf(k->label, sizeof(k->label), "%s", field(&p));
        snprintf(k->desc, sizeof(k->desc), "%s", field(&p));
        if (k->uuid[0] && !known_find(k->uuid)) {
            g_n_known++;
        }
    }
    free(text);
}

static void conf_save(void)
{
    char tmp[PATH_MAX + 16];
    xis_write_tmpname(g_conf, tmp, sizeof(tmp));
    FILE *f = xis_write_open(tmp, g_conf);
    if (!f) {
        log_msg("could not write %s", g_conf);
        return;
    }
    fprintf(f, "# ki-mount.conf - filesystems kimountd has seen; `auto` is set by kiconf's Discos tab.\n");
    fprintf(f, "# DEVICE <uuid> <auto> <removable> <last_seen> <fstype> <label> <description>  (tab separated)\n");
    for (int i = 0; i < g_n_known; i++) {
        Known *k = &g_known[i];
        fprintf(f, "DEVICE\t%s\t%d\t%d\t%ld\t%s\t%s\t%s\n", k->uuid, k->automount, k->removable, k->last_seen,
                k->fstype, k->label, k->desc);
    }
    xis_write_commit(f, tmp, g_conf, xis_check_lines);
}

/* ---- D-Bus property parsing ---- */

static void iter_string(DBusMessageIter *v, char *out, size_t outsz)
{
    int t = dbus_message_iter_get_arg_type(v);
    if (t == DBUS_TYPE_STRING || t == DBUS_TYPE_OBJECT_PATH) {
        const char *s;
        dbus_message_iter_get_basic(v, &s);
        snprintf(out, outsz, "%s", s);
    }
}

static int iter_bool(DBusMessageIter *v)
{
    if (dbus_message_iter_get_arg_type(v) != DBUS_TYPE_BOOLEAN) {
        return 0;
    }
    dbus_bool_t b;
    dbus_message_iter_get_basic(v, &b);
    return b != 0;
}

static unsigned long long iter_u64(DBusMessageIter *v)
{
    if (dbus_message_iter_get_arg_type(v) != DBUS_TYPE_UINT64) {
        return 0;
    }
    dbus_uint64_t n;
    dbus_message_iter_get_basic(v, &n);
    return n;
}

/* An `ay` holding a NUL-terminated path (MountPoints entries). */
static void iter_bytestring(DBusMessageIter *ay, char *out, size_t outsz)
{
    out[0] = 0;
    if (dbus_message_iter_get_arg_type(ay) != DBUS_TYPE_ARRAY) {
        return;
    }
    DBusMessageIter b;
    dbus_message_iter_recurse(ay, &b);
    const char *bytes = NULL;
    int n = 0;
    dbus_message_iter_get_fixed_array(&b, &bytes, &n);
    if (n > 0 && bytes) {
        size_t m = (size_t)n < outsz ? (size_t)n : outsz - 1;
        memcpy(out, bytes, m);
        out[m] = 0;
    }
}

/* Reads one interface's a{sv} into dev (Block/Filesystem/Partition). */
static void parse_dev_props(Dev *d, const char *iface, DBusMessageIter *dict)
{
    DBusMessageIter e;
    dbus_message_iter_recurse(dict, &e);
    for (; dbus_message_iter_get_arg_type(&e) == DBUS_TYPE_DICT_ENTRY; dbus_message_iter_next(&e)) {
        DBusMessageIter kv, v;
        dbus_message_iter_recurse(&e, &kv);
        const char *key;
        dbus_message_iter_get_basic(&kv, &key);
        dbus_message_iter_next(&kv);
        dbus_message_iter_recurse(&kv, &v);
        if (!strcmp(iface, UD_BLOCK)) {
            if (!strcmp(key, "IdUUID")) iter_string(&v, d->uuid, sizeof(d->uuid));
            else if (!strcmp(key, "IdLabel")) iter_string(&v, d->label, sizeof(d->label));
            else if (!strcmp(key, "IdType")) iter_string(&v, d->fstype, sizeof(d->fstype));
            else if (!strcmp(key, "Drive")) iter_string(&v, d->drive, sizeof(d->drive));
            else if (!strcmp(key, "HintIgnore")) d->hint_ignore = iter_bool(&v);
            else if (!strcmp(key, "Configuration")) {
                /* a(sa{sv}): non-empty means fstab/crypttab owns it */
                DBusMessageIter a;
                dbus_message_iter_recurse(&v, &a);
                d->configured = dbus_message_iter_get_arg_type(&a) == DBUS_TYPE_STRUCT;
            }
        } else if (!strcmp(iface, UD_FS)) {
            if (!strcmp(key, "MountPoints")) {
                DBusMessageIter a;
                dbus_message_iter_recurse(&v, &a);
                d->n_mounts = 0;
                for (; dbus_message_iter_get_arg_type(&a) == DBUS_TYPE_ARRAY && d->n_mounts < MAX_MOUNTS;
                     dbus_message_iter_next(&a)) {
                    iter_bytestring(&a, d->mounts[d->n_mounts], sizeof(d->mounts[0]));
                    d->n_mounts++;
                }
            }
        } else if (!strcmp(iface, UD_PART)) {
            if (!strcmp(key, "Type")) iter_string(&v, d->part_type, sizeof(d->part_type));
        }
    }
}

/* a{sa{sv}} of one object: fills d. */
static void parse_object(Dev *d, const char *path, DBusMessageIter *ifaces)
{
    memset(d, 0, sizeof(*d));
    snprintf(d->path, sizeof(d->path), "%s", path);
    DBusMessageIter e;
    dbus_message_iter_recurse(ifaces, &e);
    for (; dbus_message_iter_get_arg_type(&e) == DBUS_TYPE_DICT_ENTRY; dbus_message_iter_next(&e)) {
        DBusMessageIter kv;
        dbus_message_iter_recurse(&e, &kv);
        const char *iface;
        dbus_message_iter_get_basic(&kv, &iface);
        dbus_message_iter_next(&kv);
        if (!strcmp(iface, UD_FS)) d->has_fs = 1;
        if (!strcmp(iface, UD_LOOP)) d->is_loop = 1;
        parse_dev_props(d, iface, &kv);
    }
}

/* ---- udisks calls ---- */

static DBusMessage *call(const char *path, const char *iface, const char *method, int first_type, ...)
{
    DBusMessage *m = dbus_message_new_method_call(UD_NAME, path, iface, method);
    if (!m) {
        return NULL;
    }
    va_list ap;
    va_start(ap, first_type);
    dbus_message_append_args_valist(m, first_type, ap);
    va_end(ap);
    DBusError err;
    dbus_error_init(&err);
    DBusMessage *r = dbus_connection_send_with_reply_and_block(g_bus, m, 5000, &err);
    dbus_message_unref(m);
    if (dbus_error_is_set(&err)) {
        dbus_error_free(&err);
        return NULL;
    }
    return r;
}

/* Drive props kimountd keeps: the "is it removable" answer and a name. */
static void drive_info(const char *drive, int *removable, char *desc, size_t descsz)
{
    *removable = 0;
    desc[0] = 0;
    if (!drive[0] || !strcmp(drive, "/")) {
        return;
    }
    const char *iface = UD_DRIVE;
    DBusMessage *r = call(drive, PROPS_IFACE, "GetAll", DBUS_TYPE_STRING, &iface, DBUS_TYPE_INVALID);
    if (!r) {
        return;
    }
    char vendor[64] = "", model[96] = "", bus[16] = "";
    unsigned long long size = 0;
    int rm = 0, media_rm = 0;
    DBusMessageIter it, e;
    dbus_message_iter_init(r, &it);
    dbus_message_iter_recurse(&it, &e);
    for (; dbus_message_iter_get_arg_type(&e) == DBUS_TYPE_DICT_ENTRY; dbus_message_iter_next(&e)) {
        DBusMessageIter kv, v;
        dbus_message_iter_recurse(&e, &kv);
        const char *key;
        dbus_message_iter_get_basic(&kv, &key);
        dbus_message_iter_next(&kv);
        dbus_message_iter_recurse(&kv, &v);
        if (!strcmp(key, "Vendor")) iter_string(&v, vendor, sizeof(vendor));
        else if (!strcmp(key, "Model")) iter_string(&v, model, sizeof(model));
        else if (!strcmp(key, "ConnectionBus")) iter_string(&v, bus, sizeof(bus));
        else if (!strcmp(key, "Removable")) rm = iter_bool(&v);
        else if (!strcmp(key, "MediaRemovable")) media_rm = iter_bool(&v);
        else if (!strcmp(key, "Size")) size = iter_u64(&v);
    }
    dbus_message_unref(r);
    *removable = rm || media_rm || !strcmp(bus, "usb") || !strcmp(bus, "sdio") || !strcmp(bus, "ieee1394");
    char sz[24] = "";
    if (size >= 1000ULL * 1000 * 1000 * 1000) {
        snprintf(sz, sizeof(sz), "%.1f TB", (double)size / 1e12);
    } else if (size > 0) {
        snprintf(sz, sizeof(sz), "%.0f GB", (double)size / 1e9);
    }
    snprintf(desc, descsz, "%s%s%s%s%s", vendor, vendor[0] && model[0] ? " " : "", model, sz[0] ? ", " : "", sz);
}

/* EFI system partitions: on every internal disk, never something to mount. */
static int is_esp(const Dev *d)
{
    return !strcasecmp(d->part_type, "c12a7328-f81f-11d2-ba4b-00a0c93ec93b") || !strcmp(d->part_type, "0xef");
}

static int under_user_media(const char *mp)
{
    return !strncmp(mp, "/media/", 7) || !strncmp(mp, "/run/media/", 11);
}

/* Whether d is a filesystem this desktop should list at all. */
static int candidate(const Dev *d)
{
    if (!d->has_fs || !d->uuid[0] || d->is_loop || d->hint_ignore || d->configured || is_esp(d)) {
        return 0;
    }
    if (!strcmp(d->fstype, "swap") || !strcmp(d->fstype, "crypto_LUKS")) {
        return 0;
    }
    for (int i = 0; i < d->n_mounts; i++) {
        if (!under_user_media(d->mounts[i])) {
            return 0; /* mounted by the system somewhere else: not ours */
        }
    }
    return 1;
}

static void mount_reply(DBusPendingCall *pc, void *data)
{
    char *uuid = data;
    DBusMessage *r = dbus_pending_call_steal_reply(pc);
    if (r && dbus_message_get_type(r) == DBUS_MESSAGE_TYPE_ERROR) {
        const char *msg = "";
        dbus_message_get_args(r, NULL, DBUS_TYPE_STRING, &msg, DBUS_TYPE_INVALID);
        fprintf(stderr, "kimountd: mounting %s failed: %s\n", uuid, msg);
    } else if (r) {
        const char *mp = "";
        dbus_message_get_args(r, NULL, DBUS_TYPE_STRING, &mp, DBUS_TYPE_INVALID);
        fprintf(stderr, "kimountd: mounted %s at %s\n", uuid, mp);
    }
    if (r) {
        dbus_message_unref(r);
    }
    dbus_pending_call_unref(pc);
}

/* Asynchronous: a polkit password prompt can take as long as the user
 * likes, and the loop keeps serving other devices meanwhile. */
static void mount_dev(const Dev *d)
{
    DBusMessage *m = dbus_message_new_method_call(UD_NAME, d->path, UD_FS, "Mount");
    if (!m) {
        return;
    }
    DBusMessageIter it, dict;
    dbus_message_iter_init_append(m, &it);
    dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &dict);
    dbus_message_iter_close_container(&it, &dict);
    DBusPendingCall *pc = NULL;
    /* No timeout: the prompt above. */
    if (dbus_connection_send_with_reply(g_bus, m, &pc, DBUS_TIMEOUT_INFINITE) && pc) {
        dbus_pending_call_set_notify(pc, mount_reply, strdup(d->uuid), free);
    }
    dbus_message_unref(m);
    dbus_connection_flush(g_bus);
}

/* Records d if it is new (or reappeared), and mounts it if marked. 1 if
 * ki-mount.conf needs writing. */
static int handle_dev(const Dev *d, int mount_now)
{
    if (!candidate(d)) {
        return 0;
    }
    int removable;
    char desc[160];
    drive_info(d->drive, &removable, desc, sizeof(desc));

    int changed = 0;
    Known *k = known_find(d->uuid);
    if (!k) {
        if (g_n_known >= MAX_KNOWN) {
            return 0;
        }
        k = &g_known[g_n_known++];
        memset(k, 0, sizeof(*k));
        snprintf(k->uuid, sizeof(k->uuid), "%s", d->uuid);
        changed = 1;
    }
    if (k->removable != removable || strcmp(k->fstype, d->fstype) || strcmp(k->label, d->label) ||
        strcmp(k->desc, desc) || mount_now) {
        k->removable = removable;
        snprintf(k->fstype, sizeof(k->fstype), "%s", d->fstype);
        snprintf(k->label, sizeof(k->label), "%s", d->label);
        snprintf(k->desc, sizeof(k->desc), "%s", desc);
        k->last_seen = (long)time(NULL);
        changed = 1;
    }
    if (mount_now && k->automount && d->n_mounts == 0) {
        mount_dev(d);
    }
    return changed;
}

/* Every filesystem udisks knows right now; mount_now at startup and
 * after kiconf changed the marks. */
static void scan_all(int mount_now)
{
    DBusMessage *r = call(UD_ROOT, OM_IFACE, "GetManagedObjects", DBUS_TYPE_INVALID);
    if (!r) {
        log_msg("udisks2 not reachable%s", "");
        return;
    }
    conf_load();
    int changed = 0;
    DBusMessageIter it, e;
    dbus_message_iter_init(r, &it);
    dbus_message_iter_recurse(&it, &e);
    for (; dbus_message_iter_get_arg_type(&e) == DBUS_TYPE_DICT_ENTRY; dbus_message_iter_next(&e)) {
        DBusMessageIter kv;
        dbus_message_iter_recurse(&e, &kv);
        const char *path;
        dbus_message_iter_get_basic(&kv, &path);
        dbus_message_iter_next(&kv);
        Dev d;
        parse_object(&d, path, &kv);
        changed |= handle_dev(&d, mount_now);
    }
    dbus_message_unref(r);
    if (changed) {
        conf_save();
    }
}

/* One object again, after it appeared or its mount points changed. */
static void rescan_object(const char *path, int mount_now)
{
    Dev d;
    memset(&d, 0, sizeof(d));
    snprintf(d.path, sizeof(d.path), "%s", path);
    static const char *const ifaces[] = {UD_BLOCK, UD_FS, UD_PART};
    for (int i = 0; i < 3; i++) {
        const char *iface = ifaces[i];
        DBusMessage *r = call(path, PROPS_IFACE, "GetAll", DBUS_TYPE_STRING, &iface, DBUS_TYPE_INVALID);
        if (!r) {
            if (i == 0) {
                return; /* not a block device (any more) */
            }
            continue;
        }
        if (i == 1) {
            d.has_fs = 1;
        }
        DBusMessageIter it;
        dbus_message_iter_init(r, &it);
        parse_dev_props(&d, iface, &it);
        dbus_message_unref(r);
    }
    if (!strncmp(path, UD_ROOT "/block_devices/loop", strlen(UD_ROOT "/block_devices/loop"))) {
        d.is_loop = 1;
    }
    conf_load();
    if (handle_dev(&d, mount_now)) {
        conf_save();
    }
}

static DBusHandlerResult on_signal(DBusConnection *c, DBusMessage *m, void *data)
{
    (void)c;
    (void)data;
    const char *path = dbus_message_get_path(m);
    if (dbus_message_is_signal(m, OM_IFACE, "InterfacesAdded")) {
        const char *obj = NULL;
        DBusMessageIter it, a;
        dbus_message_iter_init(m, &it);
        dbus_message_iter_get_basic(&it, &obj);
        dbus_message_iter_next(&it);
        dbus_message_iter_recurse(&it, &a);
        for (; dbus_message_iter_get_arg_type(&a) == DBUS_TYPE_DICT_ENTRY; dbus_message_iter_next(&a)) {
            DBusMessageIter kv;
            const char *iface;
            dbus_message_iter_recurse(&a, &kv);
            dbus_message_iter_get_basic(&kv, &iface);
            if (!strcmp(iface, UD_FS)) {
                rescan_object(obj, 1); /* plugged in (or just formatted) */
                break;
            }
        }
    } else if (path && dbus_message_is_signal(m, PROPS_IFACE, "PropertiesChanged")) {
        const char *iface = NULL;
        DBusMessageIter it;
        dbus_message_iter_init(m, &it);
        dbus_message_iter_get_basic(&it, &iface);
        if (iface && !strcmp(iface, UD_FS)) {
            rescan_object(path, 0); /* mounted/unmounted by hand: maybe now worth listing */
        }
    }
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static void on_sig(int s)
{
    if (s == SIGHUP) {
        g_hup = 1;
    } else {
        g_term = 1;
    }
    char b = 0;
    ssize_t n = write(g_sigpipe[1], &b, 1);
    (void)n;
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--version")) {
            printf("kimountd %s\n", KIMOUNTD_VERSION);
            return 0;
        }
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("usage: kimountd   (automounts what kiconf's Discos tab marks; see ki-mount.conf)\n");
            return 0;
        }
    }
    conf_path();

    DBusError err;
    dbus_error_init(&err);
    g_bus = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
    if (!g_bus) {
        fprintf(stderr, "kimountd: no system bus: %s\n", err.message);
        return 1;
    }
    dbus_connection_set_exit_on_disconnect(g_bus, FALSE);
    dbus_bus_add_match(g_bus, "type='signal',sender='" UD_NAME "',interface='" OM_IFACE "',member='InterfacesAdded'",
                       NULL);
    dbus_bus_add_match(g_bus,
                       "type='signal',sender='" UD_NAME "',interface='" PROPS_IFACE
                       "',member='PropertiesChanged',arg0='" UD_FS "'",
                       NULL);
    dbus_connection_add_filter(g_bus, on_signal, NULL, NULL);

    if (pipe(g_sigpipe) != 0) {
        return 1;
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sig;
    sigaction(SIGHUP, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    scan_all(1);

    int fd = -1;
    dbus_connection_get_unix_fd(g_bus, &fd);
    while (!g_term) {
        struct pollfd p[2] = {{fd, POLLIN, 0}, {g_sigpipe[0], POLLIN, 0}};
        if (poll(p, 2, -1) < 0 && errno != EINTR) {
            break;
        }
        if (p[1].revents & POLLIN) {
            char b[16];
            ssize_t n = read(g_sigpipe[0], b, sizeof(b));
            (void)n;
        }
        if (g_hup) {
            g_hup = 0;
            scan_all(1); /* kiconf changed the marks: mount what is newly marked */
        }
        if (!dbus_connection_read_write(g_bus, 0)) {
            log_msg("system bus gone%s", "");
            break;
        }
        while (dbus_connection_dispatch(g_bus) == DBUS_DISPATCH_DATA_REMAINS) {
        }
    }
    return 0;
}
