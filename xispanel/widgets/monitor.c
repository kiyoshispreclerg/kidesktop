/*
 * monitor widget - one system reading (CPU%, RAM%, a temperature, ...)
 * shown as a number or a bar. One widget instance monitors exactly one
 * thing, on purpose: several `WIDGET ... monitor` lines side by side is
 * how you build a row of readings, each with its own metric, style,
 * width, color and refresh interval, instead of one widget with a
 * mini-language for describing a whole dashboard.
 *
 * Cost: every source here is a plain read() of a small procfs/sysfs file
 * -- no sampling thread, no external process, no libraries. The two
 * multi-value files (/proc/stat, /proc/meminfo) are parsed once per
 * refresh into a snapshot shared by *every* monitor widget on every
 * panel (see stat_snapshot()/meminfo_snapshot()), so ten CPU-core
 * widgets still read /proc/stat once, not ten times. Sensor paths under
 * /sys/class/hwmon and /sys/class/drm are resolved once at init and
 * remembered, so no directory scanning happens per tick. And, like
 * clock.c, a tick only marks the panel dirty when the *displayed* text
 * or bar length actually changed, so a steady reading costs no repaint.
 *
 * Options:
 *   metric=cpu|ram|swap|gpu|vram|cpu_temp|gpu_temp   (default cpu)
 *   index=<n>     which core (cpu), card (gpu/vram/gpu_temp) or hwmon
 *                 temp input (cpu_temp/gpu_temp); unset = total for cpu,
 *                 average across cards/inputs for the rest
 *   style=text|bar|both    (default text)
 *   orientation=vertical|horizontal   bar direction (default vertical)
 *   width=<px>    widget length along the panel (default: fits the text
 *                 for text/both, 8px for a vertical bar, 3x thickness for
 *                 a horizontal one)
 *   interval=<ms> refresh period (default 1000)
 *   color=#RRGGBB[AA]      bar fill (default: the panel's fg)
 *   track_color=#RRGGBB[AA]  bar background (default: fg at low alpha)
 *   high=<value>  reading at or above this switches the bar/text to
 *                 high_color -- percent for the % metrics, degrees for
 *                 the temperatures
 *   high_color=#RRGGBB[AA]   (default #e05050)
 *   label=<text>  short prefix drawn before the value ("CPU 42%")
 *   font_size=<px>  text size, default the panel's own
 *   hwmon=<name>  override the hwmon chip name for the temperature
 *                 metrics (default: the first known CPU/GPU chip found)
 *
 * Not covered on purpose: NVIDIA GPUs expose no busy/VRAM/temperature
 * sysfs interface of this shape (it takes NVML or shelling out to
 * nvidia-smi, i.e. a process spawn per tick), so gpu/vram/gpu_temp only
 * work on the amdgpu/i915-style interfaces read here.
 */
#include "../xispanel.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MONITOR_MAX_CPUS 64
#define MONITOR_MAX_CARDS 8
#define MONITOR_MAX_TEMP_INPUTS 16
/* Shared snapshots are re-read at most this often no matter how many
 * widgets ask -- a floor on the procfs traffic when several monitors are
 * configured with short intervals. */
#define MONITOR_SNAPSHOT_MIN_MS 200

enum monitor_metric {
    METRIC_CPU,
    METRIC_RAM,
    METRIC_SWAP,
    METRIC_GPU,
    METRIC_VRAM,
    METRIC_CPU_TEMP,
    METRIC_GPU_TEMP,
};

enum monitor_style { STYLE_TEXT, STYLE_BAR, STYLE_BOTH };

typedef struct {
    enum monitor_metric metric;
    enum monitor_style style;
    int vertical;
    int index; /* -1 = total/average */
    int width_cfg;
    int interval_ms;
    double font_size;
    char label[32];
    char hwmon_name[32];

    int has_color, has_track, has_high_color;
    double col_r, col_g, col_b, col_a;
    double trk_r, trk_g, trk_b, trk_a;
    double hi_r, hi_g, hi_b, hi_a;
    double high; /* 0 = no threshold configured */

    /* Resolved once, then reused every tick -- see the file comment. */
    char sensor_path[PATH_MAX];      /* single temp input, or "" */
    char sensor_dir[PATH_MAX];       /* hwmon dir when averaging inputs */
    char card_path[PATH_MAX];        /* /sys/class/drm/cardN/device, or "" */
    int resolved;

    double value;     /* last reading: percent, or degrees for the temps */
    int has_value;
    char text[32];    /* what's currently drawn -- the repaint trigger */
} MonitorPriv;

