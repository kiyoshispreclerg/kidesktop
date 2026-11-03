/* ks_rec.c - screen recording: capture thread, method choice and sinks.
 *
 * Capture, cheapest first:
 *   GPU copy  the server CopyArea()s the root into a pixmap of ours, which
 *             DRI3 exports once as a dma-buf; the libav plugin hands it to
 *             VAAPI (zero-copy encode, or GPU colour conversion + download
 *             for a software encoder). Nothing is read back by the X
 *             server, which is what makes it light: a GetImage stalls the
 *             whole server on a synchronous glReadPixels. Used only when
 *             the server sets PresentCapabilityXiSRootCopy on the CRTC
 *             (otherwise a per-CRTC flip leaves the root pixmap stale and
 *             the video freezes) -- same detection as our OBS fork's
 *             xshm-input.c -- or when forced with copy_method=gpu.
 *   SHM       MIT-SHM GetImage of the root, the classic method.
 *
 * Encoding: the libav plugin when it loads, else BGRX frames piped into the
 * ffmpeg command (which then also records the audio itself).
 *
 * Censored windows are painted black on the copy (server-side FillRectangle
 * on our pixmap, or memset on the SHM buffer); the screen is untouched. */
#include "ks.h"
#include "ks_plugin.h"
#include "../shared/xis_i18n.h"
#include <xcb/xcb.h>
#include <xcb/xcbext.h>
#include <xcb/shm.h>
#include <xcb/randr.h>
#include <xcb/xfixes.h>
#include <xcb/render.h>
#include <math.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef KISHOT_PLUGINDIR
#define KISHOT_PLUGINDIR "/usr/local/lib/kishot"
#endif

/* ---- capability probe ---------------------------------------------------- */

static KsRecCaps caps;
static gboolean probed;
static const KsPluginApi *plugin;
static GPtrArray *cli_encoders;

/* Known video encoders, in "auto" preference order. */
static const char *const kKnownEncoders[] = {
    "h264_vaapi", "hevc_vaapi", "vp9_vaapi", "av1_vaapi",
    "libx264", "libx265", "libvpx-vp9", "libvpx", "libsvtav1", "libaom-av1",
    "mpeg4", "ffv1", NULL
};

static void load_plugin(void)
{
    char *paths[3] = { NULL, NULL, NULL };
    const char *env = g_getenv("KISHOT_PLUGIN");
    if (env && *env)
        paths[0] = g_strdup(env);
    /* running from the build tree */
    char *self = g_file_read_link("/proc/self/exe", NULL);
    if (self) {
        char *dir = g_path_get_dirname(self);
        paths[1] = g_build_filename(dir, "plugin", "kishot-rec-av.so", NULL);
        g_free(dir);
        g_free(self);
    }
    paths[2] = g_build_filename(KISHOT_PLUGINDIR, "kishot-rec-av.so", NULL);
    for (int i = 0; i < 3 && !plugin; i++) {
        if (!paths[i] || !g_file_test(paths[i], G_FILE_TEST_EXISTS))
            continue;
        void *h = dlopen(paths[i], RTLD_NOW | RTLD_LOCAL);
        if (!h) {
            g_printerr("kishot: plugin %s not loaded: %s\n", paths[i], dlerror());
            continue;
        }
        KsPluginEntry entry = (KsPluginEntry)dlsym(h, KS_PLUGIN_ENTRY);
        const KsPluginApi *api = entry ? entry() : NULL;
        if (!api || api->abi != KS_PLUGIN_ABI) {
            g_printerr("kishot: plugin %s has the wrong interface\n", paths[i]);
            dlclose(h);
            continue;
        }
        plugin = api;
    }
    for (int i = 0; i < 3; i++)
        g_free(paths[i]);
}

static char *run_ffmpeg(const char *arg)
{
    char *argv[] = { "ffmpeg", "-hide_banner", (char *)arg, NULL };
    char *out = NULL;
    if (!g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL,
                      NULL, NULL, &out, NULL, NULL, NULL))
        return NULL;
    return out;
}

static void probe_cli(void)
{
    char *path = g_find_program_in_path("ffmpeg");
    if (!path)
        return;
    g_free(path);
    char *enc = run_ffmpeg("-encoders");
    if (!enc)
        return;
    caps.cli = TRUE;
    cli_encoders = g_ptr_array_new();
    /* lines look like " V....D libx264              libx264 H.264 ..." */
    char **lines = g_strsplit(enc, "\n", -1);
    for (int k = 0; kKnownEncoders[k]; k++)
        for (char **l = lines; *l; l++) {
            char flags[16], name[64];
            if (sscanf(*l, " %15s %63s", flags, name) == 2 && flags[0] == 'V' &&
                !strcmp(name, kKnownEncoders[k]))
                g_ptr_array_add(cli_encoders, (gpointer)kKnownEncoders[k]);
        }
    g_strfreev(lines);
    g_free(enc);
}

static char *x_render_node(void);
static char *x_node;            /* the X server's GPU, e.g. /dev/dri/renderD128 */

const KsRecCaps *ks_rec_probe(const KsConfig *cfg)
{
    if (!probed) {
        probed = TRUE;
        signal(SIGPIPE, SIG_IGN);
        load_plugin();
        x_node = x_render_node();
        if (plugin)
            plugin->set_device(x_node);
        probe_cli();
        caps.plugin = plugin != NULL;
        caps.vaapi = plugin && plugin->vaapi_available();
        caps.audio = ks_audio_available();
    }
    if (caps.encoders)
        g_ptr_array_free(caps.encoders, TRUE);
    caps.encoders = g_ptr_array_new();
    gboolean use_plugin = plugin && cfg->backend != KS_BACKEND_CLI;
    if (use_plugin) {
        for (const char *const *e = plugin->encoders(); *e; e++)
            g_ptr_array_add(caps.encoders, (gpointer)*e);
    } else if (caps.cli && cfg->backend != KS_BACKEND_PLUGIN) {
        for (guint i = 0; i < cli_encoders->len; i++)
            g_ptr_array_add(caps.encoders, cli_encoders->pdata[i]);
    }
    g_free(caps.why_not);
    caps.why_not = NULL;
    caps.usable = caps.encoders->len > 0;
    if (!caps.usable) {
        if (cfg->backend == KS_BACKEND_PLUGIN && !plugin)
            caps.why_not = g_strdup(_("the libav plugin (kishot-rec-av.so) could not be loaded"));
        else if (!plugin && !caps.cli)
            caps.why_not = g_strdup(_("install ffmpeg to record the screen"));
        else
            caps.why_not = g_strdup(_("no usable video encoder was found"));
    }
    return &caps;
}

/* ---- X helpers (raw requests, so kishot does not link xcb-present/dri3) -- */

#define PRESENT_QUERY_CAPABILITIES 4
#define PRESENT_CAP_XIS_ROOT_COPY 0x80000000u
static xcb_extension_t present_id = { "Present", 0 };
static xcb_extension_t dri3_id = { "DRI3", 0 };

static xcb_extension_t xscale_id = { "X-SCALE", 0 };

/* A request with no reply; returns its cookie for xcb_request_check(). */
static xcb_void_cookie_t raw_void_request(xcb_connection_t *c, xcb_extension_t *ext, uint8_t minor,
                                          void *req, size_t len, int flags)
{
    struct iovec parts[4];
    parts[2].iov_base = req;
    parts[2].iov_len = len;
    parts[3].iov_base = NULL;
    parts[3].iov_len = -len & 3;
    xcb_protocol_request_t proto = { 2, ext, minor, 1 };
    xcb_void_cookie_t ck = { xcb_send_request(c, flags, parts + 2, &proto) };
    return ck;
}

static void *raw_request(xcb_connection_t *c, xcb_extension_t *ext, uint8_t minor,
                         void *req, size_t len, int fds_flag, xcb_generic_error_t **err)
{
    struct iovec parts[4];
    parts[2].iov_base = req;
    parts[2].iov_len = len;
    parts[3].iov_base = NULL;
    parts[3].iov_len = -len & 3;
    xcb_protocol_request_t proto = { 2, ext, minor, 1 };
    unsigned int seq = xcb_send_request(c, XCB_REQUEST_CHECKED | fds_flag, parts + 2, &proto);
    return xcb_wait_for_reply(c, seq, err);
}

/* The render node of the GPU the X server draws with: DRI3Open gives a fd
 * on it, sysfs maps that device to its renderD node. */
