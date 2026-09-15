/* findcursor.c - the shrinking-square "find the cursor" flash (see
 * findcursor.h). One override-redirect, screen-sized window, shaped to a
 * hollow square band exactly like outline.c's wireframe look -- same band
 * technique, same theme color and thickness (kiwm.conf's outline_width=)
 * -- except this one is *always* the wireframe, compositor or not. The
 * effect is a locate-the-pointer flash, not a preview of where a window
 * will land; a soft, animatable, compositor-filled rectangle would still
 * be visible, but a crisp band snapping shut on the cursor reads better
 * for this, so it deliberately skips outline.c's filled look for now (see
 * findcursor.h and outline.c's own comment on the two looks).
 *
 * The animation itself is four fixed keyframes across 500ms -- centered on
 * the pointer throughout, shrinking from a quarter of the screen's shorter
 * side down to a small square right at the cursor -- plus a short hold
 * before it's hidden, so the final position actually registers instead of
 * vanishing the instant it lands. Driven from the main loop exactly like
 * client.c's pending_expose: findcursor_timeout_ms() says how long until
 * the next keyframe (or the hide) is due, findcursor_run() fires it when
 * it is. */
#include "findcursor.h"
#include "client.h"

#include <xcb/shape.h>

#include <stdlib.h>

#define FINDCURSOR_STEPS 4
#define FINDCURSOR_DURATION_MS 500.0
#define FINDCURSOR_HOLD_MS 120.0

/* Keyframe sizes, as a fraction of the starting side length -- shrinks
 * fast at first and eases into the landing spot, closer to how the eye
 * actually tracks something collapsing onto a point than a plain linear
 * shrink would. */
static const double kFrac[FINDCURSOR_STEPS] = { 1.0, 0.55, 0.24, 0.08 };

static xcb_window_t win = XCB_NONE;
static bool visible = false;
static uint32_t win_pixel = 0;
static bool have_pixel = false;
static int win_w = 0, win_h = 0;   /* screen size the window was made for */

typedef struct {
    bool active;
    int cx, cy;          /* pointer position the squares stay centered on */
    int start_size;      /* side length of the first (biggest) keyframe */
    double t_start;
    int next;             /* index into kFrac[] of the next keyframe due, or
                            * FINDCURSOR_STEPS once every keyframe has been
                            * drawn and only the hide-then-hide is left */
} Anim;

static Anim anim;

/* Same four lines as decoration.c's border_channel_to_pixel() and
 * outline.c's channel_to_pixel() -- kept local rather than shared, per
 * that comment: short enough that duplicating it costs less than a new
 * cross-file dependency. */
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

/* The theme color outline.c's wireframe band uses -- same source fields,
 * duplicated rather than exported for the same reason as channel_to_pixel()
 * above. */
static uint32_t band_color_pixel(void)
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

static void mark_window(void)
{
    xcb_shape_rectangles(wm.conn, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_INPUT, XCB_CLIP_ORDERING_UNSORTED,
                         win, 0, 0, 0, NULL);
    xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE, win, wm.atoms.kiwm_layer,
                        XCB_ATOM_STRING, 8, 7, "outline");
}

static void ensure_window(void)
{
    uint32_t pixel = band_color_pixel();

    if (win == XCB_NONE) {
        win = xcb_generate_id(wm.conn);
        uint32_t values[] = { pixel, 1, XCB_EVENT_MASK_EXPOSURE };
        xcb_create_window(wm.conn, wm.screen->root_depth, win, wm.root,
                          0, 0, (uint16_t)wm.screen_w, (uint16_t)wm.screen_h, 0,
                          XCB_WINDOW_CLASS_INPUT_OUTPUT, wm.screen->root_visual,
                          XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK, values);
        win_w = wm.screen_w;
        win_h = wm.screen_h;
        win_pixel = pixel;
        have_pixel = true;
        mark_window();
        return;
    }

    if (win_w != wm.screen_w || win_h != wm.screen_h) {
        uint32_t geo[] = { (uint32_t)wm.screen_w, (uint32_t)wm.screen_h };
        xcb_configure_window(wm.conn, win, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, geo);
        win_w = wm.screen_w;
        win_h = wm.screen_h;
    }
    if (!have_pixel || pixel != win_pixel) {
        xcb_change_window_attributes(wm.conn, win, XCB_CW_BACK_PIXEL, &pixel);
        xcb_clear_area(wm.conn, 0, win, 0, 0, 0, 0);
        win_pixel = pixel;
        have_pixel = true;
    }
}

