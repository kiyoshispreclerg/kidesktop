/*
 * thumb.c - live window thumbnails for tasklist's tooltip (show_thumbs=
 * yes), via the XComposite + XDamage extensions. Gated on both an active
 * compositor (checked by looking for an owner of the _NET_WM_CM_S<screen>
 * selection -- the same convention every compositing WM/EWMH client uses
 * to detect one) and, per the Makefile, whether libXcomposite/libXdamage
 * were available at build time at all -- see thumb_stub.c for the
 * build-time fallback.
 *
 * Once a compositor is running, every top-level window is automatically
 * redirected to an offscreen pixmap (that's what "compositing" means),
 * so unlike an explicit-compositor implementation, this never needs to
 * call XCompositeRedirectWindow itself -- XCompositeNameWindowPixmap()
 * on a *redirected* window just works. That pixmap, and the cairo
 * surface wrapping it, are then held for as long as the window is
 * watched (see ThumbWatch::pix) rather than re-named per frame: a
 * composite pixmap updates in place as the window redraws, so the only
 * thing per-frame re-naming ever bought was papering over the classic
 * stale-composite-pixmap pitfall (the pixmap ID silently stops updating
 * across unmap/map or resize) at the cost of a round-trip and two
 * allocations on every single repaint. Those specific transitions are
 * caught directly instead, via StructureNotify on the target window --
 * see note_structure_event(). *When* it's painted is driven by XDamage,
 * not a blind timer: thumb_watch()/thumb_handle_event()/thumb_take_
 * dirty() (below) let tooltip.c repaint the popup exactly when the
 * watched window's content actually changed (server-side damage
 * tracking), which is what makes it look genuinely live -- the earlier
 * fixed-interval-repaint approach looked like a slideshow of periodic
 * screenshots instead, since it repainted at some arbitrary rate
 * regardless of whether anything had actually changed.
 *
 * The catch: on a reparenting WM, the window `_NET_CLIENT_LIST` reports
 * is not necessarily the one that's actually redirected -- some WMs
 * (confirmed on this repo's own KWin fork) wrap the client in one or
 * more of their own frame windows, and it's an *ancestor* frame that's
 * redirected, not the raw client window, which fails with a `BadMatch`.
 * `thumb_paint()` retries up the `XQueryTree()` parent chain (capped at
 * 4 hops) until one succeeds or it hits the root, rather than assuming
 * either "always the client" or "always the immediate parent".
 *
 * XCompositeNameWindowPixmap can also raise a BadMatch for an entirely
 * unrelated reason -- the window closed between the tasklist poll that
 * found it and this paint -- so every call here runs under a temporary,
 * deliberately-permissive XErrorHandler regardless. (xispanel.c also
 * installs a permanent permissive handler for the whole process, since
 * this general "window disappeared mid-query" race isn't unique to
 * thumbnails -- see x_error_handler() there.)
 */
#include "xispanel.h"

#include <X11/Xlib.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <cairo/cairo-xlib.h>

#include <signal.h>
#include <stdio.h>

static int g_composite_checked = 0;
static int g_composite_ext_present = 0;

static int g_damage_checked = 0;
static int g_damage_ext_present = 0;
static int g_damage_event_base = 0;
static int g_damage_error_base = 0;

static volatile sig_atomic_t g_thumb_had_error;

static Window resolve_composited_window(Window win, XWindowAttributes *out_wa, Pixmap *out_pix);

static int thumb_error_handler(Display *dpy, XErrorEvent *ev)
{
    (void)dpy;
    (void)ev;
    g_thumb_had_error = 1;
    return 0;
}

int thumb_available(void)
{
    if (!g_composite_checked) {
        g_composite_checked = 1;
        int event_base, error_base;
        g_composite_ext_present = XCompositeQueryExtension(g_dpy, &event_base, &error_base);
    }
    if (!g_composite_ext_present) {
        return 0;
    }

    char prop_name[32];
    snprintf(prop_name, sizeof(prop_name), "_NET_WM_CM_S%d", g_screen);
    Atom cm_atom = XInternAtom(g_dpy, prop_name, False);
    return XGetSelectionOwner(g_dpy, cm_atom) != None;
}

