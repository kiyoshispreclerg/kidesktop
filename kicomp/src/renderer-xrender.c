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
#include "window.h"
#include "shadow.h"
#include "text.h"
#include "region.h"
#include "density.h"

#include <math.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* Depth -> picture format, for a drawable that has no visual of its own:
 * a Pixmap. The standard format for the depth is what every client that
 * draws into one will have used. */
static xcb_render_pictformat_t format_for_depth(uint8_t depth)
{
    formats_load();
    if (!formats)
        return 0;

    xcb_render_pictforminfo_iterator_t fi =
        xcb_render_query_pict_formats_formats_iterator(formats);
    for (; fi.rem; xcb_render_pictforminfo_next(&fi)) {
        xcb_render_pictforminfo_t *f = fi.data;
        if (f->type != XCB_RENDER_PICT_TYPE_DIRECT || f->depth != depth)
            continue;
        /* 32-bit wants the alpha channel; 24-bit must not pretend to have
         * one. Both are the first DIRECT format of their depth with the
         * matching alpha mask. */
        if (depth == 32 && f->direct.alpha_mask == 0)
            continue;
        if (depth != 32 && f->direct.alpha_mask != 0)
            continue;
        return f->id;
    }
    return 0;
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

static void stash_free(CompWindow *w);

static void xr_window_shape_invalidate(CompWindow *w)
{
    w->shaped = false;
    if (w->shape_mask) {
        xcb_render_free_picture(comp.conn, w->shape_mask);
        w->shape_mask = 0;
    }
    if (!w->shape)
        return;
    xcb_xfixes_destroy_region(comp.conn, w->shape);
    w->shape = 0;
}

static void xr_window_invalidate(CompWindow *w)
{
    xr_window_shape_invalidate(w);
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

static void xr_window_free(CompWindow *w)
{
    xr_window_invalidate(w);
    stash_free(w);
}

static bool xr_window_has_content(const CompWindow *w)
{
    return w->picture != 0;
}

/* ---- the stash: contents a resize replaced (renderer.h) ---- */

static void stash_free(CompWindow *w)
{
    if (w->prev_picture) {
        xcb_render_free_picture(comp.conn, w->prev_picture);
        w->prev_picture = 0;
    }
    if (w->prev_pixmap) {
        xcb_free_pixmap(comp.conn, w->prev_pixmap);
        w->prev_pixmap = 0;
    }
    w->prev_holds = 0;
}

static void xr_window_stash(CompWindow *w, const CompRect *was)
{
    if (!w->picture) {
        /* Nothing bound to stash. Whatever was there is still the most
         * recent thing this window ever looked like, so leave it. */
        return;
    }

    /* One stash at a time: a second resize while an effect is drawing the
     * first would leave two, and the effect wants the newest anyway. */
    stash_free(w);

    w->prev_pixmap = w->pixmap;
    w->prev_picture = w->picture;
    /* The rectangle those pixels covered -- the *old* one. The window's
     * own geometry has already been updated to the new size by the time
     * this runs, which is exactly the trap: stashing window_rect(w) here
     * records the size the contents are not. */
    w->prev_rect = *was;
    w->prev_holds = 0;

    w->pixmap = 0;
    w->picture = 0;

    /* The alpha mask and shape belong to the size that just changed. */
    if (w->alpha) {
        xcb_render_free_picture(comp.conn, w->alpha);
        w->alpha = 0;
    }
    xr_window_shape_invalidate(w);
}

static bool xr_window_has_stash(const CompWindow *w)
{
    return w->prev_picture != 0;
}

static CompRect xr_window_stash_rect(const CompWindow *w)
{
    return w->prev_rect;
}

static void xr_stash_hold(CompWindow *w)
{
    w->prev_holds++;
}

static void xr_stash_release(CompWindow *w)
{
    if (w->prev_holds > 0)
        w->prev_holds--;
    if (w->prev_holds == 0)
        stash_free(w);
}

static void xr_stash_drop_unheld(CompWindow *w)
{
    if (w->prev_holds == 0)
        stash_free(w);
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
/* ------------------------------------------------------------------ */
/* X-DENSITY: the client's own denser contents                         */
/* ------------------------------------------------------------------ */

static void xr_window_density_invalidate(CompWindow *w, bool decoration)
{
    xcb_render_picture_t *pict = decoration ? &w->deco_density_picture
                                            : &w->density_picture;
    if (*pict) {
        xcb_render_free_picture(comp.conn, *pict);
        *pict = 0;
    }
}

/* A Picture for the window's density pixmap, and the client's rectangle
 * inside the frame -- that pixmap holds the client's content alone, with
 * no decoration around it, so where the decoration ends has to be known
 * to put it back in the right place.
 *
 * Both are cached until the client publishes a different pixmap or the
 * window is resized; the round trip is paid once per density change, not
 * per frame. */
static xcb_render_picture_t density_picture(CompWindow *w, bool decoration)
{
    xcb_render_picture_t *cached = decoration ? &w->deco_density_picture
                                              : &w->density_picture;
    xcb_pixmap_t *pixmap = decoration ? &w->deco_density_pixmap
                                      : &w->density_pixmap;

    if (*cached)
        return *cached;
    if (*pixmap == 0)
        return 0;
    if (!decoration && w->client == XCB_NONE)
        return 0;

    /* The client's geometry comes along for the contents: that pixmap
     * holds the client's own drawing with no decoration around it, so
     * where the decoration ends has to be known to put it back in the
     * right place. The decoration's pixmap covers the whole frame and
     * needs no such offset. */
    xcb_get_geometry_cookie_t cc = { 0 };
    if (!decoration)
        cc = xcb_get_geometry(comp.conn, w->client);
    xcb_get_geometry_cookie_t pc = xcb_get_geometry(comp.conn, *pixmap);

    xcb_get_geometry_reply_t *cg = decoration ? NULL
                                  : xcb_get_geometry_reply(comp.conn, cc, NULL);
    xcb_get_geometry_reply_t *pg = xcb_get_geometry_reply(comp.conn, pc, NULL);

    if (!pg || (!decoration && !cg)) {
        /* The publisher died, or named a pixmap it had already freed. Not
         * an error worth shouting about -- the window simply draws from
         * its own contents this frame. */
        free(cg);
        free(pg);
        *pixmap = 0;
        return 0;
    }

    if (cg) {
        w->client_rect.x = cg->x + cg->border_width;
        w->client_rect.y = cg->y + cg->border_width;
        w->client_rect.w = cg->width;
        w->client_rect.h = cg->height;
    }

    uint8_t depth = pg->depth;
    free(cg);
    free(pg);

    xcb_render_pictformat_t fmt = format_for_depth(depth);
    if (!fmt)
        return 0;

    xcb_render_picture_t pict = xcb_generate_id(comp.conn);
    xcb_render_create_picture(comp.conn, pict, *pixmap, fmt, 0, NULL);

    *cached = pict;
    return pict;
}

/* ------------------------------------------------------------------ */
/* logical -> physical                                                 */
/* ------------------------------------------------------------------ */

/* Everything above the renderer works in the output's *logical*
 * coordinates (comp.h): the scene, the effects, the damage, the WM. The
 * target pixmap is the size of the output's *physical* scanout. These
 * three turn one into the other, and they are the only place in this file
 * that knows an output can be scaled -- at scale 1 they are a subtraction
 * and an identity, which is exactly what the code did before scaling
 * existed. */
/* The output's lens as a plain magnification and origin (comp.h's view).
 * It is always a scale about a point -- that is all zoom asks for -- so
 * reading it back as one is exact rather than an approximation, and it
 * lets the whole backend keep speaking in "root coordinates to target
 * pixels" instead of growing a second, parallel notion of where things
 * are.
 *
 * Which is the point: fold the lens in *here*, and shadows, shape clips,
 * the density layers and the damage rectangles all follow it without
 * knowing it exists, because every one of them is written in terms of
 * these three functions. */
static float lens_of(const CompOutput *o, float *ox, float *oy)
{
    float k = o->view.m[0][0];
    if (k <= 0.0f)
        k = 1.0f;
    *ox = o->view.m[0][3];
    *oy = o->view.m[1][3];
    return k;
}

static int to_target_x(const CompOutput *o, int x)
{
    float ox, oy;
    float k = lens_of(o, &ox, &oy);
    float lensed = (float)x * k + ox;
    return (int)((lensed - (float)o->rect.x) * o->scale + 0.5f);
}

static int to_target_y(const CompOutput *o, int y)
{
    float ox, oy;
    float k = lens_of(o, &ox, &oy);
    float lensed = (float)y * k + oy;
    return (int)((lensed - (float)o->rect.y) * o->scale + 0.5f);
}

static int to_target_len(const CompOutput *o, int v)
{
    float ox, oy;
    float k = lens_of(o, &ox, &oy);
    int r = (int)((float)v * k * o->scale + 0.5f);
    return r > 0 ? r : (v > 0 ? 1 : 0);
}

/* Target pixels back to logical root coordinates: undo the physical
 * scale, put the output's origin back, then undo the lens. Built here
 * because three different draws need exactly this chain -- the windows,
 * the dense layers and the wallpaper -- and a lens missing from any one
 * of them is that piece drawn at the wrong magnification. */
static void target_to_root(const CompOutput *o, CompTransform *m)
{
    float ox, oy;
    float k = lens_of(o, &ox, &oy);

    comp_transform_identity(m);
    if (o->scale != 1.0f)
        comp_transform_scale(m, 1.0f / o->scale, 1.0f / o->scale);
    comp_transform_translate(m, (float)o->rect.x, (float)o->rect.y);

    if (k != 1.0f || ox != 0.0f || oy != 0.0f) {
        comp_transform_translate(m, -ox, -oy);
        comp_transform_scale(m, 1.0f / k, 1.0f / k);
    }
}

/* A rectangle in logical root coordinates, as target pixels. */
static xcb_rectangle_t to_target_rect(const CompOutput *o, const CompRect *r)
{
    xcb_rectangle_t out = {
        (int16_t)to_target_x(o, r->x),
        (int16_t)to_target_y(o, r->y),
        (uint16_t)to_target_len(o, r->w),
        (uint16_t)to_target_len(o, r->h),
    };
    return out;
}

/* A server-side region, multiplied by this output's scale. XFixes has no
 * scale operator, so the rectangles are fetched, multiplied here and a new
 * region built from them -- one round trip, and only when the shape (or
 * the scale) actually changed, never per frame.
 *
 * Returns XCB_NONE when there is nothing to scale, which the callers read
 * as "use the region as it is". */
static xcb_xfixes_region_t region_scaled(const CompOutput *o,
                                         xcb_xfixes_region_t region,
                                         int offset_x, int offset_y)
{
    if (region == XCB_NONE || !comp.caps.xfixes)
        return XCB_NONE;

    xcb_xfixes_fetch_region_reply_t *r = xcb_xfixes_fetch_region_reply(comp.conn,
        xcb_xfixes_fetch_region(comp.conn, region), NULL);
    if (!r)
        return XCB_NONE;

    int count = xcb_xfixes_fetch_region_rectangles_length(r);
    xcb_rectangle_t *rects = xcb_xfixes_fetch_region_rectangles(r);
    if (count <= 0) {
        free(r);
        return XCB_NONE;
    }

    xcb_rectangle_t *scaled = malloc(sizeof(*scaled) * (size_t)count);
    if (!scaled) {
        free(r);
        return XCB_NONE;
    }

    for (int i = 0; i < count; i++) {
        /* The region arrives in its own space (window-relative for a
         * shape); the offset puts it in logical root coordinates first. */
        CompRect rc = {
            rects[i].x + offset_x, rects[i].y + offset_y,
            rects[i].width, rects[i].height
        };
        scaled[i] = to_target_rect(o, &rc);
    }

    xcb_xfixes_region_t out = xcb_generate_id(comp.conn);
    xcb_xfixes_create_region(comp.conn, out, (uint32_t)count, scaled);

    free(scaled);
    free(r);
    return out;
}

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

    /* While a round trip is being paid for anyway: where that shape
     * actually is. An unshaped window reports its whole rectangle, which
     * is the same answer its geometry would have given. */
    w->shaped = false;
    xcb_shape_query_extents_reply_t *ext = xcb_shape_query_extents_reply(comp.conn,
        xcb_shape_query_extents(comp.conn, w->id), NULL);
    if (ext) {
        w->shaped = ext->bounding_shaped;
        w->shape_extents.x = ext->bounding_shape_extents_x;
        w->shape_extents.y = ext->bounding_shape_extents_y;
        w->shape_extents.w = ext->bounding_shape_extents_width;
        w->shape_extents.h = ext->bounding_shape_extents_height;
        free(ext);
    }

    w->shape = reg;
    return reg;
}

/* 1x1 repeating A8 picture holding one constant alpha value, used as the
 * mask for the whole window. The value is the *node's* opacity, not the
 * window's: _NET_WM_WINDOW_OPACITY sets a baseline, and a fade effect
 * scales it per frame. The picture is created once and refilled when the
 * value changes -- one request per frame during a fade, instead of
 * building and tearing down a picture sixty times a second. */
static xcb_render_picture_t window_alpha(CompWindow *w, float opacity)
{
    if (opacity >= 1.0f)
        return XCB_NONE;
    if (opacity < 0.0f)
        opacity = 0.0f;

    if (w->alpha) {
        if (w->alpha_value != opacity) {
            xcb_render_color_t c = {
                .red = 0, .green = 0, .blue = 0,
                .alpha = (uint16_t)(opacity * 0xffff)
            };
            xcb_rectangle_t r = { 0, 0, 1, 1 };
            xcb_render_fill_rectangles(comp.conn, XCB_RENDER_PICT_OP_SRC,
                                       w->alpha, c, 1, &r);
            w->alpha_value = opacity;
        }
        return w->alpha;
    }

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
        .alpha = (uint16_t)(opacity * 0xffff)
    };
    xcb_rectangle_t r = { 0, 0, 1, 1 };
    xcb_render_fill_rectangles(comp.conn, XCB_RENDER_PICT_OP_SRC, pict, c, 1, &r);

    xcb_free_pixmap(comp.conn, pm);   /* the picture keeps it alive */
    w->alpha = pict;
    w->alpha_value = opacity;
    return pict;
}

/* The window's shape as a mask picture (CompWindow::shape_mask): the
 * alternative to clipping with the region, for the nodes the region
 * cannot follow -- a window being scaled by an effect, or drawn on an
 * output whose pixels are not logical pixels. The mask lives in the same
 * space as the window's pixmap, so whatever matrix samples the pixmap
 * samples the mask, and the silhouette scales with the contents.
 *
 * It carries the opacity as its value, which is what makes it *the*
 * mask rather than one of two: XRender takes a single mask picture, and
 * the constant-alpha one (window_alpha) cannot be combined with this in
 * the same composite. Refilled only when the value changes -- a fade
 * costs two fills per frame, a steady window none.
 *
 * XCB_NONE when the window is not shaped, or the mask cannot be made:
 * the caller then draws the rectangle, as it always did. */
static xcb_render_picture_t window_shape_mask(CompWindow *w, float opacity)
{
    xcb_xfixes_region_t shape = window_shape(w);
    if (shape == XCB_NONE || !w->shaped)
        return XCB_NONE;
    if (opacity < 0.0f)
        opacity = 0.0f;
    if (opacity > 1.0f)
        opacity = 1.0f;

    CompRect r = window_rect(w);
    if (r.w <= 0 || r.h <= 0)
        return XCB_NONE;

    if (!w->shape_mask) {
        xcb_render_pictformat_t fmt = format_a8();
        if (!fmt)
            return XCB_NONE;

        xcb_pixmap_t pm = xcb_generate_id(comp.conn);
        xcb_create_pixmap(comp.conn, 8, pm, comp.root,
                          (uint16_t)r.w, (uint16_t)r.h);
        xcb_render_picture_t pict = xcb_generate_id(comp.conn);
        xcb_render_create_picture(comp.conn, pict, pm, fmt, 0, NULL);
        xcb_free_pixmap(comp.conn, pm);   /* the picture keeps it alive */

        w->shape_mask = pict;
        w->shape_mask_alpha = -1.0f;
    }

    if (w->shape_mask_alpha != opacity) {
        xcb_rectangle_t whole = { 0, 0, (uint16_t)r.w, (uint16_t)r.h };
        xcb_render_color_t clear = { 0, 0, 0, 0 };
        xcb_render_color_t fill = {
            0, 0, 0, (uint16_t)(opacity * 0xffff)
        };

        /* Nothing outside the shape, the value inside it. The region is
         * window-relative and the pixmap starts at the border, so the
         * clip is offset by the border to line the two up. */
        xcb_xfixes_set_picture_clip_region(comp.conn, w->shape_mask,
                                           XCB_XFIXES_REGION_NONE, 0, 0);
        xcb_render_fill_rectangles(comp.conn, XCB_RENDER_PICT_OP_SRC,
                                   w->shape_mask, clear, 1, &whole);
        xcb_xfixes_set_picture_clip_region(comp.conn, w->shape_mask, shape,
                                           (int16_t)w->border, (int16_t)w->border);
        xcb_render_fill_rectangles(comp.conn, XCB_RENDER_PICT_OP_SRC,
                                   w->shape_mask, fill, 1, &whole);
        xcb_xfixes_set_picture_clip_region(comp.conn, w->shape_mask,
                                           XCB_XFIXES_REGION_NONE, 0, 0);
        w->shape_mask_alpha = opacity;
    }

    return w->shape_mask;
}

/* ------------------------------------------------------------------ */
/* background                                                          */
/* ------------------------------------------------------------------ */

static void xr_background_invalidate(void)
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
     * surface spanning every monitor (section 18).
     *
     * Sized to the *physical* scanout, not to the logical desktop: a
     * scaled output draws its logical scene magnified into real pixels, so
     * that a client which redrew itself densely (X-DENSITY) lands sharp
     * instead of being resampled through a smaller intermediate. At scale
     * 1 the two are the same rectangle. */
    xo->pixmap = xcb_generate_id(comp.conn);
    xcb_create_pixmap(comp.conn, comp.screen->root_depth, xo->pixmap, comp.root,
                      (uint16_t)o->physical.w, (uint16_t)o->physical.h);

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


/* ------------------------------------------------------------------ */
/* shadows (shadow.h)                                                  */
/* ------------------------------------------------------------------ */

/* Everything a shadow of one radius is made of. The blur of a rectangle
 * is separable, so the whole thing reduces to one 1-D edge profile:
 * corners are the profile times itself, edges are the profile repeated
 * along the side, and the middle is solid. Built once per radius. */
typedef struct {
    int radius;
    bool valid;
    /* corner[0..3] = top-left, top-right, bottom-left, bottom-right */
    xcb_render_picture_t corner[4];
    /* edge[0..3] = top, bottom, left, right (1px wide/tall, repeating) */
    xcb_render_picture_t edge[4];
} ShadowTiles;

/* Two sets is enough in practice: focused and unfocused radii. */
static ShadowTiles shadow_tiles[2];

/* Solid colour to shade through the masks, one cached at a time. */
static xcb_render_picture_t shadow_color_pict;
static float shadow_color_key[4] = { -1, -1, -1, -1 };

static void shadow_tiles_free(ShadowTiles *t)
{
    for (int i = 0; i < 4; i++) {
        if (t->corner[i]) xcb_render_free_picture(comp.conn, t->corner[i]);
        if (t->edge[i])   xcb_render_free_picture(comp.conn, t->edge[i]);
    }
    memset(t, 0, sizeof(*t));
}

/* An A8 picture from raw alpha, `repeat` for the strips that tile along
 * an edge. X wants each scanline padded to four bytes. */
static xcb_render_picture_t alpha_picture(const uint8_t *alpha, int w, int h, bool repeat)
{
    xcb_render_pictformat_t fmt = format_a8();
    if (!fmt || w <= 0 || h <= 0)
        return XCB_NONE;

    int stride = (w + 3) & ~3;
    uint8_t *padded = calloc((size_t)stride * (size_t)h, 1);
    if (!padded)
        return XCB_NONE;
    for (int y = 0; y < h; y++)
        memcpy(padded + (size_t)y * stride, alpha + (size_t)y * w, (size_t)w);

    xcb_pixmap_t pm = xcb_generate_id(comp.conn);
    xcb_create_pixmap(comp.conn, 8, pm, comp.root, (uint16_t)w, (uint16_t)h);

    xcb_gcontext_t gc = xcb_generate_id(comp.conn);
    xcb_create_gc(comp.conn, gc, pm, 0, NULL);
    xcb_put_image(comp.conn, XCB_IMAGE_FORMAT_Z_PIXMAP, pm, gc,
                  (uint16_t)w, (uint16_t)h, 0, 0, 0, 8,
                  (uint32_t)(stride * h), padded);
    xcb_free_gc(comp.conn, gc);
    free(padded);

    xcb_render_picture_t pict = xcb_generate_id(comp.conn);
    uint32_t rep = XCB_RENDER_REPEAT_NORMAL;
    xcb_render_create_picture(comp.conn, pict, pm, fmt,
                              repeat ? XCB_RENDER_CP_REPEAT : 0,
                              repeat ? &rep : NULL);
    xcb_free_pixmap(comp.conn, pm);
    return pict;
}


static ShadowTiles *shadow_tiles_for(int radius)
{
    for (int i = 0; i < 2; i++)
        if (shadow_tiles[i].valid && shadow_tiles[i].radius == radius)
            return &shadow_tiles[i];

    /* Take the free slot, or evict the second one -- with at most two
     * radii in play (focused, unfocused) this never thrashes. */
    ShadowTiles *t = shadow_tiles[0].valid ? &shadow_tiles[1] : &shadow_tiles[0];
    shadow_tiles_free(t);

    int n = radius * 2;
    uint8_t *profile = malloc((size_t)n);
    uint8_t *buf = malloc((size_t)n * (size_t)n);
    if (!profile || !buf) {
        free(profile);
        free(buf);
        return NULL;
    }
    shadow_profile(radius, profile);

    /* Corners: the profile in both axes at once. */
    for (int corner = 0; corner < 4; corner++) {
        bool flip_x = (corner == 1 || corner == 3);
        bool flip_y = (corner == 2 || corner == 3);
        for (int y = 0; y < n; y++) {
            for (int x = 0; x < n; x++) {
                uint8_t px = profile[flip_x ? n - 1 - x : x];
                uint8_t py = profile[flip_y ? n - 1 - y : y];
                buf[y * n + x] = (uint8_t)((px * py + 127) / 255);
            }
        }
        t->corner[corner] = alpha_picture(buf, n, n, false);
    }

    /* Edges: one pixel across the side, the profile along the blur,
     * repeating down (or across) the side. The blur of a straight edge is
     * the same everywhere along it, so one pixel of it is the whole
     * description -- and RepeatNormal turns that into as long an edge as
     * any window needs, at no cost per window. */
    for (int y = 0; y < n; y++)
        buf[y] = profile[y];
    t->edge[0] = alpha_picture(buf, 1, n, true);          /* top */
    for (int y = 0; y < n; y++)
        buf[y] = profile[n - 1 - y];
    t->edge[1] = alpha_picture(buf, 1, n, true);          /* bottom */
    for (int x = 0; x < n; x++)
        buf[x] = profile[x];
    t->edge[2] = alpha_picture(buf, n, 1, true);          /* left */
    for (int x = 0; x < n; x++)
        buf[x] = profile[n - 1 - x];
    t->edge[3] = alpha_picture(buf, n, 1, true);          /* right */

    free(profile);
    free(buf);

    t->radius = radius;
    t->valid = true;
    comp_log("shadow tiles built for radius %d", radius);
    return t;
}

static xcb_render_picture_t shadow_color(const CompShadowStyle *st)
{
    if (shadow_color_pict &&
        shadow_color_key[0] == st->r && shadow_color_key[1] == st->g &&
        shadow_color_key[2] == st->b && shadow_color_key[3] == st->opacity)
        return shadow_color_pict;

    if (shadow_color_pict) {
        xcb_render_free_picture(comp.conn, shadow_color_pict);
        shadow_color_pict = 0;
    }

    /* Premultiplied, like every colour XRender takes. */
    xcb_render_color_t c = {
        .red   = (uint16_t)(st->r * st->opacity * 0xffff),
        .green = (uint16_t)(st->g * st->opacity * 0xffff),
        .blue  = (uint16_t)(st->b * st->opacity * 0xffff),
        .alpha = (uint16_t)(st->opacity * 0xffff),
    };

    /* A solid fill picture is infinite in extent and needs no pixmap --
     * RENDER 0.10 and up, which is everything this compositor already
     * requires. */
    xcb_render_picture_t pict = xcb_generate_id(comp.conn);
    xcb_render_create_solid_fill(comp.conn, pict, c);

    shadow_color_pict = pict;
    shadow_color_key[0] = st->r;
    shadow_color_key[1] = st->g;
    shadow_color_key[2] = st->b;
    shadow_color_key[3] = st->opacity;
    return pict;
}

/* One masked rectangle of shadow. `mask_x/y` say which part of the mask
 * to start from -- which matters when a piece is drawn narrower than its
 * tile, as the corners are on a window smaller than twice the blur. */
/* Declared here because the shadow needs them and they are defined with
 * the drawing code further down. */
static void picture_transform_set(xcb_render_picture_t pict, const CompTransform *m);
static void picture_transform_reset(xcb_render_picture_t pict);

static void shadow_piece(CompOutput *o, xcb_render_picture_t color,
                         xcb_render_picture_t mask, int mask_x, int mask_y,
                         int x, int y, int w, int h, float mag)
{
    if (w <= 0 || h <= 0 || !mask)
        return;

    /* x/y/w/h arrive in target pixels already: draw_shadow does the
     * logical-to-physical conversion once, up front, because the tiles it
     * picks depend on the *physical* blur radius.
     *
     * `mag` is the lens, and the tile is *resampled* through it rather
     * than rebuilt at a magnified radius. A zoom magnifies the picture
     * of the desktop, and a shadow is part of that picture: stretching
     * the blur is what the screen would look like held closer, and it
     * costs one transform instead of regenerating the whole gradient
     * every time the magnification changes -- which, while a lens is
     * moving, is every frame. */
    if (mag != 1.0f) {
        CompTransform t;
        comp_transform_identity(&t);
        comp_transform_scale(&t, 1.0f / mag, 1.0f / mag);
        picture_transform_set(mask, &t);

        /* The offsets are added before the transform, so they are in the
         * magnified space too. */
        mask_x = (int)((float)mask_x * mag + 0.5f);
        mask_y = (int)((float)mask_y * mag + 0.5f);
    }

    xcb_render_composite(comp.conn, XCB_RENDER_PICT_OP_OVER, color, mask, o->target,
                         0, 0, (int16_t)mask_x, (int16_t)mask_y,
                         (int16_t)x, (int16_t)y,
                         (uint16_t)w, (uint16_t)h);

    if (mag != 1.0f)
        picture_transform_reset(mask);
}

/* Clips the target to a region intersected with this frame's damage --
 * defined with the rest of the frame's clip handling, below. */
static void clip_to_frame(CompOutput *o, xcb_xfixes_region_t extra,
                          int16_t ox, int16_t oy);

/* Draws `w`'s shadow into `o`'s target, under the window itself.
 *
 * The window's own rectangle is clipped out: an opaque window would cover
 * its shadow anyway, but a translucent one would have it show through,
 * and a shadow visible *through* the window it belongs to is the thing
 * that always looks wrong. */
static void draw_shadow(CompOutput *o, CompWindow *win, const CompRect *geom,
                        float opacity)
{
    CompShadowStyle st;
    if (!shadow_for_window(win, &st))
        return;

    /* The node's opacity, not just the style's: a window an effect is
     * fading has to take its shadow with it. Left out, a window
     * dissolving on a monitor it is leaving -- show-windows fading the
     * strip that hangs over the boundary, a fade-out that outlives its
     * window -- goes transparent and leaves a solid dark rectangle
     * sitting on the desktop where it used to be. */
    st.opacity *= opacity;
    if (st.opacity <= 0.0f)
        return;

    /* Asking for the shape here also fills in its extents, which the
     * rectangle below needs. */
    xcb_xfixes_region_t shape = window_shape(win);

    /* A shaped window casts its shadow around what can actually be seen
     * of it, not around its rectangle. For almost every window the two
     * are the same; for the few where they aren't, the difference is the
     * whole story -- VirtualBox's mini-toolbar is a screen-wide window
     * with a small bar shaped out of it, and shadowing its rectangle
     * drops a full-screen shadow behind the desktop. */
    CompRect base = *geom;
    if (win->shaped && win->shape_extents.w > 0 && win->shape_extents.h > 0) {
        CompRect ext = { win->x + win->shape_extents.x,
                         win->y + win->shape_extents.y,
                         win->shape_extents.w,
                         win->shape_extents.h };

        /* Never *bigger* than the window, though, and that clamp is not
         * pedantry: a shape belongs to the size the window had when it
         * was set, and during a resize the two disagree for a frame. The
         * WM configures the frame and reshapes it as two requests; a fast
         * drag has us painting in between, with the geometry already the
         * new size and the server's shape still the old one.
         *
         * A shadow drawn from that stale, larger extent reaches outside
         * the window rectangle -- and every rectangle any of the thirty
         * damage call sites posts is a *window* rectangle grown by
         * shadow_margin(). So the next frame repaints the window's own
         * surroundings and leaves the oversized ring stranded, one band
         * per step of the drag, on the right and below because a shape's
         * origin stays at 0,0 while only its width and height lag.
         *
         * Clamping keeps what this is actually for -- an extent *smaller*
         * than the rectangle, VirtualBox's screen-wide mini-toolbar with
         * a small bar carved out of it -- and drops the case that can
         * only be a lie. A stale shape then costs one frame of a slightly
         * too-square shadow instead of a trail that stays until something
         * else happens to repaint that ground. */
        if (!rect_intersect(&ext, geom, &base))
            base = *geom;
    }

    /* The blur is a number of *physical* pixels: a 14 px shadow on a 2x
     * output is 28 real pixels of gradient, not a 14 px gradient stretched
     * to 28. The tile cache is keyed by radius, so a scaled output simply
     * builds its own set once. */
    float lox, loy;
    float lens = lens_of(o, &lox, &loy);

    /* The tiles are built for the *unlensed* radius and stretched by the
     * lens when they are drawn (shadow_piece). Two reasons, and the
     * second is the important one: a magnified blur is what a magnified
     * screen should show, and the tile cache is keyed by radius -- so a
     * lens that moves would otherwise ask for a different radius every
     * frame and rebuild the entire gradient at each one. */
    int r = (o->scale != 1.0f) ? (int)((float)st.radius * o->scale + 0.5f)
                               : st.radius;
    if (r < 1)
        r = 1;

    /* What the blur measures on screen once the lens has been through
     * it. Everything below is laid out in these. */
    int rd = (int)((float)r * lens + 0.5f);
    if (rd < 1)
        rd = 1;

    ShadowTiles *t = shadow_tiles_for(r);
    if (!t)
        return;

    xcb_render_picture_t color = shadow_color(&st);
    if (!color)
        return;

    /* Everything below is in target pixels. */
    CompRect visible;
    if (!rect_intersect(geom, &o->rect, &visible))
        visible = *geom;   /* off this output: the clip below drops it */

    int off_x = (int)((float)st.offset_x * o->scale * lens + 0.5f);
    int off_y = (int)((float)st.offset_y * o->scale * lens + 0.5f);

    /* The shadow's own rectangle: the window's, offset, grown by the
     * blur on every side. */
    CompRect s = {
        to_target_x(o, base.x) + off_x - rd,
        to_target_y(o, base.y) + off_y - rd,
        to_target_len(o, base.w) + rd * 2,
        to_target_len(o, base.h) + rd * 2,
    };

    if (comp.caps.xfixes) {
        xcb_rectangle_t whole = {
            (int16_t)s.x, (int16_t)s.y,
            (uint16_t)s.w, (uint16_t)s.h
        };
        xcb_xfixes_region_t region = xcb_generate_id(comp.conn);
        xcb_xfixes_region_t cut = xcb_generate_id(comp.conn);
        xcb_xfixes_create_region(comp.conn, region, 1, &whole);

        /* What to cut out of the shadow: the window's *shape*, not its
         * rectangle. With a rectangle, a window with rounded corners keeps
         * a little notch of missing shadow at each corner -- the area
         * inside the rectangle but outside the window, which the clip
         * removed and nothing draws over. Cutting the real silhouette
         * instead lets the shadow reach into the corners.
         *
         * It costs three asynchronous requests more than the rectangle
         * (copy, translate, subtract) and no round trip: the region itself
         * is the one already cached for clipping the window (window_shape),
         * in window coordinates, so it only has to be moved into the
         * target's. */
        xcb_xfixes_region_t scaled_shape = XCB_NONE;
        if (shape && (o->scale != 1.0f || lens != 1.0f)) {
            /* Same reason as in clip_to_frame: the cached region is in
             * logical window coordinates and XFixes cannot scale it. */
            scaled_shape = region_scaled(o, shape, win->x, win->y);
            shape = scaled_shape;
        }

        if (shape) {
            xcb_xfixes_create_region(comp.conn, cut, 0, NULL);
            xcb_xfixes_copy_region(comp.conn, shape, cut);
            if (scaled_shape == XCB_NONE)
                xcb_xfixes_translate_region(comp.conn, cut,
                                            (int16_t)to_target_x(o, win->x),
                                            (int16_t)to_target_y(o, win->y));
        } else {
            xcb_rectangle_t hole = {
                (int16_t)to_target_x(o, base.x), (int16_t)to_target_y(o, base.y),
                (uint16_t)to_target_len(o, base.w),
                (uint16_t)to_target_len(o, base.h)
            };
            xcb_xfixes_create_region(comp.conn, cut, 1, &hole);
        }

        /* (source1, source2, destination): the shadow's rectangle minus
         * the window itself, and then minus whatever this frame is not
         * repainting. */
        xcb_xfixes_subtract_region(comp.conn, region, cut, region);
        clip_to_frame(o, region, 0, 0);
        xcb_xfixes_destroy_region(comp.conn, cut);
        xcb_xfixes_destroy_region(comp.conn, region);
        if (scaled_shape != XCB_NONE)
            xcb_xfixes_destroy_region(comp.conn, scaled_shape);
    }

    int n = r * 2;          /* the fade, in tile pixels */
    int nd = rd * 2;        /* the same fade, on screen */

    /* Corner size, halved when the shadow is narrower or shorter than two
     * corners: without this the pieces overlap in the middle and the
     * overlap is composited twice, which shows as a darker patch on any
     * window smaller than twice the blur radius -- a tooltip, a menu.
     * A clipped corner is drawn from the matching part of its tile, hence
     * the mask offsets. */
    int cw = nd, ch = nd;
    if (cw * 2 > s.w) cw = s.w / 2;
    if (ch * 2 > s.h) ch = s.h / 2;

    int inner_w = s.w - cw * 2;
    int inner_h = s.h - ch * 2;

    /* The same two measurements in *tile* pixels, because that is the
     * space the mask offsets are in: shadow_piece magnifies them by the
     * lens on its way to the server. Without this the offsets would be
     * screen distances applied to a tile that was never that big, which
     * is a corner sampled from past the end of its own gradient -- a
     * flat dark band instead of a fade. */
    int cwt = (int)((float)cw / lens + 0.5f);
    int cht = (int)((float)ch / lens + 0.5f);
    if (cwt > n) cwt = n;
    if (cht > n) cht = n;

    /* Four corners, four edges, one middle -- the whole shadow, at any
     * window size, from tiles that only ever depended on the radius. */
    shadow_piece(o, color, t->corner[0], 0, 0,
                 s.x, s.y, cw, ch, lens);
    shadow_piece(o, color, t->corner[1], n - cwt, 0,
                 s.x + s.w - cw, s.y, cw, ch, lens);
    shadow_piece(o, color, t->corner[2], 0, n - cht,
                 s.x, s.y + s.h - ch, cw, ch, lens);
    shadow_piece(o, color, t->corner[3], n - cwt, n - cht,
                 s.x + s.w - cw, s.y + s.h - ch, cw, ch, lens);

    shadow_piece(o, color, t->edge[0], 0, 0,
                 s.x + cw, s.y, inner_w, ch, lens);
    shadow_piece(o, color, t->edge[1], 0, n - cht,
                 s.x + cw, s.y + s.h - ch, inner_w, ch, lens);
    shadow_piece(o, color, t->edge[2], 0, 0,
                 s.x, s.y + ch, cw, inner_h, lens);
    shadow_piece(o, color, t->edge[3], n - cwt, 0,
                 s.x + s.w - cw, s.y + ch, cw, inner_h, lens);

    if (inner_w > 0 && inner_h > 0)
        xcb_render_composite(comp.conn, XCB_RENDER_PICT_OP_OVER, color, XCB_NONE,
                             o->target, 0, 0, 0, 0,
                             (int16_t)(s.x + cw - o->rect.x),
                             (int16_t)(s.y + ch - o->rect.y),
                             (uint16_t)inner_w, (uint16_t)inner_h);

    if (comp.caps.xfixes)
        clip_to_frame(o, XCB_NONE, 0, 0);
}

static void shadow_shutdown(void)
{
    for (int i = 0; i < 2; i++)
        shadow_tiles_free(&shadow_tiles[i]);
    if (shadow_color_pict) {
        xcb_render_free_picture(comp.conn, shadow_color_pict);
        shadow_color_pict = 0;
    }
    shadow_color_key[0] = -1;
}

/* Defined with the drawing code below; the background needs them too. */
static void picture_transform_set(xcb_render_picture_t pict, const CompTransform *m);
static void picture_transform_reset(xcb_render_picture_t pict);

/* ------------------------------------------------------------------ */
/* the frame's damage clip                                             */
/* ------------------------------------------------------------------ */

/* The part of the output being repainted this frame, as an XFixes region
 * in *target* coordinates, and whether it is simply everything. Built
 * once per frame in xr_begin and destroyed in xr_end: every draw in
 * between is clipped to it, which is what makes a partial repaint partial
 * rather than merely well-intentioned.
 *
 * This is the backend's translation of the core's client-side region
 * (region.h). The core never learns that XFixes exists; a GL backend
 * would turn the same rectangles into scissor boxes. */
static xcb_xfixes_region_t frame_clip;
static bool frame_clip_full;

/* A core region as an XFixes one, in the target's coordinates: the
 * pixmap starts at the output's origin and is in physical pixels, the
 * region arrived in logical root ones. XCB_NONE for a region with no
 * rectangles in it -- a full one has none, so the caller has to have
 * asked about that first. */
static xcb_xfixes_region_t region_to_xfixes(CompOutput *o, const CompRegion *r)
{
    xcb_rectangle_t rects[COMP_REGION_MAX];
    int n = 0;

    for (int i = 0; i < r->count && n < COMP_REGION_MAX; i++) {
        const CompRect *d = &r->rects[i];
        if (d->w <= 0 || d->h <= 0)
            continue;
        rects[n] = to_target_rect(o, d);
        n++;
    }

    if (n == 0)
        return XCB_NONE;

    xcb_xfixes_region_t region = xcb_generate_id(comp.conn);
    xcb_xfixes_create_region(comp.conn, region, (uint32_t)n, rects);
    return region;
}

static void frame_clip_build(CompOutput *o, const CompRegion *damage)
{
    frame_clip = XCB_NONE;
    frame_clip_full = true;

    if (!comp.caps.xfixes || region_is_full(damage) || damage->count <= 0)
        return;

    frame_clip = region_to_xfixes(o, damage);
    frame_clip_full = frame_clip == XCB_NONE;
}

/* The frame's clip narrowed to one node: the damage ∩ the node's own
 * clip (scene.h), which is where the node can be seen at all. Swapped
 * in for the frame clip while the node is drawn, so every clip_to_frame
 * below it intersects with this instead, and swapped back out after --
 * the node's clip is smaller than the frame's, never larger, so nothing
 * the frame did not damage is touched. */
static xcb_xfixes_region_t frame_clip_saved;
static bool frame_clip_saved_full;

static void frame_clip_narrow(CompOutput *o, const CompRegion *clip)
{
    frame_clip_saved = frame_clip;
    frame_clip_saved_full = frame_clip_full;

    if (!comp.caps.xfixes || region_is_full(clip))
        return;

    xcb_xfixes_region_t own = region_to_xfixes(o, clip);
    if (own == XCB_NONE)
        return;

    if (!frame_clip_full)
        xcb_xfixes_intersect_region(comp.conn, own, frame_clip, own);

    frame_clip = own;
    frame_clip_full = false;
}

static void frame_clip_widen(void)
{
    if (frame_clip != frame_clip_saved && frame_clip != XCB_NONE)
        xcb_xfixes_destroy_region(comp.conn, frame_clip);
    frame_clip = frame_clip_saved;
    frame_clip_full = frame_clip_saved_full;
}

static void frame_clip_destroy(void)
{
    if (frame_clip != XCB_NONE)
        xcb_xfixes_destroy_region(comp.conn, frame_clip);
    frame_clip = XCB_NONE;
    frame_clip_full = true;
}

/* Sets the target's clip to `extra` (a region in some other coordinate
 * system, offset by ox/oy -- a window's shape, a shadow's outline)
 * *intersected with* this frame's damage.
 *
 * The intersection is the point: XFixes has one clip region per picture,
 * so a backend that sets a window's shape as the clip has just thrown the
 * damage clip away and would repaint that whole window. Combining them
 * costs a copy, a translate and an intersect -- three asynchronous
 * requests, no round trip, the same shape of work draw_shadow already
 * does. Pass XCB_NONE for `extra` to clip to the damage alone. */
static void clip_to_frame(CompOutput *o, xcb_xfixes_region_t extra,
                          int16_t ox, int16_t oy)
{
    if (!comp.caps.xfixes)
        return;

    if (extra == XCB_NONE) {
        xcb_xfixes_set_picture_clip_region(comp.conn, o->target,
            frame_clip_full ? XCB_XFIXES_REGION_NONE : frame_clip, 0, 0);
        return;
    }

    /* A scaled output's target is in physical pixels while `extra` -- a
     * window's shape, a shadow's outline -- is in logical ones. XFixes
     * cannot scale a region, so it is rebuilt at the right size, and only
     * here: at scale 1 (every output today, and most of them always) not
     * a single extra request is sent. */
    float lox, loy;
    bool lensed = lens_of(o, &lox, &loy) != 1.0f || lox != 0.0f || loy != 0.0f;

    xcb_xfixes_region_t owned = XCB_NONE;
    if (o->scale != 1.0f || lensed) {
        owned = region_scaled(o, extra, ox + o->rect.x, oy + o->rect.y);
        if (owned == XCB_NONE) {
            /* Nothing to clip with: better a square corner for one frame
             * than a window clipped to the wrong silhouette. */
            xcb_xfixes_set_picture_clip_region(comp.conn, o->target,
                frame_clip_full ? XCB_XFIXES_REGION_NONE : frame_clip, 0, 0);
            return;
        }
        extra = owned;
        ox = oy = 0;
    }

    if (frame_clip_full) {
        xcb_xfixes_set_picture_clip_region(comp.conn, o->target, extra, ox, oy);
        if (owned != XCB_NONE)
            xcb_xfixes_destroy_region(comp.conn, owned);
        return;
    }

    xcb_xfixes_region_t both = xcb_generate_id(comp.conn);
    xcb_xfixes_create_region(comp.conn, both, 0, NULL);
    xcb_xfixes_copy_region(comp.conn, extra, both);
    xcb_xfixes_translate_region(comp.conn, both, ox, oy);
    xcb_xfixes_intersect_region(comp.conn, both, frame_clip, both);
    xcb_xfixes_set_picture_clip_region(comp.conn, o->target, both, 0, 0);
    xcb_xfixes_destroy_region(comp.conn, both);

    if (owned != XCB_NONE)
        xcb_xfixes_destroy_region(comp.conn, owned);
}

static void xr_begin(CompOutput *o, const CompRegion *damage)
{
    if (!o->target)
        return;

    frame_clip_build(o, damage);

    /* The background is painted through the damage clip, so an untouched
     * part of the output keeps last frame's pixels -- which is the whole
     * of "partial repaint" for a target that persists between frames.
     *
     * And *minus* wherever an opaque window is about to be drawn
     * (CompOutput's covered): those pixels are replaced before anyone
     * sees them, so painting wallpaper there first is a full pass of the
     * output's memory for nothing -- one of the passes that made a video
     * behind a frame cost more than the video. At scale 1 only: the
     * covers are logical and XFixes cannot scale a region; the scaled
     * output pays what it paid before. */
    bool bg_narrowed = false;
    if (comp.caps.xfixes && o->covered.count > 0 && o->scale == 1.0f &&
        comp_transform_is_identity(&o->view)) {
        CompRegion under = *damage;
        region_intersect_rect(&under, &o->rect);
        for (int i = 0; i < o->covered.count; i++)
            region_subtract_rect(&under, &o->covered.rects[i]);
        if (region_is_empty(&under)) {
            /* Every damaged pixel is under something opaque: no
             * background at all this frame. */
            return;
        }
        xcb_xfixes_region_t bg_clip = region_to_xfixes(o, &under);
        if (bg_clip != XCB_NONE) {
            xcb_xfixes_set_picture_clip_region(comp.conn, o->target, bg_clip, 0, 0);
            xcb_xfixes_destroy_region(comp.conn, bg_clip);
            bg_narrowed = true;
        }
    }
    if (!bg_narrowed)
        clip_to_frame(o, XCB_NONE, 0, 0);

    xcb_render_picture_t bg = background_picture();
    if (bg) {
        /* Source coordinates are root-relative so a tiled wallpaper lines
         * up across outputs exactly as it does uncomposited. On a scaled
         * output the wallpaper is magnified with everything else, through
         * a transform that maps target pixels back to root ones -- the
         * same mapping every window goes through below. */
        bool lensed = !comp_transform_is_identity(&o->view);

        if (o->scale != 1.0f || lensed) {
            /* The wallpaper is part of what is being magnified: a zoom
             * that leaves the desktop behind the windows at its own size
             * is a zoom of the windows only. Same chain as everything
             * else, because it is the same question. */
            CompTransform m;
            target_to_root(o, &m);
            picture_transform_set(bg, &m);

            xcb_render_composite(comp.conn, XCB_RENDER_PICT_OP_SRC, bg, XCB_NONE,
                                 o->target, 0, 0, 0, 0, 0, 0,
                                 (uint16_t)o->physical.w, (uint16_t)o->physical.h);
            picture_transform_reset(bg);
            return;
        }

        xcb_render_composite(comp.conn, XCB_RENDER_PICT_OP_SRC, bg, XCB_NONE,
                             o->target,
                             (int16_t)o->rect.x, (int16_t)o->rect.y, 0, 0, 0, 0,
                             (uint16_t)o->physical.w, (uint16_t)o->physical.h);
        return;
    }

    xcb_render_color_t c = { 0x1c1c, 0x1c1c, 0x1c1c, 0xffff };
    xcb_rectangle_t r = { 0, 0, (uint16_t)o->physical.w, (uint16_t)o->physical.h };
    xcb_render_fill_rectangles(comp.conn, XCB_RENDER_PICT_OP_SRC, o->target,
                               c, 1, &r);
}

/* XRender's picture transform maps *destination* coordinates back to
 * source ones, so what goes in is the inverse of the node's transform,
 * with the window's own origin subtracted -- the composite below then
 * hands it destination points in root coordinates and gets pixmap points
 * out. Fixed point 16.16, and the third row is always the affine one
 * (see comp_transform_invert_affine). */
static void picture_transform_set(xcb_render_picture_t pict, const CompTransform *m)
{
    xcb_render_transform_t t = {
        .matrix11 = (xcb_render_fixed_t)(m->m[0][0] * 65536.0f),
        .matrix12 = (xcb_render_fixed_t)(m->m[0][1] * 65536.0f),
        .matrix13 = (xcb_render_fixed_t)(m->m[0][3] * 65536.0f),
        .matrix21 = (xcb_render_fixed_t)(m->m[1][0] * 65536.0f),
        .matrix22 = (xcb_render_fixed_t)(m->m[1][1] * 65536.0f),
        .matrix23 = (xcb_render_fixed_t)(m->m[1][3] * 65536.0f),
        .matrix31 = 0,
        .matrix32 = 0,
        .matrix33 = 65536,
    };
    xcb_render_set_picture_transform(comp.conn, pict, t);
    /* Bilinear while scaled: nearest-neighbour turns a resize animation
     * into a shimmering staircase. */
    xcb_render_set_picture_filter(comp.conn, pict, 8, "bilinear", 0, NULL);
}

static void picture_transform_reset(xcb_render_picture_t pict)
{
    static const xcb_render_transform_t identity = {
        65536, 0, 0,
        0, 65536, 0,
        0, 0, 65536,
    };
    xcb_render_set_picture_transform(comp.conn, pict, identity);
    xcb_render_set_picture_filter(comp.conn, pict, 7, "nearest", 0, NULL);
}

/* One layer of X-DENSITY pixels, composited over the window that has
 * already been drawn: `area` is the logical rectangle those pixels cover
 * (the whole frame for the decoration, the client's rectangle inside it
 * for the contents), and `density` is how many of them there are per
 * logical pixel.
 *
 * OVER, not SRC: the decoration's pixmap is transparent everywhere the WM
 * didn't paint, which is exactly the hole the client's own contents show
 * through.
 */
static void draw_dense(CompOutput *o, const CompSceneNode *n, CompWindow *w,
                       xcb_render_picture_t mask, const CompRect *area,
                       float density, bool decoration)
{
    xcb_render_picture_t dense = density_picture(w, decoration);
    if (!dense)
        return;

    CompRect visible;
    if (!rect_intersect(area, &n->visible_rect, &visible))
        return;

    /* target -> logical root -> this layer's own origin -> its pixmap,
     * which is that area times the density. When the density matches the
     * output scale the whole chain is the identity and the pixels go
     * across one for one, which is the entire point of it. */
    CompTransform m;
    target_to_root(o, &m);
    comp_transform_translate(&m, (float)-area->x, (float)-area->y);
    comp_transform_scale(&m, density, density);
    picture_transform_set(dense, &m);

    int16_t dx = (int16_t)to_target_x(o, visible.x);
    int16_t dy = (int16_t)to_target_y(o, visible.y);

    xcb_render_composite(comp.conn, XCB_RENDER_PICT_OP_OVER,
                         dense, mask, o->target,
                         dx, dy, dx, dy, dx, dy,
                         (uint16_t)to_target_len(o, visible.w),
                         (uint16_t)to_target_len(o, visible.h));

    picture_transform_reset(dense);
}

static void draw_chrome(CompOutput *o, const CompScene *s);
static void draw_backdrop(CompOutput *o, const CompScene *s,
                          const CompRegion *damage);

static void xr_draw_scene(CompOutput *o, CompScene *s, const CompRegion *damage)
{
    if (!o->target)
        return;

    draw_backdrop(o, s, damage);

    for (int i = 0; i < s->count; i++) {
        CompSceneNode *n = &s->nodes[i];
        CompWindow *w = n->win;

        /* Nothing to draw here: either the window doesn't reach this
         * output, or an effect kept the node around without claiming any
         * area on it. */
        if (n->visible_rect.w <= 0 || n->visible_rect.h <= 0)
            continue;

        /* Nothing changed anywhere near it. The clip would have thrown
         * every pixel away regardless; asking first also skips binding
         * its pixmap and drawing its shadow, which is where the saving
         * actually is on a screen full of idle windows.
         *
         * The area asked about includes the shadow's reach, since that is
         * drawn outside the window -- output_damage_window_rect() grew
         * the damage by this same window's margin at the other end. */
        bool touched = false;
        for (int k = 0; k < n->clip.count && !touched; k++)
            touched = region_hits(damage, &n->clip.rects[k]);
        if (!touched && !region_is_full(&n->clip))
            continue;

        /* An effect drawing what the window looked like before its last
         * resize (shade). The node's geometry describes those contents,
         * not the window's current ones. */
        bool from_stash = n->use_stash && w->prev_picture;

        if (!from_stash && !window_bind(w)) {
            /* The window has no usable contents this frame, so it's
             * simply missing from the output -- worth saying, because
             * that is exactly what a one-frame "the window vanished"
             * glitch looks like from the outside. */
            comp_log("window 0x%x has no pixmap this frame", w->id);
            continue;
        }

        /* Freed after the draw below; declared here because the clip it
         * carries has to outlive the block that builds it. */
        xcb_xfixes_region_t owned_extents = XCB_NONE;

        /* Only where this node shows: everything drawn for it from here
         * to frame_clip_widen() is clipped out from under the opaque
         * windows above it. */
        frame_clip_narrow(o, &n->clip);

        /* Under the window, before it: a shadow is behind what casts it.
         * Skipped while transformed -- an animating window's shadow would
         * have to be transformed with it, and a shadow that stays behind
         * while the window slides away is worse than none. */
        if (comp_transform_is_identity(&n->transform))
            draw_shadow(o, w, &n->geometry, n->opacity);

        xcb_render_picture_t source = from_stash ? w->prev_picture : w->picture;

        /* A node an effect is transforming (section 22): XRender wants
         * the inverse mapping, and a matrix that isn't invertible as an
         * affine one is beyond this backend -- draw it straight rather
         * than draw nonsense. */
        CompTransform inverse;
        bool transformed = !comp_transform_is_identity(&n->transform) &&
                           comp_transform_invert_affine(&n->transform, &inverse);

        /* A transform that is only a move keeps its shape as a region
         * clip (below); anything else needs the shape as a mask. */
        float tdx = 0.0f, tdy = 0.0f;
        bool move_only = !transformed ||
                         comp_transform_is_translation(&n->transform, &tdx, &tdy);

        /* A lens counts too: the destination rectangle is magnified by
         * to_target_*, and without a matrix to sample through, XRender
         * would simply read that many more source pixels -- a window
         * cropped rather than a window enlarged. */
        float nlox, nloy;
        bool node_lensed = lens_of(o, &nlox, &nloy) != 1.0f ||
                           nlox != 0.0f || nloy != 0.0f;
        bool needs_matrix = transformed || o->scale != 1.0f || node_lensed;

        /* The shape follows the window through a scale as a mask, not
         * as a clip: the mask is sampled through the same matrix as the
         * pixmap, so a rounded corner stays round at every size of an
         * expo cell or a scale-in, and on a scaled output. The region
         * clip stays for the plain and the moved cases, where it costs
         * nothing per frame. Not for the stash: the mask describes the
         * window's current silhouette, which is not the one those pixels
         * had. */
        xcb_render_picture_t mask = XCB_NONE;
        bool shape_masked = false;
        if (comp.caps.xfixes && !from_stash &&
            needs_matrix && !(move_only && o->scale == 1.0f && !node_lensed)) {
            /* Ask for the shape before asking whether there is one: a
             * resize drops the cached shape and its `shaped` with it,
             * and the effect that animates that resize (geometry, on a
             * maximize or a restore) starts on the very next frame --
             * reading the stale flag here kept every such animation
             * square-cornered from its first frame to its last. */
            if (window_shape(w) != XCB_NONE && w->shaped) {
                mask = window_shape_mask(w, n->opacity);
                shape_masked = (mask != XCB_NONE);
            }
        }
        if (!shape_masked)
            mask = window_alpha(w, n->opacity);

        /* Clip the target to this window's shape before drawing it. The
         * region is window-relative, so the window's origin in target
         * coordinates goes in as the clip origin -- note that's the
         * window origin, not the pixmap's: the bounding shape is measured
         * from the former and reaches into the border area with negative
         * coordinates when there is one. This is what keeps kiwm's
         * rounded corners round, and a shaped client (a client's own
         * SHAPE, forwarded onto the frame by kiwm) shaped.
         *
         * Not while transformed: the region is in untransformed window
         * coordinates and XFixes can't scale it, so clipping with it
         * would carve the wrong hole. A window animating for a sixth of
         * a second with square corners is the better trade -- and the
         * GL renderer, which can transform the mask itself, is where
         * this stops being a trade at all. */
        if (comp.caps.xfixes) {
            /* No shape while drawing the stash either: the cached region
             * describes the window's *current* silhouette, which is not
             * the one those pixels had.
             *
             * Intersected with the frame's damage, never replacing it:
             * one clip region per picture means setting the shape alone
             * would repaint this window whole. */
            /* A transform that is only a move keeps its shape: XFixes
             * can translate a region, and the clip origin is where the
             * window is being *drawn* rather than where it is. That is
             * most of what the effects do -- dodge, the wall, smooth-move
             * -- and without it a shaped window animates as its whole
             * rectangle. Which is not a cosmetic loss: a window whose
             * rectangle covers the screen with a small bar shaped out of
             * it (VirtualBox's mini-toolbar) is drawn as a screen-sized
             * ghost of whatever its pixmap happens to hold.
             *
             * A scaled or rotated one goes by the mask instead
             * (shape_masked, above); the clip here is then only the
             * frame's damage. */
            xcb_xfixes_region_t shape = (from_stash || !move_only || shape_masked)
                                        ? XCB_NONE : window_shape(w);

            /* A window being scaled *without* a mask (the mask could not
             * be made) cannot keep its silhouette -- XFixes has no way to
             * scale a region -- but it can keep its extents, and for the
             * window this matters to those are not the same rectangle
             * at all. VirtualBox's mini-toolbar is a screen-sized window
             * with a small bar shaped out of it, so drawing its whole
             * rectangle while an effect shrinks it puts a screen-sized
             * ghost of stale contents in the middle of the expo grid.
             * The extents carried through the same transform are exactly
             * that bar. */
            if (!shape && !shape_masked && !from_stash && transformed && w->shaped &&
                w->shape_extents.w > 0 && w->shape_extents.h > 0) {
                window_shape(w);          /* refreshes the extents */
                CompRect ext = { w->x + w->shape_extents.x,
                                 w->y + w->shape_extents.y,
                                 w->shape_extents.w, w->shape_extents.h };
                CompRect moved = comp_transform_rect(&n->transform, &ext);

                if (moved.w > 0 && moved.h > 0) {
                    xcb_rectangle_t r = {
                        (int16_t)(moved.x - o->rect.x),
                        (int16_t)(moved.y - o->rect.y),
                        (uint16_t)moved.w, (uint16_t)moved.h
                    };
                    owned_extents = xcb_generate_id(comp.conn);
                    xcb_xfixes_create_region(comp.conn, owned_extents, 1, &r);
                }
            }

            if (owned_extents != XCB_NONE)
                clip_to_frame(o, owned_extents, 0, 0);
            else
                clip_to_frame(o, shape ? shape : XCB_NONE,
                              (int16_t)(w->x + (int)tdx - o->rect.x),
                              (int16_t)(w->y + (int)tdy - o->rect.y));
        }

        /* Source offset: where inside the window's own pixmap the visible
         * portion starts. Destination offset: the same point in target
         * pixels. That pair is the whole of "a window can cross outputs"
         * for a node nothing is transforming on an unscaled output. */
        int16_t sx = (int16_t)(n->visible_rect.x - n->geometry.x);
        int16_t sy = (int16_t)(n->visible_rect.y - n->geometry.y);
        int16_t dx = (int16_t)to_target_x(o, n->visible_rect.x);
        int16_t dy = (int16_t)to_target_y(o, n->visible_rect.y);
        uint16_t dw = (uint16_t)to_target_len(o, n->visible_rect.w);
        uint16_t dh = (uint16_t)to_target_len(o, n->visible_rect.h);

        /* A transform is needed whenever destination pixels and source
         * pixels don't step together: because an effect is transforming
         * the node, or because this output is scaled and one target pixel
         * is a fraction of a logical one -- or both, in which case the two
         * compose into a single matrix.
         *
         * XRender's matrix maps the coordinates handed to Composite into
         * the source picture, so it is built in the direction the sampling
         * goes: target pixels -> logical root -> (the effect's inverse) ->
         * the window's own pixmap. */
        if (needs_matrix) {
            /* Target pixels -> logical root: the physical scale, the
             * output's origin and the lens, all of which this output
             * puts between a root coordinate and a pixel. */
            CompTransform m;
            target_to_root(o, &m);

            if (transformed) {
                /* Applied *after* the target-to-root part: out = a * b
                 * means b first (transform.h). */
                comp_transform_multiply(&m, &inverse, &m);
            }

            comp_transform_translate(&m, (float)-n->geometry.x, (float)-n->geometry.y);
            picture_transform_set(source, &m);
            /* The mask is in the pixmap's space, so it is sampled through
             * the very same matrix -- that is the whole trick. */
            if (shape_masked)
                picture_transform_set(mask, &m);

            /* The matrix consumes target coordinates now, so that is what
             * goes in as the source point. */
            sx = dx;
            sy = dy;
        }

        /* The part of the window known to be opaque (window.h's opaque:
         * the client inside the frame, when it has no alpha and no shape)
         * is *copied*, and only the rest -- the frame around it, whose
         * titlebar and border are translucent under kiwm -- is blended.
         * OVER reads the destination it is about to replace; on a video
         * that is a full pass of the client's pixels a frame, read for
         * nothing. Straight draws only: through a matrix the client's
         * rectangle is not a rectangle of the target any more. */
        CompRect op = { 0, 0, 0, 0 };
        if (!needs_matrix && !from_stash && mask == XCB_NONE &&
            n->opacity >= 1.0f) {
            CompRect whole = window_opaque_rect(w);
            if (!rect_intersect(&whole, &n->visible_rect, &op))
                op = (CompRect){ 0, 0, 0, 0 };
        }

        if (op.w > 0 && op.h > 0) {
            xcb_render_composite(comp.conn, XCB_RENDER_PICT_OP_SRC,
                                 source, XCB_NONE, o->target,
                                 (int16_t)(op.x - n->geometry.x),
                                 (int16_t)(op.y - n->geometry.y), 0, 0,
                                 (int16_t)to_target_x(o, op.x),
                                 (int16_t)to_target_y(o, op.y),
                                 (uint16_t)op.w, (uint16_t)op.h);

            /* The frame around it: up to four rectangles, blended. */
            CompRegion rest;
            region_clear(&rest);
            region_add(&rest, &n->visible_rect);
            region_subtract_rect(&rest, &op);
            for (int k = 0; k < rest.count; k++) {
                const CompRect *r = &rest.rects[k];
                xcb_render_composite(comp.conn, XCB_RENDER_PICT_OP_OVER,
                                     source, XCB_NONE, o->target,
                                     (int16_t)(r->x - n->geometry.x),
                                     (int16_t)(r->y - n->geometry.y), 0, 0,
                                     (int16_t)to_target_x(o, r->x),
                                     (int16_t)to_target_y(o, r->y),
                                     (uint16_t)r->w, (uint16_t)r->h);
            }
        } else {
            /* OVER: for a depth-24 window the source has no alpha
             * channel, XRender reads it as opaque, and the result is
             * identical to a plain copy. For a depth-32 window this is
             * precisely the blending an uncomposited server can't do. */
            xcb_render_composite(comp.conn, XCB_RENDER_PICT_OP_OVER,
                                 source, mask, o->target,
                                 sx, sy, sx, sy, dx, dy, dw, dh);
        }

        /* The picture outlives the frame, so the transform must not: the
         * next paint may well be an ordinary one. */
        if (needs_matrix) {
            picture_transform_reset(source);
            if (shape_masked)
                picture_transform_reset(mask);
        }

        /* And, over the client area, the client's own denser contents if
         * it published any (density.h). What was just drawn is the frame:
         * the decoration around the edges plus a magnified copy of the
         * client area; this replaces that middle part with pixels the
         * client actually drew at this size. It is a second composite
         * rather than a merge because the two sources are genuinely two
         * drawables -- the frame's pixmap and the client's pixmap -- and
         * only the second one is dense.
         *
         * Skipped while an effect is transforming the node: the offsets
         * below assume the window is where it says it is, and a window
         * mid-animation is worth exactly as much sharpness as it has
         * milliseconds left. */
        if (!transformed && !from_stash) {
            /* The decoration first, then the contents inside it -- the
             * same order they sit in, and the same order they were drawn
             * in at logical size. Each is a separate drawable published by
             * a separate program (the WM's frame, the app's window), and
             * only what each one published is dense.
             *
             * With the constant-alpha mask, not the shape one: the dense
             * pictures are sampled through matrices of their own, which
             * the shape mask (in the frame pixmap's space) would not
             * follow. The dense client area lies inside the frame, away
             * from the corners the shape is about. */
            xcb_render_picture_t dense_mask =
                shape_masked ? window_alpha(w, n->opacity) : mask;
            float density;
            if (deco_density_active(w, &density))
                draw_dense(o, n, w, dense_mask, &n->geometry, density, true);
            if (density_active(w, &density)) {
                CompRect client = {
                    n->geometry.x + w->client_rect.x,
                    n->geometry.y + w->client_rect.y,
                    w->client_rect.w, w->client_rect.h
                };
                draw_dense(o, n, w, dense_mask, &client, density, false);
            }
        }

        if (owned_extents != XCB_NONE) {
            xcb_xfixes_destroy_region(comp.conn, owned_extents);
            owned_extents = XCB_NONE;
        }

        frame_clip_widen();
    }

    draw_chrome(o, s);
}

/* An effect's own ground, under every window (scene.h). One filled
 * rectangle, blended: a ground fading in is what makes a mode read as
 * arriving rather than as appearing, and OVER with a premultiplied
 * colour is exactly that fade.
 *
 * Inside the frame's damage clip like everything else, and skipped
 * entirely when this frame is not repainting any part of it. */
static void draw_backdrop(CompOutput *o, const CompScene *s,
                          const CompRegion *damage)
{
    const CompSceneBackdrop *b = &s->backdrop;
    if (b->rect.w <= 0 || b->rect.h <= 0)
        return;
    if (!region_hits(damage, &b->rect))
        return;

    clip_to_frame(o, XCB_NONE, 0, 0);

    /* XRender wants the colour already multiplied by its own alpha --
     * the same premultiplied form every window's pixmap is in. */
    float a = b->opacity;
    xcb_render_color_t c = {
        (uint16_t)(b->r * a * 65535.0f + 0.5f),
        (uint16_t)(b->g * a * 65535.0f + 0.5f),
        (uint16_t)(b->b * a * 65535.0f + 0.5f),
        (uint16_t)(a * 65535.0f + 0.5f),
    };

    xcb_rectangle_t r = to_target_rect(o, &b->rect);
    xcb_render_fill_rectangles(comp.conn, XCB_RENDER_PICT_OP_OVER, o->target,
                               c, 1, &r);
}

/* The chrome, over everything: an effect's own labels (scene.h). Drawn
 * unscaled and unclipped by any window's silhouette -- it is not a window
 * -- but still inside the frame's damage clip, so a frame that repainted
 * a corner does not have a label appear in the middle of it. */
static void draw_chrome(CompOutput *o, const CompScene *s)
{
    if (s->chrome_count == 0 || !o->target)
        return;

    clip_to_frame(o, XCB_NONE, 0, 0);

    for (int i = 0; i < s->chrome_count; i++) {
        const CompSceneChrome *c = &s->chrome[i];
        xcb_render_picture_t pic = text_picture(c->image);
        if (pic == XCB_NONE)
            continue;

        CompRect vis;
        if (!rect_intersect(&c->rect, &o->rect, &vis))
            continue;

        xcb_render_picture_t mask = XCB_NONE;
        if (c->opacity < 1.0f) {
            /* The same constant-alpha trick the windows use, on a
             * throwaway picture: chrome fades in and out with the mode
             * that owns it. */
            static xcb_render_picture_t fade;
            static float fade_value = -1.0f;
            if (fade == XCB_NONE || fade_value != c->opacity) {
                if (fade != XCB_NONE)
                    xcb_render_free_picture(comp.conn, fade);
                xcb_pixmap_t pm = xcb_generate_id(comp.conn);
                xcb_create_pixmap(comp.conn, 8, pm, comp.root, 1, 1);
                uint32_t vals[] = { 1 };
                fade = xcb_generate_id(comp.conn);
                xcb_render_create_picture(comp.conn, fade, pm, format_a8(),
                                          XCB_RENDER_CP_REPEAT, vals);
                xcb_free_pixmap(comp.conn, pm);
                xcb_render_color_t col = {
                    0, 0, 0, (uint16_t)(c->opacity * 0xffff)
                };
                xcb_rectangle_t r = { 0, 0, 1, 1 };
                xcb_render_fill_rectangles(comp.conn, XCB_RENDER_PICT_OP_SRC,
                                           fade, col, 1, &r);
                fade_value = c->opacity;
            }
            mask = fade;
        }

        int16_t sx = (int16_t)(vis.x - c->rect.x);
        int16_t sy = (int16_t)(vis.y - c->rect.y);

        if (o->scale != 1.0f) {
            /* A scaled output draws its labels at physical size: the text
             * was rasterised in logical pixels, and stretching it is
             * exactly the blur the whole density mechanism exists to
             * avoid -- but a label half the size it should be is worse
             * than a slightly soft one, so it is scaled rather than left
             * tiny. */
            xcb_render_set_picture_filter(comp.conn, pic, 4, "good", 0, NULL);
            float inv = 1.0f / o->scale;
            xcb_render_transform_t t = {
                (int32_t)(inv * 65536.0f), 0, 0,
                0, (int32_t)(inv * 65536.0f), 0,
                0, 0, 65536
            };
            xcb_render_set_picture_transform(comp.conn, pic, t);
        }

        xcb_render_composite(comp.conn, XCB_RENDER_PICT_OP_OVER, pic, mask,
                             o->target, sx, sy, 0, 0,
                             (int16_t)to_target_x(o, vis.x),
                             (int16_t)to_target_y(o, vis.y),
                             (uint16_t)to_target_len(o, vis.w),
                             (uint16_t)to_target_len(o, vis.h));

        if (o->scale != 1.0f) {
            xcb_render_transform_t id = { 65536,0,0, 0,65536,0, 0,0,65536 };
            xcb_render_set_picture_transform(comp.conn, pic, id);
        }
    }
}

static void xr_end(CompOutput *o)
{
    /* The presenter reads the target next, and the frame's regions end
     * here: neither the last window's clip nor the damage clip may still
     * be in force. */
    if (o->target && comp.caps.xfixes)
        xcb_xfixes_set_picture_clip_region(comp.conn, o->target,
                                           XCB_XFIXES_REGION_NONE, 0, 0);
    frame_clip_destroy();
}

static xcb_pixmap_t xr_output_pixmap(const CompOutput *o)
{
    const XrOutput *xo = o->render_data;
    return xo ? xo->pixmap : XCB_NONE;
}

static void xr_shutdown(void)
{
    shadow_shutdown();
    xr_background_invalidate();
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

    .window_invalidate         = xr_window_invalidate,
    .window_shape_invalidate   = xr_window_shape_invalidate,
    .window_free               = xr_window_free,
    .window_has_content        = xr_window_has_content,
    .window_density_invalidate = xr_window_density_invalidate,

    .window_stash              = xr_window_stash,
    .window_has_stash          = xr_window_has_stash,
    .window_stash_rect         = xr_window_stash_rect,
    .stash_hold                = xr_stash_hold,
    .stash_release             = xr_stash_release,
    .stash_drop_unheld         = xr_stash_drop_unheld,

    .background_invalidate     = xr_background_invalidate,
    .output_pixmap             = xr_output_pixmap,
    .shutdown                  = xr_shutdown,
};

const CompRenderer *renderer_xrender(void)
{
    return &xrender_renderer;
}