/* The hollow square band around a `size`-by-`size` box centered on
 * (cx, cy), as up to four rectangles (top, bottom, left, right) in screen
 * (== window) coordinates -- same construction as outline.c's band_rects(),
 * just for a single centered square instead of an arbitrary rect list. */
static int band_rects(int cx, int cy, int size, xcb_rectangle_t *out)
{
    int band = wm.outline_width;
    if (band < 1)
        band = 1;
    int ox = cx - size / 2;
    int oy = cy - size / 2;

    int inner = size - 2 * band;
    if (inner <= 0) {
        out[0] = (xcb_rectangle_t){ (int16_t)ox, (int16_t)oy, (uint16_t)size, (uint16_t)size };
        return 1;
    }

    out[0] = (xcb_rectangle_t){ (int16_t)ox, (int16_t)oy, (uint16_t)size, (uint16_t)band };
    out[1] = (xcb_rectangle_t){ (int16_t)ox, (int16_t)(oy + size - band), (uint16_t)size, (uint16_t)band };
    out[2] = (xcb_rectangle_t){ (int16_t)ox, (int16_t)(oy + band), (uint16_t)band, (uint16_t)inner };
    out[3] = (xcb_rectangle_t){ (int16_t)(ox + size - band), (int16_t)(oy + band), (uint16_t)band, (uint16_t)inner };
    return 4;
}

static void draw_keyframe(int index)
{
    int size = (int)(anim.start_size * kFrac[index] + 0.5);
    /* Never collapse past the point where the band would fill solid --
     * the whole point of the last keyframe is a small hollow square
     * sitting on the cursor, not a filled blob. */
    int min_size = 4 * wm.outline_width;
    if (size < min_size)
        size = min_size;

    ensure_window();

    xcb_rectangle_t rects[4];
    int n = band_rects(anim.cx, anim.cy, size, rects);
    xcb_shape_rectangles(wm.conn, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_BOUNDING, XCB_CLIP_ORDERING_UNSORTED,
                         win, 0, 0, (uint32_t)n, rects);

    if (!visible) {
        xcb_map_window(wm.conn, win);
        visible = true;
        restack_all();
    }
    xcb_flush(wm.conn);
}

static void hide_window(void)
{
    if (!visible)
        return;
    xcb_unmap_window(wm.conn, win);
    visible = false;
    xcb_flush(wm.conn);
}

/* When keyframe `index` (0-based) is due, relative to t_start. */
static double keyframe_due(int index)
{
    return anim.t_start + FINDCURSOR_DURATION_MS * index / (FINDCURSOR_STEPS - 1);
}

void findcursor_trigger(void)
{
    xcb_query_pointer_reply_t *qp =
        xcb_query_pointer_reply(wm.conn, xcb_query_pointer(wm.conn, wm.root), NULL);
    if (!qp)
        return;

    anim.active = true;
    anim.cx = qp->root_x;
    anim.cy = qp->root_y;
    free(qp);

    int shorter = wm.screen_w < wm.screen_h ? wm.screen_w : wm.screen_h;
    anim.start_size = shorter / 4;
    anim.t_start = monotonic_ms();
    anim.next = 1;   /* the first keyframe is drawn immediately, below */

    draw_keyframe(0);
}

int findcursor_timeout_ms(void)
{
    if (!anim.active)
        return -1;

    double due = (anim.next < FINDCURSOR_STEPS)
                     ? keyframe_due(anim.next)
                     : keyframe_due(FINDCURSOR_STEPS - 1) + FINDCURSOR_HOLD_MS;
    double left = due - monotonic_ms();
    if (left < 0)
        left = 0;
    return (int)left;
}

void findcursor_run(void)
{
    if (!anim.active)
        return;

    if (anim.next < FINDCURSOR_STEPS) {
        if (monotonic_ms() < keyframe_due(anim.next))
            return;
        draw_keyframe(anim.next);
        anim.next++;
        return;
    }

    if (monotonic_ms() < keyframe_due(FINDCURSOR_STEPS - 1) + FINDCURSOR_HOLD_MS)
        return;
    hide_window();
    anim.active = false;
}

bool findcursor_owns_window(xcb_window_t window)
{
    return visible && win != XCB_NONE && window == win;
}

void findcursor_handle_expose(xcb_window_t window)
{
    (void)window;
    /* Nothing to redraw -- the background pixel is the content, and the
     * server has already refilled whatever was exposed (outline.c's
     * outline_handle_expose() explains the same thing). */
}