static char *x_render_node(void)
{
    char *path = NULL;
    xcb_connection_t *c = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(c)) {
        xcb_disconnect(c);
        return NULL;
    }
    const xcb_query_extension_reply_t *ext = xcb_get_extension_data(c, &dri3_id);
    xcb_window_t root = xcb_setup_roots_iterator(xcb_get_setup(c)).data->root;
    if (ext && ext->present) {
        struct { uint8_t major, minor; uint16_t len; uint32_t drawable, provider; } req = { 0, 0, 0, root, 0 };
        xcb_generic_error_t *err = NULL;
        struct { uint8_t type, nfd; uint16_t seq; uint32_t len; uint8_t pad[24]; } *rep =
            raw_request(c, &dri3_id, 1, &req, sizeof(req), XCB_REQUEST_REPLY_FDS, &err);
        free(err);
        if (rep && rep->nfd == 1) {
            int fd = xcb_get_reply_fds(c, rep, sizeof(*rep))[0];
            struct stat st;
            if (fstat(fd, &st) == 0 && S_ISCHR(st.st_mode)) {
                char *dir = g_strdup_printf("/sys/dev/char/%u:%u/device/drm", major(st.st_rdev), minor(st.st_rdev));
                GDir *d = g_dir_open(dir, 0, NULL);
                const char *name;
                while (d && (name = g_dir_read_name(d)))
                    if (g_str_has_prefix(name, "renderD")) {
                        path = g_build_filename("/dev/dri", name, NULL);
                        break;
                    }
                if (d)
                    g_dir_close(d);
                g_free(dir);
            }
            close(fd);
        }
        free(rep);
    }
    xcb_disconnect(c);
    return path;
}

static xcb_randr_crtc_t pick_crtc(xcb_connection_t *c, xcb_window_t root, const KsRect *a)
{
    xcb_randr_crtc_t best = XCB_NONE;
    int64_t best_overlap = -1;
    xcb_randr_get_screen_resources_current_reply_t *res = xcb_randr_get_screen_resources_current_reply(
        c, xcb_randr_get_screen_resources_current(c, root), NULL);
    if (!res)
        return XCB_NONE;
    xcb_randr_crtc_t *crtcs = xcb_randr_get_screen_resources_current_crtcs(res);
    int n = xcb_randr_get_screen_resources_current_crtcs_length(res);
    for (int i = 0; i < n; i++) {
        xcb_randr_get_crtc_info_reply_t *ci = xcb_randr_get_crtc_info_reply(
            c, xcb_randr_get_crtc_info(c, crtcs[i], res->config_timestamp), NULL);
        if (!ci)
            continue;
        if (ci->width && ci->height) {
            int64_t ox = MIN((int64_t)ci->x + ci->width, (int64_t)a->x + a->w) - MAX((int64_t)ci->x, (int64_t)a->x);
            int64_t oy = MIN((int64_t)ci->y + ci->height, (int64_t)a->y + a->h) - MAX((int64_t)ci->y, (int64_t)a->y);
            int64_t ov = (ox > 0 && oy > 0) ? ox * oy : 0;
            if (ov > best_overlap) {
                best_overlap = ov;
                best = crtcs[i];
            }
        }
        free(ci);
    }
    free(res);
    return best;
}

/* Does the server guarantee that a root CopyArea shows what is displayed? */
static gboolean server_has_root_copy(xcb_connection_t *c, xcb_window_t root, const KsRect *a)
{
    const xcb_query_extension_reply_t *ext = xcb_get_extension_data(c, &present_id);
    if (!ext || !ext->present || !xcb_get_extension_data(c, &xcb_randr_id)->present)
        return FALSE;
    xcb_randr_crtc_t crtc = pick_crtc(c, root, a);
    if (crtc == XCB_NONE)
        return FALSE;
    struct { uint8_t major, minor; uint16_t len; uint32_t target; } req = { 0, 0, 0, crtc };
    xcb_generic_error_t *err = NULL;
    struct { uint8_t type, pad0; uint16_t seq; uint32_t len; uint32_t caps; uint8_t pad1[20]; } *rep =
        raw_request(c, &present_id, PRESENT_QUERY_CAPABILITIES, &req, sizeof(req), 0, &err);
    free(err);
    if (!rep)
        return FALSE;
    gboolean ok = (rep->caps & PRESENT_CAP_XIS_ROOT_COPY) != 0;
    free(rep);
    return ok;
}

/* DRI3 BufferFromPixmap; returns the dma-buf fd or -1 */
static int pixmap_dmabuf(xcb_connection_t *c, xcb_pixmap_t pix, int *stride, uint64_t *size)
{
    const xcb_query_extension_reply_t *ext = xcb_get_extension_data(c, &dri3_id);
    if (!ext || !ext->present)
        return -1;
    xcb_generic_error_t *err = NULL;
    struct { uint8_t major, minor; uint16_t len; uint32_t maj, min; } qv = { 0, 0, 0, 1, 0 };
    void *vr = raw_request(c, &dri3_id, 0, &qv, sizeof(qv), 0, &err);
    free(err);
    err = NULL;
    if (!vr)
        return -1;
    free(vr);
    struct { uint8_t major, minor; uint16_t len; uint32_t pixmap; } req = { 0, 0, 0, pix };
    struct { uint8_t type, nfd; uint16_t seq; uint32_t len; uint32_t size; uint16_t w, h, stride;
             uint8_t depth, bpp; uint8_t pad[12]; } *rep =
        raw_request(c, &dri3_id, 3, &req, sizeof(req), XCB_REQUEST_REPLY_FDS, &err);
    free(err);
    if (!rep)
        return -1;
    int fd = -1;
    if (rep->nfd == 1 && rep->bpp == 32) {
        int *fds = xcb_get_reply_fds(c, rep, sizeof(*rep));
        fd = fds[0];
        *stride = rep->stride;
        *size = rep->size;
    }
    free(rep);
    return fd;
}

static int render_format(xcb_connection_t *c, int depth, gboolean alpha, xcb_render_pictformat_t *out)
{
    xcb_render_query_pict_formats_reply_t *r =
        xcb_render_query_pict_formats_reply(c, xcb_render_query_pict_formats(c), NULL);
    if (!r)
        return 0;
    int found = 0;
    xcb_render_pictforminfo_iterator_t it = xcb_render_query_pict_formats_formats_iterator(r);
    for (; it.rem && !found; xcb_render_pictforminfo_next(&it)) {
        xcb_render_pictforminfo_t *f = it.data;
        if (f->type != XCB_RENDER_PICT_TYPE_DIRECT || f->depth != depth)
            continue;
        if (f->direct.red_shift == 16 && f->direct.green_shift == 8 && f->direct.blue_shift == 0 &&
            f->direct.red_mask == 0xff && (alpha ? (f->direct.alpha_shift == 24 && f->direct.alpha_mask == 0xff)
                                                 : f->direct.alpha_mask == 0)) {
            *out = f->id;
            found = 1;
        }
    }
    free(r);
    return found;
}

/* ---- recording state ----------------------------------------------------- */

#define KS_RING 3

/* One monitor's part of the area, when recording at the physical size:
 * where it is in the CRTC's scanout (s*) and where it goes in the frame
 * (d*). A monitor less scaled than the frame is copied into `tmp` at its
 * own size and scaled up from there. An unscaled one (`from_root`) is read
 * from the root instead, s* being root coordinates: the server's own copy
 * of such a CRTC (XScaleCopyCrtcScanout) reads the screen pixmap without
 * what a compositor presents, and comes out black. */
#define KS_PHYS_MAX 8
typedef struct {
    xcb_randr_crtc_t crtc;
    gboolean from_root;
    int sx, sy, sw, sh;
    int dx, dy, dw, dh;
    xcb_pixmap_t tmp;
    xcb_render_picture_t tmp_pic;
} PhysPart;

