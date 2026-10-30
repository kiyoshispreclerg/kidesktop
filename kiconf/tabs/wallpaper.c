/* kiconf - Wallpaper tab: xisback socket client.
 * See kiconf.c's top doc comment for the overall design.
 *
 * The tab shows the result, not xisback's layer table: the screens are
 * drawn proportionally (like the Telas tab) with the wallpaper each one
 * really shows, slideshows cycle their pictures every 2 s, and with "one
 * per desktop" a strip of miniatures picks which desktop is being edited.
 * Every change is applied right away: the tab re-derives the whole layer
 * set the chosen mode needs ("same everywhere" = one (*,*) layer, "one per
 * screen" = (output,*), "one per desktop" = (output,N)...) from what is
 * showing now, edits the one cell in question, and sends xisback only the
 * CLEAR/SET commands that differ. The raw layer list stays under
 * "Avancado". */
#include "../common.h"
#include "../tabs.h"
#include "../../shared/xis_outputs.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>
#include <ctype.h>
#include <errno.h>
#include <gdk/gdkx.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_WP_LAYERS 512
#define WP_PATH_LEN 1024
#define WP_MAX_DESKTOPS 16
#define WP_SLIDES_MAX 8
#define WP_THUMB_SIDE 320
#define WP_TICK_SECONDS 2

typedef struct {
    char output[NAME_LEN], desktop[16], mode[16], color[16];
    int interval, shuffle, fade_ms;
    char path[WP_PATH_LEN];
} WpLayer;

typedef struct {
    WpLayer l[MAX_WP_LAYERS];
    int n;
} WpSet;

typedef struct {
    char name[XIS_OUTPUT_STR_LEN]; /* connector name */
    char id[XIS_OUTPUT_STR_LEN];   /* "edid:..." or "" */
    char value[XIS_OUTPUT_STR_LEN]; /* what a layer's output field holds for it */
    int x, y, w, h;
} WpScreen;

typedef struct {
    char *dir;
    char *files[WP_SLIDES_MAX];
    int n;
} WpSlides;

typedef struct {
    GdkPixbuf *pixbuf; /* NULL while loading or if it failed */
} WpThumb;

/* xisback's layers as of the last LIST (what is on screen now) */
static WpLayer g_wp_layers[MAX_WP_LAYERS];
static int g_n_wp_layers = 0;

static WpScreen g_screens[XIS_MAX_OUTPUTS];
static int g_n_screens = 0;
static int g_n_desktops = 1;
static int g_sel_screen = 0;
static int g_tab_desktop = 0;
static int g_loading = 0; /* set while the code (not the user) moves a widget */
static unsigned g_tick = 0;
static int g_has_slideshow = 0;

static GHashTable *g_slides;  /* dir -> WpSlides* */
static GHashTable *g_thumbs;  /* path -> WpThumb* */
static GQueue g_thumb_queue = G_QUEUE_INIT;
static guint g_thumb_idle = 0;
static guint g_apply_timer = 0;
static guint g_tick_timer = 0;

static GtkListStore *g_wp_layers_store;
static GtkWidget *g_wp_layers_view;
static GtkWidget *g_canvas, *g_strip, *g_strip_box;
static GtkWidget *g_minis[WP_MAX_DESKTOPS];
static int g_n_minis = 0;
static GtkWidget *g_radio_split, *g_radio_span, *g_radio_same, *g_radio_per, *g_row_screens;
static GtkWidget *g_wp_target_label, *g_wp_path_entry, *g_wp_mode_combo;
static GtkWidget *g_wp_interval_spin, *g_wp_shuffle_chk, *g_wp_fade_spin, *g_wp_next_btn;
static GtkWidget *g_wp_action_left, *g_wp_action_right, *g_wp_action_middle, *g_wp_action_double;
static GtkWidget *g_wp_action_scroll_up, *g_wp_action_scroll_down;
static GtkWidget *g_wp_status_label;
static char g_form_color[16] = "#000000";

enum { COL_WP_OUTPUT = 0, COL_WP_DESKTOP, COL_WP_MODE, COL_WP_INTERVAL, COL_WP_SHUFFLE, COL_WP_FADE, COL_WP_PATH, N_WP_COLS };

static void refresh_wp_layers(void);
static void wp_form_load(void);

/* ---- xisback socket client --------------------------------------------- */

static void xisback_socket_path(char *out, size_t outsz)
{
    const char *rundir = getenv("XDG_RUNTIME_DIR");
    snprintf(out, outsz, "%s/xisback.sock", (rundir && *rundir) ? rundir : "/tmp");
}

/* Sends one line, reads until EOF, same wire format as PROTOCOL.md's
 * Python example. If the daemon isn't reachable, starts it (no args ->
 * comes up as an empty daemon, see xisback's own doc comment) and retries
 * once, same as xisconf.py's _xisback_send(). */
static int xisback_send(const char *cmd, char *resp, size_t respsz)
{
    resp[0] = '\0';
    char path[PATH_MAX];
    xisback_socket_path(path, sizeof(path));

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(addr.sun_path)) {
        return 0;
    }
    memcpy(addr.sun_path, path, strlen(path) + 1);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return 0;
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        /* xisback doesn't daemonize/detach itself (same as kiwm/kicomp --
         * see spawn_replace()'s own doc comment) -- it just runs its event
         * loop forever in this child, so waitpid()ing on it here would
         * block until xisback eventually exits, i.e. forever. setsid()
         * detaches it from kiconf the same way spawn_replace() does, so
         * it outlives kiconf instead of dying with it. */
        pid_t pid = fork();
        if (pid == 0) {
            setsid();
            execlp("xisback", "xisback", (char *)NULL);
            _exit(127);
        }
        usleep(300000);
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0 || connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
            if (fd >= 0) {
                close(fd);
            }
            return 0;
        }
    }
    char line[WP_PATH_LEN + 256];
    snprintf(line, sizeof(line), "%s\n", cmd);
    if (write(fd, line, strlen(line)) < 0) {
        close(fd);
        return 0;
    }
    shutdown(fd, SHUT_WR);
    size_t total = 0;
    ssize_t n;
    while (total + 1 < respsz && (n = read(fd, resp + total, respsz - 1 - total)) > 0) {
        total += (size_t)n;
    }
    resp[total] = '\0';
    close(fd);
    return 1;
}

/* Splits a tab-separated line in place keeping empty fields (strtok would
 * swallow an empty path, shifting the color into its place). */
static int wp_split_tabs(char *line, char **f, int max)
{
    int n = 0;
    f[n++] = line;
    for (char *p = line; *p && n < max; p++) {
        if (*p == '\t') {
            *p = '\0';
            f[n++] = p + 1;
        }
    }
    return n;
}

static int xisback_list_layers(WpLayer *layers, int max)
{
    static char resp[MAX_WP_LAYERS * (WP_PATH_LEN + 160)];
    if (!xisback_send("LIST", resp, sizeof(resp))) {
        return 0;
    }
    int n = 0;
    char *save = NULL;
    char *line = strtok_r(resp, "\n", &save);
    while (line && n < max) {
        char *f[8];
        int nf = wp_split_tabs(line, f, 8);
        if (nf >= 7) {
            WpLayer *l = &layers[n++];
            snprintf(l->output, sizeof(l->output), "%s", f[0]);
            snprintf(l->desktop, sizeof(l->desktop), "%s", f[1]);
            snprintf(l->mode, sizeof(l->mode), "%s", f[2]);
            l->interval = atoi(f[3]);
            l->shuffle = atoi(f[4]);
            l->fade_ms = atoi(f[5]);
            snprintf(l->path, sizeof(l->path), "%s", f[6]);
            snprintf(l->color, sizeof(l->color), "%s", (nf >= 8 && f[7][0]) ? f[7] : "#000000");
        }
        line = strtok_r(NULL, "\n", &save);
    }
    return n;
}

