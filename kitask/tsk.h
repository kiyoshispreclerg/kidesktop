/* tsk.h - kitask: process list and system monitor for KiDesktop. */
#ifndef TSK_H
#define TSK_H

#include <gtk/gtk.h>
#include <sys/types.h>

#define KITASK_VERSION "0.1.3"

/* ---- what a refresh has to read (tsk_proc.c) -------------------------------
 * The cheap /proc files are always read; each flag below costs extra work per
 * process and is only set while a visible column needs it. */
enum {
    TSK_NEED_SMAPS = 1 << 0,    /* smaps_rollup: exclusive, proportional, swap */
    TSK_NEED_IO = 1 << 1,       /* /proc/PID/io */
    TSK_NEED_NET = 1 << 2,      /* fd scan + sock_diag */
    TSK_NEED_GPU = 1 << 3,      /* fd scan + DRM fdinfo, NVML */
    TSK_NEED_X11 = 1 << 4,      /* XRes pixmap bytes */
    TSK_NEED_TITLE = 1 << 5,    /* _NET_CLIENT_LIST titles */
};

typedef struct {
    pid_t pid, ppid;
    uid_t uid;
    char state;
    int nice, threads;
    gboolean kthread;
    unsigned long long start;   /* ticks after boot: tells a reused PID apart */
    unsigned long long ticks;   /* utime + stime */
    char comm[32];
    char *name;                 /* comm, or argv[0]'s base name when comm is cut */
    char *cmdline;              /* argv joined by spaces; "[comm]" for kernel threads */
    double cpu;                 /* % of one core over the last interval */
    double vsize, rss, shared;  /* bytes */
    double uss, pss, swap;      /* bytes; -1 when unreadable */
    double io_read, io_write;   /* bytes since the start; -1 when unreadable */
    double io_read_rate, io_write_rate;     /* bytes/s; -1 when unreadable */
    double net_rx_rate, net_tx_rate;        /* bytes/s over TCP; -1 when unreadable */
    double gpu;                 /* %, -1 when not using a GPU */
    double gpu_mem;             /* bytes, -1 when not using a GPU */
    double x11_mem;             /* bytes of X pixmaps; -1 when not an X client */
    char *title;                /* first window's title, NULL without one */
    unsigned long window;       /* that window, 0 without one */

    /* sampler state */
    gboolean seen, ran;
    unsigned long long rss_pages;
    unsigned long long smaps_rss;   /* rss_pages when smaps_rollup was last read */
    gint64 smaps_time;
    GArray *sockets;            /* guint64 socket inodes from the last fd scan */
    GArray *dri_fds;            /* int, fds open on /dev/dri */
    GHashTable *gpu_prev;       /* "client/engine" -> guint64 ns, drm-engine-* deltas */
    gboolean fds_scanned;
} TskProc;

typedef struct {
    GHashTable *procs;          /* GINT_TO_POINTER(pid) -> TskProc* */
    gint64 last_time;           /* monotonic µs of the last sample, 0 before the first */
    double elapsed;             /* seconds between the last two samples */
    unsigned long long boot_time;   /* seconds since the epoch */
    long hz, page;
    int ncpu;
} TskProcs;

void tsk_procs_init(TskProcs *p);
void tsk_procs_sample(TskProcs *p, unsigned need);
const char *tsk_user_name(uid_t uid);
/* a whole /proc/PID/<name> file, NULL when unreadable (g_free it) */
char *tsk_proc_read_file(pid_t pid, const char *name, gsize *len);

/* ---- system-wide numbers (tsk_sys.c) ---------------------------------------- */

typedef struct {
    int ncpu;
    double cpu;                 /* % of all cores */
    double *core;               /* % per core, ncpu entries */
    double mem_total, mem_free, mem_available, buffers, cached, sreclaimable, shmem;
    double swap_total, swap_free;
    double net_rx, net_tx;      /* bytes/s, all interfaces but lo */
    double disk_read, disk_write;   /* bytes/s, whole physical disks */

    /* previous counters */
    unsigned long long *prev_busy, *prev_total;     /* ncpu + 1 entries, [0] is the total */
    unsigned long long prev_rx, prev_tx, prev_rd, prev_wr;
    gint64 prev_time;
} TskSys;