typedef struct {
    KsRect area;
    /* The frame: the area, times `scale` when recording at the screens'
     * physical size (X-SCALE), else the area itself. */
    int fw, fh;
    double scale;
    PhysPart part[KS_PHYS_MAX];
    int nparts;
    gboolean phys, phys_gaps;
    xcb_pixmap_t phys_pix;      /* the SHM path composes the frame here */
    xcb_gcontext_t gc_root;     /* IncludeInferiors, for the unscaled monitors */
    xcb_render_picture_t phys_pic;
    int fps;
    gboolean pointer;
    char *path;
    KsRecFinished done;
    void *done_data;

    /* sink */
    void *ph;                   /* plugin handle */
    GPid cli_pid;
    int cli_in;
    char *cli_log;
    int cli_audio;              /* write end of ffmpeg's fd 3: S16LE PCM */
    gboolean audio_primed;      /* leading silence written */
    gboolean use_gpu;

    /* X */
    xcb_connection_t *c;
    xcb_window_t root;
    /* GPU copy: a ring, so the encoder reads a frame the server finished
     * copying one capture ago while the next copy goes to another buffer */
    xcb_pixmap_t pixmap[KS_RING];
    xcb_render_picture_t pic[KS_RING];
    int dmabuf[KS_RING];
    xcb_gcontext_t gc_copy, gc_black;
    int stride;
    uint64_t dsize;
    int shmid;
    uint8_t *shm;
    xcb_shm_seg_t seg;

    /* pointer */
    gboolean cursor_dirty;
    gboolean cursor_hidden;     /* XFixesHideCursor in effect (fork extension) */
    uint32_t *cur_img;
    int cur_w, cur_h, cur_xhot, cur_yhot;
    xcb_pixmap_t cur_pix;
    xcb_render_picture_t cur_pic;

    GThread *thread;
    volatile gint stop;
    gint64 start_us;
    char *method;
    char *error;
} Rec;

static Rec *rec;
static GMutex rec_lock;

gboolean ks_rec_active(void) { return rec != NULL; }
const char *ks_rec_method(void) { return rec ? rec->method : NULL; }
double ks_rec_elapsed(void) { return rec ? (g_get_monotonic_time() - rec->start_us) / 1e6 : 0; }

static void set_error(Rec *r, const char *fmt, ...) G_GNUC_PRINTF(2, 3);
static void set_error(Rec *r, const char *fmt, ...)
{
    if (r->error)
        return;
    va_list ap;
    va_start(ap, fmt);
    r->error = g_strdup_vprintf(fmt, ap);
    va_end(ap);
}

/* ---- pointer ------------------------------------------------------------- */

/* The fork's XFixes extension: CursorNotify subtype 1 (selected with mask bit
 * 1) says the cursor was hidden (cursor_serial 1) or shown (0) by
 * XFixesHideCursor, which changes no cursor image and so notified nothing. A
 * server without it refuses the bit and the cursor is drawn as before. */
#define KS_XFIXES_CURSOR_VISIBILITY 1
#define KS_XFIXES_CURSOR_VISIBILITY_MASK (1u << 1)

static void select_cursor_visibility(Rec *r)
{
    xcb_generic_error_t *e = xcb_request_check(r->c,
        xcb_xfixes_select_cursor_input_checked(r->c, r->root,
            XCB_XFIXES_CURSOR_NOTIFY_MASK_DISPLAY_CURSOR | KS_XFIXES_CURSOR_VISIBILITY_MASK));
    if (e) {
        free(e);
        xcb_xfixes_select_cursor_input(r->c, r->root, XCB_XFIXES_CURSOR_NOTIFY_MASK_DISPLAY_CURSOR);
    }
}

static void fetch_cursor(Rec *r)
{
    xcb_xfixes_get_cursor_image_reply_t *ci =
        xcb_xfixes_get_cursor_image_reply(r->c, xcb_xfixes_get_cursor_image(r->c), NULL);
    r->cursor_dirty = FALSE;
    if (!ci)
        return;
    g_free(r->cur_img);
    r->cur_w = ci->width;
    r->cur_h = ci->height;
    r->cur_xhot = ci->xhot;
    r->cur_yhot = ci->yhot;
    r->cur_img = g_memdup2(xcb_xfixes_get_cursor_image_cursor_image(ci), (gsize)ci->width * ci->height * 4);
    free(ci);
    if (!r->use_gpu && !r->phys)
        return;
    /* upload for the server-side composite */
    if (r->cur_pic)
        xcb_render_free_picture(r->c, r->cur_pic);
    if (r->cur_pix)
        xcb_free_pixmap(r->c, r->cur_pix);
    r->cur_pic = r->cur_pix = 0;
    xcb_render_pictformat_t argb;
    if (!r->cur_w || !r->cur_h || !render_format(r->c, 32, TRUE, &argb))
        return;
    r->cur_pix = xcb_generate_id(r->c);
    xcb_create_pixmap(r->c, 32, r->cur_pix, r->root, r->cur_w, r->cur_h);
    xcb_gcontext_t gc = xcb_generate_id(r->c);
    xcb_create_gc(r->c, gc, r->cur_pix, 0, NULL);
    xcb_put_image(r->c, XCB_IMAGE_FORMAT_Z_PIXMAP, r->cur_pix, gc, r->cur_w, r->cur_h, 0, 0, 0, 32,
                  r->cur_w * r->cur_h * 4, (const uint8_t *)r->cur_img);
    xcb_free_gc(r->c, gc);
    r->cur_pic = xcb_generate_id(r->c);
    xcb_render_create_picture(r->c, r->cur_pic, r->cur_pix, argb, 0, NULL);
    if (r->scale > 1) {
        /* Render maps destination to source, hence 1/scale; nearest, like
         * the hardware cursor the server enlarges on a scaled monitor */
        int32_t inv = (int32_t)lround(65536.0 / r->scale);
        xcb_render_transform_t t = { inv, 0, 0, 0, inv, 0, 0, 0, 1 << 16 };
        xcb_render_set_picture_transform(r->c, r->cur_pic, t);
        const char nearest[] = "nearest";
        xcb_render_set_picture_filter(r->c, r->cur_pic, sizeof(nearest) - 1, nearest, 0, NULL);
    }
}

static void poll_x_events(Rec *r)
{
    xcb_generic_event_t *ev;
    const xcb_query_extension_reply_t *xf = xcb_get_extension_data(r->c, &xcb_xfixes_id);
    while ((ev = xcb_poll_for_event(r->c))) {
        if (ev->response_type == 0 && g_getenv("KISHOT_DEBUG")) {
            xcb_generic_error_t *e = (xcb_generic_error_t *)ev;
            fprintf(stderr, "kishot: X error %d on request %d.%d\n", e->error_code, e->major_code, e->minor_code);
        }
        if (xf && (ev->response_type & 0x7f) == xf->first_event + XCB_XFIXES_CURSOR_NOTIFY &&
            ((xcb_xfixes_cursor_notify_event_t *)ev)->subtype != KS_XFIXES_CURSOR_VISIBILITY)
            r->cursor_dirty = TRUE;
        if (xf && (ev->response_type & 0x7f) == xf->first_event + XCB_XFIXES_CURSOR_NOTIFY &&
            ((xcb_xfixes_cursor_notify_event_t *)ev)->subtype == KS_XFIXES_CURSOR_VISIBILITY)
            r->cursor_hidden = ((xcb_xfixes_cursor_notify_event_t *)ev)->cursor_serial != 0;
        free(ev);
    }
}

/* The cursor, with its hotspot on (px, py). */
static void blend_cursor_cpu(Rec *r, uint8_t *buf, int stride, int px, int py)
{
    int ox = px - r->cur_xhot - r->area.x, oy = py - r->cur_yhot - r->area.y;
    for (int y = 0; y < r->cur_h; y++) {
        int dy = oy + y;
        if (dy < 0 || dy >= r->fh)
            continue;
        for (int x = 0; x < r->cur_w; x++) {
            int dx = ox + x;
            if (dx < 0 || dx >= r->fw)
                continue;
            uint32_t p = r->cur_img[y * r->cur_w + x];
            unsigned a = p >> 24;
            if (!a)
                continue;
            uint8_t *d = buf + dy * stride + dx * 4;   /* B G R X */
            d[0] = (p & 0xff) + d[0] * (255 - a) / 255;
            d[1] = ((p >> 8) & 0xff) + d[1] * (255 - a) / 255;
            d[2] = ((p >> 16) & 0xff) + d[2] * (255 - a) / 255;
        }
    }
}

/* ---- setup --------------------------------------------------------------- */