/* ---- screens and desktops ---------------------------------------------- */

static int wp_root_cardinal(const char *name, int def)
{
    Display *dpy = GDK_DISPLAY_XDISPLAY(gdk_display_get_default());
    Atom a = XInternAtom(dpy, name, True);
    if (!a) {
        return def;
    }
    Atom type;
    int fmt;
    unsigned long n, after;
    unsigned char *data = NULL;
    int v = def;
    if (XGetWindowProperty(dpy, DefaultRootWindow(dpy), a, 0, 1, False, XA_CARDINAL, &type, &fmt, &n, &after,
                           &data) == Success && data) {
        if (n >= 1 && fmt == 32) {
            v = (int)*(long *)data;
        }
        XFree(data);
    }
    return v;
}

static void wp_scan_screens(void)
{
    Display *dpy = GDK_DISPLAY_XDISPLAY(gdk_display_get_default());
    XisOutput outs[XIS_MAX_OUTPUTS];
    int no = xis_list_outputs(dpy, outs, XIS_MAX_OUTPUTS, 0);

    g_n_screens = 0;
    XRRScreenResources *res = XRRGetScreenResourcesCurrent(dpy, DefaultRootWindow(dpy));
    if (res) {
        for (int i = 0; i < res->noutput && g_n_screens < XIS_MAX_OUTPUTS; i++) {
            XRROutputInfo *oi = XRRGetOutputInfo(dpy, res, res->outputs[i]);
            if (oi && oi->connection == RR_Connected && oi->crtc) {
                XRRCrtcInfo *ci = XRRGetCrtcInfo(dpy, res, oi->crtc);
                if (ci && ci->width > 0 && ci->height > 0) {
                    WpScreen *s = &g_screens[g_n_screens++];
                    snprintf(s->name, sizeof(s->name), "%.*s", (int)sizeof(s->name) - 1, oi->name);
                    s->id[0] = '\0';
                    for (int k = 0; k < no; k++) {
                        if (strcmp(outs[k].name, s->name) == 0) {
                            snprintf(s->id, sizeof(s->id), "%s", outs[k].id);
                        }
                    }
                    snprintf(s->value, sizeof(s->value), "%s", s->id[0] ? s->id : s->name);
                    s->x = ci->x;
                    s->y = ci->y;
                    s->w = (int)ci->width;
                    s->h = (int)ci->height;
                }
                if (ci) {
                    XRRFreeCrtcInfo(ci);
                }
            }
            if (oi) {
                XRRFreeOutputInfo(oi);
            }
        }
        XRRFreeScreenResources(res);
    }
    if (g_n_screens == 0) {
        /* no RandR answer: one virtual screen so the tab still works */
        WpScreen *s = &g_screens[g_n_screens++];
        snprintf(s->name, sizeof(s->name), "*");
        snprintf(s->value, sizeof(s->value), "*");
        s->id[0] = '\0';
        s->x = s->y = 0;
        s->w = 1920;
        s->h = 1080;
    }

    /* Pair saved screens with real ones the way xisback's place_layers()
     * does (exact name/EDID first, the rest by order), so a config saved
     * under old connector names shows -- and is edited -- under those
     * same names instead of looking empty. */
    const char *saved[MAX_WP_LAYERS] = { NULL };
    for (int i = 0; i < g_n_wp_layers; i++) {
        saved[i] = g_wp_layers[i].output;
    }
    int saved_to_real[MAX_WP_LAYERS] = {0};
    int real_to_saved[XIS_MAX_OUTPUTS] = {0};
    xis_match_outputs(saved, g_n_wp_layers, outs, no, saved_to_real, real_to_saved);
    for (int i = 0; i < g_n_screens; i++) {
        for (int k = 0; k < no; k++) {
            if (strcmp(outs[k].name, g_screens[i].name) == 0 && real_to_saved[k] >= 0) {
                snprintf(g_screens[i].value, sizeof(g_screens[i].value), "%s", saved[real_to_saved[k]]);
            }
        }
    }

    g_n_desktops = wp_root_cardinal("_NET_NUMBER_OF_DESKTOPS", 1);
    if (g_n_desktops < 1) {
        g_n_desktops = 1;
    }
    if (g_n_desktops > WP_MAX_DESKTOPS) {
        g_n_desktops = WP_MAX_DESKTOPS;
    }
    if (g_sel_screen >= g_n_screens) {
        g_sel_screen = 0;
    }
    if (g_tab_desktop >= g_n_desktops) {
        g_tab_desktop = 0;
    }
}

/* ---- layer model ------------------------------------------------------- */

static int wp_out_match(const char *lo, const WpScreen *s)
{
    return (s->id[0] && strcmp(lo, s->id) == 0) || strcmp(lo, s->name) == 0 || strcmp(lo, s->value) == 0;
}

/* What `s` shows on desktop `d`: the most specific layer wins --
 * (output,d), (output,*), (*,d), (*,*) -- the same four a user could have
 * made by hand. */
static const WpLayer *wp_effective(const WpLayer *L, int n, const WpScreen *s, int d)
{
    char ds[16];
    snprintf(ds, sizeof(ds), "%d", d);
    for (int pass = 0; pass < 4; pass++) {
        int named = pass < 2;
        const char *want = (pass % 2 == 0) ? ds : "*";
        for (int i = 0; i < n; i++) {
            int out_ok = named ? wp_out_match(L[i].output, s) : strcmp(L[i].output, "*") == 0;
            if (out_ok && strcmp(L[i].desktop, want) == 0) {
                return &L[i];
            }
        }
    }
    return NULL;
}

static void wp_default_layer(WpLayer *l)
{
    memset(l, 0, sizeof(*l));
    snprintf(l->mode, sizeof(l->mode), "fill");
    snprintf(l->color, sizeof(l->color), "#000000");
    l->interval = 300;
    l->fade_ms = 1000;
}

static int wp_layer_eq(const WpLayer *a, const WpLayer *b)
{
    return strcmp(a->output, b->output) == 0 && strcmp(a->desktop, b->desktop) == 0 &&
           strcmp(a->mode, b->mode) == 0 && strcmp(a->path, b->path) == 0 && strcmp(a->color, b->color) == 0 &&
           a->interval == b->interval && a->shuffle == b->shuffle && a->fade_ms == b->fade_ms;
}

static int wp_same_look(const WpLayer *a, const WpLayer *b)
{
    if (!a || !b) {
        return a == b;
    }
    return strcmp(a->path, b->path) == 0 && strcmp(a->color, b->color) == 0 && strcmp(a->mode, b->mode) == 0;
}

static int wp_screen_known(const char *out)
{
    for (int i = 0; i < g_n_screens; i++) {
        if (wp_out_match(out, &g_screens[i])) {
            return 1;
        }
    }
    return 0;
}

/* The current layers read as a mode: any (*,x) layer means "stretched
 * over all screens", any desktop other than * means "one per desktop". */
static void wp_infer_mode(int *span, int *per)
{
    *span = *per = 0;
    for (int i = 0; i < g_n_wp_layers; i++) {
        if (strcmp(g_wp_layers[i].output, "*") == 0) {
            *span = 1;
        }
        if (strcmp(g_wp_layers[i].desktop, "*") != 0) {
            *per = 1;
        }
    }
    if (g_n_screens < 2) {
        *span = 0;
    }
    if (g_n_desktops < 2) {
        *per = 0;
    }
}

/* The complete layer set `span`/`per` call for, every cell taking what it
 * shows right now in `old`. Collapsing (several screens/desktops into
 * one) takes the source screen/desktop; expanding copies the one picture
 * to every new cell. Layers of screens that are not plugged in are kept
 * untouched. */
