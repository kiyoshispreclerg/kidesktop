/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Plugin manifests and the plugin processes (see PROTOCOL.md). A plugin is
 * started with its end of a SOCK_SEQPACKET socketpair as fd 3 and lives
 * until it is stopped; when kiview dies it gets SIGTERM (PDEATHSIG) and
 * sees the socket close. */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <glib-unix.h>

#include "kv.h"

#ifndef LIBEXECDIR
#define LIBEXECDIR "/usr/local/libexec"
#endif
#ifndef DATADIR
#define DATADIR "/usr/local/share"
#endif

#define KILL_AFTER_SECONDS 2

static GPtrArray *manifests; /* KvManifest* */

static void manifest_free(gpointer p)
{
    KvManifest *m = p;
    g_free(m->name);
    g_free(m->exec);
    g_strfreev(m->mimes);
    g_strfreev(m->exts);
    g_free(m);
}

static void load_dir(const char *dir)
{
    GDir *d = g_dir_open(dir, 0, NULL);
    if (!d)
        return;
    const char *f;
    while ((f = g_dir_read_name(d))) {
        if (!g_str_has_suffix(f, ".kvplugin"))
            continue;
        char *path = g_build_filename(dir, f, NULL);
        GKeyFile *kf = g_key_file_new();
        char *name = NULL, *exec = NULL;
        if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL) &&
            g_key_file_get_integer(kf, "Plugin", "Protocol", NULL) == 1 &&
            (name = g_key_file_get_string(kf, "Plugin", "Name", NULL)) &&
            (exec = g_key_file_get_string(kf, "Plugin", "Exec", NULL))) {
            gboolean dup = FALSE;
            for (guint i = 0; i < manifests->len; i++)
                dup |= !strcmp(((KvManifest *)manifests->pdata[i])->name, name);
            if (!dup) {
                KvManifest *m = g_new0(KvManifest, 1);
                m->name = name;
                m->exec = exec;
                name = exec = NULL;
                m->mimes = g_key_file_get_string_list(kf, "Plugin", "MimeTypes", NULL, NULL);
                m->exts = g_key_file_get_string_list(kf, "Plugin", "Extensions", NULL, NULL);
                m->priority = g_key_file_get_integer(kf, "Plugin", "Priority", NULL);
                g_ptr_array_add(manifests, m);
            }
        }
        g_free(name);
        g_free(exec);
        g_key_file_free(kf);
        g_free(path);
    }
    g_dir_close(d);
}

void kv_plugins_load(void)
{
    if (manifests)
        return;
    manifests = g_ptr_array_new_with_free_func(manifest_free);
    const char *env = g_getenv("KIVIEW_PLUGIN_DIR");
    if (env) {
        char **dirs = g_strsplit(env, ":", -1);
        for (int i = 0; dirs[i]; i++)
            if (*dirs[i])
                load_dir(dirs[i]);
        g_strfreev(dirs);
    }
    char *user = g_build_filename(g_get_user_data_dir(), "kiview", "plugins", NULL);
    load_dir(user);
    g_free(user);
    for (const char *const *sys = g_get_system_data_dirs(); *sys; sys++) {
        char *dir = g_build_filename(*sys, "kiview", "plugins", NULL);
        load_dir(dir);
        g_free(dir);
    }
    char *dir = g_build_filename(DATADIR, "kiview", "plugins", NULL);
    load_dir(dir);
    g_free(dir);
}

const KvManifest *kv_plugins_for(const char *mime, const char *name)
{
    const KvManifest *best = NULL;
    char *ext = NULL;
    const char *dot = name ? strrchr(name, '.') : NULL;
    if (dot && dot[1])
        ext = g_ascii_strdown(dot + 1, -1);
    for (int pass = 0; pass < 2 && !best; pass++) {
        for (guint i = 0; i < manifests->len; i++) {
            const KvManifest *m = manifests->pdata[i];
            gboolean hit = pass == 0 ? mime && m->mimes && g_strv_contains((const char *const *)m->mimes, mime)
                                     : ext && m->exts && g_strv_contains((const char *const *)m->exts, ext);
            if (hit && (!best || m->priority > best->priority))
                best = m;
        }
    }
    g_free(ext);
    return best;
}

struct KvProc {
    const KvManifest *man;
    int sock;
    guint watch;
    GPid pid;
    guint child_watch;
    guint kill_timer;
    gboolean exited;  /* the child was reaped */
    gboolean stopped; /* kv_proc_stop was called */
    gboolean notified;
    KvProcCb cb;
    gpointer ud;
};

static void proc_free(KvProc *p)
{
    if (p->kill_timer)
        g_source_remove(p->kill_timer);
    g_spawn_close_pid(p->pid);
    g_free(p);
}

