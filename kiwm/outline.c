/* outline.c - the rectangle kiwm draws where a window is, or is about to
 * be, without touching the window itself (see outline.h for who uses it).
 *
 * Two looks, picked per show by whether a compositor is running:
 *
 * Without one, the classic wireframe: a hollow band around the rectangle.
 * One override-redirect window the size of the whole screen, created once
 * and never moved or resized -- where the band appears is entirely its
 * bounding shape, so every update is a single ShapeRectangles request the
 * server applies in one go. (Moving/resizing a window instead meant two
 * requests per step, reshape and reconfigure, with a visible intermediate
 * state between them whichever order they were sent in.) It is painted by
 * *being* its color -- the window's background pixel -- so the server
 * fills it as part of mapping/exposing it, with no draw step that could
 * lag behind; and nothing outside the band is drawn, so a screen-sized
 * window costs nothing.
 *
 * With one, xfwm4's composited look instead: the rectangle itself, filled
 * with the same theme color at a strong transparency. Each rectangle is a
 * real ARGB window sized and positioned exactly as the rectangle, mapped
 * while shown and unmapped otherwise -- on purpose, so the compositor
 * sees an ordinary window doing ordinary things and can put its effects
 * on it: a geometry change it can animate, a map/unmap it can fade, a
 * scale like any other window's. The two-request flicker doesn't apply
 * here, since a compositor presents whole frames. Both looks carry an
 * empty input shape, so neither can ever swallow a pointer event, even
 * with a drag in flight under them, and both mark themselves as kiwm's
 * "outline" layer (wm.h's Atoms::kiwm_layer) so a compositor can tell
 * them from application windows.
 */
#include "outline.h"
#include "client.h"
#include "decoration.h"
#include "selection.h"

#include <xcb/shape.h>

#include <stdlib.h>

/* The band straddles the outlined rectangle's edge: half of kiwm.conf's
 * outline_width= reaches outside it, the rest inside (so an odd width puts
 * the extra pixel inside). Read on every show rather than cached, so a
 * reloaded config takes effect on the next outline with nothing to
 * invalidate. Only the wireframe look uses it. */
static int band_outer(void) { return wm.outline_width / 2; }
static int band_total(void) { return wm.outline_width; }

/* How many rectangles one call can outline: the window being resized plus
 * every neighbor a linked resize can drag along on either axis. */
#define OUTLINE_MAX_RECTS (1 + 2 * MAX_RESIZE_NEIGHBORS)

/* ---- wireframe look (no compositor): one shaped screen-sized window ---- */

static xcb_window_t band_win = XCB_NONE;
static bool band_visible = false;
static uint32_t band_pixel = 0;
static bool band_have_pixel = false;
static int band_win_w = 0, band_win_h = 0;   /* the screen size the window was made for */

/* ---- filled look (compositor): one ARGB window per rectangle ---- */

static xcb_window_t fill_win[OUTLINE_MAX_RECTS];
static bool fill_mapped[OUTLINE_MAX_RECTS];
static int fill_w[OUTLINE_MAX_RECTS], fill_h[OUTLINE_MAX_RECTS];   /* size the shape was cut for */
static int fill_shown = 0;                    /* how many of fill_win[] are currently up */
static uint32_t fill_pixel = 0;
static bool fill_have_pixel = false;

/* A 0..1 channel value into its slot in a visual's pixel layout. Every
 * visual kiwm draws on is TrueColor, so the masks are contiguous and this
 * is just "scale to the mask's width, shift into place". */
static uint32_t channel_to_pixel(double v, uint32_t mask)
{
    if (mask == 0)
        return 0;
    int shift = 0;
    while (((mask >> shift) & 1) == 0)
        shift++;
    uint32_t range = mask >> shift;
    if (v < 0.0) v = 0.0;
    if (v > 1.0) v = 1.0;
    return ((uint32_t)(v * range + 0.5) << shift) & mask;
}

static void outline_color(double *r, double *g, double *b)
{
    if (wm.have_theme_colors) {
        *r = wm.bg_active_r; *g = wm.bg_active_g; *b = wm.bg_active_b;
    } else {
        *r = wm.deco_bg_r; *g = wm.deco_bg_g; *b = wm.deco_bg_b;
    }
}