static WpSet *wp_build_set(const WpLayer *old, int nold, int span, int per, int src_screen, int src_desk)
{
    WpSet *set = g_new0(WpSet, 1);
    int nout = span ? 1 : g_n_screens;
    int ndesk = per ? g_n_desktops : 1;
    for (int o = 0; o < nout; o++) {
        const WpScreen *src = span ? &g_screens[src_screen] : &g_screens[o];
        for (int k = 0; k < ndesk && set->n < MAX_WP_LAYERS; k++) {
            const WpLayer *e = wp_effective(old, nold, src, per ? k : src_desk);
            WpLayer *n = &set->l[set->n++];
            if (e) {
                *n = *e;
            } else {
                wp_default_layer(n);
            }
            snprintf(n->output, sizeof(n->output), "%s", span ? "*" : src->value);
            snprintf(n->desktop, sizeof(n->desktop), "%d", k);
            if (!per) {
                snprintf(n->desktop, sizeof(n->desktop), "*");
            }
        }
    }
    for (int i = 0; i < nold && set->n < MAX_WP_LAYERS; i++) {
        if (strcmp(old[i].output, "*") != 0 && !wp_screen_known(old[i].output)) {
            set->l[set->n++] = old[i];
        }
    }
    return set;
}

static WpLayer *wp_set_find(WpSet *set, const char *out, const char *desk)
{
    for (int i = 0; i < set->n; i++) {
        if (strcmp(set->l[i].output, out) == 0 && strcmp(set->l[i].desktop, desk) == 0) {
            return &set->l[i];
        }
    }
    return NULL;
}

/* Sends xisback just what differs between what runs now (g_wp_layers) and
 * `want`: CLEARs first so two layers never overlap, then SETs. */
static void wp_commit(WpSet *want)
{
    char cmd[WP_PATH_LEN + 256], resp[256];
    for (int i = 0; i < g_n_wp_layers; i++) {
        if (!wp_set_find(want, g_wp_layers[i].output, g_wp_layers[i].desktop)) {
            snprintf(cmd, sizeof(cmd), "CLEAR\t%.127s\t%.15s", g_wp_layers[i].output, g_wp_layers[i].desktop);
            xisback_send(cmd, resp, sizeof(resp));
        }
    }
    for (int j = 0; j < want->n; j++) {
        const WpLayer *w = &want->l[j];
        int same = 0;
        for (int i = 0; i < g_n_wp_layers; i++) {
            if (wp_layer_eq(&g_wp_layers[i], w)) {
                same = 1;
                break;
            }
        }
        if (same) {
            continue;
        }
        snprintf(cmd, sizeof(cmd), "SET\t%s\t%s\t%s\t%d\t%d\t%d\t%s\t%s", w->output, w->desktop,
                 w->mode[0] ? w->mode : "fill", w->interval, w->shuffle, w->fade_ms, w->path, w->color);
        xisback_send(cmd, resp, sizeof(resp));
    }
}

/* ---- thumbnails and slideshow pictures --------------------------------- */

