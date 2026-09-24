/*
 * density.c - X-DENSITY client support (see TESTS/X-DENSITY.md for the
 * full protocol spec): lets a compositor optically zoom into one of
 * xispanel's own panel windows, transiently, without the panel's real
 * on-screen geometry ever changing.
 *
 * Detection is the manager-selection convention the spec describes in
 * its own section 5.2 (same idea as _NET_WM_CM_S<screen>): a compositor
 * that actually supports this protocol holds ownership of
 * _X_DENSITY_MANAGER_S<screen> for as long as (and only as long as) it's
 * ready to consume it. Everything below is gated on that, tracked live
 * via XFixes -- not on the three data atoms merely existing, which per
 * the spec's own section 5.1 just means *some* client interned them once,
 * possibly in a past session, not that anyone is listening right now.
 *
 * Rendering: reuses panel_paint_content() (xispanel.c) -- the same
 * logical-coordinate widget paint() calls that build the normal on-screen
 * buffer, just pointed at a bigger surface with a cairo_scale(density,
 * density) pushed first. Cairo's own vector/text rasterization redraws
 * crisp at any scale this way. One known, deliberate limitation: icons
 * are pre-rendered fixed-size ARGB32 bitmaps (see ewmh_get_icon_surface()/
 * resolve_icon_theme_name()'s target_size), so they scale up as bitmaps
 * rather than gaining any real extra sharpness -- fixing that would mean
 * re-fetching every icon at a density-scaled target size for this render
 * pass alone, a larger change than this first cut makes. Text, borders,
 * hover/active highlights, and the pager's own vector grid all render
 * genuinely sharper.
 */
#include "xispanel.h"

#include <X11/Xatom.h>
#include <X11/extensions/Xfixes.h>
#include <cairo/cairo-xlib.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Atom g_atom_density_manager;
static Atom g_atom_density_requested;
static Atom g_atom_density_scale;
static Atom g_atom_density_pixmap;

static int g_xfixes_event_base;
static int g_xfixes_error_base;
static int g_xfixes_available;
static int g_compositor_present;

/* True once (and only once) some compositor holds the manager selection
 * -- every entry point below bails out immediately when this is false,
 * per the spec's "ignore _X_DENSITY_REQUESTED entirely, don't even read
 * it" rule (section 5.2). */
static int compositor_present(void)
{
    return g_xfixes_available && g_compositor_present;
}

void density_init(void)
{
    char mgr_name[32];
    snprintf(mgr_name, sizeof(mgr_name), "_X_DENSITY_MANAGER_S%d", g_screen);
    g_atom_density_manager = XInternAtom(g_dpy, mgr_name, False);
    g_atom_density_requested = XInternAtom(g_dpy, "_X_DENSITY_REQUESTED", False);
    g_atom_density_scale = XInternAtom(g_dpy, "_X_DENSITY_SCALE", False);
    g_atom_density_pixmap = XInternAtom(g_dpy, "_X_DENSITY_PIXMAP", False);

    g_xfixes_available = XFixesQueryExtension(g_dpy, &g_xfixes_event_base, &g_xfixes_error_base);
    if (!g_xfixes_available) {
        fprintf(stderr, "xispanel: XFixes unavailable -- X-DENSITY support disabled\n");
        return;
    }
    g_compositor_present = XGetSelectionOwner(g_dpy, g_atom_density_manager) != None;
    XFixesSelectSelectionInput(g_dpy, g_root, g_atom_density_manager,
                                XFixesSetSelectionOwnerNotifyMask | XFixesSelectionWindowDestroyNotifyMask |
                                    XFixesSelectionClientCloseNotifyMask);
    if (g_compositor_present) {
        fprintf(stderr, "xispanel: X-DENSITY-compatible compositor detected\n");
    }
}

/* Snaps one panel back to 1/1 and drops its auxiliary pixmap -- the
 * compositor that would have sampled it is gone, so continuing to
 * publish one would leave it visibly frozen/blank instead of just "not
 * specially sharp" (see the spec's section 5.2 decision rules). */
