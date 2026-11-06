/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The bitmap plugin runtime: GLib main loop over kiview's socket (fd 3) and
 * our own X connection, drawing the current document with cairo straight
 * into the window kiview gave us.
 *
 * Documents are decoded on worker threads (the current one, and the
 * neighbours kiview asks us to prefetch) and kept in a small cache by id.
 * A decoder may return a reduced image (JPEG at 1/4, say); when the zoom
 * asks for more pixels than it has, the file is decoded again, sharper, and
 * the new image replaces the old one without the view moving.
 *
 * Drawing happens on idle so a burst of pans is one repaint, and always at
 * the density kiview announced: the image is resampled from its own pixels
 * to density times as many window pixels. */
#include <X11/Xlib.h>
#include <cairo-xlib.h>
#include <errno.h>
#include <fcntl.h>
#include <glib-unix.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "kvp.h"
#include "kvp_msg.h"
#include "kvp_view.h"

#define SOCK_FD 3
#define ZOOM_STEP 1.25
#define MAX_FILE_BYTES (512u * 1024 * 1024)
#define CACHE_LIMIT_BYTES (256u * 1024 * 1024)
#define MIN_FRAME_MS 20

typedef struct {
    int ref;
    gint64 id;
    char *name, *mime;
    enum { DOC_LOADING, DOC_READY, DOC_FAILED } state;
    GError *err;
    cairo_surface_t *img;
    int denom;
    int full_w, full_h;
    int flip, rot;
    char *format;
    GBytes *keep;
    KvpAnim *anim;
    gboolean upgrading, closed;
    KvpView view;
} Doc;

typedef struct {
    Doc *doc;
    int fd;         /* the file, until read */
    GBytes *data;   /* or already read (upgrade) */
    KvpTarget target;
    gboolean upgrade;
    KvpDecoded *res;
    GError *err;
} Job;

static struct {
    const KvpBitmapOps *ops;
    int sock;
    GMainLoop *loop;
    GThreadPool *pool;
    Display *dpy;
    gboolean x_watched;

    /* surface */
    Window win;
    int aw, ah;     /* logical */
    double density; /* physical per logical */
    long seq;       /* last surface's, 0: none yet */
    long framed;    /* last seq answered */
    cairo_surface_t *xs;
    guint idle;
    gboolean visible;

    /* style */
    double bg[3];
    gboolean checker;
    cairo_pattern_t *checker_pat;

    /* documents */
    GHashTable *docs; /* id -> Doc* */
    Doc *cur;
    guint anim_timer;
    gboolean anim_paused;
} P = {.sock = SOCK_FD, .density = 1, .bg = {0.12, 0.12, 0.12}, .visible = TRUE};

GQuark kvp_error_quark(void)
{
    return g_quark_from_static_string("kvp-error");
}

static void send_out(GString *m)
{
    if (!kvp_out_send(m, P.sock, -1))
        g_main_loop_quit(P.loop);
}

/* ---- documents ---- */

static void stop_anim(void)
{
    if (P.anim_timer)
        g_source_remove(P.anim_timer);
    P.anim_timer = 0;
}

static void doc_unref(Doc *d)
{
    if (--d->ref)
        return;
    if (d->img)
        cairo_surface_destroy(d->img);
    if (d->anim)
        d->anim->destroy(d->anim);
    if (d->keep)
        g_bytes_unref(d->keep);
    g_clear_error(&d->err);
    g_free(d->name);
    g_free(d->mime);
    g_free(d->format);
    g_free(d);
}

static gsize doc_bytes(const Doc *d)
{
    return d->img ? (gsize)cairo_image_surface_get_stride(d->img) * cairo_image_surface_get_height(d->img) : 0;
}

static void doc_close(Doc *d)
{
    if (d == P.cur) {
        stop_anim();
        P.cur = NULL;
    }
    d->closed = TRUE;
    g_hash_table_remove(P.docs, &d->id); /* drops the table's reference */
}

/* Keeps the cache under its limit by dropping the oldest documents that
 * aren't the one shown. */
