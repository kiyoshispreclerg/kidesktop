/*
 * launchfx.c - optional "launch feedback" animation for tasklist launcher
 * icons: on click, the icon zooms up (default 2x, configurable) while
 * fading out, then the popup goes away -- the effect Mint/modern GNOME
 * have, and KDE 3.5 also had well before compositors were common.
 *
 * Compositor-only: the popup is a real 32-bit ARGB override-redirect
 * window (XMatchVisualInfo, same recipe as toast.c/tooltip.c/menu.c).
 * Each frame just clears it to transparent and paints the icon
 * scaled+faded on top; the compositor blends that against whatever's
 * actually behind it live. No readback of any kind, no backdrop capture.
 *
 * There's deliberately no no-compositor fallback: without a compositor,
 * an ARGB window's alpha channel is simply never blended by anything and
 * shows as black. A software-blended backdrop grab was tried and dropped
 * -- reading off the root window isn't something the X protocol actually
 * guarantees for the general case, and on XiS specifically the direct-
 * flip presentation path means there may be nothing to read on the root
 * window at all. launchfx_trigger() just no-ops when no compositor is
 * currently registered.
 *
 * Only one animation plays at a time -- a second trigger while one is
 * still running tears down the first immediately and starts fresh, same
 * "last one wins" simplicity as e.g. tooltip.c's single popup.
 */
#include "xispanel.h"

#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <cairo/cairo-xlib.h>

#include <stdio.h>
#include <string.h>

typedef struct {
    int active;
    Window win;
    cairo_surface_t *surface;
    cairo_t *cr;
    /* Offscreen ARGB32 buffer paint_frame() actually composes each frame
     * into -- clear+scale+fade is three separate cairo calls, and without
     * this a compositor's own repaint could sample the window mid-way
     * through them (visible as a flicker/glitch frame). Same reasoning as
     * Panel::buf_surface in xispanel.c: compose off-screen, then a single
     * cairo_paint() blits the finished frame to the real (on-screen)
     * window surface, so the compositor only ever sees complete frames. */
    cairo_surface_t *buf_surface;
    cairo_t *buf_cr;
    cairo_surface_t *icon; /* our own reference (cairo_surface_reference()), unref'd on teardown */
    int icon_iw, icon_ih; /* icon's actual pixel size (cairo_image_surface_get_width/height) -- may be
                            * well above icon_px (icon caches are fetched at ICON_FETCH_HEADROOM for
                            * crisp downscaling, see ewmh.c), so this is what paint_frame() must scale
                            * *from*, never icon_px itself. */
    int size; /* final (zoomed) box, both dimensions -- window is always square */
    int icon_px; /* the icon's normal on-panel (1x, t=0) display size, before zoom */
    double zoom;
    double draw_x0, draw_y0; /* where (in window-local coords) the icon is drawn at t=0 -- the real
                               * icon's position, which generally isn't the window's own center (see
                               * launchfx_trigger()'s doc comment). paint_frame() lerps from here to
                               * (size/2, size/2) as t goes 0..1. */
    uint64_t start_ms, end_ms;
} LaunchFx;

static LaunchFx g_fx;

static Visual *g_argb_visual = NULL;
static int g_argb_depth = 0;
static Colormap g_argb_cmap = None;
static int g_argb_ready = 0;

static void ensure_argb_visual(void)
{
    if (g_argb_ready) {
        return;
    }
    g_argb_ready = 1;
    XVisualInfo vinfo;
    if (XMatchVisualInfo(g_dpy, g_screen, 32, TrueColor, &vinfo)) {
        g_argb_visual = vinfo.visual;
        g_argb_depth = vinfo.depth;
        g_argb_cmap = XCreateColormap(g_dpy, g_root, g_argb_visual, AllocNone);
    }
}

static int compositor_present(void)
{
    char name[32];
    snprintf(name, sizeof(name), "_NET_WM_CM_S%d", g_screen);
    Atom cm = XInternAtom(g_dpy, name, False);
    return XGetSelectionOwner(g_dpy, cm) != None;
}