/* ------------------------------------------------------------------ */
/* shared /proc snapshots                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    uint64_t total[MONITOR_MAX_CPUS + 1]; /* [0] = aggregate "cpu" line, [i+1] = cpu<i> */
    uint64_t idle[MONITOR_MAX_CPUS + 1];
    int n;
} CpuSample;

static CpuSample g_cpu_prev, g_cpu_cur;
static int g_cpu_have_prev;
static uint64_t g_cpu_at_ms;

static uint64_t g_mem_total, g_mem_available, g_swap_total, g_swap_free;
static uint64_t g_mem_at_ms;

/* Reads a whole small file into `buf`. Returns the byte count (0 on any
 * failure), always NUL-terminating. Sized for procfs/sysfs files, which
 * are generated on read and never grow unboundedly here. */
static size_t read_file(const char *path, char *buf, size_t bufsz)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        return 0;
    }
    size_t n = fread(buf, 1, bufsz - 1, f);
    fclose(f);
    buf[n] = 0;
    return n;
}

/* Reads a file holding a single integer (the shape of nearly every sysfs
 * sensor file). Returns 0 if it couldn't be read/parsed. */
static int read_int_file(const char *path, long long *out)
{
    char buf[64];
    if (!read_file(path, buf, sizeof(buf))) {
        return 0;
    }
    char *end = NULL;
    long long v = strtoll(buf, &end, 10);
    if (end == buf) {
        return 0;
    }
    *out = v;
    return 1;
}

/* Refreshes the shared /proc/stat sample if it's older than
 * MONITOR_SNAPSHOT_MIN_MS. CPU percentages are a *delta* between two
 * samples, so the first call after startup can only report 0 -- the
 * second tick onwards is real. */
static void stat_snapshot(uint64_t now)
{
    if (g_cpu_at_ms && now - g_cpu_at_ms < MONITOR_SNAPSHOT_MIN_MS) {
        return;
    }
    char buf[8192];
    if (!read_file("/proc/stat", buf, sizeof(buf))) {
        return;
    }
    CpuSample s;
    memset(&s, 0, sizeof(s));
    for (char *line = buf; line && *line;) {
        char *nl = strchr(line, '\n');
        if (nl) {
            *nl = 0;
        }
        if (!strncmp(line, "cpu", 3)) {
            int slot = -1;
            if (line[3] == ' ') {
                slot = 0;
            } else if (isdigit((unsigned char)line[3])) {
                int cpu = atoi(line + 3);
                if (cpu >= 0 && cpu < MONITOR_MAX_CPUS) {
                    slot = cpu + 1;
                    if (cpu + 1 > s.n) {
                        s.n = cpu + 1;
                    }
                }
            }
            if (slot >= 0) {
                /* user nice system idle iowait irq softirq steal ... */
                const char *p = strchr(line, ' ');
                uint64_t vals[10];
                int nv = 0;
                while (p && nv < 10) {
                    while (*p == ' ') {
                        p++;
                    }
                    if (!*p) {
                        break;
                    }
                    vals[nv++] = strtoull(p, (char **)&p, 10);
                }
                uint64_t total = 0;
                for (int i = 0; i < nv; i++) {
                    total += vals[i];
                }
                /* idle + iowait: time the CPU wasn't doing work. */
                uint64_t idle = (nv > 3 ? vals[3] : 0) + (nv > 4 ? vals[4] : 0);
                s.total[slot] = total;
                s.idle[slot] = idle;
            }
        } else if (strncmp(line, "cpu", 3) != 0) {
            /* /proc/stat lists every cpu line before anything else --
             * stop as soon as they're done rather than parsing the rest
             * of the file (intr, ctxt, ... are long and unused here). */
            if (line != buf) {
                break;
            }
        }
        line = nl ? nl + 1 : NULL;
    }
    g_cpu_prev = g_cpu_cur;
    g_cpu_have_prev = g_cpu_at_ms != 0;
    g_cpu_cur = s;
    g_cpu_at_ms = now;
}

/* CPU busy percent for `index` (-1 = the whole machine), from the delta
 * between the two most recent shared samples. -1 if there's no usable
 * delta yet (first tick) or that core doesn't exist. */