void tsk_sys_init(TskSys *s);
void tsk_sys_sample(TskSys *s);

/* ---- GPUs (tsk_gpu.c) ---------------------------------------------------- */

typedef enum { TSK_GPU_AMD, TSK_GPU_NVIDIA, TSK_GPU_OTHER } TskGpuKind;

typedef struct {
    char pci[16];               /* "0000:08:00.0"; graph sources name the GPU by it */
    char name[96];
    char driver[24];
    TskGpuKind kind;
    char devdir[160];           /* /sys/bus/pci/devices/<pci> */
    gboolean has_busy, has_vram, has_gtt;
    double busy;                /* % */
    double vram_used, vram_total, gtt_used, gtt_total;  /* bytes */
    gboolean suspended;         /* runtime-suspended: not queried, so not woken up */
    gboolean boot_vga;          /* the GPU the firmware (and usually X) started on */
    gboolean wanted;            /* a shown graph reads it: only those are sampled */
    void *nvml;                 /* nvmlDevice_t */
} TskGpu;

void tsk_gpu_init(void);
int tsk_gpu_count(void);
TskGpu *tsk_gpu_get(int i);
TskGpu *tsk_gpu_find(const char *pci);
void tsk_gpu_sample(void);
/* NVIDIA per-process numbers: refreshed by tsk_gpu_sample_procs(), read with
 * tsk_gpu_nv_proc() (FALSE when the PID isn't using an NVIDIA GPU) */
void tsk_gpu_sample_procs(void);
gboolean tsk_gpu_nv_proc(pid_t pid, double *util, double *mem);

/* ---- per-process TCP traffic (tsk_net.c) ------------------------------------ */

/* Dumps every TCP socket's byte counters; tsk_net_bytes() then answers for an inode. */
void tsk_net_sample(void);
gboolean tsk_net_bytes(guint64 inode, guint64 *rx, guint64 *tx);
/* rx/tx deltas of `inode` since the previous dump; a socket new since then counts from 0 */
void tsk_net_delta(guint64 inode, double *rx, double *tx);

/* ---- X11: pixmap memory and window titles (tsk_x11.c) ----------------------- */

void tsk_x11_sample(unsigned need);
double tsk_x11_mem(pid_t pid);                  /* -1 when not an X client */
const char *tsk_x11_title(pid_t pid, unsigned long *window);   /* NULL without a window */
void tsk_x11_activate(unsigned long window);
int tsk_x11_window_count(pid_t pid);

/* ---- configuration (tsk_config.c) ------------------------------------------- */

typedef enum { TSK_UNIT_PCT, TSK_UNIT_BYTES, TSK_UNIT_RATE } TskUnit;

typedef struct {
    char *source;               /* "cpu", "cpu.3", "ram.apps", "vram@0000:08:00.0", ... */
    double min, max;            /* in the source's config unit: %, MiB or KiB/s */
    gboolean auto_max;          /* the source's own maximum (or the history's peak) */
    GdkColor color;
    /* runtime */
    double *hist;               /* ring of values in base units */
    int hist_len, hist_pos, hist_n;
    GtkWidget *area;
} TskGraph;

typedef struct {
    char key[16];
    int width;                  /* 0: automatic */
} TskColumnCfg;

typedef enum { TSK_SCOPE_ALL, TSK_SCOPE_MINE, TSK_SCOPE_WINDOWS } TskScope;

typedef struct {
    double list_interval, graph_interval;   /* seconds */
    gboolean show_graphs;
    int graph_height, graphs_per_row;
    gboolean confirm_signals, hide_kthreads;
    TskScope scope;
    char sort_key[16];
    gboolean sort_desc;
    GArray *columns;            /* TskColumnCfg, visible ones in order */
    GPtrArray *graphs;          /* TskGraph* */
    int win_w, win_h, details_w, details_h;
} TskConfig;

