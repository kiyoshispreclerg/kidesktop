/* outline.c - the wireframe rectangle kiwm draws around a window it's
 * pointing at without touching it yet (see outline.h for who uses it).
 *
 * One override-redirect window the size of the outlined rectangle grown by
 * the band's outer half, XCB SHAPE-clipped down to just the band itself so
 * the middle stays a real hole -- the window underneath keeps showing
 * through, with no compositor involved and no clicks intercepted (the
 * input shape is emptied outright, so the outline can never swallow a
 * pointer event even while a drag is in flight under it).
 *
 * It is painted by *being* its color, not by drawing into it: the window's
 * background pixel is the focused decoration's own background color, so
 * the X server fills every pixel of it -- including whatever a resize just
 * exposed -- as part of the same operation that resizes it. An earlier
 * version drew the color in afterwards (off-screen pixmap, one
 * xcb_copy_area(), the pattern decoration.c and osd.c use) and that showed
 * exactly as reported: on each step of a drag the window resized first,
 * flashing its old contents at the new size, and only then got its color.
 * Nothing here needs the theme's background image or any real drawing, so
 * a background pixel is both simpler and atomic.
 */
#include "outline.h"
#include "client.h"

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
static uint32_t current_pixel = 0;
static bool have_pixel = false;

/* A 0..1 channel value into its slot in the visual's pixel layout. The
 * root visual is TrueColor on anything kiwm runs on, so the masks are
 * contiguous and this is just "scale to the mask's width, shift into
 * place". */
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

static uint32_t outline_pixel(void)
{
    double r, g, b;
    if (wm.have_theme_colors) {
        r = wm.bg_active_r; g = wm.bg_active_g; b = wm.bg_active_b;
    } else {
        r = wm.deco_bg_r; g = wm.deco_bg_g; b = wm.deco_bg_b;
    }
    return channel_to_pixel(r, wm.visual->red_mask) |
           channel_to_pixel(g, wm.visual->green_mask) |
           channel_to_pixel(b, wm.visual->blue_mask);
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

void outline_show(int x, int y, int w, int h)
{
    int outer = band_outer();
    int ox = x - outer;
    int oy = y - outer;
    int ow = w + 2 * outer;
    int oh = h + 2 * outer;
    if (ow <= 0 || oh <= 0)
        return;

    uint32_t pixel = outline_pixel();

    if (win == XCB_NONE) {
        win = xcb_generate_id(wm.conn);
        uint32_t values[] = { pixel, 1, XCB_EVENT_MASK_EXPOSURE };
        xcb_create_window(wm.conn, wm.screen->root_depth, win, wm.root,
                          (int16_t)ox, (int16_t)oy, (uint16_t)ow, (uint16_t)oh, 0,
                          XCB_WINDOW_CLASS_INPUT_OUTPUT, wm.screen->root_visual,
                          XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK, values);
        current_pixel = pixel;
        have_pixel = true;
        /* Empty input shape, permanently: whatever this is drawn over --
         * a window being dragged, the switcher's own hold -- must keep
         * receiving every pointer event as if the outline weren't there. */
        xcb_shape_rectangles(wm.conn, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_INPUT, XCB_CLIP_ORDERING_UNSORTED,
                             win, 0, 0, 0, NULL);
    } else if (!have_pixel || pixel != current_pixel) {
        /* Only when the theme's color actually changed under us. */
        xcb_change_window_attributes(wm.conn, win, XCB_CW_BACK_PIXEL, &pixel);
        xcb_clear_area(wm.conn, 0, win, 0, 0, 0, 0);
        current_pixel = pixel;
        have_pixel = true;
    }

    /* Shape first, geometry second, one flush for both: the band for the
     * new size is in place before the window ever appears at that size, so
     * a resize can't briefly show a full rectangle where the hole should
     * be. */
    xcb_rectangle_t rects[4];
    int n = band_rects(ow, oh, rects);
    /* UNSORTED, not Y_SORTED: the bottom band is written before the two
     * side ones, so the list genuinely isn't in ascending-y order, and
     * promising the server an ordering that doesn't hold is how this ends
     * up as a solid filled rectangle instead of a hollow frame. */
    xcb_shape_rectangles(wm.conn, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_BOUNDING, XCB_CLIP_ORDERING_UNSORTED,
                         win, 0, 0, (uint32_t)n, rects);

    uint32_t geo[] = { (uint32_t)ox, (uint32_t)oy, (uint32_t)ow, (uint32_t)oh };
    xcb_configure_window(wm.conn, win,
                         XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                         XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, geo);

    if (!visible) {
        xcb_map_window(wm.conn, win);
        visible = true;
        /* Mapping doesn't restack, and the outline has to sit above every
         * client but below the switcher overlay that's usually driving it
         * -- which is exactly what restack_all() does with LAYER_OUTLINE,
         * once the window exists for it to find. Only on the way up: a
         * drag updates this many times a second, and re-sorting the whole
         * stack for each of those would be both pointless (nothing else
         * moved) and expensive (an xcb_query_tree() round trip per step). */
        restack_all();
    }
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
    /* Nothing to redraw -- the background pixel *is* the content, so the
     * server has already refilled whatever was exposed. */
}
