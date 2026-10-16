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

gboolean ks_area_rect(KsArea area, gboolean decorations, KsRect *out)
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
        if (w) {
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

static void draw_pointer(GdkPixbuf *pb, const KsRect *r)
{
    Display *dpy = GDK_DISPLAY_XDISPLAY(gdk_display_get_default());
    int ev, err;
    if (!XFixesQueryExtension(dpy, &ev, &err))
        return;
    XFixesCursorImage *ci = XFixesGetCursorImage(dpy);
    if (!ci)
        return;
    int ox = ci->x - ci->xhot - r->x, oy = ci->y - ci->yhot - r->y;
    int W = gdk_pixbuf_get_width(pb), H = gdk_pixbuf_get_height(pb);
    int nch = gdk_pixbuf_get_n_channels(pb), stride = gdk_pixbuf_get_rowstride(pb);
    guchar *px = gdk_pixbuf_get_pixels(pb);
    for (int y = 0; y < ci->height; y++) {
        int dy = oy + y;
        if (dy < 0 || dy >= H)
            continue;
        for (int x = 0; x < ci->width; x++) {
            int dx = ox + x;
            if (dx < 0 || dx >= W)
                continue;
            /* XFixes pixels are premultiplied ARGB in an unsigned long */
            unsigned long p = ci->pixels[y * ci->width + x];
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
    GdkPixbuf *pb = gdk_pixbuf_get_from_drawable(NULL, root, NULL, r->x, r->y, 0, 0, r->w, r->h);
    if (!pb)
        return NULL;
    if (pointer)
        draw_pointer(pb, r);
    ks_censor_paint(pb, r);
    return pb;
}