static gboolean setup_gpu(Rec *r, const xcb_screen_t *scr)
{
    for (int i = 0; i < KS_RING; i++) {
        r->dmabuf[i] = -1;
        r->pixmap[i] = xcb_generate_id(r->c);
        xcb_generic_error_t *e = xcb_request_check(r->c,
            xcb_create_pixmap_checked(r->c, scr->root_depth, r->pixmap[i], r->root, r->fw, r->fh));
        if (e) {
            free(e);
            r->pixmap[i] = 0;
            return FALSE;
        }
    }
    /* IncludeInferiors, or the copy skips every top level window; no
     * graphics exposures, or each copy queues a NoExpose nobody reads */
    uint32_t v[] = { XCB_SUBWINDOW_MODE_INCLUDE_INFERIORS, 0 };
    r->gc_copy = xcb_generate_id(r->c);
    xcb_create_gc(r->c, r->gc_copy, r->pixmap[0], XCB_GC_SUBWINDOW_MODE | XCB_GC_GRAPHICS_EXPOSURES, v);
    uint32_t black[] = { 0 };
    r->gc_black = xcb_generate_id(r->c);
    xcb_create_gc(r->c, r->gc_black, r->pixmap[0], XCB_GC_FOREGROUND, black);
    xcb_render_pictformat_t fmt;
    gboolean have_fmt = render_format(r->c, scr->root_depth, FALSE, &fmt);
    for (int i = 0; i < KS_RING; i++) {
        if (have_fmt) {
            r->pic[i] = xcb_generate_id(r->c);
            xcb_render_create_picture(r->c, r->pic[i], r->pixmap[i], fmt, 0, NULL);
        }
        r->dmabuf[i] = pixmap_dmabuf(r->c, r->pixmap[i], &r->stride, &r->dsize);
        if (r->dmabuf[i] < 0)
            return FALSE;
    }
    return TRUE;
}

static void free_gpu(Rec *r)
{
    for (int i = 0; i < KS_RING; i++) {
        if (r->dmabuf[i] >= 0)
            close(r->dmabuf[i]);
        r->dmabuf[i] = -1;
        if (r->pic[i])
            xcb_render_free_picture(r->c, r->pic[i]);
        if (r->pixmap[i])
            xcb_free_pixmap(r->c, r->pixmap[i]);
        r->pic[i] = r->pixmap[i] = 0;
    }
    if (r->gc_copy)
        xcb_free_gc(r->c, r->gc_copy);
    if (r->gc_black)
        xcb_free_gc(r->c, r->gc_black);
    r->gc_copy = r->gc_black = 0;
}

static gboolean setup_shm(Rec *r)
{
    if (!xcb_get_extension_data(r->c, &xcb_shm_id)->present)
        return FALSE;
    size_t size = (size_t)r->fw * r->fh * 4;
    r->shmid = shmget(IPC_PRIVATE, size, IPC_CREAT | 0600);
    if (r->shmid < 0)
        return FALSE;
    r->shm = shmat(r->shmid, NULL, 0);
    shmctl(r->shmid, IPC_RMID, NULL);   /* freed once both sides detach */
    if (r->shm == (void *)-1) {
        r->shm = NULL;
        return FALSE;
    }
    r->seg = xcb_generate_id(r->c);
    xcb_generic_error_t *e = xcb_request_check(r->c, xcb_shm_attach_checked(r->c, r->seg, r->shmid, 0));
    if (e) {
        free(e);
        return FALSE;
    }
    r->stride = r->fw * 4;
    return TRUE;
}

/* ---- recording at the screens' physical size (X-SCALE) ------------------- */

#define XSCALE_GET_CRTC_SCANOUT 2
#define XSCALE_COPY_CRTC_SCANOUT 4

typedef struct { uint8_t major, minor; uint16_t len; uint32_t crtc, drawable;
                 int16_t src_x, src_y; uint16_t w, h; int16_t dst_x, dst_y; } XScaleCopyReq;

static xcb_void_cookie_t xscale_copy(Rec *r, xcb_randr_crtc_t crtc, xcb_drawable_t dst, int sx, int sy,
                                     int w, int h, int dx, int dy, int flags)
{
    XScaleCopyReq q = { 0, 0, 0, crtc, dst, (int16_t)sx, (int16_t)sy, (uint16_t)w, (uint16_t)h,
                        (int16_t)dx, (int16_t)dy };
    return raw_void_request(r->c, &xscale_id, XSCALE_COPY_CRTC_SCANOUT, &q, sizeof(q), flags);
}

static void free_phys(Rec *r)
{
    for (int i = 0; i < r->nparts; i++) {
        if (r->part[i].tmp_pic)
            xcb_render_free_picture(r->c, r->part[i].tmp_pic);
        if (r->part[i].tmp)
            xcb_free_pixmap(r->c, r->part[i].tmp);
    }
    r->nparts = 0;
    if (r->gc_root)
        xcb_free_gc(r->c, r->gc_root);
    r->gc_root = 0;
    if (r->phys_pic)
        xcb_render_free_picture(r->c, r->phys_pic);
    if (r->phys_pix)
        xcb_free_pixmap(r->c, r->phys_pix);
    r->phys_pic = r->phys_pix = 0;
}

/* The frame for recording `area` at the screens' physical size: every
 * monitor under it is asked its scanout size (XScaleGetCrtcScanout), and
 * the frame takes the largest scale among them, the others scaled up to it
 * -- what KWin's ScreenShotNativeResolution does. Fills the parts' source
 * and destination boxes; returns NULL when that applies, else why not. */
typedef struct {
    PhysPart part[KS_PHYS_MAX];
    gboolean direct[KS_PHYS_MAX];   /* at the frame's scale: copied straight in */
    int n, fw, fh;
    double scale;
    gboolean gaps;
} PhysPlan;

static const char *phys_plan(xcb_connection_t *c, xcb_window_t root, const KsRect *area, PhysPlan *pl)
{
    memset(pl, 0, sizeof(*pl));
    pl->scale = 1;
    const xcb_query_extension_reply_t *ext = xcb_get_extension_data(c, &xscale_id);
    if (!ext || !ext->present || !xcb_get_extension_data(c, &xcb_randr_id)->present)
        return _("the X server has no X-SCALE");
    xcb_randr_get_screen_resources_current_reply_t *res = xcb_randr_get_screen_resources_current_reply(
        c, xcb_randr_get_screen_resources_current(c, root), NULL);
    if (!res)
        return _("no RandR screen resources");
    struct { int x, y, w, h; double fx, fy; } m[KS_PHYS_MAX];
    int n = 0;
    double scale = 1;
    int64_t covered = 0;
    xcb_randr_crtc_t *crtcs = xcb_randr_get_screen_resources_current_crtcs(res);
    int ncrtc = xcb_randr_get_screen_resources_current_crtcs_length(res);
    for (int i = 0; i < ncrtc && n < KS_PHYS_MAX; i++) {
        struct { uint8_t major, minor; uint16_t len; uint32_t crtc; } q = { 0, 0, 0, crtcs[i] };
        xcb_generic_error_t *err = NULL;
        struct { uint8_t type, flags; uint16_t seq; uint32_t len; uint16_t sw, sh; int16_t x, y;
                 uint16_t w, h; uint32_t pad[3]; } *rep =
            raw_request(c, &xscale_id, XSCALE_GET_CRTC_SCANOUT, &q, sizeof(q), 0, &err);
        free(err);
        if (!rep)
            continue;
        int x1 = MAX(rep->x, area->x), y1 = MAX(rep->y, area->y);
        int x2 = MIN(rep->x + rep->w, area->x + area->w), y2 = MIN(rep->y + rep->h, area->y + area->h);
        if (rep->w && rep->h && rep->sw && rep->sh && x2 > x1 && y2 > y1) {
            PhysPart *pp = &pl->part[n];
            m[n].x = x1;
            m[n].y = y1;
            m[n].w = x2 - x1;
            m[n].h = y2 - y1;
            m[n].fx = (double)rep->sw / rep->w;
            m[n].fy = (double)rep->sh / rep->h;
            pp->crtc = crtcs[i];
            scale = MAX(scale, MAX(m[n].fx, m[n].fy));
            covered += (int64_t)m[n].w * m[n].h;
            pp->from_root = fabs(m[n].fx - 1) < 0.01 && fabs(m[n].fy - 1) < 0.01;
            if (pp->from_root) {
                pp->sx = x1;
                pp->sy = y1;
                pp->sw = x2 - x1;
                pp->sh = y2 - y1;
            } else {
                /* the scanout starts at the monitor's logical origin */
                pp->sx = (int)lround((x1 - rep->x) * m[n].fx);
                pp->sy = (int)lround((y1 - rep->y) * m[n].fy);
                pp->sw = (int)lround((x2 - rep->x) * m[n].fx) - pp->sx;
                pp->sh = (int)lround((y2 - rep->y) * m[n].fy) - pp->sy;
            }
            n++;
        }
        free(rep);
    }
    free(res);
    if (scale < 1.001)
        return _("no screen under the area is scaled");

    pl->n = n;
    pl->scale = scale;
    pl->fw = (int)lround(area->w * scale) & ~1;
    pl->fh = (int)lround(area->h * scale) & ~1;
    pl->gaps = covered < (int64_t)area->w * area->h;
    for (int i = 0; i < n; i++) {
        PhysPart *pp = &pl->part[i];
        pp->dx = (int)lround((m[i].x - area->x) * scale);
        pp->dy = (int)lround((m[i].y - area->y) * scale);
        pp->dw = MIN((int)lround((m[i].x + m[i].w - area->x) * scale), pl->fw) - pp->dx;
        pp->dh = MIN((int)lround((m[i].y + m[i].h - area->y) * scale), pl->fh) - pp->dy;
        pl->direct[i] = fabs(m[i].fx - scale) < 0.01 && fabs(m[i].fy - scale) < 0.01;
    }
    return NULL;
}

