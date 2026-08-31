/* XRender renderer backend (section 28, "first objective: a simple
 * renderer; OpenGL later").
 *
 * What it does, and nothing more: for each output, paint the root
 * background into that output's own pixmap, then composite every scene
 * node over it, bottom to top, with PictOpOver. Because the source
 * pictures carry the window's real visual format, a depth-32 window's
 * alpha channel is honoured -- which is the one visible difference
 * between this and an uncomposited screen, and the acceptance criterion
 * for this prototype.
 *
 * Everything here is deliberately whole-output: no region-based repaint,
 * no per-node clipping beyond the composite rectangle itself. Damage only
 * decides *which outputs* repaint (section 39); narrowing that to regions
 * is a later optimization that shouldn't change any of the interfaces.
 */
#include "renderer.h"
#include "output.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const CompRenderer *renderer;

typedef struct {
    xcb_pixmap_t pixmap;
} XrOutput;

static xcb_render_query_pict_formats_reply_t *formats;

/* Root background, rebuilt lazily whenever _XROOTPMAP_ID changes. */
static xcb_render_picture_t bg_picture;
static bool bg_checked;

static void formats_load(void)
{
    if (formats)
        return;
    formats = xcb_render_query_pict_formats_reply(comp.conn,
        xcb_render_query_pict_formats(comp.conn), NULL);
}

/* Visual -> picture format. Doing this by hand keeps libxcb-render-util
 * out of the dependency list for the sake of one lookup. */
static xcb_render_pictformat_t format_for_visual(xcb_visualid_t visual)
{
    formats_load();
    if (!formats)
        return 0;

    xcb_render_pictscreen_iterator_t si =
        xcb_render_query_pict_formats_screens_iterator(formats);
    for (; si.rem; xcb_render_pictscreen_next(&si)) {
        xcb_render_pictdepth_iterator_t di =
            xcb_render_pictscreen_depths_iterator(si.data);
        for (; di.rem; xcb_render_pictdepth_next(&di)) {
            xcb_render_pictvisual_t *vis = xcb_render_pictdepth_visuals(di.data);
            int n = xcb_render_pictdepth_visuals_length(di.data);
            for (int i = 0; i < n; i++)
                if (vis[i].visual == visual)
                    return vis[i].format;
        }
    }
    return 0;
}

xcb_render_pictformat_t comp_root_pictformat(void)
{
    return format_for_visual(comp.screen->root_visual);
}

/* The standard A8 format, used for the constant-alpha mask that backs
 * _NET_WM_WINDOW_OPACITY. */
