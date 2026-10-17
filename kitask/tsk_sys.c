/* tsk_sys.c - system-wide numbers for the graphs and the status bar:
 * /proc/stat (CPU, per core), /proc/meminfo, /proc/net/dev and
 * /proc/diskstats (whole physical disks only, so partitions and device-mapper
 * volumes on top of them aren't counted twice). */
#include "tsk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static GHashTable *physical_disks;   /* name -> 1 */

static void find_disks(void)
{
    physical_disks = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    GDir *d = g_dir_open("/sys/block", 0, NULL);
    if (!d)
        return;
    const char *n;
    while ((n = g_dir_read_name(d))) {
        /* a real disk has a device link; loop, ram, zram and dm-* have none
         * (zram has one on some kernels: it's swap, skipped by name) */
        char *dev = g_build_filename("/sys/block", n, "device", NULL);
        if (g_file_test(dev, G_FILE_TEST_EXISTS) && strncmp(n, "zram", 4))
            g_hash_table_add(physical_disks, g_strdup(n));
        g_free(dev);
    }
    g_dir_close(d);
}

void tsk_sys_init(TskSys *s)
{
    memset(s, 0, sizeof(*s));
    s->ncpu = (int)sysconf(_SC_NPROCESSORS_CONF);
    if (s->ncpu < 1)
        s->ncpu = 1;
    s->core = g_new0(double, s->ncpu);
    s->prev_busy = g_new0(unsigned long long, s->ncpu + 1);
    s->prev_total = g_new0(unsigned long long, s->ncpu + 1);
    find_disks();
    tsk_sys_sample(s);
}

static void sample_cpu(TskSys *s)
{
    FILE *f = fopen("/proc/stat", "re");
    if (!f)
        return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "cpu", 3))
            break;
        int idx = 0;
        const char *p = line + 3;
        if (*p != ' ') {
            idx = atoi(p) + 1;
            if (idx > s->ncpu)
                continue;
        }
        while (*p && *p != ' ')
            p++;
        unsigned long long v[10] = {0};
        sscanf(p, "%llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6],
               &v[7]);
        /* user nice system idle iowait irq softirq steal; guest is inside user */
        unsigned long long idle = v[3] + v[4];
        unsigned long long total = v[0] + v[1] + v[2] + v[3] + v[4] + v[5] + v[6] + v[7];
        unsigned long long busy = total - idle;
        double pct = 0;
        if (s->prev_total[idx] && total > s->prev_total[idx])
            pct = (double)(busy - s->prev_busy[idx]) / (double)(total - s->prev_total[idx]) * 100.0;
        s->prev_busy[idx] = busy;
        s->prev_total[idx] = total;
        pct = CLAMP(pct, 0, 100);
        if (idx == 0)
            s->cpu = pct;
        else
            s->core[idx - 1] = pct;
    }
    fclose(f);
}

static void sample_mem(TskSys *s)
{
    FILE *f = fopen("/proc/meminfo", "re");
    if (!f)
        return;
    char key[64];
    double kb;
    while (fscanf(f, "%63s %lf kB\n", key, &kb) == 2) {
        double b = kb * 1024.0;
        if (!strcmp(key, "MemTotal:")) s->mem_total = b;
        else if (!strcmp(key, "MemFree:")) s->mem_free = b;
        else if (!strcmp(key, "MemAvailable:")) s->mem_available = b;
        else if (!strcmp(key, "Buffers:")) s->buffers = b;
        else if (!strcmp(key, "Cached:")) s->cached = b;
        else if (!strcmp(key, "SReclaimable:")) s->sreclaimable = b;
        else if (!strcmp(key, "Shmem:")) s->shmem = b;
        else if (!strcmp(key, "SwapTotal:")) s->swap_total = b;
        else if (!strcmp(key, "SwapFree:")) s->swap_free = b;
    }
    fclose(f);
}

static void sample_io(TskSys *s, double dt)
{
    unsigned long long rx = 0, tx = 0, rd = 0, wr = 0;
    char line[512];
    FILE *f = fopen("/proc/net/dev", "re");
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            char *colon = strchr(line, ':');
            if (!colon)
                continue;
            *colon = '\0';
            char *name = g_strstrip(line);
            if (!strcmp(name, "lo"))
                continue;
            unsigned long long r, t;
            if (sscanf(colon + 1, "%llu %*u %*u %*u %*u %*u %*u %*u %llu", &r, &t) == 2) {
                rx += r;
                tx += t;
            }
        }
        fclose(f);
    }
    f = fopen("/proc/diskstats", "re");
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            char name[64];
            unsigned long long sr, sw;
            if (sscanf(line, "%*u %*u %63s %*u %*u %llu %*u %*u %*u %llu", name, &sr, &sw) == 3 &&
                g_hash_table_contains(physical_disks, name)) {
                rd += sr * 512;   /* diskstats sectors are always 512 bytes */
                wr += sw * 512;
            }
        }
        fclose(f);
    }
    if (dt > 0 && s->prev_time) {
        s->net_rx = rx >= s->prev_rx ? (double)(rx - s->prev_rx) / dt : 0;
        s->net_tx = tx >= s->prev_tx ? (double)(tx - s->prev_tx) / dt : 0;
        s->disk_read = rd >= s->prev_rd ? (double)(rd - s->prev_rd) / dt : 0;
        s->disk_write = wr >= s->prev_wr ? (double)(wr - s->prev_wr) / dt : 0;
    }
    s->prev_rx = rx;
    s->prev_tx = tx;
    s->prev_rd = rd;
    s->prev_wr = wr;
}

void tsk_sys_sample(TskSys *s)
{
    gint64 now = g_get_monotonic_time();
    double dt = s->prev_time ? (double)(now - s->prev_time) / 1e6 : 0;
    sample_cpu(s);
    sample_mem(s);
    sample_io(s, dt);
    s->prev_time = now;
}
