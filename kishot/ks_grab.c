/* ks_grab.c - which rectangle an area means, and a still grab of it.
 *
 * Screenshots read the root window with a plain GetImage (through
 * gdk-pixbuf): one frame costs nothing worth optimising, and XiS servers
 * already make GetImage see per-CRTC flips. The pointer is drawn from
 * XFixes' cursor image, since GetImage never includes it. */
#include "ks.h"
#include <gdk/gdkx.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xrender.h>
#include <X11/extensions/Xrandr.h>
#include <string.h>

gboolean ks_rect_clip_to_screen(KsRect *r)
{
    GdkScreen *scr = gdk_screen_get_default();
    int sw = gdk_screen_get_width(scr), sh = gdk_screen_get_height(scr);
    int x1 = MAX(r->x, 0), y1 = MAX(r->y, 0);
    int x2 = MIN(r->x + r->w, sw), y2 = MIN(r->y + r->h, sh);
    if (x2 <= x1 || y2 <= y1)
        return FALSE;
    r->x = x1;
    r->y = y1;
    r->w = x2 - x1;
    r->h = y2 - y1;
    return TRUE;
}

static Window active_window(Display *dpy)
{
    Atom prop = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
    Atom type;
    int fmt;
    unsigned long n, after;
    unsigned char *data = NULL;
    Window w = None;
    if (XGetWindowProperty(dpy, DefaultRootWindow(dpy), prop, 0, 1, False, XA_WINDOW,
                           &type, &fmt, &n, &after, &data) == Success && data) {
        if (n == 1 && fmt == 32)
            w = *(Window *)data;
        XFree(data);
    }
    return w;
}

/* The child of the root that contains w (the WM frame when reparented). */
static Window toplevel_of(Display *dpy, Window w)
{
    Window root = DefaultRootWindow(dpy);
    for (;;) {
        Window r, parent, *kids = NULL;
        unsigned int n;
        if (!XQueryTree(dpy, w, &r, &parent, &kids, &n))
            return None;
        if (kids)
            XFree(kids);
        if (parent == root || parent == None)
            return w;
        w = parent;
    }
}

static gboolean window_rect(Display *dpy, Window w, KsRect *out)
{
    XWindowAttributes a;
    Window child;
    int x, y;
    if (!XGetWindowAttributes(dpy, w, &a))
        return FALSE;
    XTranslateCoordinates(dpy, w, DefaultRootWindow(dpy), 0, 0, &x, &y, &child);
    out->x = x;
    out->y = y;
    out->w = a.width;
    out->h = a.height;
    return TRUE;
}

int ks_monitor_count(void)
{
    return gdk_screen_get_n_monitors(gdk_screen_get_default());
}

/* Monitor names come from RandR 1.5 ("DP-1", "HDMI-1", or what
 * `xrandr --setmonitor` named them): GDK2 gives no connector names. The
 * GDK monitor is matched to its RandR one by geometry; without RandR 1.5 a
 * monitor is "Monitor N", N counting from 1 in GDK's order. */
char *ks_monitor_name(int i)
{
    GdkScreen *scr = gdk_screen_get_default();
    Display *dpy = GDK_DISPLAY_XDISPLAY(gdk_display_get_default());
    GdkRectangle g;
    char *name = NULL;
    gdk_screen_get_monitor_geometry(scr, i, &g);
    int n = 0;
    XRRMonitorInfo *mi = XRRGetMonitors(dpy, DefaultRootWindow(dpy), True, &n);
    for (int k = 0; mi && k < n && !name; k++)
        if (mi[k].x == g.x && mi[k].y == g.y && mi[k].width == g.width && mi[k].height == g.height) {
            char *an = XGetAtomName(dpy, mi[k].name);
            if (an) {
                name = g_strdup(an);
                XFree(an);
            }
        }
    if (mi)
        XRRFreeMonitors(mi);
    return name ? name : g_strdup_printf("Monitor %d", i + 1);
}

gboolean ks_monitor_rect_by_name(const char *name, KsRect *out)
{
    GdkScreen *scr = gdk_screen_get_default();
    if (!name)
        return FALSE;
    for (int i = 0; i < gdk_screen_get_n_monitors(scr); i++) {
        char *n = ks_monitor_name(i);
        /* "Monitor N" also matches by position: what older configs saved */
        char *legacy = g_strdup_printf("Monitor %d", i + 1);
        gboolean hit = !strcmp(n, name) || !strcmp(legacy, name);
        g_free(n);
        g_free(legacy);
        if (hit) {
            GdkRectangle g;
            gdk_screen_get_monitor_geometry(scr, i, &g);
            *out = (KsRect){ g.x, g.y, g.width, g.height };
            return TRUE;
        }
    }
    return FALSE;
}

gboolean ks_area_rect(KsArea area, gboolean decorations, KsRect *out)
{
    return ks_area_rect_except(area, decorations, 0, out);
}