static int wp_is_image_name(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot) {
        return 0;
    }
    static const char *const exts[] = {"jpg", "jpeg", "png", "bmp", "gif", "webp", "tif", "tiff", "svg", NULL};
    for (int i = 0; exts[i]; i++) {
        if (g_ascii_strcasecmp(dot + 1, exts[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

static int wp_name_cmp(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void wp_slides_free(gpointer p)
{
    WpSlides *s = p;
    for (int i = 0; i < s->n; i++) {
        g_free(s->files[i]);
    }
    g_free(s->dir);
    g_free(s);
}

static WpSlides *wp_slides_get(const char *dir)
{
    if (!g_slides) {
        g_slides = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, wp_slides_free);
    }
    WpSlides *s = g_hash_table_lookup(g_slides, dir);
    if (s) {
        return s;
    }
    s = g_new0(WpSlides, 1);
    s->dir = g_strdup(dir);
    GDir *d = g_dir_open(dir, 0, NULL);
    if (d) {
        char *names[512] = { NULL };
        int nn = 0;
        const char *name;
        while ((name = g_dir_read_name(d)) && nn < 512) {
            if (wp_is_image_name(name)) {
                names[nn++] = g_strdup(name);
            }
        }
        g_dir_close(d);
        qsort(names, (size_t)nn, sizeof(char *), wp_name_cmp);
        for (int i = 0; i < nn; i++) {
            if (s->n < WP_SLIDES_MAX) {
                s->files[s->n++] = g_build_filename(dir, names[i], NULL);
            }
            g_free(names[i]);
        }
    }
    g_hash_table_insert(g_slides, s->dir, s);
    return s;
}

static int wp_path_is_dir(const char *path)
{
    return path[0] && g_file_test(path, G_FILE_TEST_IS_DIR);
}

/* The picture the layer shows at this tick (a slideshow cycles). */
static const char *wp_layer_image(const WpLayer *l)
{
    if (!l->path[0]) {
        return NULL;
    }
    if (wp_path_is_dir(l->path)) {
        WpSlides *s = wp_slides_get(l->path);
        return s->n ? s->files[g_tick % (unsigned)s->n] : NULL;
    }
    return l->path;
}

static void wp_redraw_all(void)
{
    if (g_canvas) {
        gtk_widget_queue_draw(g_canvas);
    }
    for (int i = 0; i < g_n_minis; i++) {
        gtk_widget_queue_draw(g_minis[i]);
    }
}

static gboolean wp_thumb_idle(gpointer data)
{
    (void)data;
    char *path = g_queue_pop_head(&g_thumb_queue);
    if (!path) {
        g_thumb_idle = 0;
        return FALSE;
    }
    WpThumb *t = g_hash_table_lookup(g_thumbs, path);
    if (t) {
        t->pixbuf = gdk_pixbuf_new_from_file_at_size(path, WP_THUMB_SIDE, WP_THUMB_SIDE, NULL);
    }
    g_free(path);
    wp_redraw_all();
    return TRUE;
}

static void wp_thumb_free(gpointer p)
{
    WpThumb *t = p;
    if (t->pixbuf) {
        g_object_unref(t->pixbuf);
    }
    g_free(t);
}

/* NULL while the picture is still loading (one per idle pass, so a folder
 * of big photos never freezes the tab) or if it cannot be decoded. */
static GdkPixbuf *wp_thumb_get(const char *path)
{
    if (!g_thumbs) {
        g_thumbs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, wp_thumb_free);
    }
    WpThumb *t = g_hash_table_lookup(g_thumbs, path);
    if (t) {
        return t->pixbuf;
    }
    if (g_hash_table_size(g_thumbs) > 48) {
        g_hash_table_remove_all(g_thumbs);
    }
    g_hash_table_insert(g_thumbs, g_strdup(path), g_new0(WpThumb, 1));
    g_queue_push_tail(&g_thumb_queue, g_strdup(path));
    if (!g_thumb_idle) {
        g_thumb_idle = g_idle_add(wp_thumb_idle, NULL);
    }
    return NULL;
}

/* ---- preview drawing --------------------------------------------------- */

static void wp_parse_color(const char *hex, double *r, double *g, double *b)
{
    unsigned v = 0;
    *r = *g = *b = 0;
    if (hex && sscanf(hex[0] == '#' ? hex + 1 : hex, "%6x", &v) == 1) {
        *r = ((v >> 16) & 0xff) / 255.0;
        *g = ((v >> 8) & 0xff) / 255.0;
        *b = (v & 0xff) / 255.0;
    }
}

/* Places the screens' bounding box centered in cw x ch. */
static void wp_layout(int cw, int ch, int margin, double *bx, double *by, double *ox, double *oy, double *scale)
{
    int x0 = g_screens[0].x, y0 = g_screens[0].y;
    int x1 = x0 + g_screens[0].w, y1 = y0 + g_screens[0].h;
    for (int i = 1; i < g_n_screens; i++) {
        x0 = MIN(x0, g_screens[i].x);
        y0 = MIN(y0, g_screens[i].y);
        x1 = MAX(x1, g_screens[i].x + g_screens[i].w);
        y1 = MAX(y1, g_screens[i].y + g_screens[i].h);
    }
    double bw = x1 - x0, bh = y1 - y0;
    double sx = (cw - 2.0 * margin) / bw, sy = (ch - 2.0 * margin) / bh;
    *scale = sx < sy ? sx : sy;
    if (*scale <= 0) {
        *scale = 0.01;
    }
    *bx = x0;
    *by = y0;
    *ox = (cw - bw * *scale) / 2;
    *oy = (ch - bh * *scale) / 2;
}

static void wp_screen_rect(int i, double bx, double by, double ox, double oy, double scale, double *x, double *y,
                           double *w, double *h)
{
    *x = ox + (g_screens[i].x - bx) * scale;
    *y = oy + (g_screens[i].y - by) * scale;
    *w = g_screens[i].w * scale;
    *h = g_screens[i].h * scale;
}

/* Draws every screen as it looks on `desktop`. A (*,x) layer is one
 * picture across the whole arrangement, clipped to each screen. */
static void wp_render(cairo_t *cr, int cw, int ch, int desktop, int margin, int labels, int show_sel)
{
    double bx, by, ox, oy, scale;
    wp_layout(cw, ch, margin, &bx, &by, &ox, &oy, &scale);

    double ax0 = 1e9, ay0 = 1e9, ax1 = -1e9, ay1 = -1e9;
    for (int i = 0; i < g_n_screens; i++) {
        double x, y, w, h;
        wp_screen_rect(i, bx, by, ox, oy, scale, &x, &y, &w, &h);
        ax0 = MIN(ax0, x);
        ay0 = MIN(ay0, y);
        ax1 = MAX(ax1, x + w);
        ay1 = MAX(ay1, y + h);
    }

    for (int i = 0; i < g_n_screens; i++) {
        double x, y, w, h;
        wp_screen_rect(i, bx, by, ox, oy, scale, &x, &y, &w, &h);
        const WpLayer *l = wp_effective(g_wp_layers, g_n_wp_layers, &g_screens[i], desktop);

        double r, g, b;
        wp_parse_color(l ? l->color : "#000000", &r, &g, &b);
        cairo_set_source_rgb(cr, r, g, b);
        cairo_rectangle(cr, x, y, w, h);
        cairo_fill(cr);

        const char *img = l ? wp_layer_image(l) : NULL;
        GdkPixbuf *pb = img ? wp_thumb_get(img) : NULL;
        if (pb) {
            int spans = strcmp(l->output, "*") == 0;
            double area_x = spans ? ax0 : x, area_y = spans ? ay0 : y;
            double area_w = spans ? ax1 - ax0 : w, area_h = spans ? ay1 - ay0 : h;
            double pw = gdk_pixbuf_get_width(pb), ph = gdk_pixbuf_get_height(pb);
            double sx, sy;
            if (strcmp(l->mode, "stretch") == 0) {
                sx = area_w / pw;
                sy = area_h / ph;
            } else {
                sx = sy = MAX(area_w / pw, area_h / ph);
            }
            cairo_save(cr);
            cairo_rectangle(cr, x, y, w, h);
            cairo_clip(cr);
            cairo_translate(cr, area_x + (area_w - pw * sx) / 2, area_y + (area_h - ph * sy) / 2);
            cairo_scale(cr, sx, sy);
            gdk_cairo_set_source_pixbuf(cr, pb, 0, 0);
            cairo_paint(cr);
            cairo_restore(cr);
        }

        int selected = show_sel && i == g_sel_screen;
        if (selected) {
            cairo_set_source_rgb(cr, 0.25, 0.60, 1.0);
        } else {
            cairo_set_source_rgb(cr, 1, 1, 1);
        }
        cairo_set_line_width(cr, selected ? 3 : 1);
        cairo_rectangle(cr, x, y, w, h);
        cairo_stroke(cr);

        if (labels) {
            cairo_set_font_size(cr, 12);
            cairo_set_source_rgb(cr, 0, 0, 0);
            cairo_move_to(cr, x + 7, y + 17);
            cairo_show_text(cr, g_screens[i].name);
            cairo_set_source_rgb(cr, 1, 1, 1);
            cairo_move_to(cr, x + 6, y + 16);
            cairo_show_text(cr, g_screens[i].name);
        }
    }
}

static gboolean wp_canvas_expose(GtkWidget *widget, GdkEventExpose *event, gpointer data)
{
    (void)event;
    (void)data;
    cairo_t *cr = gdk_cairo_create(widget->window);
    cairo_set_source_rgb(cr, 0.14, 0.15, 0.16);
    cairo_paint(cr);
    wp_render(cr, widget->allocation.width, widget->allocation.height, g_tab_desktop, 20, 1, 1);
    cairo_destroy(cr);
    return TRUE;
}

static int wp_hit_screen(double px, double py)
{
    double bx, by, ox, oy, scale;
    wp_layout(g_canvas->allocation.width, g_canvas->allocation.height, 20, &bx, &by, &ox, &oy, &scale);
    for (int i = 0; i < g_n_screens; i++) {
        double x, y, w, h;
        wp_screen_rect(i, bx, by, ox, oy, scale, &x, &y, &w, &h);
        if (px >= x && px < x + w && py >= y && py < y + h) {
            return i;
        }
    }
    return -1;
}

static gboolean wp_canvas_press(GtkWidget *widget, GdkEventButton *event, gpointer data)
{
    (void)widget;
    (void)data;
    int i = wp_hit_screen(event->x, event->y);
    if (i >= 0 && i != g_sel_screen) {
        g_sel_screen = i;
        wp_form_load();
        wp_redraw_all();
    }
    return TRUE;
}

static gboolean wp_mini_expose(GtkWidget *widget, GdkEventExpose *event, gpointer data)
{
    (void)event;
    int idx = GPOINTER_TO_INT(data);
    int cw = widget->allocation.width, ch = widget->allocation.height;
    int selected = idx == g_tab_desktop;
    cairo_t *cr = gdk_cairo_create(widget->window);
    if (selected) {
        cairo_set_source_rgb(cr, 0.20, 0.30, 0.45);
    } else {
        cairo_set_source_rgb(cr, 0.17, 0.18, 0.19);
    }
    cairo_paint(cr);
    wp_render(cr, cw, ch - 18, idx, 8, 0, 0);

    char cap[32];
    snprintf(cap, sizeof(cap), _("Desktop %d"), idx + 1);
    cairo_set_font_size(cr, 11);
    cairo_text_extents_t ext;
    cairo_text_extents(cr, cap, &ext);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_move_to(cr, (cw - ext.width) / 2 - ext.x_bearing, ch - 5);
    cairo_show_text(cr, cap);
    if (selected) {
        cairo_set_source_rgb(cr, 0.25, 0.60, 1.0);
        cairo_set_line_width(cr, 3);
        cairo_rectangle(cr, 1.5, 1.5, cw - 3, ch - 3);
        cairo_stroke(cr);
    }
    cairo_destroy(cr);
    return TRUE;
}

static gboolean wp_mini_press(GtkWidget *widget, GdkEventButton *event, gpointer data)
{
    (void)widget;
    (void)event;
    g_tab_desktop = GPOINTER_TO_INT(data);
    wp_form_load();
    wp_redraw_all();
    return TRUE;
}

static gboolean wp_tick(gpointer data)
{
    (void)data;
    g_tick++;
    /* only a slideshow moves, and only worth redrawing while visible */
    if (g_has_slideshow && g_canvas && GTK_WIDGET_MAPPED(g_canvas)) {
        wp_redraw_all();
    }
    return TRUE;
}

/* ---- form <-> layer, apply right away ---------------------------------- */

static void wp_read_radios(int *span, int *per)
{
    *span = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_radio_span));
    *per = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_radio_per));
}