static double cpu_percent(int index)
{
    int slot = index < 0 ? 0 : index + 1;
    if (!g_cpu_have_prev || slot < 0 || slot > MONITOR_MAX_CPUS) {
        return -1;
    }
    uint64_t dt = g_cpu_cur.total[slot] - g_cpu_prev.total[slot];
    uint64_t di = g_cpu_cur.idle[slot] - g_cpu_prev.idle[slot];
    if (g_cpu_cur.total[slot] < g_cpu_prev.total[slot] || dt == 0) {
        return -1;
    }
    if (di > dt) {
        di = dt;
    }
    return 100.0 * (double)(dt - di) / (double)dt;
}

static void meminfo_snapshot(uint64_t now)
{
    if (g_mem_at_ms && now - g_mem_at_ms < MONITOR_SNAPSHOT_MIN_MS) {
        return;
    }
    char buf[4096];
    if (!read_file("/proc/meminfo", buf, sizeof(buf))) {
        return;
    }
    g_mem_at_ms = now;
    for (char *line = buf; line && *line;) {
        char *nl = strchr(line, '\n');
        if (nl) {
            *nl = 0;
        }
        unsigned long long v = 0;
        const char *colon = strchr(line, ':');
        if (colon) {
            v = strtoull(colon + 1, NULL, 10); /* kB */
        }
        if (!strncmp(line, "MemTotal:", 9)) {
            g_mem_total = v;
        } else if (!strncmp(line, "MemAvailable:", 13)) {
            g_mem_available = v;
        } else if (!strncmp(line, "SwapTotal:", 10)) {
            g_swap_total = v;
        } else if (!strncmp(line, "SwapFree:", 9)) {
            g_swap_free = v;
        }
        line = nl ? nl + 1 : NULL;
    }
}

/* ------------------------------------------------------------------ */
/* sysfs sensor discovery (once per widget, at first use)               */
/* ------------------------------------------------------------------ */

/* hwmon chip names that expose a CPU package/core temperature, and the
 * GPU ones, in preference order. A `hwmon=` option overrides both lists
 * for a machine whose chip isn't here. */
static const char *g_cpu_chips[] = {"k10temp", "zenpower", "coretemp", "cpu_thermal", "acpitz", NULL};
static const char *g_gpu_chips[] = {"amdgpu", "radeon", "i915", "nouveau", NULL};

static int name_in_list(const char *name, const char *const *list)
{
    for (int i = 0; list[i]; i++) {
        if (!strcmp(name, list[i])) {
            return 1;
        }
    }
    return 0;
}

/* Finds the hwmon directory of the wanted chip (an explicit `hwmon=`
 * name, else the first entry matching the CPU or GPU list). Leaves `out`
 * empty when nothing matches -- the widget then just draws "n/d". */
static void find_hwmon_dir(const char *want_name, int gpu, char *out, size_t outsz)
{
    out[0] = 0;
    DIR *d = opendir("/sys/class/hwmon");
    if (!d) {
        return;
    }
    struct dirent *de;
    while ((de = readdir(d))) {
        if (strncmp(de->d_name, "hwmon", 5) != 0) {
            continue;
        }
        char path[PATH_MAX], name[64];
        snprintf(path, sizeof(path), "/sys/class/hwmon/%s/name", de->d_name);
        if (!read_file(path, name, sizeof(name))) {
            continue;
        }
        size_t len = strlen(name);
        while (len && (name[len - 1] == '\n' || name[len - 1] == '\r')) {
            name[--len] = 0;
        }
        int match = want_name && want_name[0] ? !strcmp(name, want_name)
                                              : name_in_list(name, gpu ? g_gpu_chips : g_cpu_chips);
        if (match) {
            snprintf(out, outsz, "/sys/class/hwmon/%s", de->d_name);
            break;
        }
    }
    closedir(d);
}

/* Finds /sys/class/drm/card<N>/device for the GPU metrics: the `index`th
 * card that actually exposes `probe_file` (gpu_busy_percent or the VRAM
 * pair), or the first such card when index < 0. The connector entries
 * (card0-DP-1, ...) are skipped -- only the card itself carries these. */