static void enforce_cap(void)
{
    for (;;) {
        gsize total = 0;
        Doc *oldest = NULL;
        GHashTableIter it;
        gpointer k, v;
        g_hash_table_iter_init(&it, P.docs);
        while (g_hash_table_iter_next(&it, &k, &v)) {
            Doc *d = v;
            total += doc_bytes(d);
            if (d != P.cur && d->state != DOC_LOADING && (!oldest || d->id < oldest->id))
                oldest = d;
        }
        if (total <= CACHE_LIMIT_BYTES || !oldest)
            return;
        doc_close(oldest);
    }
}

static void send_view(void)
{
    Doc *d = P.cur;
    if (!d || d->state != DOC_READY)
        return;
    GString *m = kvp_out("view");
    kvp_out_i(m, "id", d->id);
    kvp_out_d(m, "zoom", d->view.zoom);
    kvp_out_i(m, "fit", d->view.mode == KVP_FIT || d->view.mode == KVP_FIT_WIDTH);
    kvp_out_i(m, "scroll_x", kvp_view_scrolls_x(&d->view));
    kvp_out_i(m, "scroll_y", kvp_view_scrolls_y(&d->view));
    kvp_out_i(m, "page", 1);
    kvp_out_i(m, "pages", 1);
    send_out(m);
}

/* ---- drawing ---- */

/* 16x16 logical pixels of two greys, repeating. */
static cairo_pattern_t *checker_pattern(void)
{
    if (!P.checker_pat) {
        cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, 16, 16);
        cairo_t *cr = cairo_create(s);
        cairo_set_source_rgb(cr, 0.8, 0.8, 0.8);
        cairo_paint(cr);
        cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
        cairo_rectangle(cr, 0, 0, 8, 8);
        cairo_rectangle(cr, 8, 8, 8, 8);
        cairo_fill(cr);
        cairo_destroy(cr);
        P.checker_pat = cairo_pattern_create_for_surface(s);
        cairo_pattern_set_extend(P.checker_pat, CAIRO_EXTEND_REPEAT);
        cairo_surface_destroy(s);
    }
    return P.checker_pat;
}

/* Maps the document's pixels to the logical area: flip, rotation, zoom,
 * scroll. */
static void image_matrix(cairo_t *cr, const Doc *d)
{
    const KvpView *v = &d->view;
    double ox, oy;
    kvp_view_origin(v, &ox, &oy);
    cairo_translate(cr, -ox, -oy);
    cairo_scale(cr, v->zoom, v->zoom);
    switch (v->rot) {
    case 1:
        cairo_translate(cr, v->ih, 0);
        cairo_rotate(cr, M_PI / 2);
        break;
    case 2:
        cairo_translate(cr, v->iw, v->ih);
        cairo_rotate(cr, M_PI);
        break;
    case 3:
        cairo_translate(cr, 0, v->iw);
        cairo_rotate(cr, -M_PI / 2);
        break;
    }
    if (v->flip) {
        cairo_translate(cr, v->iw, 0);
        cairo_scale(cr, -1, 1);
    }
}

static void draw_now(void)
{
    if (!P.xs)
        return;
    Doc *d = P.cur;
    cairo_t *cr = cairo_create(P.xs);
    cairo_push_group(cr);
    cairo_set_source_rgb(cr, P.bg[0], P.bg[1], P.bg[2]);
    cairo_paint(cr);
    if (d && d->state == DOC_READY && d->img) {
        double s = P.density;
        cairo_scale(cr, s, s);
        cairo_save(cr);
        image_matrix(cr, d);
        cairo_rectangle(cr, 0, 0, d->view.iw, d->view.ih);
        cairo_restore(cr);
        cairo_clip(cr);
        if (cairo_image_surface_get_format(d->img) == CAIRO_FORMAT_ARGB32 && P.checker) {
            cairo_set_source(cr, checker_pattern());
            cairo_paint(cr);
        }
        image_matrix(cr, d);
        /* the surface may be a reduction of the document */
        cairo_scale(cr, (double)d->view.iw / cairo_image_surface_get_width(d->img),
                    (double)d->view.ih / cairo_image_surface_get_height(d->img));
        cairo_set_source_surface(cr, d->img, 0, 0);
        double dev = d->view.zoom * s; /* device pixels per document pixel */
        cairo_pattern_set_filter(cairo_get_source(cr), dev >= 4 ? CAIRO_FILTER_NEAREST
                                                     : dev < 1  ? CAIRO_FILTER_GOOD
                                                                : CAIRO_FILTER_BILINEAR);
        cairo_paint(cr);
    }
    cairo_pop_group_to_source(cr);
    cairo_identity_matrix(cr);
    cairo_paint(cr);
    cairo_destroy(cr);
    cairo_surface_flush(P.xs);
    XFlush(P.dpy);

    if (P.seq && P.framed != P.seq) {
        P.framed = P.seq;
        GString *m = kvp_out("frame");
        kvp_out_i(m, "seq", P.seq);
        send_out(m);
    }
}

