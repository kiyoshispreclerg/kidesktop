/*
 * process.c - search plugin: if the typed query (2+ chars, to avoid
 * matching half the process table on one letter) names a running
 * process owned by the current user (matched against /proc/[pid]/comm,
 * case-insensitive substring), offers three results per match --
 * Finalizar (SIGTERM), Matar (SIGKILL), Matar forcado/arvore (SIGKILL
 * to the whole process group, so children die too) -- the same
 * "graceful / kill / kill tree" tiers a real desktop task manager
 * offers, without one running here. Never matches processes owned by
 * another user, and never offers to kill xisserve's own pid.
 */
#include "../xisserve.h"

#include <dirent.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

typedef struct {
    pid_t pid;
    int sig;
    gboolean to_group;
} ProcessKillTarget;

static void process_kill_activate(ResultEntry *self)
{
    ProcessKillTarget *t = (ProcessKillTarget *)self->activate_data;
    if (!t) return;
    if (t->to_group) {
        pid_t pgid = getpgid(t->pid);
        kill(pgid > 0 ? -pgid : -t->pid, t->sig);
    } else {
        kill(t->pid, t->sig);
    }
}

static GdkPixbuf *process_icon(void)
{
    return xisserve_resolve_icon("system-run", XISSERVE_ICON_PX);
}

/* /proc/pid/comm is the kernel-truncated (15-char) process name --
 * enough to search by and to show, without a full cmdline parse. */
static gboolean read_proc_comm(pid_t pid, char *out, size_t outsz)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/comm", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f) return FALSE;
    gboolean ok = fgets(out, outsz, f) != NULL;
    fclose(f);
    if (ok) {
        size_t l = strlen(out);
        while (l > 0 && (out[l - 1] == '\n' || out[l - 1] == '\r')) out[--l] = 0;
    }
    return ok && out[0];
}

static void add_kill_result(GPtrArray *results, pid_t pid, const char *comm,
                             const char *label_prefix, int sig, gboolean to_group)
{
    ResultEntry *e = g_new0(ResultEntry, 1);
    snprintf(e->name, sizeof(e->name), "%s: %s (PID %d)", label_prefix, comm, (int)pid);
    snprintf(e->subtitle, sizeof(e->subtitle), "Processo em execucao");
    e->from_desktop = FALSE;
    e->icon = process_icon();

    ProcessKillTarget *t = g_new0(ProcessKillTarget, 1);
    t->pid = pid;
    t->sig = sig;
    t->to_group = to_group;
    e->activate_data = t;
    e->activate_data_free = g_free;
    e->activate_fn = process_kill_activate;

    g_ptr_array_add(results, e);
}

void plugin_process_search(const char *query, GPtrArray *results)
{
    int max_matches = xisserve_config_get_int("PROCESS", "search_max", 6);
    if (max_matches <= 0) return;
    if (strlen(query) < 2) return;

    uid_t self_uid = getuid();
    pid_t self_pid = getpid();
    gchar *query_cf = g_utf8_casefold(query, -1);

    DIR *d = opendir("/proc");
    if (!d) {
        g_free(query_cf);
        return;
    }
    struct dirent *de;
    int shown = 0;
    while ((de = readdir(d)) && shown < max_matches) {
        if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
        pid_t pid = (pid_t)atoi(de->d_name);
        if (pid <= 0 || pid == self_pid) continue;

        char procdir[32];
        snprintf(procdir, sizeof(procdir), "/proc/%d", (int)pid);
        struct stat st;
        if (stat(procdir, &st) != 0 || st.st_uid != self_uid) continue;

        char comm[256];
        if (!read_proc_comm(pid, comm, sizeof(comm))) continue;

        gchar *comm_cf = g_utf8_casefold(comm, -1);
        gboolean match = strstr(comm_cf, query_cf) != NULL;
        g_free(comm_cf);
        if (!match) continue;

        add_kill_result(results, pid, comm, "Finalizar", SIGTERM, FALSE);
        add_kill_result(results, pid, comm, "Matar", SIGKILL, FALSE);
        add_kill_result(results, pid, comm, "Matar forcado (arvore)", SIGKILL, TRUE);
        shown++;
    }
    closedir(d);
    g_free(query_cf);
}