void tsk_config_load(TskConfig *c);
void tsk_config_save(const TskConfig *c);
TskGraph *tsk_graph_new(const char *source);
void tsk_graph_free(TskGraph *g);

/* ---- graph sources (tsk_graph.c) -------------------------------------------- */

/* Every source this machine has: fills `keys` (char*, owned) for the settings' list. */
GPtrArray *tsk_graph_sources(void);
char *tsk_graph_source_label(const char *source);
TskUnit tsk_graph_source_unit(const char *source);
/* the source's default color and limits for a new graph */
void tsk_graph_source_defaults(const char *source, GdkColor *color, double *max, gboolean *auto_max);
gboolean tsk_graph_source_valid(const char *source);

typedef struct TskApp TskApp;

GtkWidget *tsk_graphs_new(TskApp *app);
void tsk_graphs_rebuild(TskApp *app);
void tsk_graphs_push(TskApp *app);     /* appends the latest TskSys/TskGpu values */

/* ---- columns and list (tsk_list.c) ------------------------------------------ */

typedef enum { TSK_FMT_TEXT, TSK_FMT_INT, TSK_FMT_PCT, TSK_FMT_BYTES, TSK_FMT_RATE, TSK_FMT_TIME, TSK_FMT_DATE } TskFmt;

typedef struct {
    const char *key;
    const char *title;          /* N_() */
    const char *tip;            /* N_(), header tooltip */
    TskFmt fmt;
    unsigned need;
} TskColumnDef;

extern const int tsk_ncolumns;
int tsk_column_index(const char *key);      /* -1 for an unknown key */
const TskColumnDef *tsk_column(int i);

GtkWidget *tsk_list_new(TskApp *app);
void tsk_list_update(TskApp *app);
void tsk_list_rebuild_columns(TskApp *app);
void tsk_list_refilter(TskApp *app);
GList *tsk_list_selected_pids(TskApp *app);   /* GINT_TO_POINTER(pid) */
unsigned tsk_list_need(TskApp *app);
void tsk_list_columns_menu(TskApp *app, GtkMenuShell *menu);

/* ---- actions (tsk_actions.c) -------------------------------------------------- */

void tsk_send_signal(TskApp *app, int sig);
void tsk_renice(TskApp *app);
void tsk_activate_window(TskApp *app);
GtkWidget *tsk_process_menu(TskApp *app);
void tsk_details(TskApp *app, pid_t pid);

/* ---- settings dialog (tsk_prefs.c) -------------------------------------------- */

void tsk_prefs_show(TskApp *app, int page);

/* ---- usage pie chart (tsk_pie.c) ---------------------------------------------- */

enum { TSK_PIE_CPU, TSK_PIE_RAM };
void tsk_pie_show(TskApp *app, int kind);

/* ---- the app (kitask.c) ------------------------------------------------------- */

struct TskApp {
    TskConfig cfg;
    TskProcs procs;
    TskSys sys;
    GtkWidget *win, *graphs_box, *graphs_table, *list, *filter, *scope, *status;
    GtkWidget *show_graphs_item;
    GtkListStore *store;
    GtkTreeModel *filter_model, *sort_model;
    GHashTable *rows;           /* pid -> its row in store (tsk_list.c) */
    guint list_timer, graph_timer, save_timer;
    gboolean list_paused;
    unsigned extra_need;        /* sampler needs of an open pie chart, on top of the columns' */
    gboolean pie_open;          /* its timer drives the sampler: the list's stands aside */
};

void tsk_app_restart_timers(TskApp *app);
void tsk_app_save_soon(TskApp *app);
void tsk_app_refresh_now(TskApp *app);
void tsk_app_apply_graphs_visible(TskApp *app);

/* formatting shared by the list, the graphs and the details */
char *tsk_fmt_bytes(double b);
char *tsk_fmt_rate(double b);

#endif /* TSK_H */