static gboolean idle_draw(gpointer data)
{
    (void)data;
    P.idle = 0;
    draw_now();
    return G_SOURCE_REMOVE;
}

static void redraw(void)
{
    if (!P.idle)
        P.idle = g_idle_add(idle_draw, NULL);
}

/* ---- decoding ---- */

static GBytes *read_file(int fd, GError **err)
{
    GByteArray *a = g_byte_array_new();
    guchar buf[65536];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n < 0) {
            g_set_error(err, KVP_ERROR, KVP_ERR_IO, "%s", g_strerror(errno));
            g_byte_array_free(a, TRUE);
            return NULL;
        }
        if (n == 0)
            break;
        if (a->len + (gsize)n > MAX_FILE_BYTES) {
            g_set_error(err, KVP_ERROR, KVP_ERR_TOOLARGE, "file too large");
            g_byte_array_free(a, TRUE);
            return NULL;
        }
        g_byte_array_append(a, buf, (guint)n);
    }
    return g_byte_array_free_to_bytes(a);
}

static gboolean job_done(gpointer data);

static void worker(gpointer data, gpointer user)
{
    (void)user;
    Job *j = data;
    GBytes *bytes = j->data;
    if (!bytes && j->fd >= 0) {
        bytes = read_file(j->fd, &j->err);
        close(j->fd);
        j->fd = -1;
    }
    if (bytes) {
        /* load() borrows the bytes, taking its own reference to keep them */
        j->res = P.ops->load(bytes, j->doc->name, j->doc->mime, &j->target, &j->err);
        g_bytes_unref(bytes);
        j->data = NULL;
    }
    g_idle_add(job_done, j);
}

static void start_job(Doc *d, int fd, GBytes *data, double min_zoom, gboolean upgrade)
{
    Job *j = g_new0(Job, 1);
    j->doc = d;
    d->ref++;
    j->fd = fd;
    j->data = data;
    j->upgrade = upgrade;
    j->target.min_zoom = min_zoom;
    j->target.area_w = (int)ceil(P.aw * P.density);
    j->target.area_h = (int)ceil(P.ah * P.density);
    if (upgrade)
        d->upgrading = TRUE;
    g_thread_pool_push(P.pool, j, NULL);
}

/* The zoom asks for more pixels than the image has: decode it again. */
static void maybe_upgrade(Doc *d)
{
    if (!d || d->state != DOC_READY || d->upgrading || !d->keep || d->denom <= 1)
        return;
    double dev = d->view.zoom * P.density;
    if (kvp_pick_denom(dev, 0, 0, d->full_w, d->full_h, 0) < d->denom)
        start_job(d, -1, g_bytes_ref(d->keep), dev, TRUE);
}

static void show_failed(Doc *d)
{
    GString *o = kvp_out("failed");
    kvp_out_i(o, "id", d->id);
    const char *reason = "io";
    if (d->err) {
        switch (d->err->code) {
        case KVP_ERR_UNSUPPORTED: reason = "unsupported"; break;
        case KVP_ERR_CORRUPT: reason = "corrupt"; break;
        case KVP_ERR_TOOLARGE: reason = "toolarge"; break;
        default: break;
        }
    }
    kvp_out_s(o, "reason", reason);
    kvp_out_s(o, "msg", d->err ? d->err->message : "");
    send_out(o);
}

static gboolean anim_tick(gpointer data);

static void schedule_anim(void)
{
    stop_anim();
    Doc *d = P.cur;
    if (d && d->anim && !P.anim_paused && P.visible)
        P.anim_timer = g_timeout_add((guint)MAX(d->anim->delay_ms, MIN_FRAME_MS), anim_tick, NULL);
}