const char *ks_rec_physical_size(const KsRect *area, int *w, int *h)
{
    xcb_connection_t *c = ks_capture_gdk_connection();
    PhysPlan pl;
    KsRect a = *area;
    a.w &= ~1;
    a.h &= ~1;
    const char *why = phys_plan(c, xcb_setup_roots_iterator(xcb_get_setup(c)).data->root, &a, &pl);
    *w = why ? a.w : pl.fw;
    *h = why ? a.h : pl.fh;
    return why;
}

/* Sets the recording up at the physical size (phys_plan()). Returns NULL
 * when it is, else why not (and the frame stays the area). */
static const char *setup_phys(Rec *r, const xcb_screen_t *scr)
{
    PhysPlan pl;
    const char *why = phys_plan(r->c, r->root, &r->area, &pl);
    if (why)
        return why;
    int n = pl.n;
    memcpy(r->part, pl.part, sizeof(r->part));
    r->scale = pl.scale;
    r->fw = pl.fw;
    r->fh = pl.fh;
    r->phys_gaps = pl.gaps;
    xcb_render_pictformat_t fmt;
    gboolean have_fmt = render_format(r->c, scr->root_depth, FALSE, &fmt);
    for (int i = 0; i < n; i++) {
        PhysPart *pp = &r->part[i];
        if (pl.direct[i])
            continue;
        if (pp->from_root && !r->gc_root) {
            uint32_t v[] = { XCB_SUBWINDOW_MODE_INCLUDE_INFERIORS, 0 };
            r->gc_root = xcb_generate_id(r->c);
            xcb_create_gc(r->c, r->gc_root, r->root, XCB_GC_SUBWINDOW_MODE | XCB_GC_GRAPHICS_EXPOSURES, v);
        }
        if (!have_fmt || pp->sw <= 0 || pp->sh <= 0) {
            r->nparts = i;
            free_phys(r);
            return _("Render can't scale a less scaled screen up");
        }
        pp->tmp = xcb_generate_id(r->c);
        xcb_create_pixmap(r->c, scr->root_depth, pp->tmp, r->root, pp->sw, pp->sh);
        pp->tmp_pic = xcb_generate_id(r->c);
        xcb_render_create_picture(r->c, pp->tmp_pic, pp->tmp, fmt, 0, NULL);
        xcb_render_transform_t t = { (int32_t)lround(65536.0 * pp->sw / MAX(pp->dw, 1)), 0, 0,
                                     0, (int32_t)lround(65536.0 * pp->sh / MAX(pp->dh, 1)), 0, 0, 0, 1 << 16 };
        xcb_render_set_picture_transform(r->c, pp->tmp_pic, t);
        const char bilinear[] = "bilinear";
        xcb_render_set_picture_filter(r->c, pp->tmp_pic, sizeof(bilinear) - 1, bilinear, 0, NULL);
    }
    r->nparts = n;
    if (g_getenv("KISHOT_DEBUG"))
        for (int i = 0; i < n; i++)
            fprintf(stderr, "kishot: phys part %d crtc 0x%x src %dx%d+%d+%d -> %dx%d+%d+%d%s\n", i,
                    r->part[i].crtc, r->part[i].sw, r->part[i].sh, r->part[i].sx, r->part[i].sy,
                    r->part[i].dw, r->part[i].dh, r->part[i].dx, r->part[i].dy, pl.direct[i] ? "" : " (scaled)");

    /* the screen-read permission (Xnotify) is asked here, not mid-recording */
    xcb_pixmap_t probe = xcb_generate_id(r->c);
    xcb_create_pixmap(r->c, scr->root_depth, probe, r->root, 1, 1);
    xcb_generic_error_t *e = xcb_request_check(r->c,
        xscale_copy(r, r->part[0].crtc, probe, 0, 0, 1, 1, 0, 0, XCB_REQUEST_CHECKED));
    xcb_free_pixmap(r->c, probe);
    if (e) {
        free(e);
        free_phys(r);
        return _("reading the screen at its physical size was denied");
    }
    r->phys = TRUE;
    return NULL;
}

/* The SHM path reads the frame back from a pixmap composed on the server. */
static gboolean setup_phys_target(Rec *r, const xcb_screen_t *scr)
{
    r->phys_pix = xcb_generate_id(r->c);
    xcb_generic_error_t *e = xcb_request_check(r->c,
        xcb_create_pixmap_checked(r->c, scr->root_depth, r->phys_pix, r->root, r->fw, r->fh));
    if (e) {
        free(e);
        r->phys_pix = 0;
        return FALSE;
    }
    xcb_render_pictformat_t fmt;
    if (render_format(r->c, scr->root_depth, FALSE, &fmt)) {
        r->phys_pic = xcb_generate_id(r->c);
        xcb_render_create_picture(r->c, r->phys_pic, r->phys_pix, fmt, 0, NULL);
    }
    if (!r->gc_black) {
        uint32_t black[] = { 0 };
        r->gc_black = xcb_generate_id(r->c);
        xcb_create_gc(r->c, r->gc_black, r->phys_pix, XCB_GC_FOREGROUND, black);
    }
    return TRUE;
}

/* One frame at the physical size into `dst`, then the hidden windows and
 * the pointer, all scaled to it. */
static void compose_phys(Rec *r, xcb_pixmap_t dst, xcb_render_picture_t pic, gboolean ptr_ok, int px, int py)
{
    if (r->phys_gaps) {
        xcb_rectangle_t all = { 0, 0, r->fw, r->fh };
        xcb_poly_fill_rectangle(r->c, dst, r->gc_black, 1, &all);
    }
    for (int i = 0; i < r->nparts; i++) {
        PhysPart *pp = &r->part[i];
        if (!pp->tmp) {
            xscale_copy(r, pp->crtc, dst, pp->sx, pp->sy, MIN(pp->sw, pp->dw), MIN(pp->sh, pp->dh),
                        pp->dx, pp->dy, 0);
        } else if (pic) {
            if (pp->from_root)
                xcb_copy_area(r->c, r->root, pp->tmp, r->gc_root, pp->sx, pp->sy, 0, 0, pp->sw, pp->sh);
            else
                xscale_copy(r, pp->crtc, pp->tmp, pp->sx, pp->sy, pp->sw, pp->sh, 0, 0, 0);
            xcb_render_composite(r->c, XCB_RENDER_PICT_OP_SRC, pp->tmp_pic, XCB_NONE, pic, 0, 0, 0, 0,
                                 pp->dx, pp->dy, pp->dw, pp->dh);
        }
    }
    int n;
    KsRect *rs = ks_censor_rects(&n);
    if (n) {
        xcb_rectangle_t *xr = g_new(xcb_rectangle_t, n);
        for (int i = 0; i < n; i++) {
            int x1 = (int)floor((rs[i].x - r->area.x) * r->scale), y1 = (int)floor((rs[i].y - r->area.y) * r->scale);
            int x2 = (int)ceil((rs[i].x + rs[i].w - r->area.x) * r->scale);
            int y2 = (int)ceil((rs[i].y + rs[i].h - r->area.y) * r->scale);
            xr[i] = (xcb_rectangle_t){ x1, y1, x2 - x1, y2 - y1 };
        }
        xcb_poly_fill_rectangle(r->c, dst, r->gc_black, n, xr);
        g_free(xr);
    }
    g_free(rs);
    if (ptr_ok && r->cur_pic && pic) {
        xcb_render_composite(r->c, XCB_RENDER_PICT_OP_OVER, r->cur_pic, XCB_NONE, pic, 0, 0, 0, 0,
                             (int)lround((px - r->area.x - r->cur_xhot) * r->scale),
                             (int)lround((py - r->area.y - r->cur_yhot) * r->scale),
                             (int)lround(r->cur_w * r->scale), (int)lround(r->cur_h * r->scale));
    }
}

/* ---- ffmpeg command sink ------------------------------------------------- */

typedef struct { const char *log; int audio_fd; } ChildSetup;