static void wp_sync_radios(void)
{
    int span, per;
    wp_infer_mode(&span, &per);
    g_loading++;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(span ? g_radio_span : g_radio_split), TRUE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(per ? g_radio_per : g_radio_same), TRUE);
    gtk_widget_set_sensitive(g_row_screens, g_n_screens > 1);
    gtk_widget_set_sensitive(g_radio_per, g_n_desktops > 1);
    gtk_widget_set_sensitive(g_radio_same, g_n_desktops > 1);
    g_loading--;
}

/* Where an edit lands, in the layout the radios currently describe. */
static void wp_target_key(char *out, size_t outsz, char *desk, size_t desksz)
{
    int span, per;
    wp_read_radios(&span, &per);
    snprintf(out, outsz, "%s", span ? "*" : g_screens[g_sel_screen].value);
    if (per) {
        snprintf(desk, desksz, "%d", g_tab_desktop);
    } else {
        snprintf(desk, desksz, "*");
    }
}

static void wp_form_load(void)
{
    const WpLayer *l = wp_effective(g_wp_layers, g_n_wp_layers, &g_screens[g_sel_screen], g_tab_desktop);
    WpLayer def;
    if (!l) {
        wp_default_layer(&def);
        l = &def;
    }
    int span, per;
    wp_read_radios(&span, &per);

    char label[256];
    if (span) {
        snprintf(label, sizeof(label), _("Editando: todas as telas"));
    } else {
        snprintf(label, sizeof(label), _("Editando: %s"), g_screens[g_sel_screen].name);
    }
    if (per) {
        size_t len = strlen(label);
        snprintf(label + len, sizeof(label) - len, _(" - Desktop %d"), g_tab_desktop + 1);
    }

    g_loading++;
    gtk_label_set_text(GTK_LABEL(g_wp_target_label), label);
    gtk_entry_set_text(GTK_ENTRY(g_wp_path_entry), l->path);
    gtk_combo_box_set_active(GTK_COMBO_BOX(g_wp_mode_combo), strcmp(l->mode, "stretch") == 0 ? 1 : 0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_wp_interval_spin), l->interval > 0 ? l->interval : 300);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_wp_shuffle_chk), l->shuffle);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_wp_fade_spin), l->fade_ms / 1000.0);
    snprintf(g_form_color, sizeof(g_form_color), "%s", l->color);
    int slideshow = wp_path_is_dir(l->path);
    gtk_widget_set_sensitive(g_wp_interval_spin, slideshow);
    gtk_widget_set_sensitive(g_wp_shuffle_chk, slideshow);
    gtk_widget_set_sensitive(g_wp_fade_spin, slideshow);
    gtk_widget_set_sensitive(g_wp_next_btn, slideshow);
    g_loading--;
}

static void wp_apply_form(void)
{
    if (g_loading) {
        return;
    }
    if (g_apply_timer) {
        g_source_remove(g_apply_timer);
        g_apply_timer = 0;
    }
    int span, per;
    wp_read_radios(&span, &per);
    WpSet *set = wp_build_set(g_wp_layers, g_n_wp_layers, span, per, g_sel_screen, g_tab_desktop);
    char out[XIS_OUTPUT_STR_LEN], desk[16];
    wp_target_key(out, sizeof(out), desk, sizeof(desk));
    WpLayer *t = wp_set_find(set, out, desk);
    if (t) {
        const char *path = gtk_entry_get_text(GTK_ENTRY(g_wp_path_entry));
        snprintf(t->path, sizeof(t->path), "%s", path);
        snprintf(t->mode, sizeof(t->mode), "%s",
                 gtk_combo_box_get_active(GTK_COMBO_BOX(g_wp_mode_combo)) == 1 ? "stretch" : "fill");
        t->interval = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_wp_interval_spin));
        t->shuffle = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_wp_shuffle_chk));
        t->fade_ms = (int)(gtk_spin_button_get_value(GTK_SPIN_BUTTON(g_wp_fade_spin)) * 1000 + 0.5);
        snprintf(t->color, sizeof(t->color), "%s", g_form_color);
        wp_commit(set);
    }
    g_free(set);
    refresh_wp_layers();
}

static gboolean wp_apply_timeout(gpointer data)
{
    (void)data;
    g_apply_timer = 0;
    wp_apply_form();
    return FALSE;
}

/* Spin buttons fire on every step; wait for the user to stop. */
static void wp_schedule_apply(void)
{
    if (g_loading) {
        return;
    }
    if (g_apply_timer) {
        g_source_remove(g_apply_timer);
    }
    g_apply_timer = g_timeout_add(400, wp_apply_timeout, NULL);
}

static void on_wp_form_changed(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    wp_schedule_apply();
}

static void on_wp_path_activate(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    wp_apply_form();
}

static void wp_set_path_and_apply(const char *path)
{
    g_loading++;
    gtk_entry_set_text(GTK_ENTRY(g_wp_path_entry), path);
    g_loading--;
    wp_apply_form();
}

static void wp_choose(GtkFileChooserAction action, const char *title)
{
    GtkWidget *dlg = gtk_file_chooser_dialog_new(title, NULL, action, _("_Cancelar"), GTK_RESPONSE_CANCEL,
                                                 _("_Abrir"), GTK_RESPONSE_ACCEPT, NULL);
    char *picked = NULL;
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        picked = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
    }
    gtk_widget_destroy(dlg);
    if (picked) {
        wp_set_path_and_apply(picked);
        g_free(picked);
    }
}

static void on_wp_pick_file(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    wp_choose(GTK_FILE_CHOOSER_ACTION_OPEN, _("Escolher imagem"));
}

static void on_wp_pick_folder(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    wp_choose(GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, _("Escolher pasta de slideshow"));
}

static void on_wp_pick_color(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    GtkWidget *dlg = gtk_color_selection_dialog_new(_("Cor solida"));
    GtkColorSelection *sel = GTK_COLOR_SELECTION(GTK_COLOR_SELECTION_DIALOG(dlg)->colorsel);
    GdkColor c;
    if (gdk_color_parse(g_form_color, &c)) {
        gtk_color_selection_set_current_color(sel, &c);
    }
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK) {
        gtk_color_selection_get_current_color(sel, &c);
        snprintf(g_form_color, sizeof(g_form_color), "#%02x%02x%02x", c.red >> 8, c.green >> 8, c.blue >> 8);
        gtk_widget_destroy(dlg);
        wp_set_path_and_apply("");
        return;
    }
    gtk_widget_destroy(dlg);
}

static void on_wp_next(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    char out[XIS_OUTPUT_STR_LEN], desk[16], cmd[256], resp[256];
    wp_target_key(out, sizeof(out), desk, sizeof(desk));
    snprintf(cmd, sizeof(cmd), "NEXT\t%s\t%s", out, desk);
    xisback_send(cmd, resp, sizeof(resp));
}