static gboolean anim_tick(gpointer data)
{
    (void)data;
    P.anim_timer = 0;
    Doc *d = P.cur;
    if (!d || !d->anim)
        return G_SOURCE_REMOVE;
    gboolean looped = FALSE;
    if (!d->anim->next(d->anim, d->img, &looped)) {
        d->anim->destroy(d->anim);
        d->anim = NULL;
        return G_SOURCE_REMOVE;
    }
    if (looped) {
        GString *m = kvp_out("anim_loop");
        kvp_out_i(m, "id", d->id);
        send_out(m);
    }
    redraw();
    schedule_anim();
    return G_SOURCE_REMOVE;
}

/* Tells kiview the shown document is ready (or failed) and draws it. */
static void announce(Doc *d)
{
    if (d->state == DOC_FAILED) {
        show_failed(d);
        redraw();
        return;
    }
    kvp_view_set_area(&d->view, P.aw, P.ah);
    GString *o = kvp_out("opened");
    kvp_out_i(o, "id", d->id);
    kvp_out_i(o, "w", d->full_w);
    kvp_out_i(o, "h", d->full_h);
    kvp_out_i(o, "pages", 1);
    kvp_out_i(o, "anim", d->anim != NULL);
    kvp_out_s(o, "format", d->format);
    send_out(o);
    send_view();
    maybe_upgrade(d);
    schedule_anim();
    redraw();
}

static gboolean job_done(gpointer data)
{
    Job *j = data;
    Doc *d = j->doc;
    d->upgrading = FALSE;
    if (!d->closed) {
        if (j->res) {
            KvpDecoded *r = j->res;
            if (d->img)
                cairo_surface_destroy(d->img);
            d->img = r->img;
            d->denom = r->denom;
            if (j->upgrade) {
                if (r->keep)
                    g_bytes_unref(r->keep); /* the document already has it */
            } else {
                d->full_w = r->full_w;
                d->full_h = r->full_h;
                d->flip = r->flip;
                d->rot = r->rot;
                d->format = r->format;
                d->keep = r->keep;
                d->anim = r->anim;
                d->state = DOC_READY;
                d->view.aw = P.aw;
                d->view.ah = P.ah;
                kvp_view_init(&d->view, r->full_w, r->full_h);
                kvp_view_set_orientation(&d->view, r->flip, r->rot);
            }
            if (j->upgrade)
                g_free(r->format);
            g_free(r);
            j->res = NULL;
            if (d == P.cur) {
                if (j->upgrade) {
                    redraw();
                    maybe_upgrade(d);
                } else {
                    announce(d);
                }
            }
        } else if (!j->upgrade) {
            d->state = DOC_FAILED;
            d->err = j->err;
            j->err = NULL;
            if (d == P.cur)
                announce(d);
        }
        enforce_cap();
    }
    if (j->res) /* the document went away meanwhile */
        kvp_decoded_free(j->res);
    g_clear_error(&j->err);
    doc_unref(d);
    g_free(j);
    return G_SOURCE_REMOVE;
}

void kvp_decoded_free(KvpDecoded *r)
{
    if (!r)
        return;
    if (r->img)
        cairo_surface_destroy(r->img);
    if (r->keep)
        g_bytes_unref(r->keep);
    if (r->anim)
        r->anim->destroy(r->anim);
    g_free(r->format);
    g_free(r);
}

static Doc *doc_new(const KvpMsg *m)
{
    Doc *d = g_new0(Doc, 1);
    d->ref = 1; /* the table's */
    d->id = kvp_msg_get_int(m, "id", 0);
    d->name = g_strdup(kvp_msg_get(m, "name") ? kvp_msg_get(m, "name") : "");
    d->mime = g_strdup(kvp_msg_get(m, "mime") ? kvp_msg_get(m, "mime") : "");
    d->state = DOC_LOADING;
    d->denom = 1;
    g_hash_table_replace(P.docs, &d->id, d);
    return d;
}

static void make_current(Doc *d)
{
    stop_anim();
    P.cur = d;
    if (d && d->state != DOC_LOADING)
        announce(d);
    else
        redraw(); /* blank until it arrives */
}

/* ---- X and surface ---- */

static gboolean on_x(gint fd, GIOCondition cond, gpointer data);