static void teardown(void)
{
    if (!g_fx.active) {
        return;
    }
    if (g_fx.buf_cr) {
        cairo_destroy(g_fx.buf_cr);
    }
    if (g_fx.buf_surface) {
        cairo_surface_destroy(g_fx.buf_surface);
    }
    if (g_fx.cr) {
        cairo_destroy(g_fx.cr);
    }
    if (g_fx.surface) {
        cairo_surface_destroy(g_fx.surface);
    }
    if (g_fx.win) {
        XDestroyWindow(g_dpy, g_fx.win);
    }
    if (g_fx.icon) {
        cairo_surface_destroy(g_fx.icon);
    }
    memset(&g_fx, 0, sizeof(g_fx));
}

/* Paints one frame at animation fraction t (0=start, 1=done): the icon
 * scaled to `1 + (zoom-1)*t` of its base size, faded to alpha `1-t`, and
 * drawn at a point lerped from (draw_x0, draw_y0) -- the real icon's
 * position -- to the popup's own center, so t=0 lands pixel-for-pixel on
 * the real button icon (growing out of it) and t=1 ends up centered in
 * the fully on-screen popup (never clipped at full zoom) -- see
 * launchfx_trigger()'s doc comment for why those two points differ. */
static void paint_frame(double t)
{
    cairo_t *cr = g_fx.buf_cr;

    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_restore(cr);

    /* base_scale alone (t=0) must draw the icon at icon_px -- its normal
     * on-panel size -- not at its raw cached pixel size, which is bigger
     * (see icon_iw/icon_ih's doc comment). zoom_scale then grows that by
     * up to `zoom`x on top. */
    double base_scale = g_fx.icon_px / (double)(g_fx.icon_iw > g_fx.icon_ih ? g_fx.icon_iw : g_fx.icon_ih);
    double zoom_scale = 1.0 + (g_fx.zoom - 1.0) * t;
    double scale = base_scale * zoom_scale;
    double alpha = 1.0 - t;

    double cx = g_fx.draw_x0 + (g_fx.size / 2.0 - g_fx.draw_x0) * t;
    double cy = g_fx.draw_y0 + (g_fx.size / 2.0 - g_fx.draw_y0) * t;

    cairo_save(cr);
    cairo_translate(cr, cx, cy);
    cairo_scale(cr, scale, scale);
    cairo_set_source_surface(cr, g_fx.icon, -g_fx.icon_iw / 2.0, -g_fx.icon_ih / 2.0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
    cairo_paint_with_alpha(cr, alpha);
    cairo_restore(cr);

    /* Single atomic blit of the finished frame onto the real window --
     * see buf_surface's doc comment. */
    cairo_set_operator(g_fx.cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_surface(g_fx.cr, g_fx.buf_surface, 0, 0);
    cairo_paint(g_fx.cr);

    cairo_surface_flush(g_fx.surface);
}

/* Kicks off the animation, centered on (cx, cy) in root coordinates --
 * the caller works out the icon's actual on-screen center (see the
 * tasklist.c call site), not just wherever the pointer happened to be
 * within the button. `icon` is only read/referenced here, never modified
 * or assumed to outlive the call. `icon_px` is the icon's normal
 * on-panel size; `zoom` (e.g. 2.0) is how large it grows by the end;
 * `duration_ms` is how long the whole animation takes. Does nothing if
 * `icon` is NULL, no compositor is currently registered, or clamps
 * `zoom`/`duration_ms` if given a degenerate value. */
void launchfx_trigger(cairo_surface_t *icon, int cx, int cy, int icon_px, double zoom, int duration_ms)
{
    if (!icon || icon_px <= 0 || cairo_surface_status(icon) != CAIRO_STATUS_SUCCESS) {
        return;
    }
    if (cairo_image_surface_get_width(icon) <= 0 || cairo_image_surface_get_height(icon) <= 0) {
        return;
    }
    if (!compositor_present()) {
        return;
    }
    ensure_argb_visual();
    if (!g_argb_visual) {
        return;
    }
    if (zoom < 1.05) {
        zoom = 1.05;
    }
    if (duration_ms < 16) {
        duration_ms = 16;
    }

    teardown(); /* only one animation plays at a time */

    int size = (int)(icon_px * zoom + 0.5);
    if (size < 1) {
        size = 1;
    }

    /* The *window* itself is clamped to stay fully on-screen -- unlike an
     * unclamped box, this is what guarantees the icon is never clipped
     * once fully zoomed. Centering the window on the real icon would clip
     * it for anything near a screen edge (e.g. the first icon on a panel
     * flush against a corner, an extremely common case). The real icon
     * position (cx, cy) is still used as where *drawing* starts within
     * this window -- see draw_x0/draw_y0 below and paint_frame(). */
    int x = cx - size / 2;
    int y = cy - size / 2;
    int screen_w = DisplayWidth(g_dpy, g_screen);
    int screen_h = DisplayHeight(g_dpy, g_screen);
    if (size >= screen_w) {
        x = 0;
    } else if (x < 0) {
        x = 0;
    } else if (x + size > screen_w) {
        x = screen_w - size;
    }
    if (size >= screen_h) {
        y = 0;
    } else if (y < 0) {
        y = 0;
    } else if (y + size > screen_h) {
        y = screen_h - size;
    }

    XSetWindowAttributes attrs;
    memset(&attrs, 0, sizeof(attrs));
    attrs.override_redirect = True;
    attrs.colormap = g_argb_cmap;
    attrs.border_pixel = 0;
    attrs.background_pixel = 0;
    attrs.event_mask = ExposureMask;

    g_fx.win = XCreateWindow(g_dpy, g_root, x, y, size, size, 0, g_argb_depth, InputOutput, g_argb_visual,
                              CWOverrideRedirect | CWColormap | CWBorderPixel | CWBackPixel | CWEventMask, &attrs);
    g_fx.surface = cairo_xlib_surface_create(g_dpy, g_fx.win, g_argb_visual, size, size);
    g_fx.cr = cairo_create(g_fx.surface);
    g_fx.buf_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
    g_fx.buf_cr = cairo_create(g_fx.buf_surface);

    g_fx.icon = cairo_surface_reference(icon);
    g_fx.icon_iw = cairo_image_surface_get_width(icon);
    g_fx.icon_ih = cairo_image_surface_get_height(icon);
    g_fx.size = size;
    g_fx.icon_px = icon_px;
    g_fx.zoom = zoom;
    g_fx.draw_x0 = cx - x; /* real icon position, in this window's own local coords */
    g_fx.draw_y0 = cy - y;
    g_fx.start_ms = now_ms();
    g_fx.end_ms = g_fx.start_ms + (uint64_t)duration_ms;
    g_fx.active = 1;

    /* Paint before mapping, not after: an unpainted freshly-mapped ARGB
     * window can show a garbage/blank frame for the instant before its
     * first real paint reaches the compositor -- visible as a flicker
     * right as the animation appears. */
    paint_frame(0.0);
    XMapWindow(g_dpy, g_fx.win);
    XRaiseWindow(g_dpy, g_fx.win);
}

void launchfx_tick(uint64_t now)
{
    if (!g_fx.active) {
        return;
    }
    if (now >= g_fx.end_ms) {
        teardown();
        return;
    }
    double duration_ms = (double)(g_fx.end_ms - g_fx.start_ms);
    double t = (double)(now - g_fx.start_ms) / duration_ms;
    if (t < 0.0) {
        t = 0.0;
    } else if (t > 1.0) {
        t = 1.0;
    }
    paint_frame(t);
}

uint64_t launchfx_next_wake_ms(void)
{
    /* ~30fps while active, same cadence panel.c's own any_animating path
     * already uses -- see xispanel.c's main loop timeout_ms computation. */
    return g_fx.active ? now_ms() + 33 : 0;
}

int launchfx_handle_event(const XEvent *ev)
{
    if (!g_fx.active) {
        return 0;
    }
    Window w = None;
    switch (ev->type) {
    case Expose:
        w = ev->xexpose.window;
        break;
    default:
        return 0;
    }
    if (w != g_fx.win) {
        return 0;
    }
    double duration_ms = (double)(g_fx.end_ms - g_fx.start_ms);
    double t = (double)(now_ms() - g_fx.start_ms) / duration_ms;
    if (t < 0.0) {
        t = 0.0;
    } else if (t > 1.0) {
        t = 1.0;
    }
    paint_frame(t);
    return 1;
}