static int damage_available(void)
{
    if (!g_damage_checked) {
        g_damage_checked = 1;
        g_damage_ext_present = XDamageQueryExtension(g_dpy, &g_damage_event_base, &g_damage_error_base);
    }
    return g_damage_ext_present;
}

/* Live-thumbnail tracking: tooltip.c calls thumb_watch() for every window
 * a shown tooltip currently displays a thumbnail of (re-synced on every
 * show_popup(), see there), and thumb_unwatch_all() once that tooltip
 * closes or moves to different window(s). Each watched window gets an
 * XDamage handle (XDamageReportNonEmpty -- only care *that* something
 * changed, not the precise region); thumb_handle_event(), called from
 * xispanel.c's main event loop, sets g_thumb_dirty as notifications for a
 * watched window arrive, and tooltip_tick() polls/clears that flag once
 * per iteration via thumb_take_dirty() to decide whether to repaint.
 * This is what makes the thumbnail track the window's actual live
 * content (like plasmashell's/compiz's own live previews) instead of
 * being a single snapshot taken when the tooltip first opened, or --
 * worse -- looking like a slideshow of periodic screenshots if driven by
 * a blind timer instead of real damage events. */
#define THUMB_MAX_WATCHES 8
typedef struct {
    Window win; /* as passed by the caller (tooltip.c) -- only used for the idempotency check below */
    Damage damage;
    /* The ancestor thumb_watch() actually resolved `win` to (see
     * resolve_composited_window()) -- cached here so thumb_paint() can
     * skip straight to it on every subsequent frame instead of redoing
     * the whole "try the client window, fail, walk up to the parent"
     * dance (with its XSync round-trip) on every single repaint. At
     * video framerates that dance was the entire bottleneck: retried
     * every frame, it capped the achievable repaint rate at a couple of
     * frames a second regardless of how fast damage events arrived. */
    Window target;

    /* The composited pixmap for `target` and its cairo wrapper, both
     * held across frames. A window's composite pixmap keeps updating in
     * place as the window redraws -- that's the whole point of it -- so
     * re-naming it (and rebuilding a cairo surface around it) on every
     * repaint bought nothing and cost a round-trip plus two allocations
     * per frame, and defeated any caching cairo itself does per surface.
     * The one thing it *is* wrong across is the events that make the old
     * pixmap stale (resize, unmap/map, destroy) -- so instead of paying
     * that cost every frame to paper over a rare case, thumb_handle_
     * event() watches for exactly those (StructureNotify on `target`,
     * selected in thumb_watch()) and drops the cached pair, letting the
     * next thumb_paint() name a fresh one. `pix == None` simply means
     * "needs re-naming". */
    Pixmap pix;
    cairo_surface_t *surf;
    int w, h;        /* target's size as of when `pix` was named -- kept current by ConfigureNotify */
    Visual *visual;
    int mapped;      /* 0 while target is unmapped: nothing valid to draw, don't try */

    /* Set when thumb_watch() had to add StructureNotifyMask to `target`'s
     * event mask itself (it wasn't already selected) -- thumb_unwatch_
     * all() then takes it back off again, so a window we're no longer
     * watching stops feeding this process events. */
    int added_structure;
} ThumbWatch;
static ThumbWatch g_watches[THUMB_MAX_WATCHES];
static int g_n_watches = 0;
static volatile sig_atomic_t g_thumb_dirty = 0;

/* Releases just the cached composited pixmap + cairo surface, leaving the
 * watch itself (damage handle, resolved target, event-mask change) in
 * place -- the next thumb_paint() re-names a fresh pixmap for the same
 * target. Freeing the pixmap is safe even once the window is gone: a
 * named composite pixmap outlives its window (it just stops updating),
 * which is exactly why it has to be explicitly dropped here rather than
 * relied on to go away on its own. */
static void watch_drop_pixmap(ThumbWatch *w)
{
    if (w->surf) {
        cairo_surface_destroy(w->surf);
        w->surf = NULL;
    }
    if (w->pix != None) {
        XFreePixmap(g_dpy, w->pix);
        w->pix = None;
    }
}