static void find_drm_card(int index, const char *probe_file, char *out, size_t outsz)
{
    out[0] = 0;
    int seen = 0;
    for (int card = 0; card < MONITOR_MAX_CARDS; card++) {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "/sys/class/drm/card%d/device/%s", card, probe_file);
        long long dummy;
        if (!read_int_file(path, &dummy)) {
            continue;
        }
        if (index < 0 || index == seen || index == card) {
            snprintf(out, outsz, "/sys/class/drm/card%d/device", card);
            return;
        }
        seen++;
    }
}

static void monitor_resolve_sources(MonitorPriv *mp)
{
    mp->resolved = 1;
    mp->sensor_path[0] = mp->sensor_dir[0] = mp->card_path[0] = 0;

    switch (mp->metric) {
    case METRIC_CPU_TEMP:
    case METRIC_GPU_TEMP: {
        char dir[PATH_MAX];
        find_hwmon_dir(mp->hwmon_name, mp->metric == METRIC_GPU_TEMP, dir, sizeof(dir));
        if (!dir[0]) {
            return;
        }
        if (mp->index >= 0) {
            snprintf(mp->sensor_path, sizeof(mp->sensor_path), "%s/temp%d_input", dir, mp->index);
        } else {
            snprintf(mp->sensor_dir, sizeof(mp->sensor_dir), "%s", dir);
        }
        break;
    }
    case METRIC_GPU:
        find_drm_card(mp->index, "gpu_busy_percent", mp->card_path, sizeof(mp->card_path));
        break;
    case METRIC_VRAM:
        find_drm_card(mp->index, "mem_info_vram_total", mp->card_path, sizeof(mp->card_path));
        break;
    default:
        break;
    }
}

/* Average of every temp*_input in an hwmon directory (index unset), in
 * degrees; -1 if the chip has none. */
static double hwmon_average_temp(const char *dir)
{
    double sum = 0;
    int n = 0;
    for (int i = 1; i <= MONITOR_MAX_TEMP_INPUTS; i++) {
        char path[PATH_MAX];
        long long v;
        snprintf(path, sizeof(path), "%s/temp%d_input", dir, i);
        if (read_int_file(path, &v)) {
            sum += v / 1000.0;
            n++;
        }
    }
    return n ? sum / n : -1;
}

/* The widget's current reading -- percent for everything except the
 * temperatures, which are degrees Celsius. -1 means "unavailable"
 * (sensor missing, or no CPU delta yet). */
static double monitor_read(MonitorPriv *mp, uint64_t now)
{
    if (!mp->resolved) {
        monitor_resolve_sources(mp);
    }
    switch (mp->metric) {
    case METRIC_CPU:
        stat_snapshot(now);
        return cpu_percent(mp->index);
    case METRIC_RAM:
        meminfo_snapshot(now);
        if (!g_mem_total) {
            return -1;
        }
        return 100.0 * (double)(g_mem_total - g_mem_available) / (double)g_mem_total;
    case METRIC_SWAP:
        meminfo_snapshot(now);
        if (!g_swap_total) {
            return 0; /* no swap configured -- 0% is the honest reading */
        }
        return 100.0 * (double)(g_swap_total - g_swap_free) / (double)g_swap_total;
    case METRIC_GPU: {
        if (!mp->card_path[0]) {
            return -1;
        }
        char path[PATH_MAX];
        long long v;
        snprintf(path, sizeof(path), "%s/gpu_busy_percent", mp->card_path);
        return read_int_file(path, &v) ? (double)v : -1;
    }
    case METRIC_VRAM: {
        if (!mp->card_path[0]) {
            return -1;
        }
        char path[PATH_MAX];
        long long used, total;
        snprintf(path, sizeof(path), "%s/mem_info_vram_used", mp->card_path);
        if (!read_int_file(path, &used)) {
            return -1;
        }
        snprintf(path, sizeof(path), "%s/mem_info_vram_total", mp->card_path);
        if (!read_int_file(path, &total) || total <= 0) {
            return -1;
        }
        return 100.0 * (double)used / (double)total;
    }
    case METRIC_CPU_TEMP:
    case METRIC_GPU_TEMP: {
        if (mp->sensor_path[0]) {
            long long v;
            return read_int_file(mp->sensor_path, &v) ? v / 1000.0 : -1;
        }
        if (mp->sensor_dir[0]) {
            return hwmon_average_temp(mp->sensor_dir);
        }
        return -1;
    }
    }
    return -1;
}