/* runs in the child between fork and exec */
static void child_setup(gpointer data)
{
    ChildSetup *cs = data;
    int fd = open(cs->log, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) {
        dup2(fd, 2);
        close(fd);
    }
    /* dup2 clears close-on-exec, so ffmpeg inherits it as pipe:3 */
    if (cs->audio_fd >= 0)
        dup2(cs->audio_fd, 3);
}

static gboolean is_vaapi(const char *enc) { return g_str_has_suffix(enc, "_vaapi"); }

static const char *container_format(const char *container)
{
    if (!g_ascii_strcasecmp(container, "mkv"))
        return "matroska";
    return container;
}

static gboolean cli_open(Rec *r, const KsConfig *cfg, const char *enc, char **error)
{
    GPtrArray *a = g_ptr_array_new_with_free_func(g_free);
#define ARG(...) g_ptr_array_add(a, g_strdup_printf(__VA_ARGS__))
    ARG("ffmpeg"); ARG("-hide_banner"); ARG("-loglevel"); ARG("error"); ARG("-y");
    if (is_vaapi(enc)) {
        ARG("-vaapi_device"); ARG("%s", x_node ? x_node : "/dev/dri/renderD128");
    }
    ARG("-thread_queue_size"); ARG("64");
    ARG("-f"); ARG("rawvideo"); ARG("-pix_fmt"); ARG("bgr0");
    ARG("-video_size"); ARG("%dx%d", r->fw, r->fh);
    ARG("-framerate"); ARG("%d", r->fps); ARG("-i"); ARG("pipe:0");
    /* sound comes from kishot itself (libpulse-simple, system and mic
     * already mixed) through a second pipe: both inputs end when we close
     * them, and both are timed by us */
    int apipe[2] = { -1, -1 };
    gboolean audio = (cfg->audio_system || cfg->audio_mic) && caps.audio && pipe(apipe) == 0;
    if (audio) {
        ARG("-thread_queue_size"); ARG("1024");
        ARG("-f"); ARG("s16le"); ARG("-ar"); ARG("%d", KS_AUDIO_RATE); ARG("-ac"); ARG("%d", KS_AUDIO_CHANNELS);
        ARG("-i"); ARG("pipe:3");
    }
    int q = cfg->quality;
    if (is_vaapi(enc)) {
        ARG("-vf"); ARG("format=nv12,hwupload");
        ARG("-c:v"); ARG("%s", enc); ARG("-qp"); ARG("%d", q);
    } else {
        ARG("-c:v"); ARG("%s", enc);
        if (!strcmp(enc, "libx264") || !strcmp(enc, "libx265")) {
            ARG("-preset"); ARG("veryfast"); ARG("-crf"); ARG("%d", q);
        } else if (!strcmp(enc, "libvpx-vp9") || !strcmp(enc, "libvpx")) {
            ARG("-deadline"); ARG("realtime"); ARG("-cpu-used"); ARG("8");
            ARG("-crf"); ARG("%d", MAX(q, 4)); ARG("-b:v"); ARG("0");
        } else if (!strcmp(enc, "libsvtav1")) {
            ARG("-preset"); ARG("10"); ARG("-crf"); ARG("%d", q);
        } else if (!strcmp(enc, "mpeg4")) {
            ARG("-q:v"); ARG("%d", 1 + q * 30 / 51);
        }
        if (strcmp(enc, "ffv1")) {
            ARG("-pix_fmt"); ARG("yuv420p");
        }
    }
    if (audio) {
        gboolean webm = !g_ascii_strcasecmp(cfg->container, "webm");
        ARG("-c:a"); ARG(webm ? "libopus" : "aac"); ARG("-b:a"); ARG("160k");
    }
    ARG("-f"); ARG("%s", container_format(cfg->container));
    ARG("%s", r->path);
#undef ARG
    g_ptr_array_add(a, NULL);

    char *tmpl = g_build_filename(g_get_tmp_dir(), "kishot-ffmpeg-XXXXXX", NULL);
    int fd = g_mkstemp(tmpl);
    if (fd >= 0)
        close(fd);
    r->cli_log = tmpl;
    GError *err = NULL;
    ChildSetup cs = { r->cli_log, apipe[0] };
    gboolean ok = g_spawn_async_with_pipes(NULL, (char **)a->pdata, NULL,
                                           G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD |
                                               G_SPAWN_STDOUT_TO_DEV_NULL,
                                           child_setup, &cs, &r->cli_pid, &r->cli_in,
                                           NULL, NULL, &err);
    g_ptr_array_free(a, TRUE);
    if (apipe[0] >= 0)
        close(apipe[0]);
    if (ok && audio) {
        r->cli_audio = apipe[1];
        fcntl(r->cli_audio, F_SETFD, FD_CLOEXEC);
    } else if (apipe[1] >= 0) {
        close(apipe[1]);
    }
    if (!ok) {
        *error = g_strdup(err->message);
        g_error_free(err);
        return FALSE;
    }
    return TRUE;
}

static gboolean write_all(int fd, const uint8_t *p, size_t n)
{
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return FALSE;
        }
        p += w;
        n -= w;
    }
    return TRUE;
}

static char *read_log_tail(const char *path)
{
    char *txt = NULL;
    if (!g_file_get_contents(path, &txt, NULL, NULL) || !txt)
        return NULL;
    g_strstrip(txt);
    if (!*txt) {
        g_free(txt);
        return NULL;
    }
    size_t n = strlen(txt);
    char *tail = g_strdup(n > 400 ? txt + n - 400 : txt);
    g_free(txt);
    return tail;
}

static void cli_close(Rec *r)
{
    if (r->cli_audio >= 0)
        close(r->cli_audio);
    r->cli_audio = -1;
    if (r->cli_in >= 0)
        close(r->cli_in);
    r->cli_in = -1;
    r->cli_audio = -1;
    if (r->cli_pid) {
        int status = 0;
        gboolean interrupted = FALSE;
        /* stdin closed: ffmpeg drains its queues and finishes the file on
         * its own (a few seconds with pulse inputs). SIGINT is its clean
         * stop but drops what is still queued, so only after a while;
         * SIGKILL is the last resort. */
        for (int i = 0; i < 500; i++) {
            if (waitpid(r->cli_pid, &status, WNOHANG) == r->cli_pid)
                break;
            if (i == 300) {
                kill(r->cli_pid, SIGINT);
                interrupted = TRUE;
            }
            if (i == 499) {
                kill(r->cli_pid, SIGKILL);
                waitpid(r->cli_pid, &status, 0);
            }
            g_usleep(20000);
        }
        if (interrupted && WIFEXITED(status) && WEXITSTATUS(status) == 255)
            status = 0;     /* ffmpeg's exit code after a SIGINT */
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            /* ffmpeg's own message beats "stopped accepting frames" */
            char *tail = r->cli_log ? read_log_tail(r->cli_log) : NULL;
            g_free(r->error);
            r->error = NULL;
            set_error(r, _("ffmpeg failed: %s"), tail ? tail : _("unknown error"));
            g_free(tail);
        }
        g_spawn_close_pid(r->cli_pid);
        r->cli_pid = 0;
    }
    if (r->cli_log) {
        unlink(r->cli_log);
        g_free(r->cli_log);
        r->cli_log = NULL;
    }
}

/* ---- audio into the plugin or the ffmpeg pipe ---------------------------- */

static void audio_cb(const int16_t *pcm, int frames, void *data)
{
    Rec *r = data;
    gint64 pts = g_get_monotonic_time() - r->start_us - (gint64)frames * 1000000 / KS_AUDIO_RATE;
    if (r->ph) {
        plugin->audio(r->ph, pcm, frames, MAX(pts, 0));
        return;
    }
    if (r->cli_audio < 0)
        return;
    if (!r->audio_primed) {
        /* raw PCM has no timestamps: line its start up with the video's */
        r->audio_primed = TRUE;
        size_t n = (size_t)(MAX(pts, 0) * KS_AUDIO_RATE / 1000000) * KS_AUDIO_CHANNELS * 2;
        uint8_t *zero = g_malloc0(n ? n : 1);
        write_all(r->cli_audio, zero, n);
        g_free(zero);
    }
    if (!write_all(r->cli_audio, (const uint8_t *)pcm, (size_t)frames * KS_AUDIO_CHANNELS * 2)) {
        close(r->cli_audio);
        r->cli_audio = -1;
    }
}

/* ---- capture thread ------------------------------------------------------ */

