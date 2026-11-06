/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The bitmap plugin runtime: GLib main loop over kiview's socket (fd 3) and
 * our own X connection, drawing the current image with cairo straight into
 * the window kiview gave us. Drawing happens on idle so a burst of pans is
 * one repaint, and always at the density kiview announced: the image is
 * resampled from its own pixels to density times as many window pixels. */
#include <X11/Xlib.h>
#include <fcntl.h>
#include <cairo-xlib.h>
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

static struct {
    const KvpBitmapOps *ops;
    int sock;
    GMainLoop *loop;
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

    /* style */
    double bg[3];
    gboolean checker;
    cairo_pattern_t *checker_pat;

    /* document */
    long id;
    cairo_surface_t *img;
    gboolean has_alpha;
    KvpView view;
} P = {.sock = SOCK_FD, .density = 1, .bg = {0.12, 0.12, 0.12}};

GQuark kvp_error_quark(void)
{
    return g_quark_from_static_string("kvp-error");
}

static void send_out(GString *m)
{
    if (!kvp_out_send(m, P.sock, -1))
        g_main_loop_quit(P.loop);
}

static void send_view(void)
{
    if (!P.img)
        return;
    GString *m = kvp_out("view");
    kvp_out_i(m, "id", P.id);
    kvp_out_d(m, "zoom", P.view.zoom);
    kvp_out_i(m, "fit", P.view.mode == KVP_FIT || P.view.mode == KVP_FIT_WIDTH);
    kvp_out_i(m, "scroll_x", kvp_view_scrolls_x(&P.view));
    kvp_out_i(m, "scroll_y", kvp_view_scrolls_y(&P.view));
    kvp_out_i(m, "page", 1);
    kvp_out_i(m, "pages", 1);
    send_out(m);
}

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

/* Maps document pixels to the logical area for the view's rotation. */
static void image_matrix(cairo_t *cr)
{
    const KvpView *v = &P.view;
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
}