void thumb_watch(Window win)
{
    if (win == None || !thumb_available() || !damage_available()) {
        return;
    }
    for (int i = 0; i < g_n_watches; i++) {
        if (g_watches[i].win == win) {
            return; /* already watching */
        }
    }
    if (g_n_watches >= THUMB_MAX_WATCHES) {
        return; /* group tooltips stay well under this cap */
    }

    XErrorHandler prev = XSetErrorHandler(thumb_error_handler);

    /* Must create the Damage handle on the *same* window thumb_paint()
     * actually reads a composited pixmap from, not blindly on `win` --
     * on this KWin fork, `win` (the raw client window _NET_CLIENT_LIST
     * reports) very often isn't the one that's composite-redirected, an
     * ancestor frame is (see resolve_composited_window()'s doc comment).
     * Watching the wrong window silently never generates the damage
     * events this whole mechanism depends on -- it doesn't error, it
     * just never fires, which is much harder to notice than a crash. */
    XWindowAttributes wa;
    Pixmap pix = None;
    Window target = resolve_composited_window(win, &wa, &pix);
    if (target == None) {
        XSetErrorHandler(prev);
        return;
    }

    g_thumb_had_error = 0;
    Damage d = XDamageCreate(g_dpy, target, XDamageReportNonEmpty);
    XSync(g_dpy, False);
    if (g_thumb_had_error) {
        XFreePixmap(g_dpy, pix);
        XSetErrorHandler(prev);
        return; /* window closed between resolving `target` and here */
    }

    ThumbWatch *w = &g_watches[g_n_watches];
    w->win = win;
    w->damage = d;
    w->target = target;
    /* Keep the pixmap resolve_composited_window() already named instead
     * of freeing it and naming another one on the first paint -- it's the
     * same pixmap, and this is the cache thumb_paint() reads from. */
    w->pix = pix;
    w->surf = NULL;
    w->w = wa.width;
    w->h = wa.height;
    w->visual = wa.visual;
    w->mapped = 1; /* resolve_composited_window() only succeeds on an IsViewable window */

    /* Invalidation signal for the cached pixmap above. Mostly redundant
     * -- ewmh_watch_init() already holds SubstructureNotifyMask on the
     * root, which delivers these for any *direct child of root*, and
     * `target` is normally either a client window or the WM frame around
     * it, both of which are. Selecting it here anyway covers the case
     * resolve_composited_window()'s multi-hop walk exists for in the
     * first place (a WM that nests frames more than one deep, where
     * `target` is not a root child and root's mask says nothing about
     * it). Preserves the existing mask rather than replacing it --
     * XSelectInput overwrites this client's whole mask on the window, and
     * xispanel holds PropertyChangeMask on client windows for the polling
     * widgets (see ewmh_watch_windows(), which ORs the same way). */
    w->added_structure = 0;
    if (!(wa.your_event_mask & StructureNotifyMask)) {
        XSelectInput(g_dpy, target, wa.your_event_mask | StructureNotifyMask);
        w->added_structure = 1;
    }

    XSetErrorHandler(prev);
    g_n_watches++;
}

void thumb_unwatch_all(void)
{
    if (g_n_watches == 0) {
        return;
    }
    XErrorHandler prev = XSetErrorHandler(thumb_error_handler);
    for (int i = 0; i < g_n_watches; i++) {
        g_thumb_had_error = 0;
        watch_drop_pixmap(&g_watches[i]);
        XDamageDestroy(g_dpy, g_watches[i].damage); /* BadDamage if the window already closed -- ignored, same as everywhere else */
        if (g_watches[i].added_structure) {
            /* Re-read rather than restoring the mask thumb_watch() saw:
             * ewmh_watch_windows() may legitimately have added
             * PropertyChangeMask to this same window in between (it runs
             * on every _NET_CLIENT_LIST change), and writing back the
             * stale mask would silently drop it, leaving the tasklist
             * blind to that window's title changes. Clearing only the one
             * bit we added is correct regardless of what else happened.
             * The round-trip is affordable here -- this runs on tooltip
             * close, not per frame. */
            XWindowAttributes wa;
            g_thumb_had_error = 0;
            if (XGetWindowAttributes(g_dpy, g_watches[i].target, &wa) && !g_thumb_had_error) {
                XSelectInput(g_dpy, g_watches[i].target, wa.your_event_mask & ~StructureNotifyMask);
            }
        }
    }
    XSetErrorHandler(prev);
    g_n_watches = 0;
    g_thumb_dirty = 0;
}