static void paint_censor_gpu(Rec *r, int slot)
{
    int n;
    KsRect *rs = ks_censor_rects(&n);
    if (!n)
        return;
    xcb_rectangle_t *xr = g_new(xcb_rectangle_t, n);
    for (int i = 0; i < n; i++)
        xr[i] = (xcb_rectangle_t){ rs[i].x - r->area.x, rs[i].y - r->area.y, rs[i].w, rs[i].h };
    xcb_poly_fill_rectangle(r->c, r->pixmap[slot], r->gc_black, n, xr);
    g_free(xr);
    g_free(rs);
}

static void paint_censor_cpu(Rec *r, uint8_t *buf)
{
    int n;
    KsRect *rs = ks_censor_rects(&n);
    for (int i = 0; i < n; i++) {
        int x1 = MAX(rs[i].x - r->area.x, 0), y1 = MAX(rs[i].y - r->area.y, 0);
        int x2 = MIN(rs[i].x + rs[i].w - r->area.x, r->fw);
        int y2 = MIN(rs[i].y + rs[i].h - r->area.y, r->fh);
        if (x2 <= x1)
            continue;
        for (int y = y1; y < y2; y++)
            memset(buf + (size_t)y * r->stride + x1 * 4, 0, (size_t)(x2 - x1) * 4);
    }
    g_free(rs);
}

static gboolean finished_idle(gpointer data)
{
    Rec *r = data;
    g_thread_join(r->thread);
    g_mutex_lock(&rec_lock);
    rec = NULL;
    g_mutex_unlock(&rec_lock);
    ks_capture_release();
    r->done(r->error ? NULL : r->path, r->error, r->done_data);
    g_free(r->path);
    g_free(r->method);
    g_free(r->error);
    g_free(r->cur_img);
    g_free(r);
    return FALSE;
}

static gpointer capture_thread(gpointer data)
{
    Rec *r = data;
    const gint64 frame_us = 1000000 / r->fps;
    gint64 next = r->start_us;
    gint64 last_index = -1;
    int ring_next = 0, pend_slot = -1;
    gint64 pend_index = 0;

    while (!g_atomic_int_get(&r->stop) && !r->error) {
        gint64 now = g_get_monotonic_time();
        if (now < next) {
            g_usleep(next - now);
            continue;
        }
        gint64 index = (now - r->start_us) / frame_us;
        next = r->start_us + (index + 1) * frame_us;

        int px = 0, py = 0;
        gboolean ptr_ok = FALSE;
        poll_x_events(r);
        if (r->pointer) {
            if (r->cursor_dirty)
                fetch_cursor(r);
            xcb_query_pointer_reply_t *qp = r->cursor_hidden ? NULL :
                xcb_query_pointer_reply(r->c, xcb_query_pointer(r->c, r->root), NULL);
            if (qp) {
                px = qp->root_x;
                py = qp->root_y;
                ptr_ok = qp->same_screen;
                free(qp);
            }
        }

        if (r->use_gpu && r->phys) {
            int slot = ring_next++ % KS_RING;
            compose_phys(r, r->pixmap[slot], r->pic[slot], ptr_ok, px, py);
            free(xcb_get_input_focus_reply(r->c, xcb_get_input_focus(r->c), NULL));
            if (pend_slot >= 0 && plugin->video_dmabuf(r->ph, pend_slot, pend_index * 1000000 / r->fps) < 0)
                set_error(r, _("the video encoder failed"));
            pend_slot = slot;
            pend_index = index;
        } else if (r->use_gpu) {
            int slot = ring_next++ % KS_RING;
            KsCapView cv;
            ks_capture_view(r->c, &r->area, &cv);
            /* Not when every output under the area comes from the
             * compositor's picture anyway: that copy would be overwritten
             * whole. */
            if (!(cv.ok && cv.n_slot == cv.n))
                xcb_copy_area(r->c, r->root, r->pixmap[slot], r->gc_copy, r->area.x, r->area.y, 0, 0,
                              r->area.w, r->area.h);
            /* Where something hidden shows, the compositor's censored
             * picture goes over the copy; where nothing does, its word that
             * the screen is clean, still true after the copy (read over this
             * same connection, so the server handled the copy first), is
             * what lets the rectangles go. */
            KsCapSlot cs[KS_CAP_MAX_OUT];
            int ncs = 0;
            gboolean slots_ok = TRUE;
            if (cv.ok && cv.n_slot && (slots_ok = ks_capture_fetch(&cv, cs, &ncs))) {
                ks_capture_copy_xcb(r->c, cs, ncs, &r->area, r->pixmap[slot], r->gc_copy);
                /* the slots are ours to give back only once the server has
                 * started the copy */
                free(xcb_get_input_focus_reply(r->c, xcb_get_input_focus(r->c), NULL));
                ks_capture_return(cs, ncs);
            }
            if (!(slots_ok && ks_capture_unchanged(r->c, &cv)))
                paint_censor_gpu(r, slot);
            if (ptr_ok && r->cur_pic && r->pic[slot]) {
                xcb_render_composite(r->c, XCB_RENDER_PICT_OP_OVER, r->cur_pic, XCB_NONE, r->pic[slot], 0, 0, 0, 0,
                                     px - r->cur_xhot - r->area.x, py - r->cur_yhot - r->area.y,
                                     r->cur_w, r->cur_h);
            }
            /* a round trip: the server has at least started the copy */
            free(xcb_get_input_focus_reply(r->c, xcb_get_input_focus(r->c), NULL));
            /* Encode the frame copied one capture ago, not this one: the
             * encoder reads a buffer the GPU finished long since, and the
             * next copy lands in a third buffer nobody is reading. The
             * same one-frame lag is what makes OBS's reads safe. */
            if (pend_slot >= 0 && plugin->video_dmabuf(r->ph, pend_slot, pend_index * 1000000 / r->fps) < 0)
                set_error(r, _("the video encoder failed"));
            pend_slot = slot;
            pend_index = index;
        } else {
            xcb_generic_error_t *e = NULL;
            KsCapView cv = { 0 };
            if (r->phys)
                compose_phys(r, r->phys_pix, r->phys_pic, ptr_ok, px, py);
            else
                ks_capture_view(r->c, &r->area, &cv);
            xcb_shm_get_image_reply_t *img = xcb_shm_get_image_reply(r->c,
                r->phys ? xcb_shm_get_image(r->c, r->phys_pix, 0, 0, r->fw, r->fh, ~0u,
                                            XCB_IMAGE_FORMAT_Z_PIXMAP, r->seg, 0)
                        : xcb_shm_get_image(r->c, r->root, r->area.x, r->area.y, r->area.w, r->area.h, ~0u,
                                            XCB_IMAGE_FORMAT_Z_PIXMAP, r->seg, 0), &e);
            if (!img) {
                free(e);
                set_error(r, _("could not read the screen"));
                break;
            }
            free(img);
            KsCapSlot cs[KS_CAP_MAX_OUT];
            int ncs = 0;
            gboolean slots_ok = TRUE;
            if (cv.ok && cv.n_slot) {
                slots_ok = ks_capture_fetch(&cv, cs, &ncs) &&
                           ks_capture_copy_buffer(r->c, cs, ncs, &r->area, r->shm, r->stride);
                ks_capture_return(cs, ncs);      /* the read above was synchronous */
            }
            if (!r->phys && !(slots_ok && ks_capture_unchanged(r->c, &cv)))
                paint_censor_cpu(r, r->shm);
            if (!r->phys && ptr_ok && r->cur_img)
                blend_cursor_cpu(r, r->shm, r->stride, px, py);
            if (r->ph) {
                if (plugin->video_cpu(r->ph, r->shm, r->stride, index * 1000000 / r->fps) < 0)
                    set_error(r, _("the video encoder failed"));
            } else {
                /* raw video has no timestamps: repeat the frame to fill
                 * the slots we were too late for */
                gint64 copies = last_index < 0 ? 1 : MIN(index - last_index, (gint64)r->fps);
                for (gint64 k = 0; k < copies && !r->error; k++)
                    if (!write_all(r->cli_in, r->shm, (size_t)r->stride * r->fh))
                        set_error(r, _("ffmpeg stopped accepting frames"));
            }
        }
        last_index = index;
    }

    if (ks_audio_available())
        ks_audio_stop();
    /* the last frame is still waiting for its turn */
    if (r->ph && pend_slot >= 0 && !r->error &&
        plugin->video_dmabuf(r->ph, pend_slot, pend_index * 1000000 / r->fps) < 0)
        set_error(r, _("the video encoder failed"));
    if (r->ph) {
        char err[256] = "";
        if (plugin->close(r->ph, err, sizeof(err)) != 0)
            set_error(r, "%s", err);
        r->ph = NULL;
    } else {
        cli_close(r);
    }
    free_phys(r);
    free_gpu(r);
    if (r->cur_pic)
        xcb_render_free_picture(r->c, r->cur_pic);
    if (r->cur_pix)
        xcb_free_pixmap(r->c, r->cur_pix);
    if (r->shm) {
        xcb_shm_detach(r->c, r->seg);
        shmdt(r->shm);
    }
    xcb_disconnect(r->c);
    g_idle_add(finished_idle, r);
    return NULL;
}

