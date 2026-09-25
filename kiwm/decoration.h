#ifndef KIWM_DECORATION_H
#define KIWM_DECORATION_H

#include "wm.h"

/* Sane upper bound on a configured corner radius -- purely to keep the
 * rectangle list build_rounded_rects() below builds (and the
 * one-row-per-pixel loop building it) from blowing up if a theme's colors
 * file has a typo like border_radius=5000. No real titlebar needs a
 * rounder corner than this; also sizes osd.c's own rects buffer since it
 * reuses build_rounded_rects() for the OSD window's chrome. */
#define MAX_CORNER_RADIUS 128

void load_decoration(void);
bool client_deco_visible(Client *c);
void draw_decoration(Client *c);

/* The decoration's painting on its own, into any Cairo context at any
 * scale -- what lets density.c render it a second time, densely, without
 * a second copy of the drawing code. `argb` says whether the surface has
 * an alpha channel to clear first. */
void paint_deco(Client *c, cairo_t *cr, int w, int h, bool focused, bool argb);

/* Resolved position/width of one titlebar element (see wm.h's
 * DecoElemKind/wm.deco_layout). */
typedef struct {
    DecoElemKind kind;
    int x, width;
} DecoSlot;

/* Fills `out` (up to max_out) with each titlebar element's on-screen
 * [x, x+width) span for a frame of the given width, in wm.deco_layout
 * order -- the single shared layout math events.c's hit-testing/hover and
 * decoration.c's drawing both build on, so they can never disagree about
 * where an element actually is. Elements invoking an action `c` doesn't
 * permit (see wm.h's Client::allow_*) are left out entirely, so the
 * returned count can be smaller than wm.deco_layout_count and the title
 * absorbs the freed width. `c` may be NULL for a client-independent
 * layout. Returns the number of slots filled. */
int compute_deco_layout(const Client *c, int frame_width, DecoSlot *out, int max_out);

/* Loads (or reloads) c->icon from _NET_WM_ICON, picking whichever of the
 * property's multiple embedded sizes is closest to the titlebar icon
 * slot's size without being tiny. Safe to call with no icon present
 * (leaves c->icon NULL, drawn blank) or repeatedly (replaces any
 * previous surface). */
void load_client_icon(Client *c);

/* Clips c->frame's bounding shape to a rounded rect per wm.radius_tl/tr/
 * br/bl (theme's colors file, border_radius=), or resets it to the plain
 * rectangle if all four are 0. Call whenever the frame's size changes
 * (see client.c's configure_frame()) -- a no-op if the server has no
 * XCB SHAPE extension. */
void apply_rounded_shape(Client *c);

/* Writes _KIWM_CORNER_RADIUS on c->frame with the four radii
 * apply_rounded_shape() just clipped the bounding shape to (frame-local
 * logical pixels), *only* when they differ from what is already there
 * (Client::published_radii/published_t*) -- a plain resize revisits this
 * on every frame of the drag with the same theme radii each time, and
 * without the cache that would be a ChangeProperty per frame for no
 * reason. A compositor doing X-DENSITY reads it to round a scaled-up
 * decoration analytically instead of stretching a 1x shape mask -- see
 * kicomp's renderer-gl.c.
 *
 * All-zero is a real value here, not "absent": it is exactly what a
 * square theme, or a maximized frame with round_maximized off, or a
 * frame filling its output, actually clips to -- a plain rectangle,
 * which is what the four zeros analytically describe too. What must
 * *not* come through here is a shape this doesn't describe at all: see
 * withdraw_corner_radii() below for that case. */
void publish_corner_radii(Client *c, int tl, int tr, int br, int bl);

/* Removes _KIWM_CORNER_RADIUS from c->frame (only sending the request if
 * one is actually there -- same cache as publish_corner_radii()), for a
 * shape this doesn't describe: a client-shaped window's silhouette
 * (shape.c), the one case where the frame's bounding shape is not a
 * rounded rect at all. Publishing all-zero there would tell a compositor
 * "this frame is a plain rectangle", which for a shaped client is
 * false -- VirtualBox's mini-toolbar is the standing example, a
 * screen-sized frame with a small bar carved out of it. Withdrawing
 * instead leaves the property genuinely absent, which is what tells a
 * compositor to fall back to the real shape mask. */
void withdraw_corner_radii(Client *c);

/* Builds a rounded-rectangle region for a w x h box with the given
 * per-corner radii, as a list of xcb_rectangle_t suitable for
 * xcb_shape_rectangles() -- the shared building block behind
 * apply_rounded_shape() above, also reused as-is by osd.c to shape its own
 * override-redirect OSD window the same way (no compositor needed there
 * either). One rectangle per corner-arc row plus one for the flat middle
 * band. Returns the number of rectangles written (never more than
 * 2*MAX_CORNER_RADIUS + 1, the size out[] must have room for). */
int build_rounded_rects(int w, int h, int tl, int tr, int br, int bl,
                        xcb_rectangle_t *out, int max_out);

/* Pango-backed title text drawing (pango_text.c) -- see that file's
 * comment. Call pango_text_init() once at startup (main.c's setup_wm()),
 * before the first draw_decoration(). */
void pango_text_init(const char *family);
void pango_show_text_boxed(cairo_t *cr, double x, double top_y, double box_h, double max_width_px, double size_px,
                            const char *text, bool center, double *out_w);

/* The same, for the titlebar's title element: also applies the theme's
 * font_weight=/font_style= and draws its title_shadow=/title_outline=
 * (see wm.h's title_* fields), and takes the fill color itself rather than
 * inheriting whatever cairo's source happens to be, since it has two other
 * colors of its own to set first. With every effect left at its default
 * this is pango_show_text_boxed() with an explicit color. */
void pango_show_title_text(cairo_t *cr, double x, double top_y, double box_h, double max_width_px,
                           double size_px, const char *text, bool center,
                           double fr, double fg, double fb);

#endif /* KIWM_DECORATION_H */
