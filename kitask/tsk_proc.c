/* tsk_proc.c - one sample of every process in /proc.
 *
 * The cheap files (stat, statm) are read for every process on every refresh;
 * the rest only when a visible column asks for it (TSK_NEED_*), and the costly
 * ones -- smaps_rollup walks the page tables, the fd scan readlinks every
 * descriptor -- only for processes that ran since the previous sample: a
 * process that didn't run can't have changed its private memory or opened a
 * socket, so its last numbers still hold. Everything goes through openat() on
 * the /proc/PID directory, with fixed buffers. */
#include "tsk.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PF_KTHREAD 0x00200000
#define SMAPS_MAX_AGE (30 * G_USEC_PER_SEC)
#define SMAPS_BUDGET 12

static int read_at(int dirfd, const char *name, char *buf, size_t sz)
{
    int fd = openat(dirfd, name, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    size_t n = 0;
    while (n < sz - 1) {
        ssize_t r = read(fd, buf + n, sz - 1 - n);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            break;
        n += (size_t)r;
    }
    close(fd);
    buf[n] = '\0';
    return (int)n;
}

char *tsk_proc_read_file(pid_t pid, const char *name, gsize *len)
{
    char path[64], *data = NULL;
    snprintf(path, sizeof(path), "/proc/%d/%s", (int)pid, name);
    if (!g_file_get_contents(path, &data, len, NULL))
        return NULL;
    return data;
}

const char *tsk_user_name(uid_t uid)
{
    static GHashTable *names;
    if (!names)
        names = g_hash_table_new_full(NULL, NULL, NULL, g_free);
    const char *n = g_hash_table_lookup(names, GUINT_TO_POINTER(uid));
    if (n)
        return n;
    struct passwd *pw = getpwuid(uid);
    char *s = pw ? g_strdup(pw->pw_name) : g_strdup_printf("%u", (unsigned)uid);
    g_hash_table_insert(names, GUINT_TO_POINTER(uid), s);
    return s;
}

static void proc_free(gpointer data)
{
    TskProc *p = data;
    g_free(p->name);
    g_free(p->cmdline);
    g_free(p->title);
    if (p->sockets)
        g_array_free(p->sockets, TRUE);
    if (p->dri_fds)
        g_array_free(p->dri_fds, TRUE);
    if (p->gpu_prev)
        g_hash_table_destroy(p->gpu_prev);
    g_free(p);
}

void tsk_procs_init(TskProcs *p)
{
    memset(p, 0, sizeof(*p));
    p->procs = g_hash_table_new_full(NULL, NULL, NULL, proc_free);
    p->hz = sysconf(_SC_CLK_TCK);
    p->page = sysconf(_SC_PAGESIZE);
    p->ncpu = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (p->hz <= 0)
        p->hz = 100;
    if (p->ncpu < 1)
        p->ncpu = 1;
    char buf[4096];
    int dfd = open("/proc", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0 && read_at(dfd, "stat", buf, sizeof(buf)) > 0) {
        char *b = strstr(buf, "\nbtime ");
        if (b)
            p->boot_time = strtoull(b + 7, NULL, 10);
    }
    if (dfd >= 0)
        close(dfd);
}

/* argv joined by spaces; also sets the display name */
static void read_cmdline(TskProc *p, int dfd)
{
    char buf[8192];
    int n = read_at(dfd, "cmdline", buf, sizeof(buf));
    g_free(p->cmdline);
    g_free(p->name);
    p->name = NULL;
    if (n <= 0) {
        p->cmdline = g_strdup_printf("[%s]", p->comm);
        p->name = g_strdup(p->comm);
        return;
    }
    while (n > 0 && buf[n - 1] == '\0')
        n--;
    /* argv[0]'s base name when the kernel cut comm at 15 characters */
    if (strlen(p->comm) >= 15) {
        const char *slash = strrchr(buf, '/');
        const char *base = slash ? slash + 1 : buf;
        if (!strncmp(base, p->comm, strlen(p->comm))) {
            size_t len = strcspn(base, " :");
            p->name = g_strndup(base, len);
        }
    }
    if (!p->name)
        p->name = g_strdup(p->comm);
    for (int i = 0; i < n; i++)
        if (buf[i] == '\0' || buf[i] == '\n')
            buf[i] = ' ';
    p->cmdline = g_strndup(buf, (gsize)n);
}

static double kb_field(const char *buf, const char *key)
{
    const char *s = strstr(buf, key);
    if (!s)
        return 0;
    return strtod(s + strlen(key), NULL) * 1024.0;
}

static void read_smaps(TskProc *p, int dfd)
{
    char buf[2048];
    if (read_at(dfd, "smaps_rollup", buf, sizeof(buf)) <= 0) {
        p->uss = p->pss = p->swap = -1;
        return;
    }
    p->pss = kb_field(buf, "\nPss:");
    p->uss = kb_field(buf, "\nPrivate_Clean:") + kb_field(buf, "\nPrivate_Dirty:");
    p->swap = kb_field(buf, "\nSwap:");
}

static void read_io(TskProc *p, int dfd, double elapsed)
{
    char buf[512];
    if (read_at(dfd, "io", buf, sizeof(buf)) <= 0) {
        p->io_read = p->io_write = p->io_read_rate = p->io_write_rate = -1;
        return;
    }
    const char *r = strstr(buf, "\nread_bytes:"), *w = strstr(buf, "\nwrite_bytes:");
    double rd = r ? strtod(r + 12, NULL) : 0, wr = w ? strtod(w + 13, NULL) : 0;
    if (p->io_read >= 0 && elapsed > 0) {
        p->io_read_rate = MAX(0, rd - p->io_read) / elapsed;
        p->io_write_rate = MAX(0, wr - p->io_write) / elapsed;
    } else {
        p->io_read_rate = p->io_write_rate = 0;
    }
    p->io_read = rd;
    p->io_write = wr;
}

/* Re-lists the sockets and DRM descriptors the process holds. FALSE when the
 * fd directory can't be read (someone else's process). */
static gboolean scan_fds(TskProc *p, int dfd)
{
    int fdd = openat(dfd, "fd", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fdd < 0)
        return FALSE;
    DIR *d = fdopendir(fdd);
    if (!d) {
        close(fdd);
        return FALSE;
    }
    if (!p->sockets)
        p->sockets = g_array_new(FALSE, FALSE, sizeof(guint64));
    if (!p->dri_fds)
        p->dri_fds = g_array_new(FALSE, FALSE, sizeof(int));
    g_array_set_size(p->sockets, 0);
    g_array_set_size(p->dri_fds, 0);
    struct dirent *e;
    char link[64];
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        ssize_t n = readlinkat(fdd, e->d_name, link, sizeof(link) - 1);
        if (n <= 0)
            continue;
        link[n] = '\0';
        if (!strncmp(link, "socket:[", 8)) {
            guint64 ino = g_ascii_strtoull(link + 8, NULL, 10);
            g_array_append_val(p->sockets, ino);
        } else if (!strncmp(link, "/dev/dri/", 9)) {
            int fd = atoi(e->d_name);
            g_array_append_val(p->dri_fds, fd);
        }
    }
    closedir(d);
    return TRUE;
}