static void on_surface(const KvpMsg *m)
{
    Window win = (Window)kvp_msg_get_int(m, "xid", 0);
    int w = (int)kvp_msg_get_int(m, "w", 0), h = (int)kvp_msg_get_int(m, "h", 0);
    const char *dstr = kvp_msg_get(m, "density");
    double dens = 1;
    if (dstr && strchr(dstr, '/')) {
        double num = g_ascii_strtod(dstr, NULL), den = g_ascii_strtod(strchr(dstr, '/') + 1, NULL);
        if (num > 0 && den > 0)
            dens = num / den;
    }
    if (!win || w < 1 || h < 1)
        return;
    if (!P.dpy && !(P.dpy = XOpenDisplay(NULL))) {
        g_printerr("kiview plugin: can't open the display\n");
        g_main_loop_quit(P.loop);
        return;
    } else if (P.dpy && !P.x_watched) {
        P.x_watched = TRUE;
        g_unix_fd_add(ConnectionNumber(P.dpy), G_IO_IN, on_x, NULL);
    }
    if (P.xs) {
        cairo_surface_destroy(P.xs);
        P.xs = NULL;
    }
    XWindowAttributes wa;
    if (!XGetWindowAttributes(P.dpy, win, &wa))
        return;
    if (win != P.win)
        XSelectInput(P.dpy, win, ExposureMask | VisibilityChangeMask);
    P.win = win;
    P.aw = w;
    P.ah = h;
    P.density = dens;
    P.seq = kvp_msg_get_int(m, "seq", 0);
    P.xs = cairo_xlib_surface_create(P.dpy, win, wa.visual, wa.width, wa.height);
    if (P.cur && P.cur->state == DOC_READY) {
        kvp_view_set_area(&P.cur->view, w, h);
        send_view();
        maybe_upgrade(P.cur);
    }
    draw_now(); /* contents first; draw_now answers the seq */
}

static void on_style(const KvpMsg *m)
{
    const char *bg = kvp_msg_get(m, "bg");
    unsigned r, g, b;
    if (bg && sscanf(bg, "#%2x%2x%2x", &r, &g, &b) == 3) {
        P.bg[0] = r / 255.0;
        P.bg[1] = g / 255.0;
        P.bg[2] = b / 255.0;
    }
    if (kvp_msg_get(m, "checker"))
        P.checker = kvp_msg_get_int(m, "checker", 0) != 0;
    redraw();
}

/* ---- commands ---- */

static void changed_view(void)
{
    send_view();
    maybe_upgrade(P.cur);
    redraw();
}

static void on_zoom(const KvpMsg *m)
{
    Doc *d = P.cur;
    if (!d || d->state != DOC_READY)
        return;
    const char *mode = kvp_msg_get(m, "mode");
    double ax = kvp_msg_get_double(m, "x", P.aw / 2.0), ay = kvp_msg_get_double(m, "y", P.ah / 2.0);
    if (!mode)
        return;
    if (!strcmp(mode, "fit"))
        kvp_view_set_mode(&d->view, KVP_FIT);
    else if (!strcmp(mode, "fit-width"))
        kvp_view_set_mode(&d->view, KVP_FIT_WIDTH);
    else if (!strcmp(mode, "actual"))
        kvp_view_set_mode(&d->view, KVP_ACTUAL);
    else if (!strcmp(mode, "set"))
        kvp_view_zoom_set(&d->view, kvp_msg_get_double(m, "value", d->view.zoom), ax, ay);
    else if (!strcmp(mode, "by"))
        kvp_view_zoom_by(&d->view, kvp_msg_get_double(m, "factor", ZOOM_STEP), ax, ay);
    else
        return;
    changed_view();
}

static Doc *find_doc(const KvpMsg *m)
{
    gint64 id = kvp_msg_get_int(m, "id", 0);
    return g_hash_table_lookup(P.docs, &id);
}