/* Drops the cached composited pixmap of any watch whose `target` this
 * structure event refers to, and keeps its cached geometry/map state
 * current -- see ThumbWatch::pix's doc comment for why the cache is
 * invalidated by event instead of simply not existing.
 *
 * Deliberately does *not* report the event as consumed (thumb_handle_
 * event() returns 0 for these): xispanel.c's dispatch chain is a
 * `else if` ladder, and its own ConfigureNotify branch -- the only signal
 * it has for a window changing output -- comes after this one. Reporting
 * a ConfigureNotify as handled here would silently stop that re-poll for
 * as long as a thumbnail tooltip happens to be open. */
static void note_structure_event(const XEvent *ev)
{
    Window w = None;
    switch (ev->type) {
    case ConfigureNotify:
        w = ev->xconfigure.window;
        break;
    case MapNotify:
        w = ev->xmap.window;
        break;
    case UnmapNotify:
        w = ev->xunmap.window;
        break;
    case DestroyNotify:
        w = ev->xdestroywindow.window;
        break;
    default:
        return;
    }
    for (int i = 0; i < g_n_watches; i++) {
        if (g_watches[i].target != w) {
            continue;
        }
        watch_drop_pixmap(&g_watches[i]);
        if (ev->type == ConfigureNotify) {
            g_watches[i].w = ev->xconfigure.width;
            g_watches[i].h = ev->xconfigure.height;
        } else {
            g_watches[i].mapped = (ev->type == MapNotify);
        }
        /* Repaint on the next tick: the popup is currently showing pixels
         * from a pixmap that just went stale, and for a resize the frame
         * after this one is a different size. */
        g_thumb_dirty = 1;
        break;
    }
}

int thumb_handle_event(const XEvent *ev)
{
    if (g_n_watches > 0) {
        note_structure_event(ev); /* returns 0 below either way -- see its doc comment */
    }
    if (!damage_available() || ev->type != g_damage_event_base + XDamageNotify) {
        return 0;
    }
    const XDamageNotifyEvent *dev = (const XDamageNotifyEvent *)ev;
    /* Acknowledges the event and resets the reported region -- required
     * even for a damage handle we're no longer tracking (a stale event
     * for a just-unwatched window), otherwise the server keeps queuing
     * more for it. */
    XDamageSubtract(g_dpy, dev->damage, None, None);
    for (int i = 0; i < g_n_watches; i++) {
        if (g_watches[i].damage == dev->damage) {
            g_thumb_dirty = 1;
            break;
        }
    }
    return 1;
}

int thumb_take_dirty(void)
{
    if (!g_thumb_dirty) {
        return 0;
    }
    g_thumb_dirty = 0;
    return 1;
}

/* Tries to get a live composited pixmap for exactly `win`. Returns None
 * (leaving *out_wa untouched) on any failure -- BadMatch is common here
 * on a reparenting WM, where the *client* window itself was never
 * individually redirected, only its WM-added frame around it (see
 * thumb_paint()'s fallback below). */
static Pixmap try_name_window_pixmap(Window win, XWindowAttributes *out_wa)
{
    if (!XGetWindowAttributes(g_dpy, win, out_wa) || g_thumb_had_error || out_wa->map_state != IsViewable ||
        out_wa->width <= 0 || out_wa->height <= 0) {
        return None;
    }
    Pixmap pix = XCompositeNameWindowPixmap(g_dpy, win);
    XSync(g_dpy, False);
    if (g_thumb_had_error || pix == None) {
        return None;
    }
    return pix;
}

