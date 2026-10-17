/* tsk_gpu.c - the GPUs: what each one is, how busy it is and how much of
 * its memory is in use.
 *
 * Every /sys/class/drm/cardN is one GPU, named by its PCI address. amdgpu
 * publishes busy % and VRAM/GTT use in sysfs; NVIDIA's driver only through
 * NVML, dlopen()ed so kitask neither links nor needs it. Other drivers (i915,
 * nouveau...) have no global numbers in sysfs: they are listed, and their
 * processes still show GPU columns from fdinfo, but offer no graphs.
 *
 * A runtime-suspended GPU (the dGPU of a hybrid laptop, mostly) is never
 * queried: NVML would wake it up and keep it awake, costing battery just to
 * draw a flat line. */
#include "tsk.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static GPtrArray *gpus;     /* TskGpu* */

/* ---- NVML, declared by hand: only the calls used here -------------------- */

typedef int (*NvInit)(void);
typedef int (*NvCount)(unsigned *);
typedef int (*NvHandle)(unsigned, void **);
typedef int (*NvName)(void *, char *, unsigned);
typedef struct { char legacy[16]; unsigned domain, bus, device, pci_id, subsys; char bus_id[32]; } NvPci;
typedef int (*NvPciInfo)(void *, NvPci *);
typedef struct { unsigned gpu, memory; } NvUtil;
typedef int (*NvUtilRates)(void *, NvUtil *);
typedef struct { unsigned long long total, free, used; } NvMem;
typedef int (*NvMemInfo)(void *, NvMem *);
typedef struct { unsigned pid; unsigned long long ts; unsigned sm, mem, enc, dec; } NvProcUtil;
typedef int (*NvProcUtilFn)(void *, NvProcUtil *, unsigned *, unsigned long long);
typedef struct { unsigned pid; unsigned long long used; unsigned gi, ci; } NvProcInfo2;
typedef struct { unsigned pid; unsigned long long used; } NvProcInfo1;
typedef int (*NvProcs)(void *, unsigned *, void *);

#define NVML_SUCCESS 0
#define NVML_ERROR_INSUFFICIENT_SIZE 7
#define NVML_VALUE_NOT_AVAILABLE (~0ULL)

static struct {
    void *lib;
    gboolean tried, ok;
    NvCount count;
    NvHandle handle;
    NvName name;
    NvPciInfo pci;
    NvUtilRates util;
    NvMemInfo mem;
    NvProcUtilFn proc_util;
    NvProcs gfx_procs, compute_procs;
    gboolean procs_v1;      /* only the old 16-byte process info is there */
} nv;

static GHashTable *nv_procs;    /* pid -> NvProcStat* */
typedef struct { double util, mem; } NvProcStat;
static unsigned long long nv_last_ts;