static void reset_one_panel_density(Panel *p, void *ctx)
{
    (void)ctx;
    if (p->density_num != 1 || p->density_den != 1) {
        p->density_num = 1;
        p->density_den = 1;
        XDeleteProperty(g_dpy, p->win, g_atom_density_scale);
        XDeleteProperty(g_dpy, p->win, g_atom_density_pixmap);
    }
    density_panel_destroyed(p);
}

static void density_reset_all_panels(void)
{
    panel_foreach(reset_one_panel_density, NULL);
}

/* Forward-declared: defined in the generic-layer section below, but
 * needed here first -- see that definition's own doc comment. */
static void density_reset_all_layers(void);

int density_handle_xfixes_event(const XEvent *ev)
{
    if (!g_xfixes_available || ev->type != g_xfixes_event_base + XFixesSelectionNotify) {
        return 0;
    }
    int now_present = XGetSelectionOwner(g_dpy, g_atom_density_manager) != None;
    if (now_present != g_compositor_present) {
        g_compositor_present = now_present;
        fprintf(stderr, "xispanel: X-DENSITY compositor %s\n", now_present ? "appeared" : "disappeared");
        if (!now_present) {
            density_reset_all_panels();
            density_reset_all_layers();
        }
    }
    return 1;
}

int density_handle_property(Panel *p, const XPropertyEvent *ev)
{
    if (ev->window != p->win || ev->atom != g_atom_density_requested) {
        return 0;
    }
    if (!compositor_present()) {
        /* Per spec: without a confirmed compositor, ignore the request
         * entirely -- don't even read it. Whoever wrote it either isn't a
         * real compositor, or the selection hasn't propagated to us yet. */
        return 1;
    }
    Atom actual_type;
    int actual_format;
    unsigned long n_items, bytes_after;
    unsigned char *prop = NULL;
    int num = 1, den = 1;
    if (XGetWindowProperty(g_dpy, p->win, g_atom_density_requested, 0, 2, False, XA_CARDINAL, &actual_type,
                            &actual_format, &n_items, &bytes_after, &prop) == Success &&
        prop) {
        if (n_items >= 2) {
            long *v = (long *)(void *)prop;
            if (v[0] > 0 && v[1] > 0) {
                num = (int)v[0];
                den = (int)v[1];
            }
        }
        XFree(prop);
    }
    if (num != p->density_num || den != p->density_den) {
        p->density_num = num;
        p->density_den = den;
        p->dirty = 1; /* next repaint picks up density_render() below with the new factor */
        if (num == 1 && den == 1) {
            /* Withdrawn -- delete what we published *now*, not just stop
             * republishing it. density_render() below no-ops the instant
             * density_num/den read back 1/1 (its "vastly common case, do
             * nothing" early-out), so nothing would otherwise ever delete
             * these or free the pixmap -- the compositor's own density_
             * active() only ever trusts *our* last-published SCALE/PIXMAP,
             * never the mere absence of its own request (kicomp/src/
             * density.c: "num/den -- what the client says it is actually
             * drawing at -- believed over what we asked for"), so a stale
             * SCALE=N/1 left in place keeps it compositing the same frozen
             * pixmap over this window forever, over top of whatever the
             * window keeps drawing normally underneath -- visible as the
             * whole panel appearing to freeze the instant a zoom that once
             * touched it ends, while it keeps responding to input just
             * fine underneath (this generic property write is all the
             * window itself ever needed to look right again; nothing
             * about a panel's own paint() calls was ever the problem). */
            XDeleteProperty(g_dpy, p->win, g_atom_density_scale);
            XDeleteProperty(g_dpy, p->win, g_atom_density_pixmap);
            density_panel_destroyed(p);
        }
    }
    return 1;
}

