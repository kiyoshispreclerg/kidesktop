/* SPDX-License-Identifier: GPL-3.0-or-later */
/* See xis_density_source.h. Taken from Kiplay's ui/videowin.c. */
#include "xis_density_source.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xrender.h>
#include <gdk/gdkx.h>

/* A density beyond this is not a zoom anyone asked for. */
#define MAX_DENSITY 8.0
#define FORCED_DEN 1000

static struct {
    GtkWidget *area;
    GtkWindow *top;
    XisDensityChanged changed;
    gpointer ud;
    Window win;
    int w, h;         /* the window's size now: the area's, times the density */
    long num, den;    /* 0/1: not redirected */
    bool usable;      /* the server has Composite and Damage */
    bool forced;      /* the density comes from the environment */
    bool shown;
    int damage_event;
    Damage damage;
    Picture src, dst; /* the window, the area */
    guint copy_idle;
    Atom scale_atom, sources_atom;
    bool announce;    /* write _X_DENSITY_SOURCES after the next frame */
    bool announced;   /* it is on the toplevel */
    long sources[5];
} sw = {.shown = true, .den = 1};

static Display *dpy(void)
{
    return GDK_WINDOW_XDISPLAY(sw.area->window);
}

static Window top_xid(void)
{
    GdkWindow *w = GTK_WIDGET(sw.top)->window;
    return w ? GDK_WINDOW_XID(w) : None;
}

static double scale(void)
{
    return sw.num > 0 ? (double)sw.num / (double)sw.den : 1.0;
}

/* The window's contents, scaled down, into the area. */
static void copy(void)
{
    if (!sw.damage)
        return;
    int w = sw.area->allocation.width, h = sw.area->allocation.height;
    XRenderComposite(dpy(), PictOpSrc, sw.src, None, sw.dst, 0, 0, 0, 0, 0, 0, (unsigned)MAX(w, 1),
                     (unsigned)MAX(h, 1));
}

static gboolean copy_later(gpointer data)
{
    (void)data;
    sw.copy_idle = 0;
    copy();
    return G_SOURCE_REMOVE;
}

/* _X_DENSITY_SOURCES: the area and the window while dense and shown,
 * nothing otherwise. */
static void update_sources(void)
{
    Window top = top_xid();
    if (!top)
        return;
    Display *d = dpy();
    /* Not while the area is unmapped, nor before it has a size (GTK's 1x1
     * at -1, -1). */
    const GtkAllocation *a = &sw.area->allocation;
    bool wanted = sw.damage && sw.shown && gtk_widget_get_mapped(sw.area) &&
                  gtk_widget_get_toplevel(sw.area) == GTK_WIDGET(sw.top) && a->width > 1 &&
                  a->height > 1;
    long v[5] = {0};
    if (wanted) {
        int x, y;
        Window child;
        if (!XTranslateCoordinates(d, GDK_WINDOW_XID(sw.area->window), top, 0, 0, &x, &y, &child))
            wanted = false;
        v[0] = x;
        v[1] = y;
        v[2] = a->width;
        v[3] = a->height;
        v[4] = (long)sw.win;
    }
    if (!wanted) {
        if (sw.announced)
            XDeleteProperty(d, top, sw.sources_atom);
        sw.announced = false;
        return;
    }
    if (sw.announced && memcmp(v, sw.sources, sizeof v) == 0)
        return;
    XChangeProperty(d, top, sw.sources_atom, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)v, 5);
    memcpy(sw.sources, v, sizeof v);
    sw.announced = true;
}

/* The source picture maps area pixels to window pixels. */
static void set_transform(void)
{
    double s = scale();
    XTransform t = {{{XDoubleToFixed(s), 0, 0}, {0, XDoubleToFixed(s), 0}, {0, 0, XDoubleToFixed(1)}}};
    XRenderSetPictureTransform(dpy(), sw.src, &t);
    XRenderSetPictureFilter(dpy(), sw.src, s == 1 ? FilterNearest : FilterGood, NULL, 0);
}

/* Whether the window changed size. */
static bool fit(void)
{
    if (!sw.win || !gtk_widget_get_realized(sw.area))
        return false;
    double s = scale();
    int w = MAX((int)ceil(sw.area->allocation.width * s), 1);
    int h = MAX((int)ceil(sw.area->allocation.height * s), 1);
    if (w == sw.w && h == sw.h)
        return false;
    sw.w = w;
    sw.h = h;
    XResizeWindow(dpy(), sw.win, (unsigned)w, (unsigned)h);
    return true;
}