static gboolean nvml_load(void)
{
    if (nv.tried)
        return nv.ok;
    nv.tried = TRUE;
    nv.lib = dlopen("libnvidia-ml.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!nv.lib)
        return FALSE;
    NvInit init = (NvInit)dlsym(nv.lib, "nvmlInit_v2");
    nv.count = (NvCount)dlsym(nv.lib, "nvmlDeviceGetCount_v2");
    nv.handle = (NvHandle)dlsym(nv.lib, "nvmlDeviceGetHandleByIndex_v2");
    nv.name = (NvName)dlsym(nv.lib, "nvmlDeviceGetName");
    nv.pci = (NvPciInfo)dlsym(nv.lib, "nvmlDeviceGetPciInfo_v3");
    if (!nv.pci)
        nv.pci = (NvPciInfo)dlsym(nv.lib, "nvmlDeviceGetPciInfo_v2");
    nv.util = (NvUtilRates)dlsym(nv.lib, "nvmlDeviceGetUtilizationRates");
    nv.mem = (NvMemInfo)dlsym(nv.lib, "nvmlDeviceGetMemoryInfo");
    nv.proc_util = (NvProcUtilFn)dlsym(nv.lib, "nvmlDeviceGetProcessUtilization");
    nv.gfx_procs = (NvProcs)dlsym(nv.lib, "nvmlDeviceGetGraphicsRunningProcesses_v3");
    nv.compute_procs = (NvProcs)dlsym(nv.lib, "nvmlDeviceGetComputeRunningProcesses_v3");
    if (!nv.gfx_procs) {
        nv.gfx_procs = (NvProcs)dlsym(nv.lib, "nvmlDeviceGetGraphicsRunningProcesses_v2");
        nv.compute_procs = (NvProcs)dlsym(nv.lib, "nvmlDeviceGetComputeRunningProcesses_v2");
    }
    if (!nv.gfx_procs) {
        nv.gfx_procs = (NvProcs)dlsym(nv.lib, "nvmlDeviceGetGraphicsRunningProcesses");
        nv.compute_procs = (NvProcs)dlsym(nv.lib, "nvmlDeviceGetComputeRunningProcesses");
        nv.procs_v1 = TRUE;
    }
    if (!init || !nv.count || !nv.handle || !nv.pci || init() != NVML_SUCCESS) {
        dlclose(nv.lib);
        nv.lib = NULL;
        return FALSE;
    }
    nv.ok = TRUE;
    return TRUE;
}

/* ---- discovery ------------------------------------------------------------ */

static char *read_line(const char *dir, const char *name)
{
    char *path = g_build_filename(dir, name, NULL), *data = NULL;
    if (g_file_get_contents(path, &data, NULL, NULL))
        g_strstrip(data);
    g_free(path);
    return data;
}

static double read_num(const char *dir, const char *name, gboolean *ok)
{
    char *s = read_line(dir, name);
    double v = s ? g_ascii_strtod(s, NULL) : 0;
    if (ok)
        *ok = s != NULL;
    g_free(s);
    return v;
}

/* the device's name in pci.ids */
static gboolean pci_ids_name(unsigned vendor, unsigned device, char *out, size_t outsz)
{
    static const char *const files[] = {"/usr/share/misc/pci.ids", "/usr/share/hwdata/pci.ids",
                                        "/usr/share/pci.ids"};
    for (size_t i = 0; i < G_N_ELEMENTS(files); i++) {
        FILE *f = fopen(files[i], "re");
        if (!f)
            continue;
        char line[512];
        gboolean in_vendor = FALSE, found = FALSE;
        while (fgets(line, sizeof(line), f)) {
            if (line[0] == '#' || line[0] == '\n')
                continue;
            if (line[0] != '\t') {
                if (in_vendor)
                    break;
                in_vendor = strtoul(line, NULL, 16) == vendor && line[4] == ' ';
            } else if (in_vendor && line[1] != '\t' && strtoul(line + 1, NULL, 16) == device) {
                /* "TU117 [GeForce GTX 1650]": the marketing name, without the chip's */
                char *name = g_strstrip(line + 6), *lb = strchr(name, '['), *rb = strrchr(name, ']');
                if (lb && rb > lb + 1) {
                    *rb = '\0';
                    name = lb + 1;
                }
                g_strlcpy(out, name, outsz);
                found = TRUE;
                break;
            }
        }
        fclose(f);
        if (found)
            return TRUE;
    }
    return FALSE;
}

/* the boot GPU first, then by PCI address */
static gint by_pci(gconstpointer a, gconstpointer b)
{
    const TskGpu *x = *(TskGpu *const *)a, *y = *(TskGpu *const *)b;
    if (x->boot_vga != y->boot_vga)
        return x->boot_vga ? -1 : 1;
    return strcmp(x->pci, y->pci);
}

void tsk_gpu_init(void)
{
    gpus = g_ptr_array_new();
    nv_procs = g_hash_table_new_full(NULL, NULL, NULL, g_free);
    GDir *d = g_dir_open("/sys/class/drm", 0, NULL);
    if (!d)
        return;
    const char *n;
    while ((n = g_dir_read_name(d))) {
        if (strncmp(n, "card", 4) || !g_ascii_isdigit(n[4]) || strchr(n, '-'))
            continue;
        char *link = g_build_filename("/sys/class/drm", n, "device", NULL);
        char *real = realpath(link, NULL);
        g_free(link);
        if (!real)
            continue;
        char *pci = g_path_get_basename(real);
        if (tsk_gpu_find(pci) || strlen(pci) != 12) {   /* not a PCI device (a virtual card) */
            g_free(pci);
            free(real);
            continue;
        }
        TskGpu *g = g_new0(TskGpu, 1);
        g_strlcpy(g->pci, pci, sizeof(g->pci));
        g_strlcpy(g->devdir, real, sizeof(g->devdir));
        g_free(pci);
        free(real);
        char *drv_link = g_build_filename(g->devdir, "driver", NULL);
        char *drv = g_file_read_link(drv_link, NULL);
        if (drv) {
            char *base = g_path_get_basename(drv);
            g_strlcpy(g->driver, base, sizeof(g->driver));
            g_free(base);
            g_free(drv);
        }
        g_free(drv_link);
        char *vs = read_line(g->devdir, "vendor"), *ds = read_line(g->devdir, "device");
        unsigned vendor = vs ? (unsigned)strtoul(vs, NULL, 16) : 0, device = ds ? (unsigned)strtoul(ds, NULL, 16) : 0;
        g_free(vs);
        g_free(ds);
        g->kind = !strcmp(g->driver, "amdgpu") ? TSK_GPU_AMD : !strcmp(g->driver, "nvidia") ? TSK_GPU_NVIDIA
                                                                                             : TSK_GPU_OTHER;
        if (!pci_ids_name(vendor, device, g->name, sizeof(g->name)))
            g_snprintf(g->name, sizeof(g->name), "GPU %s (%s)", g->pci, *g->driver ? g->driver : "?");
        g->boot_vga = read_num(g->devdir, "boot_vga", NULL) > 0;
        if (g->kind == TSK_GPU_AMD) {
            read_num(g->devdir, "gpu_busy_percent", &g->has_busy);
            g->vram_total = read_num(g->devdir, "mem_info_vram_total", &g->has_vram);
            g->gtt_total = read_num(g->devdir, "mem_info_gtt_total", &g->has_gtt);
        } else if (g->kind == TSK_GPU_NVIDIA) {
            /* NVML numbers are there whenever the library is */
            g->has_busy = g->has_vram = TRUE;
        }
        g_ptr_array_add(gpus, g);
    }
    g_dir_close(d);
    g_ptr_array_sort(gpus, by_pci);
}

int tsk_gpu_count(void)
{
    return gpus ? (int)gpus->len : 0;
}

TskGpu *tsk_gpu_get(int i)
{
    return gpus->pdata[i];
}

TskGpu *tsk_gpu_find(const char *pci)
{
    for (guint i = 0; gpus && i < gpus->len; i++)
        if (!strcmp(((TskGpu *)gpus->pdata[i])->pci, pci))
            return gpus->pdata[i];
    return NULL;
}

static gboolean suspended(TskGpu *g)
{
    char *s = read_line(g->devdir, "power/runtime_status");
    gboolean r = s && !strcmp(s, "suspended");
    g_free(s);
    return r;
}

/* the NVML handle of `g`, matched by PCI address; NULL when there is none */
static void *nv_device(TskGpu *g)
{
    if (g->nvml || !nvml_load())
        return g->nvml;
    unsigned n = 0;
    if (nv.count(&n) != NVML_SUCCESS)
        return NULL;
    unsigned dom, bus, dev, fn;
    if (sscanf(g->pci, "%x:%x:%x.%x", &dom, &bus, &dev, &fn) != 4)
        return NULL;
    for (unsigned i = 0; i < n; i++) {
        void *h;
        NvPci info;
        if (nv.handle(i, &h) != NVML_SUCCESS || nv.pci(h, &info) != NVML_SUCCESS)
            continue;
        if (info.domain == dom && info.bus == bus && info.device == dev) {
            g->nvml = h;
            char name[96];
            if (nv.name && nv.name(h, name, sizeof(name)) == NVML_SUCCESS)
                g_strlcpy(g->name, name, sizeof(g->name));
            break;
        }
    }
    return g->nvml;
}

void tsk_gpu_sample(void)
{
    for (guint i = 0; gpus && i < gpus->len; i++) {
        TskGpu *g = gpus->pdata[i];
        if (!g->wanted)
            continue;
        g->suspended = suspended(g);
        if (g->suspended) {
            g->busy = 0;
            continue;
        }
        if (g->kind == TSK_GPU_AMD) {
            g->busy = read_num(g->devdir, "gpu_busy_percent", NULL);
            g->vram_used = read_num(g->devdir, "mem_info_vram_used", NULL);
            g->gtt_used = read_num(g->devdir, "mem_info_gtt_used", NULL);
        } else if (g->kind == TSK_GPU_NVIDIA) {
            void *h = nv_device(g);
            if (!h) {
                g->has_busy = g->has_vram = FALSE;
                continue;
            }
            NvUtil u;
            NvMem m;
            if (nv.util && nv.util(h, &u) == NVML_SUCCESS)
                g->busy = u.gpu;
            if (nv.mem && nv.mem(h, &m) == NVML_SUCCESS) {
                g->vram_used = (double)m.used;
                g->vram_total = (double)m.total;
            }
        }
    }
}

/* ---- NVIDIA per process ------------------------------------------------------ */

static NvProcStat *nv_stat(pid_t pid)
{
    NvProcStat *s = g_hash_table_lookup(nv_procs, GINT_TO_POINTER(pid));
    if (!s) {
        s = g_new0(NvProcStat, 1);
        s->util = s->mem = -1;
        g_hash_table_insert(nv_procs, GINT_TO_POINTER(pid), s);
    }
    return s;
}

static void nv_add_procs(void *h, NvProcs fn)
{
    if (!fn)
        return;
    unsigned n = 64;
    size_t stride = nv.procs_v1 ? sizeof(NvProcInfo1) : sizeof(NvProcInfo2);
    char *buf = g_malloc0(stride * n);
    int r = fn(h, &n, buf);
    if (r == NVML_ERROR_INSUFFICIENT_SIZE) {
        buf = g_realloc(buf, stride * n);
        r = fn(h, &n, buf);
    }
    if (r == NVML_SUCCESS)
        for (unsigned i = 0; i < n; i++) {
            const NvProcInfo1 *p = (const NvProcInfo1 *)(buf + i * stride);   /* pid, used lead both */
            NvProcStat *s = nv_stat((pid_t)p->pid);
            if (p->used != NVML_VALUE_NOT_AVAILABLE)
                s->mem = MAX(s->mem, 0) + (double)p->used;
            else if (s->util < 0)
                s->util = 0;   /* listed: it is on the GPU, memory just unknown */
        }
    g_free(buf);
}

void tsk_gpu_sample_procs(void)
{
    g_hash_table_remove_all(nv_procs);
    unsigned long long newest = nv_last_ts;
    for (guint i = 0; gpus && i < gpus->len; i++) {
        TskGpu *g = gpus->pdata[i];
        if (g->kind != TSK_GPU_NVIDIA || suspended(g))
            continue;
        void *h = nv_device(g);
        if (!h)
            continue;
        nv_add_procs(h, nv.gfx_procs);
        nv_add_procs(h, nv.compute_procs);
        if (!nv.proc_util)
            continue;
        unsigned n = 0;
        if (nv.proc_util(h, NULL, &n, nv_last_ts) != NVML_ERROR_INSUFFICIENT_SIZE || !n)
            continue;
        NvProcUtil *s = g_new0(NvProcUtil, n);
        if (nv.proc_util(h, s, &n, nv_last_ts) == NVML_SUCCESS)
            for (unsigned k = 0; k < n; k++) {
                NvProcStat *st = nv_stat((pid_t)s[k].pid);
                st->util = MAX(st->util, (double)s[k].sm);
                newest = MAX(newest, s[k].ts);
            }
        g_free(s);
    }
    nv_last_ts = newest;
}

gboolean tsk_gpu_nv_proc(pid_t pid, double *util, double *mem)
{
    NvProcStat *s = nv_procs ? g_hash_table_lookup(nv_procs, GINT_TO_POINTER(pid)) : NULL;
    if (!s)
        return FALSE;
    *util = s->util;
    *mem = s->mem;
    return TRUE;
}