static int metric_is_temp(enum monitor_metric m)
{
    return m == METRIC_CPU_TEMP || m == METRIC_GPU_TEMP;
}

/* ------------------------------------------------------------------ */
/* widget                                                              */
/* ------------------------------------------------------------------ */

static int monitor_init(PanelWidget *w)
{
    MonitorPriv *mp = w->priv;
    char buf[64];

    mp->metric = METRIC_CPU;
    if (kv_get(w->config_kv, "metric", buf, sizeof(buf))) {
        if (!strcmp(buf, "ram")) {
            mp->metric = METRIC_RAM;
        } else if (!strcmp(buf, "swap")) {
            mp->metric = METRIC_SWAP;
        } else if (!strcmp(buf, "gpu")) {
            mp->metric = METRIC_GPU;
        } else if (!strcmp(buf, "vram")) {
            mp->metric = METRIC_VRAM;
        } else if (!strcmp(buf, "cpu_temp")) {
            mp->metric = METRIC_CPU_TEMP;
        } else if (!strcmp(buf, "gpu_temp")) {
            mp->metric = METRIC_GPU_TEMP;
        } else if (strcmp(buf, "cpu") != 0) {
            fprintf(stderr, "xispanel: monitor: unknown metric '%s', using cpu\n", buf);
        }
    }

    mp->index = kv_get_int(w->config_kv, "index", -1);

    mp->style = STYLE_TEXT;
    if (kv_get(w->config_kv, "style", buf, sizeof(buf))) {
        if (!strcmp(buf, "bar")) {
            mp->style = STYLE_BAR;
        } else if (!strcmp(buf, "both")) {
            mp->style = STYLE_BOTH;
        }
    }
    mp->vertical = 1;
    if (kv_get(w->config_kv, "orientation", buf, sizeof(buf)) && !strcmp(buf, "horizontal")) {
        mp->vertical = 0;
    }

    mp->width_cfg = kv_get_int(w->config_kv, "width", 0);
    mp->interval_ms = kv_get_int(w->config_kv, "interval", 1000);
    if (mp->interval_ms < 100) {
        mp->interval_ms = 100; /* a monitor is not a profiler -- see the file comment */
    }
    if (kv_get(w->config_kv, "font_size", buf, sizeof(buf))) {
        mp->font_size = atof(buf);
    }
    kv_get(w->config_kv, "label", mp->label, sizeof(mp->label));
    kv_get(w->config_kv, "hwmon", mp->hwmon_name, sizeof(mp->hwmon_name));

    if (kv_get(w->config_kv, "color", buf, sizeof(buf))) {
        mp->has_color = parse_hex_color(buf, &mp->col_r, &mp->col_g, &mp->col_b, &mp->col_a);
    }
    if (kv_get(w->config_kv, "track_color", buf, sizeof(buf))) {
        mp->has_track = parse_hex_color(buf, &mp->trk_r, &mp->trk_g, &mp->trk_b, &mp->trk_a);
    }
    mp->hi_r = 0.88;
    mp->hi_g = 0.31;
    mp->hi_b = 0.31;
    mp->hi_a = 1.0;
    if (kv_get(w->config_kv, "high_color", buf, sizeof(buf))) {
        mp->has_high_color = parse_hex_color(buf, &mp->hi_r, &mp->hi_g, &mp->hi_b, &mp->hi_a);
    }
    if (kv_get(w->config_kv, "high", buf, sizeof(buf))) {
        mp->high = atof(buf);
    }

    snprintf(mp->text, sizeof(mp->text), "%s", "");
    w->next_tick_ms = now_ms();
    return 0;
}

/* The string drawn for the current reading: "n/d" when unavailable, else
 * the value with its unit, prefixed by `label=` when set. */
static void monitor_format(MonitorPriv *mp, char *out, size_t outsz)
{
    char value[24];
    if (!mp->has_value) {
        snprintf(value, sizeof(value), "n/d");
    } else if (metric_is_temp(mp->metric)) {
        snprintf(value, sizeof(value), "%.0f°C", mp->value);
    } else {
        snprintf(value, sizeof(value), "%.0f%%", mp->value);
    }
    if (mp->label[0]) {
        snprintf(out, outsz, "%s %s", mp->label, value);
    } else {
        snprintf(out, outsz, "%s", value);
    }
}