/* (Re)creates p->density_pixmap/surface/cr (and the CPU-side img_surface/
 * img_cr widgets actually paint into -- see the doc comment on those
 * fields in xispanel.h) at pw x ph if the size (or pixmap itself) doesn't
 * already match -- same "only touch what changed" shape as
 * panel_create_surface()'s own resize path. */
static int density_ensure_pixmap(Panel *p, int pw, int ph)
{
    if (p->density_pixmap != None && p->density_pixmap_w == pw && p->density_pixmap_h == ph) {
        return 1;
    }
    density_panel_destroyed(p);
    p->density_pixmap = XCreatePixmap(g_dpy, p->win, (unsigned)pw, (unsigned)ph, (unsigned)p->depth);
    if (p->density_pixmap == None) {
        return 0;
    }
    p->density_surface = cairo_xlib_surface_create(g_dpy, p->density_pixmap, p->visual, pw, ph);
    p->density_cr = cairo_create(p->density_surface);
    p->density_img_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pw, ph);
    p->density_img_cr = cairo_create(p->density_img_surface);
    if (g_font_face) {
        cairo_set_font_face(p->density_img_cr, g_font_face);
    }
    cairo_set_font_size(p->density_img_cr, panel_text_size(p));
    p->density_pixmap_w = pw;
    p->density_pixmap_h = ph;
    return 1;
}

void density_render(Panel *p)
{
    if (p->density_num == 1 && p->density_den == 1) {
        return; /* the vastly common case -- nothing requested, nothing to do */
    }
    if (!compositor_present()) {
        /* The compositor vanished between the property write and this
         * repaint (density_handle_xfixes_event() already resets on that
         * path normally, but a request could in principle land in the
         * same event-loop iteration as the disappearance) -- don't publish
         * a pixmap nobody will read. */
        return;
    }
    double scale = (double)p->density_num / p->density_den;
    int pw = (int)(p->w * scale + 0.5);
    int ph = (int)(p->h * scale + 0.5);
    if (pw < 1 || ph < 1) {
        return;
    }
    if (!density_ensure_pixmap(p, pw, ph)) {
        return;
    }

    /* Draw off-screen first, one atomic blit onto the actual pixmap after
     * -- see the doc comment on density_img_surface/density_cr in
     * xispanel.h for why (a Pixmap has no auto-Damage, so writing widgets
     * to it directly, one X request per fill/stroke, let a Damage-
     * tracking compositor resample mid-frame -- visible as flicker). */
    panel_paint_content(p, p->density_img_cr, scale);
    cairo_surface_flush(p->density_img_surface);

    /* Content first, announce after (spec section 4) -- avoids the
     * compositor sampling a stale-sized or half-drawn pixmap. */
    cairo_save(p->density_cr);
    cairo_set_operator(p->density_cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_surface(p->density_cr, p->density_img_surface, 0, 0);
    cairo_paint(p->density_cr);
    cairo_restore(p->density_cr);
    cairo_surface_flush(p->density_surface);

    long scale_val[2] = {p->density_num, p->density_den};
    XChangeProperty(g_dpy, p->win, g_atom_density_scale, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)scale_val,
                     2);
    /* Rewriting the same XID every frame is deliberate, not redundant --
     * a Pixmap generates no Damage/Expose of its own when its content
     * changes (it's just memory, not a live surface), so this PropertyNotify
     * is the compositor's only signal to resample, even when the XID
     * itself hasn't changed since last frame (spec section 4). */
    long pixmap_val = (long)p->density_pixmap;
    XChangeProperty(g_dpy, p->win, g_atom_density_pixmap, XA_CARDINAL, 32, PropModeReplace,
                     (unsigned char *)&pixmap_val, 1);
    XFlush(g_dpy);
}