/* ---- start/stop ---------------------------------------------------------- */

static const char *pick_encoder(const KsConfig *cfg)
{
    if (strcmp(cfg->vcodec, "auto")) {
        for (guint i = 0; i < caps.encoders->len; i++)
            if (!strcmp(caps.encoders->pdata[i], cfg->vcodec))
                return caps.encoders->pdata[i];
        return NULL;
    }
    gboolean webm = !g_ascii_strcasecmp(cfg->container, "webm");
    /* The ffmpeg command can't mux VAAPI output into mkv/mp4 when the
     * driver has no packed headers (Mesa radeonsi before 22): no extradata.
     * The plugin works around that; with the command, "auto" skips VAAPI. */
    gboolean cli = !plugin || cfg->backend == KS_BACKEND_CLI;
    for (int pass = 0; pass < 2; pass++)
        for (guint i = 0; i < caps.encoders->len; i++) {
            const char *e = caps.encoders->pdata[i];
            if (webm && !strstr(e, "vp9") && !strstr(e, "vpx") && !strstr(e, "av1"))
                continue;
            if (pass == 0 && cli && is_vaapi(e))
                continue;
            return e;
        }
    return NULL;
}

gboolean ks_rec_start(const KsConfig *cfg, const KsRect *area, const char *path,
                      KsRecFinished done, void *data, char **error)
{
    if (rec) {
        *error = g_strdup(_("already recording"));
        return FALSE;
    }
    ks_rec_probe(cfg);
    if (!caps.usable) {
        *error = g_strdup(caps.why_not);
        return FALSE;
    }
    const char *enc = pick_encoder(cfg);
    if (!enc) {
        *error = g_strdup_printf(_("the encoder \"%s\" is not available for %s files"), cfg->vcodec, cfg->container);
        return FALSE;
    }

    Rec *r = g_new0(Rec, 1);
    r->area = *area;
    r->area.w &= ~1;            /* 4:2:0 encoders want even sizes */
    r->area.h &= ~1;
    r->fw = r->area.w;
    r->fh = r->area.h;
    r->scale = 1;
    r->fps = cfg->fps;
    r->pointer = cfg->rec_pointer;
    r->path = g_strdup(path);
    r->done = done;
    r->done_data = data;
    for (int i = 0; i < KS_RING; i++)
        r->dmabuf[i] = -1;
    r->cli_in = -1;
    r->cli_audio = -1;
    r->shmid = -1;

    int screen_num = 0;
    r->c = xcb_connect(NULL, &screen_num);
    if (xcb_connection_has_error(r->c)) {
        *error = g_strdup(_("could not connect to the X server"));
        goto fail;
    }
    xcb_screen_iterator_t si = xcb_setup_roots_iterator(xcb_get_setup(r->c));
    for (int i = 0; i < screen_num; i++)
        xcb_screen_next(&si);
    const xcb_screen_t *scr = si.data;
    r->root = scr->root;

    const char *phys_why = cfg->rec_physical ? setup_phys(r, scr) : NULL;
    if (!r->phys) {
        r->fw = r->area.w;
        r->fh = r->area.h;
        r->scale = 1;
    }

    gboolean use_plugin = plugin && cfg->backend != KS_BACKEND_CLI;
    const char *copy_note = "SHM";
    if (use_plugin && caps.vaapi && cfg->copy_mode != KS_COPY_SHM) {
        gboolean want = cfg->copy_mode == KS_COPY_GPU || server_has_root_copy(r->c, r->root, &r->area);
        if (want) {
            if (setup_gpu(r, scr))
                r->use_gpu = TRUE;
            else
                free_gpu(r);
        }
        if (!want)
            copy_note = _("SHM (the server does not guarantee root copies)");
        else if (!r->use_gpu)
            copy_note = _("SHM (GPU copy unavailable: no DRI3)");
    }
    if (!r->use_gpu && !setup_shm(r)) {
        *error = g_strdup(_("MIT-SHM is not available"));
        goto fail;
    }
    if (r->phys && !r->use_gpu && !setup_phys_target(r, scr)) {
        *error = g_strdup(_("could not allocate the frame at the physical size"));
        goto fail;
    }

    if (r->pointer) {
        if (xcb_get_extension_data(r->c, &xcb_xfixes_id)->present) {
            xcb_xfixes_query_version_reply_t *v =
                xcb_xfixes_query_version_reply(r->c, xcb_xfixes_query_version(r->c, 4, 0), NULL);
            free(v);
            select_cursor_visibility(r);
            fetch_cursor(r);
        } else {
            r->pointer = FALSE;
        }
    }

    gboolean want_audio = (cfg->audio_system || cfg->audio_mic);
    if (use_plugin) {
        KsEncParams p = {
            .width = r->fw, .height = r->fh, .fps = r->fps, .path = r->path,
            .container = cfg->container, .vcodec = enc, .quality = cfg->quality,
            .audio = want_audio && caps.audio, .sample_rate = KS_AUDIO_RATE, .channels = KS_AUDIO_CHANNELS,
            .input = r->use_gpu ? KS_INPUT_DMABUF : KS_INPUT_CPU,
            .dmabuf_n = r->use_gpu ? KS_RING : 0, .dmabuf_stride = r->stride, .dmabuf_size = r->dsize,
        };
        for (int i = 0; r->use_gpu && i < KS_RING; i++)
            p.dmabuf_fd[i] = r->dmabuf[i];
        char err[256] = "";
        r->ph = plugin->open(&p, err, sizeof(err));
        if (!r->ph && r->use_gpu) {
            /* VAAPI could not import the buffer: retry from SHM */
            g_printerr("kishot: GPU copy failed (%s), using SHM\n", err);
            free_gpu(r);
            r->use_gpu = FALSE;
            copy_note = _("SHM (the dma-buf import failed)");
            if (setup_shm(r) && (!r->phys || setup_phys_target(r, scr))) {
                p.input = KS_INPUT_CPU;
                r->ph = plugin->open(&p, err, sizeof(err));
            }
        }
        if (!r->ph) {
            *error = g_strdup(err);
            goto fail;
        }
        r->method = g_strdup_printf("%s → %s%s", r->use_gpu ? _("GPU copy") : copy_note,
                                    plugin->describe(r->ph),
                                    want_audio && !caps.audio ? _(" (no audio: libpulse missing)") : "");
    } else {
        if (!cli_open(r, cfg, enc, error))
            goto fail;
        r->method = g_strdup_printf(_("%s → ffmpeg %s%s"), "SHM", enc,
                                    want_audio && !caps.audio ? _(" (no audio: libpulse missing)") : "");
    }

    if (r->phys) {
        char *m = g_strdup_printf(_("%s, physical size %dx%d"), r->method, r->fw, r->fh);
        g_free(r->method);
        r->method = m;
    } else if (phys_why) {
        char *m = g_strdup_printf(_("%s (logical size: %s)"), r->method, phys_why);
        g_free(r->method);
        r->method = m;
    }

    r->start_us = g_get_monotonic_time();
    if ((r->ph || r->cli_audio >= 0) && want_audio && caps.audio)
        ks_audio_start(cfg->audio_system, cfg->audio_mic, audio_cb, r);
    g_mutex_lock(&rec_lock);
    rec = r;
    g_mutex_unlock(&rec_lock);
    ks_capture_acquire();
    r->thread = g_thread_new("kishot-capture", capture_thread, r);
    return TRUE;

fail:
    if (r->ph)
        plugin->close(r->ph, NULL, 0);
    cli_close(r);
    if (r->c) {
        free_phys(r);
        free_gpu(r);
        if (r->shm) {
            shmdt(r->shm);
        }
        xcb_disconnect(r->c);
    }
    g_free(r->path);
    g_free(r->cur_img);
    g_free(r->method);
    g_free(r->error);
    g_free(r);
    return FALSE;
}

void ks_rec_stop(void)
{
    if (rec)
        g_atomic_int_set(&rec->stop, 1);
}