/* Dropping a picture or folder on a screen sets it there. */
static void on_wp_drop(GtkWidget *widget, GdkDragContext *ctx, gint x, gint y, GtkSelectionData *sd, guint info,
                       guint time, gpointer data)
{
    (void)widget;
    (void)info;
    (void)data;
    gboolean ok = FALSE;
    gchar **uris = gtk_selection_data_get_uris(sd);
    if (uris && uris[0]) {
        gchar *file = g_filename_from_uri(uris[0], NULL, NULL);
        if (file) {
            int i = wp_hit_screen(x, y);
            if (i >= 0) {
                g_sel_screen = i;
            }
            wp_form_load();
            wp_set_path_and_apply(file);
            g_free(file);
            ok = TRUE;
        }
    }
    g_strfreev(uris);
    gtk_drag_finish(ctx, ok, FALSE, time);
}

static int wp_confirm(const char *msg)
{
    GtkWidget *dlg = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE, "%s",
                                            msg);
    dialog_add_answers(dlg, GTK_BUTTONS_YES_NO);
    int yes = gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_YES;
    gtk_widget_destroy(dlg);
    return yes;
}

/* A radio changed: re-derive the layer set for the new layout. Going from
 * several pictures to one asks first, since the others would be lost. */
static void on_wp_mode_toggled(GtkWidget *widget, gpointer data)
{
    (void)data;
    if (g_loading || !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget))) {
        return;
    }
    int span, per, old_span, old_per;
    wp_read_radios(&span, &per);
    wp_infer_mode(&old_span, &old_per);

    const WpScreen *src = &g_screens[g_sel_screen];
    const WpLayer *ref = wp_effective(g_wp_layers, g_n_wp_layers, src, g_tab_desktop);
    int differs = 0;
    if (old_per && !per) {
        for (int d = 0; d < g_n_desktops; d++) {
            differs |= !wp_same_look(wp_effective(g_wp_layers, g_n_wp_layers, src, d), ref);
        }
    }
    if (!old_span && span) {
        for (int i = 0; i < g_n_screens; i++) {
            differs |= !wp_same_look(wp_effective(g_wp_layers, g_n_wp_layers, &g_screens[i], g_tab_desktop), ref);
        }
    }
    if (differs && !wp_confirm(_("Ha papeis de parede diferentes que serao descartados; fica so o da selecao "
                                 "atual. Continuar?"))) {
        wp_sync_radios();
        return;
    }
    WpSet *set = wp_build_set(g_wp_layers, g_n_wp_layers, span, per, g_sel_screen, g_tab_desktop);
    wp_commit(set);
    g_free(set);
    refresh_wp_layers();
}

/* ---- refresh from xisback ---------------------------------------------- */

static void wp_destroy_child(GtkWidget *w, gpointer data)
{
    (void)data;
    gtk_widget_destroy(w);
}

static void wp_rebuild_minis(void)
{
    int want = g_n_desktops;
    int span, per;
    wp_infer_mode(&span, &per);
    if (g_n_minis != want) {
        gtk_container_foreach(GTK_CONTAINER(g_strip_box), wp_destroy_child, NULL);
        g_n_minis = 0;
        for (int i = 0; i < want; i++) {
            GtkWidget *da = gtk_drawing_area_new();
            gtk_widget_set_size_request(da, 130, 92);
            gtk_widget_add_events(da, GDK_BUTTON_PRESS_MASK);
            g_signal_connect(da, "expose-event", G_CALLBACK(wp_mini_expose), GINT_TO_POINTER(i));
            g_signal_connect(da, "button-press-event", G_CALLBACK(wp_mini_press), GINT_TO_POINTER(i));
            gtk_box_pack_start(GTK_BOX(g_strip_box), da, FALSE, FALSE, 0);
            gtk_widget_show(da);
            g_minis[g_n_minis++] = da;
        }
    }
    if (per) {
        gtk_widget_show(g_strip);
    } else {
        gtk_widget_hide(g_strip);
    }
}

static void refresh_wp_layers(void)
{
    g_n_wp_layers = xisback_list_layers(g_wp_layers, MAX_WP_LAYERS);
    wp_scan_screens();
    if (g_slides) {
        g_hash_table_remove_all(g_slides);
    }

    gtk_list_store_clear(g_wp_layers_store);
    g_has_slideshow = 0;
    for (int i = 0; i < g_n_wp_layers; i++) {
        WpLayer *l = &g_wp_layers[i];
        char interval_s[16], fade_s[16];
        snprintf(interval_s, sizeof(interval_s), "%d", l->interval);
        snprintf(fade_s, sizeof(fade_s), "%.1fs", l->fade_ms / 1000.0);
        GtkTreeIter it;
        gtk_list_store_append(g_wp_layers_store, &it);
        gtk_list_store_set(g_wp_layers_store, &it, COL_WP_OUTPUT, l->output, COL_WP_DESKTOP, l->desktop,
                           COL_WP_MODE, l->mode, COL_WP_INTERVAL, interval_s, COL_WP_SHUFFLE,
                           l->shuffle ? _("sim") : _("nao"), COL_WP_FADE, fade_s, COL_WP_PATH, l->path, -1);
        if (wp_path_is_dir(l->path)) {
            g_has_slideshow = 1;
        }
    }
    char status[64];
    snprintf(status, sizeof(status), _("%d camada(s) ativa(s)."), g_n_wp_layers);
    gtk_label_set_text(GTK_LABEL(g_wp_status_label), status);

    wp_sync_radios();
    wp_rebuild_minis();
    wp_form_load();
    wp_redraw_all();
}

/* ---- advanced: the raw layer list -------------------------------------- */

static int wp_selected_index(void)
{
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(g_wp_layers_view));
    GtkTreeIter it;
    GtkTreeModel *model;
    if (!gtk_tree_selection_get_selected(sel, &model, &it)) {
        return -1;
    }
    GtkTreePath *path = gtk_tree_model_get_path(model, &it);
    int idx = gtk_tree_path_get_indices(path)[0];
    gtk_tree_path_free(path);
    return idx;
}

static void on_wp_clear_selected(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    int idx = wp_selected_index();
    if (idx < 0 || idx >= g_n_wp_layers) {
        return;
    }
    char cmd[256], resp[256];
    snprintf(cmd, sizeof(cmd), "CLEAR\t%.127s\t%.15s", g_wp_layers[idx].output, g_wp_layers[idx].desktop);
    xisback_send(cmd, resp, sizeof(resp));
    refresh_wp_layers();
}

static void on_wp_clear_all(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    char resp[256];
    xisback_send("CLEARALL", resp, sizeof(resp));
    refresh_wp_layers();
}

static void on_wp_refresh(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    refresh_wp_layers();
}

static void on_wp_map(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    refresh_wp_layers();
}

/* ---- click actions (global to xisback) --------------------------------- */

static void on_wp_actions_save(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    const char *left = gtk_entry_get_text(GTK_ENTRY(g_wp_action_left));
    const char *right = gtk_entry_get_text(GTK_ENTRY(g_wp_action_right));
    const char *middle = gtk_entry_get_text(GTK_ENTRY(g_wp_action_middle));
    const char *dbl = gtk_entry_get_text(GTK_ENTRY(g_wp_action_double));
    const char *scroll_up = gtk_entry_get_text(GTK_ENTRY(g_wp_action_scroll_up));
    const char *scroll_down = gtk_entry_get_text(GTK_ENTRY(g_wp_action_scroll_down));
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "SETACTIONS\t%s\t%s\t%s\t%s\t%s\t%s",
              left, right, middle, dbl, scroll_up, scroll_down);
    char resp[256];
    if (xisback_send(cmd, resp, sizeof(resp))) {
        kiconf_track_rebase(GTK_WIDGET(data));
    }
}

static GtkWidget *g_wp_lazy_chk;

static void on_wp_lazy_toggled(GtkWidget *widget, gpointer data)
{
    (void)data;
    if (g_loading) {
        return;
    }
    char cmd[32], resp[64];
    snprintf(cmd, sizeof(cmd), "SETLAZY\t%d", gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget)) ? 1 : 0);
    xisback_send(cmd, resp, sizeof(resp));
}