void density_panel_destroyed(Panel *p)
{
    if (p->density_img_cr) {
        cairo_destroy(p->density_img_cr);
        p->density_img_cr = NULL;
    }
    if (p->density_img_surface) {
        cairo_surface_destroy(p->density_img_surface);
        p->density_img_surface = NULL;
    }
    if (p->density_cr) {
        cairo_destroy(p->density_cr);
        p->density_cr = NULL;
    }
    if (p->density_surface) {
        cairo_surface_destroy(p->density_surface);
        p->density_surface = NULL;
    }
    if (p->density_pixmap != None) {
        XFreePixmap(g_dpy, p->density_pixmap);
        p->density_pixmap = None;
    }
    p->density_pixmap_w = 0;
    p->density_pixmap_h = 0;
}

/* ---- X-DENSITY: generic per-window layer (see the doc comment on this
 * struct's forward declaration in xispanel.h) ----
 *
 * Every field mirrors one of Panel's own density_* fields one for one --
 * this is that same pixmap/surface/cr/img_surface/img_cr shape, just
 * addressed through a pointer instead of being spelled out on Panel,
 * so tooltip/toast/menu/launchfx can each have one (or several, for
 * menu's cascaded submenus) without their own copy of density_ensure_
 * pixmap()/density_render()'s bodies. */
struct DensityLayer {
    Window win;
    Visual *visual;
    int depth;
    void (*paint)(cairo_t *cr, double scale, void *ctx);
    void *ctx;

    int num, den;
    int pixmap_w, pixmap_h;
    Pixmap pixmap;
    cairo_surface_t *surface;
    cairo_t *cr;
    cairo_surface_t *img_surface;
    cairo_t *img_cr;

    struct DensityLayer *next;
};

/* Every live layer, so density_reset_all_panels()'s sibling below can
 * snap every one of them back to 1/1 when the compositor disappears --
 * unlike Panel, there's no g_panels[]-style fixed array these windows
 * already live in for that walk to reuse. */
static DensityLayer *g_layers;

DensityLayer *density_layer_register(Window win, Visual *visual, int depth,
                                      void (*paint)(cairo_t *cr, double scale, void *ctx), void *ctx)
{
    DensityLayer *dl = calloc(1, sizeof(*dl));
    if (!dl) {
        return NULL;
    }
    dl->win = win;
    dl->visual = visual;
    dl->depth = depth;
    dl->paint = paint;
    dl->ctx = ctx;
    dl->num = dl->den = 1;
    dl->next = g_layers;
    g_layers = dl;
    return dl;
}

static void density_layer_free_pixmap(DensityLayer *dl)
{
    if (dl->img_cr) {
        cairo_destroy(dl->img_cr);
        dl->img_cr = NULL;
    }
    if (dl->img_surface) {
        cairo_surface_destroy(dl->img_surface);
        dl->img_surface = NULL;
    }
    if (dl->cr) {
        cairo_destroy(dl->cr);
        dl->cr = NULL;
    }
    if (dl->surface) {
        cairo_surface_destroy(dl->surface);
        dl->surface = NULL;
    }
    if (dl->pixmap != None) {
        XFreePixmap(g_dpy, dl->pixmap);
        dl->pixmap = None;
    }
    dl->pixmap_w = dl->pixmap_h = 0;
}

void density_layer_unregister(DensityLayer *dl)
{
    if (!dl) {
        return;
    }
    density_layer_free_pixmap(dl);
    for (DensityLayer **pp = &g_layers; *pp; pp = &(*pp)->next) {
        if (*pp == dl) {
            *pp = dl->next;
            break;
        }
    }
    free(dl);
}

/* Snaps every registered layer back to 1/1 and drops its auxiliary pixmap
 * -- the popup-side equivalent of reset_one_panel_density() above, called
 * alongside it from density_reset_all_panels()'s call site below. */
static void density_reset_all_layers(void)
{
    for (DensityLayer *dl = g_layers; dl; dl = dl->next) {
        if (dl->num != 1 || dl->den != 1) {
            dl->num = dl->den = 1;
            XDeleteProperty(g_dpy, dl->win, g_atom_density_scale);
            XDeleteProperty(g_dpy, dl->win, g_atom_density_pixmap);
        }
        density_layer_free_pixmap(dl);
    }
}