static int monitor_on_tick(PanelWidget *w, uint64_t now)
{
    MonitorPriv *mp = w->priv;
    w->next_tick_ms = now + mp->interval_ms;

    double v = monitor_read(mp, now);
    double old_value = mp->value;
    int had_value = mp->has_value;
    mp->has_value = v >= 0;
    mp->value = v >= 0 ? v : 0;

    char old_text[sizeof(mp->text)];
    snprintf(old_text, sizeof(old_text), "%s", mp->text);
    monitor_format(mp, mp->text, sizeof(mp->text));

    if (strcmp(old_text, mp->text) != 0) {
        return 1;
    }
    /* The text rounds to whole units, so a bar would otherwise sit frozen
     * between two integers -- repaint when the bar's own filled length
     * would move by a pixel or more. */
    if (mp->style != STYLE_TEXT && had_value == mp->has_value) {
        double span = mp->vertical ? w->thickness : (w->len > 0 ? w->len : w->thickness);
        double scale = metric_is_temp(mp->metric) ? (mp->high > 0 ? mp->high : 100.0) : 100.0;
        if (scale > 0 && span > 0 && (int)(old_value / scale * span) != (int)(mp->value / scale * span)) {
            return 1;
        }
    }
    return 0;
}

static double monitor_text_size(MonitorPriv *mp, const Panel *p)
{
    return mp->font_size > 0 ? mp->font_size : panel_text_size(p);
}

static void monitor_measure(PanelWidget *w, int cross_axis, int *out_len, int *out_min_len)
{
    MonitorPriv *mp = w->priv;
    Panel *p = w->panel;

    int len;
    if (mp->width_cfg > 0) {
        len = mp->width_cfg;
    } else if (mp->style == STYLE_BAR) {
        len = mp->vertical ? 8 : cross_axis * 3;
    } else {
        /* Measured off a full-width sample rather than the live text, so
         * the widget doesn't change width (shifting every widget beside
         * it) each time the reading crosses 9 -> 10 -> 100. */
        char sample[48];
        if (mp->label[0]) {
            snprintf(sample, sizeof(sample), "%s %s", mp->label, metric_is_temp(mp->metric) ? "100°C" : "100%");
        } else {
            snprintf(sample, sizeof(sample), "%s", metric_is_temp(mp->metric) ? "100°C" : "100%");
        }
        double tw;
        pango_text_extents_ellipsized(p->cr, sample, monitor_text_size(mp, p), 0, &tw, NULL);
        len = (int)tw + 8;
        if (mp->style == STYLE_BOTH) {
            len += mp->vertical ? 10 : 0;
        }
    }
    if (len < 4) {
        len = 4;
    }
    *out_len = len;
    *out_min_len = len;
}

/* Fraction of the bar to fill, 0..1. Percent metrics are out of 100;
 * a temperature is out of `high=` when set (so the bar means "how close
 * to the limit I care about"), else a flat 100 degrees. */
static double monitor_fill_fraction(const MonitorPriv *mp)
{
    if (!mp->has_value) {
        return 0;
    }
    double scale = metric_is_temp(mp->metric) ? (mp->high > 0 ? mp->high : 100.0) : 100.0;
    double f = scale > 0 ? mp->value / scale : 0;
    return f < 0 ? 0 : (f > 1 ? 1 : f);
}