/* Walks from `win` up through its ancestors (capped at 4 hops) looking
 * for whichever one actually names a live composited pixmap --
 * reparenting WMs (KWin included) often only redirect the frame window
 * they wrap the client in, not the raw client window _NET_CLIENT_LIST
 * reports (see the file comment). Returns None (leaving out_wa/out_pix
 * untouched) if nothing up the chain works. Must be called with the
 * permissive thumb_error_handler already installed (both thumb_paint()
 * and thumb_watch() do this themselves) -- every X call here can fail on
 * an ordinary "window closed mid-query" race, not just the redirect
 * mismatch this is nominally for. */
static Window resolve_composited_window(Window win, XWindowAttributes *out_wa, Pixmap *out_pix)
{
    g_thumb_had_error = 0;
    Pixmap pix = try_name_window_pixmap(win, out_wa);
    if (pix != None) {
        *out_pix = pix;
        return win;
    }

    g_thumb_had_error = 0;
    Window root_ret, parent, *kids = NULL;
    unsigned int n_kids = 0;
    Window probe = win;
    for (int hops = 0; hops < 4; hops++) {
        g_thumb_had_error = 0; /* stale failure from the previous hop's try_name_window_pixmap()
                                 * would otherwise look like *this* hop's XQueryTree failed */
        if (!XQueryTree(g_dpy, probe, &root_ret, &parent, &kids, &n_kids) || g_thumb_had_error) {
            break;
        }
        if (kids) {
            XFree(kids);
            kids = NULL;
        }
        if (parent == None || parent == root_ret) {
            break;
        }
        pix = try_name_window_pixmap(parent, out_wa);
        if (pix != None) {
            *out_pix = pix;
            return parent;
        }
        probe = parent;
    }
    return None;
}

/* Draws `surf` (a `sw` x `sh` composited window surface) scaled to fit
 * inside [x,y,max_w,max_h], preserving aspect ratio, centered, never
 * upscaled. Both the caller's `cr` target and `surf` are X pixmaps, which
 * is what makes this one server-side XRender composite with a scaling
 * transform rather than a full download-scale-upload of the source window
 * -- see TooltipPopup::back_pix's doc comment in tooltip.c. */
static void paint_scaled(cairo_t *cr, cairo_surface_t *surf, int sw, int sh, double x, double y, double max_w,
                          double max_h)
{
    double scale = max_w / sw < max_h / sh ? max_w / sw : max_h / sh;
    if (scale > 1.0) {
        scale = 1.0; /* never upscale a small window past its real size */
    }
    double draw_w = sw * scale;
    double draw_h = sh * scale;
    double dx = x + (max_w - draw_w) / 2.0;
    double dy = y + (max_h - draw_h) / 2.0;

    /* The composited pixmap's contents are rewritten by the X server as
     * the window redraws -- entirely behind cairo's back. Without this,
     * cairo is free to keep serving a representation it built earlier
     * (see the EXTEND_PAD note below for the one it actually builds), and
     * the thumbnail freezes on whichever frame it first drew while still
     * repainting at full rate. Only matters now that the surface is
     * cached across frames; when it was rebuilt per paint the question
     * never arose. */
    cairo_surface_mark_dirty(surf);

    cairo_save(cr);
    cairo_translate(cr, dx, dy);
    cairo_scale(cr, scale, scale);
    cairo_set_source_surface(cr, surf, 0, 0);

    /* EXTEND_PAD, not cairo's default EXTEND_NONE, is what keeps this a
     * genuine zero-copy server-side composite. XRender has no equivalent
     * of EXTEND_NONE, so cairo emulates it by copying the whole source
     * into a fresh pixmap with a 1px transparent border and compositing
     * from *that* -- for a 1080p window that's an 8 MB server-side copy
     * standing between us and the actual draw (and it's the copy cairo
     * was caching, i.e. the freeze above). EXTEND_PAD maps straight onto
     * RENDER's RepeatPad, so the live pixmap is used as the source
     * picture directly. The two modes only differ *outside* the source
     * rectangle, and the cairo_rectangle()/cairo_fill() below covers
     * exactly the source rectangle and nothing more -- cairo_paint()
     * would have filled the whole clip and let PAD smear the edge pixels
     * across it. */
    cairo_pattern_t *pat = cairo_get_source(cr);
    cairo_pattern_set_extend(pat, CAIRO_EXTEND_PAD);
    cairo_pattern_set_filter(pat, CAIRO_FILTER_BILINEAR);
    cairo_rectangle(cr, 0, 0, sw, sh);
    cairo_fill(cr);
    cairo_restore(cr);
}