static void load_wp_lazy(void)
{
    char resp[64];
    if (!xisback_send("LAZY", resp, sizeof(resp))) {
        return;
    }
    g_loading++;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_wp_lazy_chk), atoi(resp) != 0);
    g_loading--;
}

static void load_wp_actions(void)
{
    char resp[1024];
    if (!xisback_send("ACTIONS", resp, sizeof(resp))) {
        return;
    }
    char *save = NULL;
    char *left = strtok_r(resp, "\t", &save);
    char *right = left ? strtok_r(NULL, "\t", &save) : NULL;
    char *middle = right ? strtok_r(NULL, "\t", &save) : NULL;
    char *dbl = middle ? strtok_r(NULL, "\t", &save) : NULL;
    char *scroll_up = dbl ? strtok_r(NULL, "\t", &save) : NULL;
    char *scroll_down = scroll_up ? strtok_r(NULL, "\n", &save) : NULL;
    gtk_entry_set_text(GTK_ENTRY(g_wp_action_left), left ? left : "");
    gtk_entry_set_text(GTK_ENTRY(g_wp_action_right), right ? right : "");
    gtk_entry_set_text(GTK_ENTRY(g_wp_action_middle), middle ? middle : "");
    gtk_entry_set_text(GTK_ENTRY(g_wp_action_double), dbl ? dbl : "");
    gtk_entry_set_text(GTK_ENTRY(g_wp_action_scroll_up), scroll_up ? scroll_up : "");
    gtk_entry_set_text(GTK_ENTRY(g_wp_action_scroll_down), scroll_down ? scroll_down : "");
}

/* ---- tab --------------------------------------------------------------- */

static GtkWidget *wp_radio_row(const char *title, GtkWidget **first, GtkWidget **second, const char *l1,
                               const char *l2)
{
    GtkWidget *row = gtk_hbox_new(FALSE, 12);
    GtkWidget *label = gtk_label_new(title);
    gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
    gtk_widget_set_size_request(label, 90, -1);
    *first = gtk_radio_button_new_with_label(NULL, l1);
    *second = gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(*first), l2);
    gtk_box_pack_start(GTK_BOX(row), label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), *first, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), *second, FALSE, FALSE, 0);
    g_signal_connect(*first, "toggled", G_CALLBACK(on_wp_mode_toggled), NULL);
    g_signal_connect(*second, "toggled", G_CALLBACK(on_wp_mode_toggled), NULL);
    return row;
}

/* The tab is torn down whenever kiconf leaves it: stop everything that
 * would still touch its widgets, applying a change that was still waiting
 * for the user to stop clicking a spin button. */
static void on_wp_destroy(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    if (g_apply_timer) {
        g_source_remove(g_apply_timer);
        g_apply_timer = 0;
        wp_apply_form();
    }
    if (g_tick_timer) {
        g_source_remove(g_tick_timer);
        g_tick_timer = 0;
    }
    if (g_thumb_idle) {
        g_source_remove(g_thumb_idle);
        g_thumb_idle = 0;
    }
    /* Queued thumbnails would otherwise stay "still loading" forever. */
    char *path;
    while ((path = g_queue_pop_head(&g_thumb_queue))) {
        g_free(path);
    }
    if (g_thumbs) {
        g_hash_table_remove_all(g_thumbs);
    }
    g_canvas = NULL;
    g_n_minis = 0;
}