/* The drawer is told only once the server has the new size, or it would
 * ask for a window that is still the old one. */
static void notify(void)
{
    if (!sw.changed)
        return;
    XSync(dpy(), False);
    sw.changed(sw.ud);
}

static void on_allocate(GtkWidget *area, GtkAllocation *a, gpointer data)
{
    (void)area;
    (void)a;
    (void)data;
    /* A new size is announced once the drawer has drawn at it; a move
     * right away. */
    if (fit()) {
        if (sw.damage)
            sw.announce = true;
        else
            update_sources();
        notify();
    } else if (!sw.announce) {
        update_sources();
    }
}

static void on_map_changed(GtkWidget *area, gpointer data)
{
    (void)area;
    (void)data;
    if (sw.win)
        update_sources();
}

static void on_unrealize(GtkWidget *area, gpointer data)
{
    (void)area;
    (void)data;
    sw.win = 0;
    sw.damage = 0;
    sw.src = sw.dst = 0;
    sw.w = sw.h = 0;
    sw.num = 0;
    sw.den = 1;
    sw.announce = false;
    if (sw.copy_idle) {
        g_source_remove(sw.copy_idle);
        sw.copy_idle = 0;
    }
    if (sw.announced && top_xid())
        XDeleteProperty(dpy(), top_xid(), sw.sources_atom);
    sw.announced = false;
}

/* _X_DENSITY_SCALE on the toplevel as num/den, 0/1 when absent or 1/1. */
static void toplevel_density(long *num, long *den)
{
    *num = 0;
    *den = 1;
    Window top = top_xid();
    if (!top)
        return;
    Atom type;
    int format;
    unsigned long n, after;
    unsigned char *data = NULL;
    gdk_error_trap_push();
    if (XGetWindowProperty(dpy(), top, sw.scale_atom, 0, 2, False, XA_CARDINAL, &type, &format, &n,
                           &after, &data) == Success &&
        type == XA_CARDINAL && format == 32 && n == 2) {
        /* format 32 comes back as longs */
        long nu = ((long *)data)[0], de = ((long *)data)[1];
        if (nu > de && de > 0) {
            *num = nu;
            *den = de;
        }
    }
    if (data)
        XFree(data);
    (void)gdk_error_trap_pop();
}

static void set_density(long num, long den);

static GdkFilterReturn filter(GdkXEvent *xev, GdkEvent *ev, gpointer data)
{
    (void)ev;
    (void)data;
    XEvent *e = xev;
    if (sw.damage && e->type == sw.damage_event + XDamageNotify &&
        ((XDamageNotifyEvent *)e)->damage == sw.damage) {
        XDamageSubtract(e->xany.display, sw.damage, None, None);
        copy();
        if (sw.announce) {
            sw.announce = false;
            update_sources();
        }
        return GDK_FILTER_REMOVE;
    }
    if (e->type == PropertyNotify && !sw.forced && sw.win && e->xproperty.atom == sw.scale_atom &&
        e->xproperty.window == top_xid()) {
        long num, den;
        toplevel_density(&num, &den);
        set_density(num, den);
    }
    return GDK_FILTER_CONTINUE;
}

static void redirect(void)
{
    Display *d = dpy();
    Window area = GDK_WINDOW_XID(sw.area->window);
    XWindowAttributes wa;
    XGetWindowAttributes(d, sw.win, &wa);
    XRenderPictFormat *fmt = XRenderFindVisualFormat(d, wa.visual);
    if (!fmt)
        return;

    gdk_error_trap_push();
    XCompositeRedirectWindow(d, sw.win, CompositeRedirectManual);
    gdk_flush();
    if (gdk_error_trap_pop())
        return; /* somebody else redirects it already */
    /* Both through the children: the drawer's window may have its own. */
    XRenderPictureAttributes pa = {.subwindow_mode = IncludeInferiors};
    sw.src = XRenderCreatePicture(d, sw.win, fmt, CPSubwindowMode, &pa);
    sw.dst = XRenderCreatePicture(d, area, fmt, CPSubwindowMode, &pa);
    sw.damage = XDamageCreate(d, sw.win, XDamageReportNonEmpty);
}

static void unredirect(void)
{
    Display *d = dpy();
    if (!sw.damage)
        return;
    gdk_error_trap_push();
    XDamageDestroy(d, sw.damage);
    XRenderFreePicture(d, sw.src);
    XRenderFreePicture(d, sw.dst);
    XCompositeUnredirectWindow(d, sw.win, CompositeRedirectManual);
    gdk_flush();
    (void)gdk_error_trap_pop();
    sw.damage = 0;
    sw.src = sw.dst = 0;
    if (sw.copy_idle) {
        g_source_remove(sw.copy_idle);
        sw.copy_idle = 0;
    }
}

