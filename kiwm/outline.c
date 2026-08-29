/* outline.c - the wireframe rectangle kiwm draws around a window it's
 * pointing at without touching it yet (see outline.h for who uses it).
 *
 * One override-redirect window the size of the outlined rectangle grown by
 * the band's outer half, XCB SHAPE-clipped down to just the band itself so
 * the middle stays a real hole -- the window underneath keeps showing
 * through, with no compositor involved and no clicks intercepted (the
 * input shape is emptied outright, so the outline can never swallow a
 * pointer event even while a drag is in flight under it). Filled with a
 * flat color, the focused decoration's own background: this is meant to
 * read as "the titlebar's color, drawn around the window", not as another
 * themed surface, so it deliberately doesn't use the theme's background
 * image the way decoration.c does.
 */
#include "outline.h"
#include "client.h"

#include <cairo/cairo-xcb.h>
#include <xcb/shape.h>

#include <stdlib.h>

/* The band straddles the outlined rectangle's edge: half of kiwm.conf's
 * outline_width= reaches outside it, the rest inside (so an odd width puts
 * the extra pixel inside). Read on every show rather than cached, so a
 * reloaded config takes effect on the next outline with nothing to
 * invalidate. */
static int band_outer(void) { return wm.outline_width / 2; }
static int band_total(void) { return wm.outline_width; }

static xcb_window_t win = XCB_NONE;
static bool visible = false;
static int win_w = 0, win_h = 0;

static void outline_color(double *r, double *g, double *b)
{
    if (wm.have_theme_colors) {
        *r = wm.bg_active_r; *g = wm.bg_active_g; *b = wm.bg_active_b;
    } else {
        *r = wm.deco_bg_r; *g = wm.deco_bg_g; *b = wm.deco_bg_b;
    }
}

/* The band as up to four rectangles (top, bottom, left, right) in window
 * coordinates -- or one solid rectangle when the outlined window is too
 * small for the band to leave a hole at all. Returns how many were
 * written; `out` needs room for 4. */
static int band_rects(int w, int h, xcb_rectangle_t *out)
{
    int band = band_total();
    int inner_w = w - 2 * band;
    int inner_h = h - 2 * band;

    if (inner_w <= 0 || inner_h <= 0) {
        out[0] = (xcb_rectangle_t){ 0, 0, (uint16_t)w, (uint16_t)h };
        return 1;
    }

    out[0] = (xcb_rectangle_t){ 0, 0, (uint16_t)w, (uint16_t)band };
    out[1] = (xcb_rectangle_t){ 0, (int16_t)(h - band), (uint16_t)w, (uint16_t)band };
    out[2] = (xcb_rectangle_t){ 0, (int16_t)band, (uint16_t)band, (uint16_t)inner_h };
    out[3] = (xcb_rectangle_t){ (int16_t)(w - band), (int16_t)band, (uint16_t)band, (uint16_t)inner_h };
    return 4;
}

static void paint(void)
{
    if (win == XCB_NONE || win_w <= 0 || win_h <= 0)
        return;

    double r, g, b;
    outline_color(&r, &g, &b);

    xcb_pixmap_t pixmap = xcb_generate_id(wm.conn);
    xcb_create_pixmap(wm.conn, wm.screen->root_depth, pixmap, win, (uint16_t)win_w, (uint16_t)win_h);
    cairo_surface_t *surface = cairo_xcb_surface_create(wm.conn, pixmap, wm.visual, win_w, win_h);
    cairo_t *cr = cairo_create(surface);
    cairo_set_source_rgb(cr, r, g, b);
    cairo_paint(cr);
    cairo_destroy(cr);
    cairo_surface_destroy(surface);

    xcb_copy_area(wm.conn, pixmap, win, wm.deco_gc, 0, 0, 0, 0, (uint16_t)win_w, (uint16_t)win_h);
    xcb_free_pixmap(wm.conn, pixmap);
}

void outline_show(int x, int y, int w, int h)
{
    int outer = band_outer();
    int ox = x - outer;
    int oy = y - outer;
    int ow = w + 2 * outer;
    int oh = h + 2 * outer;
    if (ow <= 0 || oh <= 0)
        return;

    if (win == XCB_NONE) {
        win = xcb_generate_id(wm.conn);
        uint32_t values[] = { wm.screen->black_pixel, 1, XCB_EVENT_MASK_EXPOSURE };
        xcb_create_window(wm.conn, wm.screen->root_depth, win, wm.root,
                          (int16_t)ox, (int16_t)oy, (uint16_t)ow, (uint16_t)oh, 0,
                          XCB_WINDOW_CLASS_INPUT_OUTPUT, wm.screen->root_visual,
                          XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK, values);
        /* Empty input shape, permanently: whatever this is drawn over --
         * a window being dragged, the switcher's own hold -- must keep
         * receiving every pointer event as if the outline weren't there. */
        xcb_shape_rectangles(wm.conn, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_INPUT, XCB_CLIP_ORDERING_UNSORTED,
                             win, 0, 0, 0, NULL);
    }

    uint32_t geo[] = { (uint32_t)ox, (uint32_t)oy, (uint32_t)ow, (uint32_t)oh };
    xcb_configure_window(wm.conn, win,
                         XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                         XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, geo);
    win_w = ow;
    win_h = oh;

    xcb_rectangle_t rects[4];
    int n = band_rects(ow, oh, rects);
    /* UNSORTED, not Y_SORTED: the bottom band is written before the two
     * side ones, so the list genuinely isn't in ascending-y order, and
     * promising the server an ordering that doesn't hold is how this ends
     * up as a solid filled rectangle instead of a hollow frame. */
    xcb_shape_rectangles(wm.conn, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_BOUNDING, XCB_CLIP_ORDERING_UNSORTED,
                         win, 0, 0, (uint32_t)n, rects);

    if (!visible) {
        xcb_map_window(wm.conn, win);
        visible = true;
    }
    paint();
    /* Mapping doesn't restack, and the outline has to sit above every
     * client but below the switcher overlay that's usually driving it --
     * which is exactly what restack_all() does with LAYER_OUTLINE, once
     * the window exists for it to find. */
    restack_all();
    xcb_flush(wm.conn);
}

void outline_hide(void)
{
    if (!visible)
        return;
    xcb_unmap_window(wm.conn, win);
    visible = false;
    xcb_flush(wm.conn);
}

bool outline_visible(void)
{
    return visible;
}

bool outline_owns_window(xcb_window_t window)
{
    return visible && win != XCB_NONE && window == win;
}

void outline_handle_expose(xcb_window_t window)
{
    if (!outline_owns_window(window))
        return;
    paint();
    xcb_flush(wm.conn);
}