GtkWidget *build_wallpaper_tab(void)
{
    GtkWidget *outer = gtk_vbox_new(FALSE, 8);
    gtk_container_set_border_width(GTK_CONTAINER(outer), 12);

    g_wp_status_label = gtk_label_new("-");
    gtk_misc_set_alignment(GTK_MISC(g_wp_status_label), 0.0, 0.5);

    g_row_screens = wp_radio_row(_("Telas:"), &g_radio_split, &g_radio_span, _("Cada tela com o seu"),
                                 _("Uma imagem esticada por todas as telas"));
    gtk_box_pack_start(GTK_BOX(outer), g_row_screens, FALSE, FALSE, 0);
    GtkWidget *row_desk = wp_radio_row(_("Desktops:"), &g_radio_same, &g_radio_per,
                                       _("Mesmo em todos os desktops"), _("Um por desktop"));
    gtk_box_pack_start(GTK_BOX(outer), row_desk, FALSE, FALSE, 0);

    g_strip_box = gtk_hbox_new(FALSE, 6);
    g_strip = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(g_strip), GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(g_strip), GTK_SHADOW_NONE);
    gtk_scrolled_window_add_with_viewport(GTK_SCROLLED_WINDOW(g_strip), g_strip_box);
    gtk_widget_set_size_request(g_strip, -1, 112);
    gtk_widget_set_no_show_all(g_strip, TRUE);
    gtk_widget_show(gtk_bin_get_child(GTK_BIN(g_strip)));
    gtk_widget_show(g_strip_box);
    gtk_box_pack_start(GTK_BOX(outer), g_strip, FALSE, FALSE, 0);

    g_canvas = gtk_drawing_area_new();
    gtk_widget_set_size_request(g_canvas, 360, 240);
    gtk_widget_add_events(g_canvas, GDK_BUTTON_PRESS_MASK);
    g_signal_connect(g_canvas, "expose-event", G_CALLBACK(wp_canvas_expose), NULL);
    g_signal_connect(g_canvas, "button-press-event", G_CALLBACK(wp_canvas_press), NULL);
    static const GtkTargetEntry drop_targets[] = {{"text/uri-list", 0, 0}};
    gtk_drag_dest_set(g_canvas, GTK_DEST_DEFAULT_ALL, drop_targets, 1, GDK_ACTION_COPY);
    g_signal_connect(g_canvas, "drag-data-received", G_CALLBACK(on_wp_drop), NULL);
    gtk_box_pack_start(GTK_BOX(outer), g_canvas, TRUE, TRUE, 0);

    GtkWidget *form = gtk_vbox_new(FALSE, 6);
    g_wp_target_label = gtk_label_new("");
    gtk_misc_set_alignment(GTK_MISC(g_wp_target_label), 0.0, 0.5);
    gtk_box_pack_start(GTK_BOX(form), g_wp_target_label, FALSE, FALSE, 0);

    GtkWidget *path_row = gtk_hbox_new(FALSE, 4);
    g_wp_path_entry = gtk_entry_new();
    g_signal_connect(g_wp_path_entry, "activate", G_CALLBACK(on_wp_path_activate), NULL);
    gtk_box_pack_start(GTK_BOX(path_row), g_wp_path_entry, TRUE, TRUE, 0);
    GtkWidget *file_btn = gtk_button_new_with_label(_("Imagem..."));
    GtkWidget *folder_btn = gtk_button_new_with_label(_("Pasta (slideshow)..."));
    GtkWidget *color_btn = gtk_button_new_with_label(_("Cor solida..."));
    g_signal_connect(file_btn, "clicked", G_CALLBACK(on_wp_pick_file), NULL);
    g_signal_connect(folder_btn, "clicked", G_CALLBACK(on_wp_pick_folder), NULL);
    g_signal_connect(color_btn, "clicked", G_CALLBACK(on_wp_pick_color), NULL);
    gtk_box_pack_start(GTK_BOX(path_row), file_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(path_row), folder_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(path_row), color_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(form), path_row, FALSE, FALSE, 0);

    GtkWidget *opts = gtk_hbox_new(FALSE, 8);
    g_wp_mode_combo = gtk_combo_box_new_text();
    gtk_combo_box_append_text(GTK_COMBO_BOX(g_wp_mode_combo), _("Preencher"));
    gtk_combo_box_append_text(GTK_COMBO_BOX(g_wp_mode_combo), _("Esticar"));
    gtk_combo_box_set_active(GTK_COMBO_BOX(g_wp_mode_combo), 0);
    g_wp_interval_spin = gtk_spin_button_new_with_range(5, 86400, 5);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_wp_interval_spin), 300);
    g_wp_shuffle_chk = gtk_check_button_new_with_label(_("Aleatorio"));
    g_wp_fade_spin = gtk_spin_button_new_with_range(0.0, 5.0, 0.1);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(g_wp_fade_spin), 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_wp_fade_spin), 1.0);
    g_wp_next_btn = gtk_button_new_with_label(_("Proxima"));
    a11y_name(g_wp_mode_combo, _("Ajuste da imagem"));
    a11y_name(g_wp_interval_spin, _("Intervalo do slideshow em segundos"));
    a11y_name(g_wp_fade_spin, _("Crossfade em segundos"));
    gtk_box_pack_start(GTK_BOX(opts), g_wp_mode_combo, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(opts), gtk_label_new(_("Trocar a cada (s):")), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(opts), g_wp_interval_spin, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(opts), g_wp_shuffle_chk, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(opts), gtk_label_new(_("Fade (s):")), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(opts), g_wp_fade_spin, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(opts), g_wp_next_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(form), opts, FALSE, FALSE, 0);
    g_signal_connect(g_wp_mode_combo, "changed", G_CALLBACK(on_wp_form_changed), NULL);
    g_signal_connect(g_wp_interval_spin, "value-changed", G_CALLBACK(on_wp_form_changed), NULL);
    g_signal_connect(g_wp_shuffle_chk, "toggled", G_CALLBACK(on_wp_form_changed), NULL);
    g_signal_connect(g_wp_fade_spin, "value-changed", G_CALLBACK(on_wp_form_changed), NULL);
    g_signal_connect(g_wp_next_btn, "clicked", G_CALLBACK(on_wp_next), NULL);
    GtkWidget *form_frame = frame_with(_("Wallpaper"), form);
    gtk_widget_set_tooltip_text(form_frame, _("Clique numa tela pra escolher qual editar; arraste uma imagem ou pasta "
                                              "pra cima dela pra defini-la"));
    gtk_box_pack_start(GTK_BOX(outer), form_frame, FALSE, FALSE, 0);

    g_wp_lazy_chk = gtk_check_button_new_with_label(_("Carregar apenas o wallpaper atual (economiza memoria)"));
    gtk_widget_set_tooltip_text(g_wp_lazy_chk, _("Wallpapers de desktops ocultos nao ficam na memoria; sao "
                                                 "recarregados ao trocar de desktop. Salvo e aplicado ao iniciar o xisback."));
    g_signal_connect(g_wp_lazy_chk, "toggled", G_CALLBACK(on_wp_lazy_toggled), NULL);
    gtk_box_pack_start(GTK_BOX(outer), g_wp_lazy_chk, FALSE, FALSE, 0);
    kiconf_track_ignore(g_wp_lazy_chk);

    g_wp_layers_store = gtk_list_store_new(N_WP_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                           G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    g_wp_layers_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(g_wp_layers_store));
    const char *wp_titles[N_WP_COLS] = {_("Saida"), _("Desktop"), _("Modo"), _("Intervalo"), _("Aleatorio"), _("Fade"), _("Caminho")};
    for (int col = 0; col < N_WP_COLS; col++) {
        GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
        GtkTreeViewColumn *tvcol = gtk_tree_view_column_new_with_attributes(wp_titles[col], renderer, "text", col, NULL);
        gtk_tree_view_column_set_expand(tvcol, col == N_WP_COLS - 1);
        gtk_tree_view_append_column(GTK_TREE_VIEW(g_wp_layers_view), tvcol);
    }
    GtkWidget *wp_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(wp_scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(wp_scroll, -1, 140);
    gtk_container_add(GTK_CONTAINER(wp_scroll), g_wp_layers_view);
    GtkWidget *adv_box = gtk_vbox_new(FALSE, 4);
    gtk_box_pack_start(GTK_BOX(adv_box), g_wp_status_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(adv_box), wp_scroll, TRUE, TRUE, 0);
    GtkWidget *adv_btns = gtk_hbox_new(FALSE, 6);
    GtkWidget *clear_sel_btn = gtk_button_new_with_label(_("Limpar selecionada"));
    GtkWidget *clear_all_btn = gtk_button_new_with_label(_("Limpar todas"));
    GtkWidget *refresh_btn = gtk_button_new_with_label(_("Atualizar"));
    g_signal_connect(clear_sel_btn, "clicked", G_CALLBACK(on_wp_clear_selected), NULL);
    g_signal_connect(clear_all_btn, "clicked", G_CALLBACK(on_wp_clear_all), NULL);
    g_signal_connect(refresh_btn, "clicked", G_CALLBACK(on_wp_refresh), NULL);
    gtk_box_pack_start(GTK_BOX(adv_btns), clear_sel_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(adv_btns), clear_all_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(adv_btns), refresh_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(adv_box), adv_btns, FALSE, FALSE, 0);
    GtkWidget *adv = gtk_expander_new(_("Avancado: camadas do xisback"));
    gtk_container_add(GTK_CONTAINER(adv), adv_box);
    gtk_box_pack_start(GTK_BOX(outer), adv, FALSE, FALSE, 0);

    GtkWidget *actions_table = gtk_table_new(6, 2, FALSE);
    g_wp_action_left = gtk_entry_new();
    labeled_row(actions_table, 0, _("Clique esquerdo:"), g_wp_action_left);
    g_wp_action_right = gtk_entry_new();
    labeled_row(actions_table, 1, _("Clique direito:"), g_wp_action_right);
    g_wp_action_middle = gtk_entry_new();
    labeled_row(actions_table, 2, _("Clique do meio:"), g_wp_action_middle);
    g_wp_action_double = gtk_entry_new();
    labeled_row(actions_table, 3, _("Clique duplo:"), g_wp_action_double);
    g_wp_action_scroll_up = gtk_entry_new();
    labeled_row(actions_table, 4, _("Rolar pra cima:"), g_wp_action_scroll_up);
    g_wp_action_scroll_down = gtk_entry_new();
    labeled_row(actions_table, 5, _("Rolar pra baixo:"), g_wp_action_scroll_down);
    GtkWidget *actions_box = gtk_vbox_new(FALSE, 4);
    gtk_box_pack_start(GTK_BOX(actions_box), actions_table, FALSE, FALSE, 0);
    GtkWidget *actions_save_btn = gtk_button_new_with_label(_("Salvar acoes de clique"));
    g_signal_connect(actions_save_btn, "clicked", G_CALLBACK(on_wp_actions_save), actions_box);
    gtk_box_pack_start(GTK_BOX(actions_box), actions_save_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), frame_with(_("Acoes de clique"), actions_box), FALSE, FALSE, 0);

    load_wp_actions();
    load_wp_lazy();
    refresh_wp_layers();
    g_tick_timer = g_timeout_add_seconds(WP_TICK_SECONDS, wp_tick, NULL);
    /* Only the click actions wait for a Salvar; everything else applies
     * right away. */
    kiconf_track_ignore(g_row_screens);
    kiconf_track_ignore(row_desk);
    kiconf_track_ignore(form_frame);
    kiconf_track_ignore(adv);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_add_with_viewport(GTK_SCROLLED_WINDOW(scroll), outer);
    g_signal_connect(scroll, "map", G_CALLBACK(on_wp_map), NULL);
    g_signal_connect(scroll, "destroy", G_CALLBACK(on_wp_destroy), NULL);
    return scroll;
}