static xcb_render_pictformat_t format_a8(void)
{
    formats_load();
    if (!formats)
        return 0;

    xcb_render_pictforminfo_iterator_t it =
        xcb_render_query_pict_formats_formats_iterator(formats);
    for (; it.rem; xcb_render_pictforminfo_next(&it)) {
        xcb_render_pictforminfo_t *f = it.data;
        if (f->type == XCB_RENDER_PICT_TYPE_DIRECT && f->depth == 8 &&
            f->direct.alpha_mask == 0xff && f->direct.red_mask == 0)
            return f->id;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* per-window resources                                                */
/* ------------------------------------------------------------------ */

void renderer_window_shape_invalidate(CompWindow *w)
{
    if (!w->shape)
        return;
    xcb_xfixes_destroy_region(comp.conn, w->shape);
    w->shape = 0;
}

void renderer_window_invalidate(CompWindow *w)
{
    renderer_window_shape_invalidate(w);
    if (w->picture) {
        xcb_render_free_picture(comp.conn, w->picture);
        w->picture = 0;
    }
    if (w->pixmap) {
        xcb_free_pixmap(comp.conn, w->pixmap);
        w->pixmap = 0;
    }
    if (w->alpha) {
        xcb_render_free_picture(comp.conn, w->alpha);
        w->alpha = 0;
    }
}

void renderer_window_free(CompWindow *w)
{
    renderer_window_invalidate(w);
}

/* Binds the window's current contents. Checked, because the window can be
 * unmapped or destroyed between the event that made us want it and this
 * request -- an async BadMatch/BadWindow here would be reported as a
 * spurious error, and worse, would leave a dangling pixmap id. */
static bool window_bind(CompWindow *w)
{
    if (w->picture)
        return true;
    if (!w->mapped || w->input_only)
        return false;

    xcb_pixmap_t pm = xcb_generate_id(comp.conn);
    xcb_generic_error_t *err = xcb_request_check(comp.conn,
        xcb_composite_name_window_pixmap_checked(comp.conn, w->id, pm));
    if (err) {
        free(err);
        return false;
    }

    xcb_render_pictformat_t fmt = format_for_visual(w->visual);
    if (!fmt) {
        xcb_free_pixmap(comp.conn, pm);
        return false;
    }

    xcb_render_picture_t pict = xcb_generate_id(comp.conn);
    /* IncludeInferiors on a *pixmap* picture is a no-op, but the named
     * pixmap of a parent already contains its inferiors' output -- which
     * is exactly why compositing kiwm's frame window is enough to show
     * the reparented client and its Cairo decoration in one go. */
    xcb_render_create_picture(comp.conn, pict, pm, fmt, 0, NULL);

    w->pixmap = pm;
    w->picture = pict;
    return true;
}

/* The window's bounding shape, cached until the window is resized or
 * reshaped (see CompWindow::shape). Window-relative, so a move doesn't
 * invalidate it -- the destination origin is passed at clip time instead.
 * XCB_NONE means "no clipping", which is also the honest answer for a
 * window that just went away between the event and this request. */
static xcb_xfixes_region_t window_shape(CompWindow *w)
{
    if (w->shape)
        return w->shape;
    if (!comp.caps.xfixes)
        return XCB_NONE;

    xcb_xfixes_region_t reg = xcb_generate_id(comp.conn);
    xcb_generic_error_t *err = xcb_request_check(comp.conn,
        xcb_xfixes_create_region_from_window_checked(comp.conn, reg, w->id,
                                                     XCB_SHAPE_SK_BOUNDING));
    if (err) {
        free(err);
        return XCB_NONE;
    }

    w->shape = reg;
    return reg;
}

/* 1x1 repeating A8 picture holding the window's constant opacity. */
static xcb_render_picture_t window_alpha(CompWindow *w)
{
    if (w->opacity >= 1.0)
        return XCB_NONE;
    if (w->alpha)
        return w->alpha;

    xcb_render_pictformat_t fmt = format_a8();
    if (!fmt)
        return XCB_NONE;

    xcb_pixmap_t pm = xcb_generate_id(comp.conn);
    xcb_create_pixmap(comp.conn, 8, pm, comp.root, 1, 1);

    xcb_render_picture_t pict = xcb_generate_id(comp.conn);
    uint32_t repeat = XCB_RENDER_REPEAT_NORMAL;
    xcb_render_create_picture(comp.conn, pict, pm, fmt,
                              XCB_RENDER_CP_REPEAT, &repeat);

    xcb_render_color_t c = {
        .red = 0, .green = 0, .blue = 0,
        .alpha = (uint16_t)(w->opacity * 0xffff)
    };
    xcb_rectangle_t r = { 0, 0, 1, 1 };
    xcb_render_fill_rectangles(comp.conn, XCB_RENDER_PICT_OP_SRC, pict, c, 1, &r);

    xcb_free_pixmap(comp.conn, pm);   /* the picture keeps it alive */
    w->alpha = pict;
    return pict;
}

/* ------------------------------------------------------------------ */
/* background                                                          */
/* ------------------------------------------------------------------ */

void renderer_background_invalidate(void)
{
    if (bg_picture) {
        xcb_render_free_picture(comp.conn, bg_picture);
        bg_picture = 0;
    }
    bg_checked = false;
}

/* Wallpaper set the traditional way (_XROOTPMAP_ID / ESETROOT_PMAP_ID on
 * the root window). xisback instead owns real desktop-type windows, which
 * come through as ordinary scene nodes and need none of this -- but
 * anything set with xsetroot/feh/hsetroot would otherwise vanish behind
 * the overlay, so it's worth the two properties. */
static xcb_render_picture_t background_picture(void)
{
    if (bg_checked)
        return bg_picture;
    bg_checked = true;

    xcb_atom_t props[2] = { comp.atoms.xrootpmap_id, comp.atoms.esetroot_pmap_id };
    xcb_pixmap_t pm = XCB_NONE;

    for (int i = 0; i < 2 && pm == XCB_NONE; i++) {
        if (props[i] == XCB_NONE)
            continue;
        xcb_get_property_reply_t *r = xcb_get_property_reply(comp.conn,
            xcb_get_property(comp.conn, 0, comp.root, props[i],
                             XCB_ATOM_PIXMAP, 0, 1), NULL);
        if (!r)
            continue;
        if (r->type == XCB_ATOM_PIXMAP && r->format == 32 &&
            xcb_get_property_value_length(r) >= 4)
            pm = *(xcb_pixmap_t *)xcb_get_property_value(r);
        free(r);
    }

    if (pm == XCB_NONE)
        return 0;

    xcb_render_pictformat_t fmt = format_for_visual(comp.screen->root_visual);
    if (!fmt)
        return 0;

    xcb_render_picture_t pict = xcb_generate_id(comp.conn);
    uint32_t repeat = XCB_RENDER_REPEAT_NORMAL;
    xcb_generic_error_t *err = xcb_request_check(comp.conn,
        xcb_render_create_picture_checked(comp.conn, pict, pm, fmt,
                                          XCB_RENDER_CP_REPEAT, &repeat));
    if (err) {
        /* Stale property pointing at a freed pixmap -- common after a
         * wallpaper setter exits. Fall back to the flat fill. */
        free(err);
        return 0;
    }

    bg_picture = pict;
    return bg_picture;
}

/* ------------------------------------------------------------------ */
/* renderer ops                                                        */
/* ------------------------------------------------------------------ */

static bool xr_init(CompOutput *o)
{
    if (o->rect.w <= 0 || o->rect.h <= 0)
        return false;

    XrOutput *xo = calloc(1, sizeof(*xo));
    if (!xo)
        return false;

    /* One drawable per output, sized to that output -- never one big
     * surface spanning every monitor (section 18). */
    xo->pixmap = xcb_generate_id(comp.conn);
    xcb_create_pixmap(comp.conn, comp.screen->root_depth, xo->pixmap, comp.root,
                      (uint16_t)o->rect.w, (uint16_t)o->rect.h);

    xcb_render_pictformat_t fmt = format_for_visual(comp.screen->root_visual);
    if (!fmt) {
        xcb_free_pixmap(comp.conn, xo->pixmap);
        free(xo);
        return false;
    }

    o->target = xcb_generate_id(comp.conn);
    xcb_render_create_picture(comp.conn, o->target, xo->pixmap, fmt, 0, NULL);

    o->render_data = xo;
    return true;
}

static void xr_destroy(CompOutput *o)
{
    XrOutput *xo = o->render_data;
    if (o->target) {
        xcb_render_free_picture(comp.conn, o->target);
        o->target = 0;
    }
    if (xo) {
        if (xo->pixmap)
            xcb_free_pixmap(comp.conn, xo->pixmap);
        free(xo);
        o->render_data = NULL;
    }
}

static void xr_begin(CompOutput *o)
{
    if (!o->target)
        return;

    /* The background covers the whole output: drop whatever clip the last
     * frame's final window left behind. */
    if (comp.caps.xfixes)
        xcb_xfixes_set_picture_clip_region(comp.conn, o->target,
                                           XCB_XFIXES_REGION_NONE, 0, 0);

    xcb_render_picture_t bg = background_picture();
    if (bg) {
        /* Source coordinates are root-relative so a tiled wallpaper lines
         * up across outputs exactly as it does uncomposited. */
        xcb_render_composite(comp.conn, XCB_RENDER_PICT_OP_SRC, bg, XCB_NONE,
                             o->target,
                             (int16_t)o->rect.x, (int16_t)o->rect.y, 0, 0, 0, 0,
                             (uint16_t)o->rect.w, (uint16_t)o->rect.h);
        return;
    }

    xcb_render_color_t c = { 0x1c1c, 0x1c1c, 0x1c1c, 0xffff };
    xcb_rectangle_t r = { 0, 0, (uint16_t)o->rect.w, (uint16_t)o->rect.h };
    xcb_render_fill_rectangles(comp.conn, XCB_RENDER_PICT_OP_SRC, o->target,
                               c, 1, &r);
}

static void xr_draw_scene(CompOutput *o, CompScene *s)
{
    if (!o->target)
        return;

    for (int i = 0; i < s->count; i++) {
        CompSceneNode *n = &s->nodes[i];
        CompWindow *w = n->win;

        if (!window_bind(w)) {
            /* The window has no usable contents this frame, so it's
             * simply missing from the output -- worth saying, because
             * that is exactly what a one-frame "the window vanished"
             * glitch looks like from the outside. */
            comp_log("window 0x%x has no pixmap this frame", w->id);
            continue;
        }

        xcb_render_picture_t mask = window_alpha(w);

        /* Clip the target to this window's shape before drawing it. The
         * region is window-relative, so the window's origin in target
         * coordinates goes in as the clip origin -- note that's the
         * window origin, not the pixmap's: the bounding shape is measured
         * from the former and reaches into the border area with negative
         * coordinates when there is one. This is what keeps kiwm's
         * rounded corners round, and a shaped client (a client's own
         * SHAPE, forwarded onto the frame by kiwm) shaped. */
        if (comp.caps.xfixes) {
            xcb_xfixes_region_t shape = window_shape(w);
            xcb_xfixes_set_picture_clip_region(comp.conn, o->target,
                                               shape ? shape : XCB_XFIXES_REGION_NONE,
                                               (int16_t)(w->x - o->rect.x),
                                               (int16_t)(w->y - o->rect.y));
        }

        /* Source offset: where inside the window's own pixmap the visible
         * portion starts. Destination offset: the same point relative to
         * this output's origin. That pair is the whole of "a window can
         * cross outputs" for a non-transformed scene. */
        int16_t sx = (int16_t)(n->visible_rect.x - n->geometry.x);
        int16_t sy = (int16_t)(n->visible_rect.y - n->geometry.y);
        int16_t dx = (int16_t)(n->visible_rect.x - o->rect.x);
        int16_t dy = (int16_t)(n->visible_rect.y - o->rect.y);

        /* OVER, always: for a depth-24 window the source has no alpha
         * channel, XRender reads it as opaque, and the result is
         * identical to a plain copy. For a depth-32 window this is
         * precisely the blending an uncomposited server can't do. */
        xcb_render_composite(comp.conn, XCB_RENDER_PICT_OP_OVER,
                             w->picture, mask, o->target,
                             sx, sy, sx, sy, dx, dy,
                             (uint16_t)n->visible_rect.w,
                             (uint16_t)n->visible_rect.h);
    }
}

static void xr_end(CompOutput *o)
{
    /* The presenter composites the whole target onto the overlay next, so
     * the last window's clip must not still be in force. */
    if (o->target && comp.caps.xfixes)
        xcb_xfixes_set_picture_clip_region(comp.conn, o->target,
                                           XCB_XFIXES_REGION_NONE, 0, 0);
}

void renderer_shutdown(void)
{
    renderer_background_invalidate();
    if (formats) {
        free(formats);
        formats = NULL;
    }
}

static const CompRenderer xrender_renderer = {
    .name       = "xrender",
    .init       = xr_init,
    .destroy    = xr_destroy,
    .begin      = xr_begin,
    .draw_scene = xr_draw_scene,
    .end        = xr_end,
};

const CompRenderer *renderer_xrender(void)
{
    return &xrender_renderer;
}