gboolean ks_area_rect_except(KsArea area, gboolean decorations, unsigned long except, KsRect *out)
{
    GdkScreen *scr = gdk_screen_get_default();
    switch (area) {
    case KS_AREA_MONITOR: {
        int px, py;
        gdk_display_get_pointer(gdk_display_get_default(), NULL, &px, &py, NULL);
        GdkRectangle g;
        gdk_screen_get_monitor_geometry(scr, gdk_screen_get_monitor_at_point(scr, px, py), &g);
        *out = (KsRect){ g.x, g.y, g.width, g.height };
        return TRUE;
    }
    case KS_AREA_WINDOW: {
        Display *dpy = GDK_DISPLAY_XDISPLAY(gdk_display_get_default());
        gdk_error_trap_push();
        Window w = active_window(dpy);
        gboolean ok = FALSE;
        if (w && w != except) {
            Window top = decorations ? toplevel_of(dpy, w) : w;
            ok = top && window_rect(dpy, top, out);
        }
        gdk_flush();
        gdk_error_trap_pop();
        if (ok && ks_rect_clip_to_screen(out))
            return TRUE;
        /* no active window (desktop focused): fall back to the monitor */
        return ks_area_rect(KS_AREA_MONITOR, decorations, out);
    }
    case KS_AREA_FULL:
    case KS_AREA_REGION:
    default:
        *out = (KsRect){ 0, 0, gdk_screen_get_width(scr), gdk_screen_get_height(scr) };
        return TRUE;
    }
}

/* The pointer, scaled like the image (1.0 for a real shot). Sampled
 * nearest-neighbour from the cursor image, walking the destination. */
static void draw_pointer(GdkPixbuf *pb, const KsRect *r, double scale)
{
    Display *dpy = GDK_DISPLAY_XDISPLAY(gdk_display_get_default());
    int ev, err;
    if (!XFixesQueryExtension(dpy, &ev, &err))
        return;
    XFixesCursorImage *ci = XFixesGetCursorImage(dpy);
    if (!ci)
        return;
    int ox = (int)((ci->x - ci->xhot - r->x) * scale), oy = (int)((ci->y - ci->yhot - r->y) * scale);
    int cw = MAX(1, (int)(ci->width * scale)), ch = MAX(1, (int)(ci->height * scale));
    int W = gdk_pixbuf_get_width(pb), H = gdk_pixbuf_get_height(pb);
    int nch = gdk_pixbuf_get_n_channels(pb), stride = gdk_pixbuf_get_rowstride(pb);
    guchar *px = gdk_pixbuf_get_pixels(pb);
    for (int y = 0; y < ch; y++) {
        int dy = oy + y;
        if (dy < 0 || dy >= H)
            continue;
        int sy = MIN((int)(y / scale), ci->height - 1);
        for (int x = 0; x < cw; x++) {
            int dx = ox + x;
            if (dx < 0 || dx >= W)
                continue;
            int sx = MIN((int)(x / scale), ci->width - 1);
            /* XFixes pixels are premultiplied ARGB in an unsigned long */
            unsigned long p = ci->pixels[sy * ci->width + sx];
            unsigned a = (p >> 24) & 0xff;
            if (!a)
                continue;
            guchar *d = px + dy * stride + dx * nch;
            unsigned sr = (p >> 16) & 0xff, sg = (p >> 8) & 0xff, sb = p & 0xff;
            d[0] = sr + d[0] * (255 - a) / 255;
            d[1] = sg + d[1] * (255 - a) / 255;
            d[2] = sb + d[2] * (255 - a) / 255;
        }
    }
    XFree(ci);
}

GdkPixbuf *ks_grab_rect(const KsRect *r, gboolean pointer)
{
    GdkWindow *root = gdk_get_default_root_window();
    xcb_connection_t *xc = ks_capture_gdk_connection();
    KsCapView cv = { 0 };
    if (ks_capture_acquire())
        ks_capture_view(xc, r, &cv);
    GdkPixbuf *pb = gdk_pixbuf_get_from_drawable(NULL, root, NULL, r->x, r->y, 0, 0, r->w, r->h);
    /* Where something hidden shows, the compositor's censored picture is
     * read instead of the screen. Where nothing does, the screen is the
     * censored picture -- its word, checked again after the copy. */
    KsCapSlot slots[KS_CAP_MAX_OUT];
    int nslots = 0;
    gboolean slots_ok = TRUE;
    if (pb && cv.ok && cv.n_slot)
        slots_ok = ks_capture_fetch(&cv, slots, &nslots) &&
                   ks_capture_copy_pixbuf(slots, nslots, r, pb);
    ks_capture_return(slots, nslots);        /* the read above was synchronous */
    gboolean clear = pb && slots_ok && ks_capture_unchanged(xc, &cv);
    ks_capture_note(&cv, clear, cv.n_slot > 0);
    ks_capture_release();
    if (!pb)
        return NULL;
    if (pointer)
        draw_pointer(pb, r, 1.0);
    if (!clear) {
        ks_censor_refresh();
        ks_censor_paint(pb, r);
    }
    return pb;
}