static void on_message(const char *line, int fd)
{
    KvpMsg *m = kvp_msg_parse(line);
    if (!m) {
        if (fd >= 0)
            close(fd);
        return;
    }
    const char *v = m->verb;
    gboolean fd_used = FALSE;
    if (!strcmp(v, "surface")) {
        on_surface(m);
    } else if (!strcmp(v, "style")) {
        on_style(m);
    } else if (!strcmp(v, "open") || !strcmp(v, "prefetch")) {
        gboolean show = !strcmp(v, "open");
        Doc *d = find_doc(m);
        if (!d) {
            d = doc_new(m);
            if (fd >= 0) {
                start_job(d, fd, NULL, 0, FALSE);
                fd_used = TRUE;
            } else {
                d->state = DOC_FAILED;
                g_set_error(&d->err, KVP_ERROR, KVP_ERR_IO, "no file");
            }
        }
        if (show)
            make_current(d);
        enforce_cap();
    } else if (!strcmp(v, "show")) {
        Doc *d = find_doc(m);
        if (d)
            make_current(d);
    } else if (!strcmp(v, "close")) {
        Doc *d = find_doc(m);
        if (d)
            doc_close(d);
        redraw();
    } else if (!strcmp(v, "zoom")) {
        on_zoom(m);
    } else if (!strcmp(v, "pan") && P.cur && P.cur->state == DOC_READY) {
        kvp_view_pan(&P.cur->view, kvp_msg_get_double(m, "dx", 0), kvp_msg_get_double(m, "dy", 0));
        changed_view();
    } else if (!strcmp(v, "rotate") && P.cur && P.cur->state == DOC_READY) {
        kvp_view_rotate(&P.cur->view, kvp_msg_get_int(m, "by", 90) < 0 ? -1 : 1);
        changed_view();
    } else if (!strcmp(v, "anim")) {
        const char *a = kvp_msg_get(m, "mode");
        if (!a)
            a = kvp_msg_get(m, "state");
        P.anim_paused = a && !strcmp(a, "pause");
        schedule_anim();
    } else if (!strcmp(v, "quit")) {
        g_main_loop_quit(P.loop);
    }
    if (fd >= 0 && !fd_used)
        close(fd);
    kvp_msg_free(m);
}

static gboolean on_socket(gint fd, GIOCondition cond, gpointer data)
{
    (void)data;
    (void)cond;
    char buf[KVP_MSG_MAX];
    int rfd;
    for (;;) {
        int n = kvp_recv(fd, buf, sizeof buf, &rfd);
        if (n == 0 || n == -2) {
            g_main_loop_quit(P.loop);
            return G_SOURCE_REMOVE;
        }
        if (n < 0)
            return G_SOURCE_CONTINUE;
        on_message(buf, rfd);
    }
}

static gboolean on_x(gint fd, GIOCondition cond, gpointer data)
{
    (void)fd;
    (void)cond;
    (void)data;
    while (XPending(P.dpy)) {
        XEvent e;
        XNextEvent(P.dpy, &e);
        if (e.type == Expose && e.xexpose.count == 0) {
            redraw();
        } else if (e.type == VisibilityNotify) {
            /* animations stop while nothing of the window shows (it is
             * minimised, on another desktop, covered) */
            gboolean vis = e.xvisibility.state != VisibilityFullyObscured;
            if (vis != P.visible) {
                P.visible = vis;
                schedule_anim();
            }
        }
    }
    return G_SOURCE_CONTINUE;
}

/* The window can go away under us while kiview rebuilds the area. */
static int x_error(Display *d, XErrorEvent *e)
{
    (void)d;
    (void)e;
    return 0;
}

static guint id_hash(gconstpointer k)
{
    return g_int64_hash(k);
}

static gboolean id_equal(gconstpointer a, gconstpointer b)
{
    return g_int64_equal(a, b);
}

int kvp_bitmap_main(const KvpBitmapOps *ops)
{
    P.ops = ops;
    P.loop = g_main_loop_new(NULL, FALSE);
    int fl = fcntl(P.sock, F_GETFL);
    if (fl < 0) {
        g_printerr("kiview plugin: not started by kiview (no socket on fd 3)\n");
        return 1;
    }
    fcntl(P.sock, F_SETFL, fl | O_NONBLOCK);
    XSetErrorHandler(x_error);
    P.docs = g_hash_table_new_full(id_hash, id_equal, NULL, (GDestroyNotify)doc_unref);
    P.pool = g_thread_pool_new(worker, NULL, 2, FALSE, NULL);
    g_unix_fd_add(P.sock, G_IO_IN | G_IO_HUP, on_socket, NULL);

    GString *m = kvp_out("hello");
    kvp_out_i(m, "version", KVP_PROTOCOL_VERSION);
    kvp_out_s(m, "caps", ops->caps ? ops->caps : "");
    send_out(m);

    g_main_loop_run(P.loop);
    return 0;
}