/* The wireframe's pixel: opaque, in the root visual. */
static uint32_t band_color_pixel(void)
{
    double r, g, b;
    outline_color(&r, &g, &b);
    return channel_to_pixel(r, wm.visual->red_mask) |
           channel_to_pixel(g, wm.visual->green_mask) |
           channel_to_pixel(b, wm.visual->blue_mask);
}

/* The filled look's pixel: the same color at kiwm.conf's outline_alpha=,
 * in the ARGB visual -- *premultiplied*, which is what a compositor
 * (XRender, and everything modelled on it) reads a 32-bit pixel as. The
 * alpha lives in whatever bits the three color masks leave over. */
static uint32_t fill_color_pixel(void)
{
    double r, g, b;
    outline_color(&r, &g, &b);
    double a = wm.outline_alpha;
    uint32_t rgb_mask = wm.argb_visual->red_mask | wm.argb_visual->green_mask | wm.argb_visual->blue_mask;
    uint32_t alpha_mask = ~rgb_mask;
    return channel_to_pixel(a, alpha_mask) |
           channel_to_pixel(r * a, wm.argb_visual->red_mask) |
           channel_to_pixel(g * a, wm.argb_visual->green_mask) |
           channel_to_pixel(b * a, wm.argb_visual->blue_mask);
}

/* Shared by both looks: never take a click, and say what this is. */
static void mark_outline_window(xcb_window_t w)
{
    xcb_shape_rectangles(wm.conn, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_INPUT, XCB_CLIP_ORDERING_UNSORTED,
                         w, 0, 0, 0, NULL);
    xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE, w, wm.atoms.kiwm_layer,
                        XCB_ATOM_STRING, 8, 7, "outline");
}

/* Whether this show uses the filled look. Asked per show, not cached: a
 * compositor starting or stopping needs no restart and no event of its
 * own here -- the next outline just comes out the other way. */
static bool use_filled(void)
{
    return wm.argb_visual != NULL && compositor_running();
}

/* ---- wireframe look ---- */

/* The band as up to four rectangles (top, bottom, left, right) in *screen*
 * coordinates, around the rect (ox, oy, ow, oh) -- or one solid rectangle
 * when the outlined window is too small for the band to leave a hole at
 * all. The window is screen-sized and never moves, so these absolute
 * coordinates are also window coordinates. Returns how many were written;
 * `out` needs room for 4. */
static int band_rects(int ox, int oy, int ow, int oh, xcb_rectangle_t *out)
{
    int band = band_total();
    int inner_w = ow - 2 * band;
    int inner_h = oh - 2 * band;

    if (inner_w <= 0 || inner_h <= 0) {
        out[0] = (xcb_rectangle_t){ (int16_t)ox, (int16_t)oy, (uint16_t)ow, (uint16_t)oh };
        return 1;
    }

    out[0] = (xcb_rectangle_t){ (int16_t)ox, (int16_t)oy, (uint16_t)ow, (uint16_t)band };
    out[1] = (xcb_rectangle_t){ (int16_t)ox, (int16_t)(oy + oh - band), (uint16_t)ow, (uint16_t)band };
    out[2] = (xcb_rectangle_t){ (int16_t)ox, (int16_t)(oy + band), (uint16_t)band, (uint16_t)inner_h };
    out[3] = (xcb_rectangle_t){ (int16_t)(ox + ow - band), (int16_t)(oy + band),
                                (uint16_t)band, (uint16_t)inner_h };
    return 4;
}

static void band_hide(void)
{
    if (!band_visible)
        return;
    xcb_unmap_window(wm.conn, band_win);
    band_visible = false;
}