int density_layer_handle_property(DensityLayer *dl, const XPropertyEvent *ev)
{
    if (!dl || ev->window != dl->win || ev->atom != g_atom_density_requested) {
        return 0;
    }
    if (!compositor_present()) {
        return 1;
    }
    Atom actual_type;
    int actual_format;
    unsigned long n_items, bytes_after;
    unsigned char *prop = NULL;
    int num = 1, den = 1;
    if (XGetWindowProperty(g_dpy, dl->win, g_atom_density_requested, 0, 2, False, XA_CARDINAL, &actual_type,
                            &actual_format, &n_items, &bytes_after, &prop) == Success &&
        prop) {
        if (n_items >= 2) {
            long *v = (long *)(void *)prop;
            if (v[0] > 0 && v[1] > 0) {
                num = (int)v[0];
                den = (int)v[1];
            }
        }
        XFree(prop);
    }
    dl->num = num;
    dl->den = den;
    if (num == 1 && den == 1) {
        /* Withdrawn -- delete what we published now, same reasoning as
         * density_handle_property(Panel*)'s own doc comment above: without
         * this, the compositor keeps compositing our last dense pixmap
         * over the window forever, since it trusts our last-published
         * SCALE/PIXMAP over the mere absence of its own request. */
        XDeleteProperty(g_dpy, dl->win, g_atom_density_scale);
        XDeleteProperty(g_dpy, dl->win, g_atom_density_pixmap);
        density_layer_free_pixmap(dl);
    }
    return 1;
}

static int density_layer_ensure_pixmap(DensityLayer *dl, int pw, int ph)
{
    if (dl->pixmap != None && dl->pixmap_w == pw && dl->pixmap_h == ph) {
        return 1;
    }
    density_layer_free_pixmap(dl);
    dl->pixmap = XCreatePixmap(g_dpy, dl->win, (unsigned)pw, (unsigned)ph, (unsigned)dl->depth);
    if (dl->pixmap == None) {
        return 0;
    }
    dl->surface = cairo_xlib_surface_create(g_dpy, dl->pixmap, dl->visual, pw, ph);
    dl->cr = cairo_create(dl->surface);
    dl->img_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pw, ph);
    dl->img_cr = cairo_create(dl->img_surface);
    dl->pixmap_w = pw;
    dl->pixmap_h = ph;
    return 1;
}

void density_layer_render(DensityLayer *dl, int logical_w, int logical_h, double base_font_size)
{
    if (!dl || (dl->num == 1 && dl->den == 1)) {
        return;
    }
    if (!compositor_present()) {
        return;
    }
    double scale = (double)dl->num / dl->den;
    int pw = (int)(logical_w * scale + 0.5);
    int ph = (int)(logical_h * scale + 0.5);
    if (pw < 1 || ph < 1) {
        return;
    }
    if (!density_layer_ensure_pixmap(dl, pw, ph)) {
        return;
    }

    if (g_font_face) {
        cairo_set_font_face(dl->img_cr, g_font_face);
    }
    if (base_font_size > 0) {
        cairo_set_font_size(dl->img_cr, base_font_size);
    }
    dl->paint(dl->img_cr, scale, dl->ctx);
    cairo_surface_flush(dl->img_surface);

    cairo_save(dl->cr);
    cairo_set_operator(dl->cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_surface(dl->cr, dl->img_surface, 0, 0);
    cairo_paint(dl->cr);
    cairo_restore(dl->cr);
    cairo_surface_flush(dl->surface);

    long scale_val[2] = {dl->num, dl->den};
    XChangeProperty(g_dpy, dl->win, g_atom_density_scale, XA_CARDINAL, 32, PropModeReplace,
                     (unsigned char *)scale_val, 2);
    long pixmap_val = (long)dl->pixmap;
    XChangeProperty(g_dpy, dl->win, g_atom_density_pixmap, XA_CARDINAL, 32, PropModeReplace,
                     (unsigned char *)&pixmap_val, 1);
    XFlush(g_dpy);
}