static void monitor_paint(PanelWidget *w, cairo_t *cr)
{
    MonitorPriv *mp = w->priv;
    Panel *p = w->panel;
    int x, y, width, height;
    widget_get_rect(w, &x, &y, &width, &height);
    widget_paint_hover_bg(w, cr);

    int over_high = mp->has_value && mp->high > 0 && mp->value >= mp->high;
    double fr = over_high ? mp->hi_r : (mp->has_color ? mp->col_r : p->fg_r);
    double fg = over_high ? mp->hi_g : (mp->has_color ? mp->col_g : p->fg_g);
    double fb = over_high ? mp->hi_b : (mp->has_color ? mp->col_b : p->fg_b);
    double fa = over_high ? mp->hi_a : (mp->has_color ? mp->col_a : p->fg_a);

    int bar_x = x, bar_y = y + 3, bar_w = width, bar_h = height - 6;
    if (mp->style == STYLE_BOTH) {
        /* Bar first, then the text in what's left of the widget. */
        bar_w = mp->vertical ? 6 : width;
        if (!mp->vertical) {
            bar_h = 4;
            bar_y = y + height - 6;
        }
    }
    if (bar_h < 1) {
        bar_h = 1;
    }

    if (mp->style != STYLE_TEXT) {
        double f = monitor_fill_fraction(mp);
        /* A theme's bar.png supplies the track (row 0) and the fill
         * (row 1) as 9-slices; color= / track_color= keep working for
         * every theme that doesn't ship one. A themed track ignores
         * high_color= on purpose -- the bitmap is the look. */
        int themed = panel_draw_skin(&p->bar_skin, cr, SKIN_BAR_TRACK, bar_x, bar_y, bar_w, bar_h);
        if (!themed) {
            if (mp->has_track) {
                cairo_set_source_rgba(cr, mp->trk_r, mp->trk_g, mp->trk_b, mp->trk_a);
            } else {
                cairo_set_source_rgba(cr, p->fg_r, p->fg_g, p->fg_b, 0.18);
            }
            cairo_rectangle(cr, bar_x, bar_y, bar_w, bar_h);
            cairo_fill(cr);
        }

        double fx = bar_x, fy = bar_y, fw = bar_w, fh = bar_h;
        if (mp->vertical) {
            fh = bar_h * f;
            fy = bar_y + bar_h - fh; /* grows upward */
        } else {
            fw = bar_w * f;
        }
        if (!themed || !panel_draw_skin(&p->bar_skin, cr, SKIN_BAR_FILL, fx, fy, fw, fh)) {
            cairo_set_source_rgba(cr, fr, fg, fb, fa);
            cairo_rectangle(cr, fx, fy, fw, fh);
            cairo_fill(cr);
        }
    }

    if (mp->style != STYLE_BAR) {
        double size = monitor_text_size(mp, p);
        double tw;
        pango_text_extents_ellipsized(cr, mp->text, size, 0, &tw, NULL);
        int text_x = x, text_w = width, text_h = height;
        if (mp->style == STYLE_BOTH) {
            if (mp->vertical) {
                text_x = x + bar_w + 4;
                text_w = width - bar_w - 4;
            } else {
                text_h = height - 6; /* the horizontal bar sits under the text */
            }
        }
        cairo_set_source_rgba(cr, fr, fg, fb, mp->has_value ? fa : fa * 0.5);
        pango_show_text_boxed(cr, text_x + (text_w - tw) / 2.0, y, text_h, text_w, size, mp->text, NULL);
    }
}

static const char *metric_label(enum monitor_metric m)
{
    switch (m) {
    case METRIC_RAM:
        return "RAM";
    case METRIC_SWAP:
        return "Swap";
    case METRIC_GPU:
        return "GPU";
    case METRIC_VRAM:
        return "VRAM";
    case METRIC_CPU_TEMP:
        return "Temperatura da CPU";
    case METRIC_GPU_TEMP:
        return "Temperatura da GPU";
    default:
        return "CPU";
    }
}

static int monitor_get_tooltip(PanelWidget *w, int local_x, char *buf, size_t bufsz, int *anchor_x, int *anchor_w,
                                int *out_closable, void **out_ctx)
{
    (void)local_x;
    (void)out_closable;
    (void)out_ctx;
    MonitorPriv *mp = w->priv;

    char what[64];
    if (mp->index >= 0 && (mp->metric == METRIC_CPU)) {
        snprintf(what, sizeof(what), "CPU %d", mp->index);
    } else if (mp->index >= 0) {
        snprintf(what, sizeof(what), "%s %d", metric_label(mp->metric), mp->index);
    } else {
        snprintf(what, sizeof(what), "%s", metric_label(mp->metric));
    }

    if (!mp->has_value) {
        snprintf(buf, bufsz, "%s\nsem leitura disponível", what);
    } else if (metric_is_temp(mp->metric)) {
        snprintf(buf, bufsz, "%s\n%.1f °C", what, mp->value);
    } else {
        snprintf(buf, bufsz, "%s\n%.1f%%", what, mp->value);
    }
    *anchor_x = 0;
    *anchor_w = w->len;
    return 1;
}

const PanelWidgetOps monitor_ops = {
    .type_name = "monitor",
    .priv_size = sizeof(MonitorPriv),
    .init = monitor_init,
    .measure = monitor_measure,
    .paint = monitor_paint,
    .on_tick = monitor_on_tick,
    .get_tooltip = monitor_get_tooltip,
};