/* A small copy of r for the live preview. The server copies the area into a
 * pixmap of ours (CopyArea, the way GPU recording does: on XiS it sees what
 * the compositor flipped to each CRTC, which a Render picture of the root
 * does not -- that stays black) and Render scales that, so only the
 * thumbnail crosses the wire. */
GdkPixbuf *ks_grab_scaled(const KsRect *r, int dw, int dh, gboolean pointer)
{
    static Pixmap full;
    static int full_w, full_h;
    static GC gc;
    Display *dpy = GDK_DISPLAY_XDISPLAY(gdk_display_get_default());
    int ev, err;
    if (dw < 1 || dh < 1 || !XRenderQueryExtension(dpy, &ev, &err))
        return NULL;
    Window root = DefaultRootWindow(dpy);
    int depth = DefaultDepth(dpy, DefaultScreen(dpy));
    XRenderPictFormat *fmt = XRenderFindVisualFormat(dpy, DefaultVisual(dpy, DefaultScreen(dpy)));
    XRenderPictFormat *fmt24 = XRenderFindStandardFormat(dpy, PictStandardRGB24);
    if (!fmt || !fmt24)
        return NULL;
    gdk_error_trap_push();
    if (!gc) {
        XGCValues gv = { .subwindow_mode = IncludeInferiors, .graphics_exposures = False };
        gc = XCreateGC(dpy, root, GCSubwindowMode | GCGraphicsExposures, &gv);
    }
    if (!full || full_w != r->w || full_h != r->h) {
        if (full)
            XFreePixmap(dpy, full);
        full = XCreatePixmap(dpy, root, r->w, r->h, depth);
        full_w = r->w;
        full_h = r->h;
    }
    xcb_connection_t *xc = ks_capture_gdk_connection();
    KsCapView cv = { 0 };
    if (ks_capture_acquire())
        ks_capture_view(xc, r, &cv);
    XCopyArea(dpy, root, full, gc, r->x, r->y, r->w, r->h, 0, 0);
    KsCapSlot slots[KS_CAP_MAX_OUT];
    int nslots = 0;
    gboolean slots_ok = TRUE;
    if (cv.ok && cv.n_slot && (slots_ok = ks_capture_fetch(&cv, slots, &nslots)))
        ks_capture_copy_xlib(dpy, slots, nslots, r, full, gc);
    Picture src = XRenderCreatePicture(dpy, full, fmt, 0, NULL);
    Pixmap pm = XCreatePixmap(dpy, root, dw, dh, 24);
    Picture dst = XRenderCreatePicture(dpy, pm, fmt24, 0, NULL);
    double sx = (double)r->w / dw, sy = (double)r->h / dh;
    XTransform t = { {
        { XDoubleToFixed(sx), 0, 0 },
        { 0, XDoubleToFixed(sy), 0 },
        { 0, 0, XDoubleToFixed(1) },
    } };
    XRenderSetPictureTransform(dpy, src, &t);
    XRenderSetPictureFilter(dpy, src, FilterBilinear, NULL, 0);
    XRenderComposite(dpy, PictOpSrc, src, None, dst, 0, 0, 0, 0, 0, 0, dw, dh);
    XImage *img = XGetImage(dpy, pm, 0, 0, dw, dh, AllPlanes, ZPixmap);
    ks_capture_return(slots, nslots);        /* after the synchronous read above */
    gboolean clear = img && slots_ok && ks_capture_unchanged(xc, &cv);
    ks_capture_note(&cv, clear, cv.n_slot > 0);
    ks_capture_release();
    XRenderFreePicture(dpy, dst);
    XRenderFreePicture(dpy, src);
    XFreePixmap(dpy, pm);
    gdk_flush();
    if (gdk_error_trap_pop() || !img) {
        if (img)
            XDestroyImage(img);
        return NULL;
    }
    GdkPixbuf *pb = NULL;
    if (img->bits_per_pixel == 32) {
        pb = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, dw, dh);
        guchar *px = gdk_pixbuf_get_pixels(pb);
        int stride = gdk_pixbuf_get_rowstride(pb);
        for (int y = 0; y < dh; y++) {
            const uint32_t *s = (const uint32_t *)(img->data + y * img->bytes_per_line);
            guchar *d = px + y * stride;
            for (int x = 0; x < dw; x++, d += 3) {
                d[0] = s[x] >> 16;
                d[1] = s[x] >> 8;
                d[2] = s[x];
            }
        }
    }
    XDestroyImage(img);
    if (!pb)
        return NULL;
    double scale = (double)dw / r->w;
    if (pointer)
        draw_pointer(pb, r, scale);
    if (!clear) {
        ks_censor_refresh();
        ks_censor_paint_scaled(pb, r, scale);
    }
    return pb;
}