static void draw_now(void)
{
    if (!P.xs)
        return;
    cairo_t *cr = cairo_create(P.xs);
    cairo_push_group(cr);
    cairo_set_source_rgb(cr, P.bg[0], P.bg[1], P.bg[2]);
    cairo_paint(cr);
    if (P.img) {
        double s = P.density;
        cairo_scale(cr, s, s);
        cairo_save(cr);
        image_matrix(cr);
        cairo_rectangle(cr, 0, 0, P.view.iw, P.view.ih);
        cairo_restore(cr);
        cairo_clip(cr);
        if (P.has_alpha && P.checker) {
            cairo_set_source(cr, checker_pattern());
            cairo_paint(cr);
        }
        image_matrix(cr);
        cairo_set_source_surface(cr, P.img, 0, 0);
        double dev = P.view.zoom * s; /* device pixels per document pixel */
        cairo_pattern_t *src = cairo_get_source(cr);
        cairo_pattern_set_filter(src, dev >= 4 ? CAIRO_FILTER_NEAREST
                                     : dev < 1 ? CAIRO_FILTER_GOOD
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

static void release_doc(void)
{
    if (P.img)
        cairo_surface_destroy(P.img);
    P.img = NULL;
}

static gboolean on_x(gint fd, GIOCondition cond, gpointer data);

static void on_surface(const KvpMsg *m)
{
    Window win = (Window)kvp_msg_get_int(m, "xid", 0);
    int w = (int)kvp_msg_get_int(m, "w", 0), h = (int)kvp_msg_get_int(m, "h", 0);
    const char *d = kvp_msg_get(m, "density");
    double dens = 1;
    if (d && strchr(d, '/')) {
        double num = g_ascii_strtod(d, NULL), den = g_ascii_strtod(strchr(d, '/') + 1, NULL);
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
        XSelectInput(P.dpy, win, ExposureMask);
    P.win = win;
    P.aw = w;
    P.ah = h;
    P.density = dens;
    P.seq = kvp_msg_get_int(m, "seq", 0);
    P.xs = cairo_xlib_surface_create(P.dpy, win, wa.visual, wa.width, wa.height);
    kvp_view_set_area(&P.view, w, h);
    if (P.img)
        send_view();
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

static const char *reason_name(const GError *e)
{
    switch (e->code) {
    case KVP_ERR_UNSUPPORTED:
        return "unsupported";
    case KVP_ERR_CORRUPT:
        return "corrupt";
    case KVP_ERR_TOOLARGE:
        return "toolarge";
    default:
        return "io";
    }
}

static void on_open(const KvpMsg *m, int fd)
{
    long id = kvp_msg_get_int(m, "id", 0);
    release_doc();
    P.id = id;
    if (fd < 0) {
        GString *o = kvp_out("failed");
        kvp_out_i(o, "id", id);
        kvp_out_s(o, "reason", "io");
        kvp_out_s(o, "msg", "no file");
        send_out(o);
        redraw();
        return;
    }
    GError *err = NULL;
    char *format = NULL;
    cairo_surface_t *img =
        P.ops->load(fd, kvp_msg_get(m, "name") ? kvp_msg_get(m, "name") : "", kvp_msg_get(m, "mime") ? kvp_msg_get(m, "mime") : "", &format, &err);
    if (!img) {
        GString *o = kvp_out("failed");
        kvp_out_i(o, "id", id);
        kvp_out_s(o, "reason", err ? reason_name(err) : "corrupt");
        kvp_out_s(o, "msg", err ? err->message : "");
        send_out(o);
        g_clear_error(&err);
        g_free(format);
        redraw();
        return;
    }
    P.img = img;
    P.has_alpha = cairo_image_surface_get_format(img) == CAIRO_FORMAT_ARGB32;
    kvp_view_init(&P.view, cairo_image_surface_get_width(img), cairo_image_surface_get_height(img));
    if (P.aw)
        kvp_view_set_area(&P.view, P.aw, P.ah);

    GString *o = kvp_out("opened");
    kvp_out_i(o, "id", id);
    kvp_out_i(o, "w", P.view.iw);
    kvp_out_i(o, "h", P.view.ih);
    kvp_out_i(o, "pages", 1);
    kvp_out_i(o, "anim", 0);
    kvp_out_s(o, "format", format);
    send_out(o);
    g_free(format);
    send_view();
    redraw();
}

static void on_zoom(const KvpMsg *m)
{
    if (!P.img)
        return;
    const char *mode = kvp_msg_get(m, "mode");
    double ax = kvp_msg_get_double(m, "x", P.aw / 2.0), ay = kvp_msg_get_double(m, "y", P.ah / 2.0);
    if (!mode)
        return;
    if (!strcmp(mode, "fit"))
        kvp_view_set_mode(&P.view, KVP_FIT);
    else if (!strcmp(mode, "fit-width"))
        kvp_view_set_mode(&P.view, KVP_FIT_WIDTH);
    else if (!strcmp(mode, "actual"))
        kvp_view_set_mode(&P.view, KVP_ACTUAL);
    else if (!strcmp(mode, "set"))
        kvp_view_zoom_set(&P.view, kvp_msg_get_double(m, "value", P.view.zoom), ax, ay);
    else if (!strcmp(mode, "by"))
        kvp_view_zoom_by(&P.view, kvp_msg_get_double(m, "factor", ZOOM_STEP), ax, ay);
    else
        return;
    send_view();
    redraw();
}

static void on_message(const char *line, int fd)
{
    KvpMsg *m = kvp_msg_parse(line);
    if (!m)
        return;
    const char *v = m->verb;
    if (!strcmp(v, "surface"))
        on_surface(m);
    else if (!strcmp(v, "style"))
        on_style(m);
    else if (!strcmp(v, "open"))
        on_open(m, fd);
    else if (!strcmp(v, "close")) {
        if (kvp_msg_get_int(m, "id", P.id) == P.id) {
            release_doc();
            redraw();
        }
    } else if (!strcmp(v, "zoom"))
        on_zoom(m);
    else if (!strcmp(v, "pan") && P.img) {
        kvp_view_pan(&P.view, kvp_msg_get_double(m, "dx", 0), kvp_msg_get_double(m, "dy", 0));
        send_view();
        redraw();
    } else if (!strcmp(v, "rotate") && P.img) {
        kvp_view_rotate(&P.view, kvp_msg_get_int(m, "by", 90) < 0 ? -1 : 1);
        send_view();
        redraw();
    } else if (!strcmp(v, "quit"))
        g_main_loop_quit(P.loop);
    if (fd >= 0 && strcmp(v, "open"))
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
        if (e.type == Expose && e.xexpose.count == 0)
            redraw();
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
    g_unix_fd_add(P.sock, G_IO_IN | G_IO_HUP, on_socket, NULL);

    GString *m = kvp_out("hello");
    kvp_out_i(m, "version", KVP_PROTOCOL_VERSION);
    kvp_out_s(m, "caps", ops->caps ? ops->caps : "");
    send_out(m);

    g_main_loop_run(P.loop);
    return 0;
}