static void child_exited(GPid pid, gint status, gpointer data)
{
    (void)pid;
    (void)status;
    KvProc *p = data;
    p->exited = TRUE;
    p->child_watch = 0;
    if (p->stopped)
        proc_free(p);
    else if (!p->notified) {
        p->notified = TRUE;
        p->cb(p, NULL, p->ud);
    }
}

static gboolean kill_it(gpointer data)
{
    KvProc *p = data;
    p->kill_timer = 0;
    if (!p->exited)
        kill(p->pid, SIGKILL); /* by PID: never by name */
    return G_SOURCE_REMOVE;
}

static gboolean on_sock(gint fd, GIOCondition cond, gpointer data)
{
    (void)cond;
    KvProc *p = data;
    char buf[KVP_MSG_MAX];
    int rfd;
    for (;;) {
        int n = kvp_recv(fd, buf, sizeof buf, &rfd);
        if (n == 0 || n == -2) {
            p->watch = 0;
            if (!p->notified) {
                p->notified = TRUE;
                p->cb(p, NULL, p->ud);
            }
            return G_SOURCE_REMOVE;
        }
        if (n < 0)
            return G_SOURCE_CONTINUE;
        if (rfd >= 0)
            close(rfd);
        if (g_getenv("KIVIEW_TRACE"))
            g_printerr("kiview <- %s: %s\n", p->man->name, buf);
        KvpMsg *m = kvp_msg_parse(buf);
        if (m) {
            p->cb(p, m, p->ud);
            kvp_msg_free(m);
        }
        if (p->stopped || p->notified)
            return G_SOURCE_REMOVE;
    }
}

static void child_setup(gpointer data)
{
    int fd = GPOINTER_TO_INT(data);
    if (fd != 3) {
        dup2(fd, 3);
        close(fd);
    } else {
        fcntl(3, F_SETFD, 0);
    }
    prctl(PR_SET_PDEATHSIG, SIGTERM);
}

KvProc *kv_proc_start(const KvManifest *man, KvProcCb cb, gpointer ud)
{
    char *exe;
    if (g_path_is_absolute(man->exec)) {
        exe = g_strdup(man->exec);
    } else {
        const char *dir = g_getenv("KIVIEW_LIBEXEC_DIR");
        char *base = dir ? g_strdup(dir) : g_build_filename(LIBEXECDIR, "kiview", NULL);
        exe = g_build_filename(base, man->exec, NULL);
        g_free(base);
    }
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) < 0) {
        g_free(exe);
        return NULL;
    }
    char *argv[] = {exe, NULL};
    GPid pid;
    GError *err = NULL;
    /* The plugin's end has to survive exec: child_setup dup2()s it to 3. */
    gboolean ok = g_spawn_async(NULL, argv, NULL, G_SPAWN_DO_NOT_REAP_CHILD, child_setup,
                                GINT_TO_POINTER(sv[1]), &pid, &err);
    close(sv[1]);
    if (!ok) {
        g_printerr("kiview: can't start %s: %s\n", exe, err->message);
        g_error_free(err);
        close(sv[0]);
        g_free(exe);
        return NULL;
    }
    g_free(exe);
    fcntl(sv[0], F_SETFL, fcntl(sv[0], F_GETFL) | O_NONBLOCK);
    KvProc *p = g_new0(KvProc, 1);
    p->man = man;
    p->sock = sv[0];
    p->pid = pid;
    p->cb = cb;
    p->ud = ud;
    p->watch = g_unix_fd_add(p->sock, G_IO_IN | G_IO_HUP, on_sock, p);
    p->child_watch = g_child_watch_add(pid, child_exited, p);
    return p;
}

const KvManifest *kv_proc_manifest(const KvProc *p)
{
    return p->man;
}

void kv_proc_send(KvProc *p, GString *msg, int fd)
{
    if (g_getenv("KIVIEW_TRACE"))
        g_printerr("kiview -> %s: %s%s\n", p->man->name, msg->str, fd >= 0 ? " [fd]" : "");
    if (p->stopped || !kvp_out_send(msg, p->sock, fd))
        return; /* a dead plugin is reported by its socket */
}

void kv_proc_stop(KvProc *p)
{
    if (p->stopped)
        return;
    p->stopped = TRUE;
    p->cb = NULL;
    if (p->watch)
        g_source_remove(p->watch);
    p->watch = 0;
    if (!p->exited)
        kvp_out_send(kvp_out("quit"), p->sock, -1);
    close(p->sock);
    p->sock = -1;
    if (p->exited) {
        proc_free(p);
        return;
    }
    p->kill_timer = g_timeout_add_seconds(KILL_AFTER_SECONDS, kill_it, p);
}