static void read_net(TskProc *p, double elapsed)
{
    double rx = 0, tx = 0;
    for (guint i = 0; i < p->sockets->len; i++) {
        double r, t;
        tsk_net_delta(g_array_index(p->sockets, guint64, i), &r, &t);
        rx += r;
        tx += t;
    }
    p->net_rx_rate = elapsed > 0 ? rx / elapsed : 0;
    p->net_tx_rate = elapsed > 0 ? tx / elapsed : 0;
}

/* DRM fdinfo, both the old amdgpu format of kernels 5.14-6.x ("pasid:",
 * "vram mem: N kB", "gfx0: 1.23%") and the common drm-* keys of newer ones
 * ("drm-client-id:", "drm-memory-vram: N KiB", "drm-engine-gfx: N ns"). One
 * client can sit behind several fds: counted once, by its id. */
static void read_gpu(TskProc *p, int dfd, double elapsed, gboolean first)
{
    p->gpu = p->gpu_mem = -1;
    if (!p->dri_fds->len)
        return;
    GHashTable *clients = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    GHashTable *prev = p->gpu_prev;
    GHashTable *next = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    double util = 0, mem = 0;
    gboolean any = FALSE;
    char path[32], buf[4096];
    for (guint i = 0; i < p->dri_fds->len; i++) {
        snprintf(path, sizeof(path), "fdinfo/%d", g_array_index(p->dri_fds, int, i));
        if (read_at(dfd, path, buf, sizeof(buf)) <= 0)
            continue;
        char id[48] = "";
        double cmem_res = -1, cmem = -1, cmem_old = -1, cutil = 0;
        char **lines = g_strsplit(buf, "\n", -1);
        /* capacities first, the engines below divide by them */
        GHashTable *cap = NULL;
        for (char **l = lines; *l; l++)
            if (!strncmp(*l, "drm-engine-capacity-", 20)) {
                char *colon = strchr(*l, ':');
                if (!colon)
                    continue;
                if (!cap)
                    cap = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
                g_hash_table_insert(cap, g_strndup(*l + 20, (gsize)(colon - *l - 20)),
                                    GINT_TO_POINTER(MAX(1, atoi(colon + 1))));
            }
        for (char **l = lines; *l; l++) {
            char *colon = strchr(*l, ':');
            if (!colon)
                continue;
            *colon = '\0';
            const char *key = *l, *val = colon + 1;
            while (*val == ' ' || *val == '\t')
                val++;
            if (!strcmp(key, "drm-client-id") || !strcmp(key, "pasid")) {
                g_strlcpy(id, val, sizeof(id));
            } else if (!strcmp(key, "vram mem")) {
                cmem_old = g_ascii_strtod(val, NULL) * 1024.0;
            } else if (!strncmp(key, "drm-resident-", 13) || !strncmp(key, "drm-memory-", 11) ||
                       !strncmp(key, "drm-total-", 10)) {
                const char *region = strchr(key + 4, '-') + 1;
                if (strncmp(region, "vram", 4) && strncmp(region, "local", 5))
                    continue;
                char *unit = NULL;
                double v = g_ascii_strtod(val, &unit);
                while (unit && *unit == ' ')
                    unit++;
                if (unit && !strncmp(unit, "KiB", 3))
                    v *= 1024.0;
                else if (unit && !strncmp(unit, "MiB", 3))
                    v *= 1024.0 * 1024.0;
                if (key[4] == 'r')
                    cmem_res = MAX(cmem_res, 0) + v;
                else
                    cmem = MAX(cmem, 0) + v;
            } else if (!strncmp(key, "drm-engine-", 11) && strncmp(key, "drm-engine-capacity-", 20)) {
                const char *engine = key + 11;
                guint64 ns = g_ascii_strtoull(val, NULL, 10);
                char *k = g_strdup_printf("%s/%s", id, engine);
                gpointer old;
                if (!first && prev && g_hash_table_lookup_extended(prev, k, NULL, &old) && elapsed > 0) {
                    int c = cap ? GPOINTER_TO_INT(g_hash_table_lookup(cap, engine)) : 0;
                    double u = (double)(ns - (guint64)(gsize)old) / (elapsed * 1e9 * MAX(c, 1)) * 100.0;
                    cutil = MAX(cutil, u);
                }
                g_hash_table_replace(next, k, (gpointer)(gsize)ns);
            } else {
                /* old amdgpu rings: "gfx0: 0.13%" */
                size_t vl = strlen(val);
                if (vl > 1 && val[vl - 1] == '%' && (g_ascii_isdigit(val[0])))
                    cutil = MAX(cutil, g_ascii_strtod(val, NULL));
            }
        }
        g_strfreev(lines);
        if (cap)
            g_hash_table_destroy(cap);
        if (*id && g_hash_table_contains(clients, id))
            continue;
        if (*id)
            g_hash_table_add(clients, g_strdup(id));
        any = TRUE;
        util += cutil;
        mem += cmem_res >= 0 ? cmem_res : cmem >= 0 ? cmem : MAX(cmem_old, 0);
    }
    g_hash_table_destroy(clients);
    if (p->gpu_prev)
        g_hash_table_destroy(p->gpu_prev);
    p->gpu_prev = next;
    if (any) {
        p->gpu = MIN(util, 100.0);
        p->gpu_mem = mem;
    }
}