static void set_density(long num, long den)
{
    if (den <= 0 || num <= den) {
        num = 0;
        den = 1;
    }
    if ((double)num / (double)den > MAX_DENSITY) {
        num = (long)MAX_DENSITY * den;
    }
    if (num > 0 && !sw.usable) {
        num = 0;
        den = 1;
    }
    if (!sw.win) {
        sw.num = num; /* for when it is made */
        sw.den = den;
        return;
    }
    if (num == sw.num && (num == 0 || den == sw.den))
        return;

    if (num > 0 && !sw.damage)
        redirect();
    else if (num == 0)
        unredirect();
    if (sw.damage) {
        sw.num = num;
        sw.den = den;
    } else {
        sw.num = 0;
        sw.den = 1;
    }
    if (sw.damage)
        set_transform();
    fit();
    if (sw.damage) {
        copy();
        sw.announce = true; /* once the drawer has drawn at the new size */
    } else {
        sw.announce = false;
        update_sources(); /* gone */
    }
    gdk_flush();
    notify();
}

unsigned long xis_density_source_create(GtkWidget *area, GtkWindow *toplevel, const char *force_env,
                                        XisDensityChanged changed, gpointer user_data)
{
    if (sw.win)
        return sw.win;
    sw.area = area;
    sw.top = toplevel;
    sw.changed = changed;
    sw.ud = user_data;
    gtk_widget_realize(area);
    Display *d = dpy();

    static bool initialized;
    if (!initialized) {
        initialized = true;
        int major = 0, minor = 2, event_base, error_base;
        sw.usable = XCompositeQueryExtension(d, &event_base, &error_base) &&
                    XCompositeQueryVersion(d, &major, &minor) && (major > 0 || minor >= 2) &&
                    XDamageQueryExtension(d, &sw.damage_event, &error_base);
        sw.scale_atom = XInternAtom(d, "_X_DENSITY_SCALE", False);
        sw.sources_atom = XInternAtom(d, "_X_DENSITY_SOURCES", False);
        g_signal_connect_after(area, "size-allocate", G_CALLBACK(on_allocate), NULL);
        g_signal_connect_after(area, "map", G_CALLBACK(on_map_changed), NULL);
        g_signal_connect_after(area, "unmap", G_CALLBACK(on_map_changed), NULL);
        g_signal_connect(area, "unrealize", G_CALLBACK(on_unrealize), NULL);
        gdk_window_add_filter(NULL, filter, NULL);
        const char *env = force_env ? g_getenv(force_env) : NULL;
        if (env && *env) {
            sw.forced = true;
            double f = g_ascii_strtod(env, NULL);
            if (f > 1) {
                sw.num = (long)lround(f * FORCED_DEN);
                sw.den = FORCED_DEN;
            }
        }
    }

    sw.w = MAX(area->allocation.width, 1);
    sw.h = MAX(area->allocation.height, 1);
    /* Same visual and depth as the area (CopyFromParent), black under
     * whatever the drawer hasn't drawn yet. */
    sw.win = XCreateSimpleWindow(d, GDK_WINDOW_XID(area->window), 0, 0, (unsigned)sw.w, (unsigned)sw.h, 0,
                                 0, BlackPixel(d, DefaultScreen(d)));
    XMapWindow(d, sw.win);

    long num, den;
    if (sw.forced) {
        num = sw.num;
        den = sw.den;
    } else {
        toplevel_density(&num, &den);
    }
    sw.num = 0;
    sw.den = 1;
    set_density(num, den);
    gdk_flush();
    return sw.win;
}

unsigned long xis_density_source_window(void)
{
    return sw.win;
}

void xis_density_source_ratio(long *num, long *den)
{
    *num = sw.num > 0 ? sw.num : 1;
    *den = sw.num > 0 ? sw.den : 1;
}

void xis_density_source_set_shown(gboolean shown)
{
    if (!sw.win || sw.shown == (bool)shown)
        return;
    sw.shown = shown;
    if (shown)
        XMapWindow(dpy(), sw.win);
    else
        XUnmapWindow(dpy(), sw.win);
    /* back on screen: announced again once it has drawn */
    if (shown && sw.damage)
        sw.announce = true;
    update_sources();
    XFlush(dpy());
}

void xis_density_source_expose(void)
{
    if (sw.damage && !sw.copy_idle)
        sw.copy_idle = g_idle_add(copy_later, NULL);
}
