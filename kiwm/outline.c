/* outline.c - the wireframe rectangle kiwm draws around a window it's
 * pointing at without touching it yet (see outline.h for who uses it).
 *
 * One override-redirect window, XCB SHAPE-clipped down to just the band so
 * everything else -- the hole in the middle included -- stays untouched:
 * the windows underneath keep showing through, with no compositor involved
 * and no clicks intercepted (the input shape is emptied outright, so the
 * outline can never swallow a pointer event even while a drag is in flight
 * under it).
 *
 * It is painted by *being* its color, not by drawing into it: the window's
 * background pixel is the focused decoration's own background color, so
 * the X server fills every pixel of it as part of mapping/exposing it.
 * An earlier version drew the color in afterwards (off-screen pixmap, one
 * xcb_copy_area(), the pattern decoration.c and osd.c use), which showed
 * on every step of a drag as the window resizing first, flashing its old
 * contents at the new size, and only then getting its color.
 *
 * The window itself never moves or resizes, either. It is created once at
 * the full size of the X screen and stays there; where the outline
 * *appears* is entirely a matter of its bounding shape, so every update is
 * a single ShapeRectangles request that the server applies in one go.
 * Moving/resizing the window instead meant two requests per step -- reshape
 * and reconfigure -- with a visible intermediate state between them no
 * matter which order they were sent in: the band briefly drawn for the new
 * size at the old position, or at the new position with the old size.
 * Nothing is drawn outside the band regardless, and the input shape is
 * empty, so a screen-sized window costs nothing here.
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

/* How many rectangles one call can outline: the window being resized plus
 * every neighbor a linked resize can drag along on either axis. */
#define OUTLINE_MAX_RECTS (1 + 2 * MAX_RESIZE_NEIGHBORS)

static xcb_window_t win = XCB_NONE;
static bool visible = false;
static uint32_t current_pixel = 0;
static bool have_pixel = false;
static int win_w = 0, win_h = 0;   /* the screen size the window was made for */

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

    uint32_t pixel = outline_pixel();

    if (win == XCB_NONE) {
        win = xcb_generate_id(wm.conn);
        uint32_t values[] = { pixel, 1, XCB_EVENT_MASK_EXPOSURE };
        xcb_create_window(wm.conn, wm.screen->root_depth, win, wm.root,
                          0, 0, (uint16_t)wm.screen_w, (uint16_t)wm.screen_h, 0,
                          XCB_WINDOW_CLASS_INPUT_OUTPUT, wm.screen->root_visual,
                          XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK, values);
        win_w = wm.screen_w;
        win_h = wm.screen_h;
        current_pixel = pixel;
        have_pixel = true;
        /* Empty input shape, permanently: whatever this is drawn over --
         * a window being dragged, the switcher's own hold -- must keep
         * receiving every pointer event as if the outline weren't there.
         * Doubly important now that the window spans the whole screen. */
        xcb_shape_rectangles(wm.conn, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_INPUT, XCB_CLIP_ORDERING_UNSORTED,
                             win, 0, 0, 0, NULL);
    } else {
        if (win_w != wm.screen_w || win_h != wm.screen_h) {
            /* The screen itself changed size (RandR) -- rare, and the only
             * thing that ever reconfigures this window. */
            uint32_t geo[] = { (uint32_t)wm.screen_w, (uint32_t)wm.screen_h };
            xcb_configure_window(wm.conn, win,
                                 XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, geo);
            win_w = wm.screen_w;
            win_h = wm.screen_h;
        }
        if (!have_pixel || pixel != current_pixel) {
            /* Only when the theme's color actually changed under us. */
            xcb_change_window_attributes(wm.conn, win, XCB_CW_BACK_PIXEL, &pixel);
            xcb_clear_area(wm.conn, 0, win, 0, 0, 0, 0);
            current_pixel = pixel;
            have_pixel = true;
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
                         win, 0, 0, (uint32_t)n, rects);

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