void tsk_procs_sample(TskProcs *ps, unsigned need)
{
    gint64 now = g_get_monotonic_time();
    double elapsed = ps->last_time ? (double)(now - ps->last_time) / 1e6 : 0;
    gboolean first = ps->last_time == 0;
    ps->last_time = now;
    ps->elapsed = elapsed;

    if (need & TSK_NEED_NET)
        tsk_net_sample();
    if (need & TSK_NEED_GPU)
        tsk_gpu_sample_procs();
    if (need & (TSK_NEED_X11 | TSK_NEED_TITLE))
        tsk_x11_sample(need);

    GHashTableIter it;
    gpointer v;
    g_hash_table_iter_init(&it, ps->procs);
    while (g_hash_table_iter_next(&it, NULL, &v))
        ((TskProc *)v)->seen = FALSE;

    int proc_fd = open("/proc", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    DIR *d = proc_fd >= 0 ? fdopendir(proc_fd) : NULL;
    if (!d) {
        if (proc_fd >= 0)
            close(proc_fd);
        return;
    }
    struct dirent *e;
    char buf[1024];
    int smaps_budget = SMAPS_BUDGET;
    while ((e = readdir(d))) {
        if (!g_ascii_isdigit(e->d_name[0]))
            continue;
        pid_t pid = atoi(e->d_name);
        /* stat goes through /proc directly; the PID's own directory is only
         * opened (DIR_FD) for a process that needs more than that */
        char path[32];
        g_snprintf(path, sizeof(path), "%s/stat", e->d_name);
        struct stat st;
        if (fstatat(proc_fd, e->d_name, &st, 0) < 0 || read_at(proc_fd, path, buf, sizeof(buf)) <= 0)
            continue;
        int dfd = -1;
#define DIR_FD() (dfd >= 0 ? dfd : (dfd = openat(proc_fd, e->d_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC)))
        char *lp = strchr(buf, '('), *rp = strrchr(buf, ')');
        if (!lp || !rp)
            continue;
        char state;
        int ppid, nice, threads;
        unsigned flags;
        unsigned long long utime, stime, start;
        if (sscanf(rp + 2, "%c %d %*d %*d %*d %*d %u %*u %*u %*u %*u %llu %llu %*d %*d %*d %d %d %*d %llu",
                   &state, &ppid, &flags, &utime, &stime, &nice, &threads, &start) != 8)
            continue;
        TskProc *p = g_hash_table_lookup(ps->procs, GINT_TO_POINTER(pid));
        if (p && p->start != start) {   /* the PID was reused */
            g_hash_table_remove(ps->procs, GINT_TO_POINTER(pid));
            p = NULL;
        }
        gboolean is_new = !p;
        if (is_new) {
            p = g_new0(TskProc, 1);
            p->pid = pid;
            p->start = start;
            p->uss = p->pss = p->swap = -1;
            p->io_read = p->io_write = p->io_read_rate = p->io_write_rate = -1;
            p->net_rx_rate = p->net_tx_rate = -1;
            p->gpu = p->gpu_mem = p->x11_mem = -1;
            g_hash_table_insert(ps->procs, GINT_TO_POINTER(pid), p);
        }
        size_t clen = MIN((size_t)(rp - lp - 1), sizeof(p->comm) - 1);
        gboolean renamed = is_new || strncmp(p->comm, lp + 1, clen) || p->comm[clen];
        memcpy(p->comm, lp + 1, clen);
        p->comm[clen] = '\0';
        p->seen = TRUE;
        p->uid = st.st_uid;
        p->state = state;
        p->ppid = ppid;
        p->nice = nice;
        p->threads = threads;
        p->kthread = (flags & PF_KTHREAD) != 0;
        unsigned long long ticks = utime + stime;
        p->ran = is_new || ticks != p->ticks;
        p->cpu = (!is_new && elapsed > 0) ? (double)(ticks - p->ticks) / ps->hz / elapsed * 100.0 : 0;
        p->ticks = ticks;
        if (renamed)
            read_cmdline(p, DIR_FD());
        if (p->kthread)
            continue;   /* no memory, files or windows of their own */

        /* memory only moves when the process runs (reclaim aside) */
        if (p->ran) {
            unsigned long long size = 0, res = 0, shr = 0;
            if (read_at(DIR_FD(), "statm", buf, sizeof(buf)) > 0)
                sscanf(buf, "%llu %llu %llu", &size, &res, &shr);
            p->rss_pages = res;
            p->vsize = (double)size * ps->page;
            p->rss = (double)res * ps->page;
            p->shared = (double)shr * ps->page;
        }
        /* smaps_rollup walks the page tables: ~150 µs on average, milliseconds
         * for a big browser. A new process gets it at once; one that ran is
         * re-read when its resident memory moved by 2% and 1 MiB since the
         * last read, or after SMAPS_MAX_AGE -- within a budget per refresh,
         * so a burst of activity spreads over the next refreshes. */
        if (need & TSK_NEED_SMAPS) {
            unsigned long long diff = p->rss_pages > p->smaps_rss ? p->rss_pages - p->smaps_rss
                                                                  : p->smaps_rss - p->rss_pages;
            gboolean moved = diff * 50 > p->smaps_rss && diff * (unsigned long long)ps->page >= 1024 * 1024;
            if (p->uss < 0 ||
                (p->ran && smaps_budget > 0 && (moved || now - p->smaps_time > SMAPS_MAX_AGE))) {
                smaps_budget--;
                read_smaps(p, DIR_FD());
                p->smaps_rss = p->rss_pages;
                p->smaps_time = now;
            }
        }
        if (need & TSK_NEED_IO) {
            if (p->ran || p->io_read < 0)
                read_io(p, DIR_FD(), elapsed);
            else
                p->io_read_rate = p->io_write_rate = 0;
        }
        if (need & (TSK_NEED_NET | TSK_NEED_GPU)) {
            if (p->ran || !p->fds_scanned) {
                if (!scan_fds(p, DIR_FD())) {
                    p->net_rx_rate = p->net_tx_rate = -1;
                    p->gpu = p->gpu_mem = -1;
                    goto x11;
                }
                p->fds_scanned = TRUE;
            }
            if (need & TSK_NEED_NET)
                read_net(p, elapsed);
            if (need & TSK_NEED_GPU) {
                read_gpu(p, DIR_FD(), elapsed, first || is_new);
                double u, m;
                if (tsk_gpu_nv_proc(pid, &u, &m)) {
                    p->gpu = MIN(100.0, MAX(p->gpu, 0) + MAX(u, 0));
                    p->gpu_mem = MAX(p->gpu_mem, 0) + MAX(m, 0);
                }
            }
        } else {
            p->fds_scanned = FALSE;   /* the fd lists went stale while unused */
        }
x11:
        if (dfd >= 0)
            close(dfd);
#undef DIR_FD
        if (need & TSK_NEED_X11)
            p->x11_mem = tsk_x11_mem(pid);
        if (need & TSK_NEED_TITLE) {
            unsigned long w = 0;
            const char *t = tsk_x11_title(pid, &w);
            if (g_strcmp0(t, p->title)) {
                g_free(p->title);
                p->title = g_strdup(t);
            }
            p->window = w;
        }
    }
    closedir(d);

    g_hash_table_iter_init(&it, ps->procs);
    while (g_hash_table_iter_next(&it, NULL, &v))
        if (!((TskProc *)v)->seen)
            g_hash_table_iter_remove(&it);
}