static void band_show(const OutlineRect *in, int count)
{
    uint32_t pixel = band_color_pixel();

    if (band_win == XCB_NONE) {
        band_win = xcb_generate_id(wm.conn);
        uint32_t values[] = { pixel, 1, XCB_EVENT_MASK_EXPOSURE };
        xcb_create_window(wm.conn, wm.screen->root_depth, band_win, wm.root,
                          0, 0, (uint16_t)wm.screen_w, (uint16_t)wm.screen_h, 0,
                          XCB_WINDOW_CLASS_INPUT_OUTPUT, wm.screen->root_visual,
                          XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK, values);
        band_win_w = wm.screen_w;
        band_win_h = wm.screen_h;
        band_pixel = pixel;
        band_have_pixel = true;
        mark_outline_window(band_win);
    } else {
        if (band_win_w != wm.screen_w || band_win_h != wm.screen_h) {
            /* The screen itself changed size (RandR) -- rare, and the only
             * thing that ever reconfigures this window. */
            uint32_t geo[] = { (uint32_t)wm.screen_w, (uint32_t)wm.screen_h };
            xcb_configure_window(wm.conn, band_win,
                                 XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, geo);
            band_win_w = wm.screen_w;
            band_win_h = wm.screen_h;
        }
        if (!band_have_pixel || pixel != band_pixel) {
            /* Only when the theme's color actually changed under us. */
            xcb_change_window_attributes(wm.conn, band_win, XCB_CW_BACK_PIXEL, &pixel);
            xcb_clear_area(wm.conn, 0, band_win, 0, 0, 0, 0);
            band_pixel = pixel;
            band_have_pixel = true;
        }
    }

    /* The whole update: one request, wherever the outlines have to be
     * now, however many of them there are. */
    xcb_rectangle_t rects[OUTLINE_MAX_RECTS * 4];
    int n = 0;
    int outer = band_outer();
    for (int i = 0; i < count; i++) {
        int ox = in[i].x - outer;
        int oy = in[i].y - outer;
        int ow = in[i].w + 2 * outer;
        int oh = in[i].h + 2 * outer;
        if (ow <= 0 || oh <= 0)
            continue;
        n += band_rects(ox, oy, ow, oh, &rects[n]);
    }
    if (n == 0)
        return;
    /* UNSORTED, not Y_SORTED: the bottom band is written before the two
     * side ones, so the list genuinely isn't in ascending-y order, and
     * promising the server an ordering that doesn't hold is how this ends
     * up as a solid filled rectangle instead of a hollow frame. */
    xcb_shape_rectangles(wm.conn, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_BOUNDING, XCB_CLIP_ORDERING_UNSORTED,
                         band_win, 0, 0, (uint32_t)n, rects);

    if (!band_visible) {
        xcb_map_window(wm.conn, band_win);
        band_visible = true;
        /* Mapping doesn't restack, and the outline has to sit above every
         * client but below the switcher overlay that's usually driving it
         * -- which is exactly what restack_all() does with LAYER_OUTLINE,
         * once the window exists for it to find. Only on the way up: a
         * drag updates this many times a second, and re-sorting the whole
         * stack for each of those would be both pointless (nothing else
         * moved) and expensive (an xcb_query_tree() round trip per step). */
        restack_all();
    }
}

/* ---- filled look ---- */

/* The theme's corner radii (the same ones the frames and the switcher
 * overlay use, decoration.c's build_rounded_rects()) cut into the filled
 * rectangle's bounding shape, so it reads as the window it stands for.
 * Under a compositor the shape is respected through every effect -- a
 * scaled or moved outline keeps its corners. Only when the size changed:
 * the shape is in window coordinates and survives a plain move. */
static void fill_shape(int slot, int w, int h)
{
    if (!wm.shape_ext_present || (fill_w[slot] == w && fill_h[slot] == h))
        return;
    fill_w[slot] = w;
    fill_h[slot] = h;

    bool square = (wm.radius_tl == 0 && wm.radius_tr == 0 && wm.radius_br == 0 && wm.radius_bl == 0);
    if (square) {
        xcb_shape_mask(wm.conn, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_BOUNDING, fill_win[slot], 0, 0,
                       XCB_PIXMAP_NONE);
        return;
    }
    xcb_rectangle_t rects[2 * MAX_CORNER_RADIUS + 1];
    int n = build_rounded_rects(w, h, wm.radius_tl, wm.radius_tr, wm.radius_br, wm.radius_bl,
                                rects, (int)(sizeof(rects) / sizeof(rects[0])));
    xcb_shape_rectangles(wm.conn, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_BOUNDING, XCB_CLIP_ORDERING_Y_SORTED,
                         fill_win[slot], 0, 0, (uint32_t)n, rects);
}

static void fill_hide(void)
{
    for (int i = 0; i < OUTLINE_MAX_RECTS; i++) {
        if (fill_mapped[i]) {
            xcb_unmap_window(wm.conn, fill_win[i]);
            fill_mapped[i] = false;
        }
    }
    fill_shown = 0;
}