int thumb_paint(cairo_t *cr, Window win, double x, double y, double max_w, double max_h)
{
    if (!thumb_available()) {
        return 0;
    }

    XErrorHandler prev = XSetErrorHandler(thumb_error_handler);
    g_thumb_had_error = 0;

    XWindowAttributes wa;
    Pixmap pix = None;

    /* Fast path: if `win` is currently being watched, everything this
     * paint needs is already cached -- which ancestor is actually
     * composite-redirected (ThumbWatch::target), that target's live
     * pixmap and cairo surface (ThumbWatch::pix/surf), and its geometry,
     * kept current by note_structure_event(). So the steady-state repaint
     * issues *no X requests of its own at all* beyond the one XRender
     * composite cairo emits: no round-trip, no XSync, no allocation.
     * That's what lets damage-driven repaints (many times a second for
     * video content) keep up -- redoing the full "try the client, fail,
     * walk up to the parent, XSync to confirm" dance from scratch on
     * every frame (the slow path below) was the original bottleneck,
     * capping the real-world repaint rate at a couple of frames a second
     * no matter how fast damage events arrived. Any async X error from a
     * since-closed window is silently absorbed by the process-wide
     * permissive handler once this function's own temporary one is
     * restored below -- same "log and continue" tradeoff already made
     * everywhere else here. */
    for (int i = 0; i < g_n_watches; i++) {
        if (g_watches[i].win != win) {
            continue;
        }
        ThumbWatch *w = &g_watches[i];
        if (!w->mapped || w->w <= 0 || w->h <= 0) {
            /* Minimized/unmapped since the tooltip opened -- there is no
             * live content to show. Same graceful "leave the reserved
             * space blank" outcome the callers already handle. */
            XSetErrorHandler(prev);
            return 0;
        }
        if (w->pix == None) {
            /* First paint after an invalidating structure event (or after
             * a failed attempt) -- name a fresh pixmap for the same
             * target. The XSync is affordable *here* precisely because
             * this is no longer the per-frame path. */
            g_thumb_had_error = 0;
            Pixmap fresh = XCompositeNameWindowPixmap(g_dpy, w->target);
            XSync(g_dpy, False);
            if (g_thumb_had_error || fresh == None) {
                /* Fall through to the slow path rather than giving up:
                 * the cached `target` can genuinely go stale if the WM
                 * re-parents the window while the tooltip is open, and a
                 * full re-resolve finds the new frame. If the window is
                 * simply gone, the slow path fails too and returns 0. */
                break;
            }
            w->pix = fresh;
        }
        if (!w->surf) {
            w->surf = cairo_xlib_surface_create(g_dpy, w->pix, w->visual, w->w, w->h);
            if (cairo_surface_status(w->surf) != CAIRO_STATUS_SUCCESS) {
                watch_drop_pixmap(w);
                break; /* same re-resolve fallback as above */
            }
        }
        paint_scaled(cr, w->surf, w->w, w->h, x, y, max_w, max_h);
        XSetErrorHandler(prev);
        return 1;
    }

    if (pix == None) {
        /* No cache entry (win isn't currently watched -- e.g. the very
         * first frame, painted before show_popup() calls thumb_watch()),
         * or the fast path above didn't even get a pixmap ID back --
         * fall back to the full resolve, a one-off cost in that case. */
        g_thumb_had_error = 0;
        if (resolve_composited_window(win, &wa, &pix) == None) {
            XSetErrorHandler(prev);
            return 0;
        }
    }

    cairo_surface_t *surf = cairo_xlib_surface_create(g_dpy, pix, wa.visual, wa.width, wa.height);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        XFreePixmap(g_dpy, pix);
        XSetErrorHandler(prev);
        return 0;
    }

    paint_scaled(cr, surf, wa.width, wa.height, x, y, max_w, max_h);

    cairo_surface_destroy(surf);
    XFreePixmap(g_dpy, pix);
    XSetErrorHandler(prev);
    return 1;
}