static void fill_show(const OutlineRect *in, int count)
{
    uint32_t pixel = fill_color_pixel();
    bool color_changed = !fill_have_pixel || pixel != fill_pixel;
    fill_pixel = pixel;
    fill_have_pixel = true;

    bool newly_mapped = false;
    int used = 0;

    for (int i = 0; i < count && used < OUTLINE_MAX_RECTS; i++) {
        const OutlineRect *r = &in[i];
        if (r->w <= 0 || r->h <= 0)
            continue;
        int slot = used++;

        if (fill_win[slot] == XCB_NONE) {
            fill_win[slot] = xcb_generate_id(wm.conn);
            /* Value list in ascending CW_* bit order, as osd.c explains:
             * BACK_PIXEL, BORDER_PIXEL (mandatory for a window whose depth
             * differs from its parent's), OVERRIDE_REDIRECT, EVENT_MASK,
             * COLORMAP. */
            uint32_t values[] = { pixel, 0, 1, XCB_EVENT_MASK_EXPOSURE, wm.argb_colormap };
            xcb_create_window(wm.conn, 32, fill_win[slot], wm.root,
                              (int16_t)r->x, (int16_t)r->y, (uint16_t)r->w, (uint16_t)r->h, 0,
                              XCB_WINDOW_CLASS_INPUT_OUTPUT, wm.argb_visual->visual_id,
                              XCB_CW_BACK_PIXEL | XCB_CW_BORDER_PIXEL |
                              XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK |
                              XCB_CW_COLORMAP, values);
            mark_outline_window(fill_win[slot]);
            fill_w[slot] = fill_h[slot] = 0;
        } else {
            if (color_changed) {
                xcb_change_window_attributes(wm.conn, fill_win[slot], XCB_CW_BACK_PIXEL, &pixel);
                xcb_clear_area(wm.conn, 0, fill_win[slot], 0, 0, 0, 0);
            }
            /* The window *is* the rectangle: a real move/resize, which is
             * exactly what lets the compositor animate it. */
            uint32_t geo[] = { (uint32_t)r->x, (uint32_t)r->y, (uint32_t)r->w, (uint32_t)r->h };
            xcb_configure_window(wm.conn, fill_win[slot],
                                 XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                                 XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, geo);
        }
        fill_shape(slot, r->w, r->h);

        if (!fill_mapped[slot]) {
            xcb_map_window(wm.conn, fill_win[slot]);
            fill_mapped[slot] = true;
            newly_mapped = true;
        }
    }

    /* Rectangles that were up last time but aren't wanted now. */
    for (int i = used; i < OUTLINE_MAX_RECTS; i++) {
        if (fill_mapped[i]) {
            xcb_unmap_window(wm.conn, fill_win[i]);
            fill_mapped[i] = false;
        }
    }
    fill_shown = used;

    if (newly_mapped)
        restack_all();   /* same reasoning as band_show(): only when something new came up */
}

/* ---- public API ---- */

void outline_show(int x, int y, int w, int h)
{
    OutlineRect one = { x, y, w, h };
    outline_show_rects(&one, 1);
}

void outline_show_rects(const OutlineRect *in, int count)
{
    if (!in || count <= 0)
        return;
    if (count > OUTLINE_MAX_RECTS)
        count = OUTLINE_MAX_RECTS;

    /* A compositor that came or went since the last show: the look it
     * was drawn in goes away with it. */
    if (use_filled()) {
        band_hide();
        fill_show(in, count);
    } else {
        fill_hide();
        band_show(in, count);
    }
    xcb_flush(wm.conn);
}

void outline_hide(void)
{
    if (!band_visible && fill_shown == 0)
        return;
    band_hide();
    fill_hide();
    xcb_flush(wm.conn);
}

bool outline_visible(void)
{
    return band_visible || fill_shown > 0;
}

bool outline_owns_window(xcb_window_t window)
{
    if (band_visible && band_win != XCB_NONE && window == band_win)
        return true;
    for (int i = 0; i < OUTLINE_MAX_RECTS; i++)
        if (fill_mapped[i] && fill_win[i] == window)
            return true;
    return false;
}

void outline_handle_expose(xcb_window_t window)
{
    if (!outline_owns_window(window))
        return;
    /* Nothing to redraw in either look -- the background pixel *is* the
     * content, so the server has already refilled whatever was exposed. */
}
