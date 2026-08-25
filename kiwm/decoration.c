/* Decoration: Cairo + Imlib2. Loads an optional theme from wm.theme_path
 * (kiwm.conf's theme=, default "greenxp" -- the same folder xispanel's own
 * theme backgrounds use, sharing its bg.png + 9-slice "slice" sidecar
 * convention) -- bg.png/slice, btns.png/btns.slice (window-control button
 * sprite sheet) and colors (per-focus titlebar/border colors). Every piece
 * loads independently; whatever isn't found just falls back to a plain
 * flat kiwm.conf-configured look, there's no all-or-nothing theme
 * requirement. */
#include "decoration.h"
#include "wm.h"

#include <cairo/cairo-xcb.h>
#include <xcb/shape.h>
#include <Imlib2.h>

#include <math.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static cairo_surface_t *load_png_argb(const char *path)
{
    Imlib_Image img = imlib_load_image(path);
    if (!img)
        return NULL;

    imlib_context_set_image(img);
    int iw = imlib_image_get_width();
    int ih = imlib_image_get_height();
    if (iw <= 0 || ih <= 0 || iw > 4096 || ih > 4096) {
        imlib_free_image();
        return NULL;
    }

    DATA32 *src = imlib_image_get_data_for_reading_only();
    if (!src) {
        imlib_free_image();
        return NULL;
    }

    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, iw, ih);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        imlib_free_image();
        return NULL;
    }

    unsigned char *dst = cairo_image_surface_get_data(surf);
    int stride = cairo_image_surface_get_stride(surf);
    for (int y = 0; y < ih; y++) {
        uint32_t *row = (uint32_t *)(void *)(dst + y * stride);
        for (int x = 0; x < iw; x++) {
            uint32_t argb = src[y * iw + x];
            uint8_t a = (uint8_t)((argb >> 24) & 0xff);
            uint8_t r = (uint8_t)((argb >> 16) & 0xff);
            uint8_t g = (uint8_t)((argb >> 8) & 0xff);
            uint8_t b = (uint8_t)(argb & 0xff);
            r = (uint8_t)((r * a) / 255);
            g = (uint8_t)((g * a) / 255);
            b = (uint8_t)((b * a) / 255);
            row[x] = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        }
    }
    cairo_surface_mark_dirty(surf);

    imlib_free_image();
    return surf;
}

/* Resolves <wm.theme_path>/<name> (kiwm.conf's theme=, default "greenxp")
 * against the same three relative-location conventions the old hardcoded
 * bg.png search used ($KIWM_DECO_BG only replaces bg.png itself, not the
 * whole theme folder). Returns true and fills `out` with the first
 * candidate that actually exists, or false if none do -- caller decides
 * what "not found" means (individual pieces just fall back on their own). */
static bool find_theme_file(const char *name, char *out, size_t outsz)
{
    const char *prefixes[] = { "..", ".", "" };
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
        if (prefixes[i][0])
            snprintf(out, outsz, "%s/%s/%s", prefixes[i], wm.theme_path, name);
        else
            snprintf(out, outsz, "%s/%s", wm.theme_path, name);
        FILE *f = fopen(out, "r");
        if (f) {
            fclose(f);
            return true;
        }
    }
    return false;
}

/* "#rrggbb" (leading '#' optional) -> 0..1 doubles. Same format as
 * kiwm.conf's deco_bg=/deco_fg=/border_color=. */
static bool parse_hex_color(const char *s, double *r, double *g, double *b)
{
    if (s[0] == '#')
        s++;
    unsigned int ri, gi, bi;
    if (sscanf(s, "%2x%2x%2x", &ri, &gi, &bi) != 3)
        return false;
    *r = ri / 255.0;
    *g = gi / 255.0;
    *b = bi / 255.0;
    return true;
}

/* Sidecar "measurements" file for a 9-slice bg_image: plain key=value
 * lines, same format/spirit as xispanel's own bg.png/slice loader.
 * Missing file or missing keys just default that inset to 0 -- a
 * 0-everywhere slice degrades to a plain full-image stretch. */
static void load_bg_slice_file(const char *path, int *l, int *t, int *r, int *b)
{
    *l = *t = *r = *b = 0;
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    char line[128];
    int v;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "left=%d", &v) == 1) *l = v;
        else if (sscanf(line, "top=%d", &v) == 1) *t = v;
        else if (sscanf(line, "right=%d", &v) == 1) *r = v;
        else if (sscanf(line, "bottom=%d", &v) == 1) *b = v;
    }
    fclose(f);
}

static void load_bg_theme(void)
{
    const char *env = getenv("KIWM_DECO_BG");
    char path[512];

    if (env && env[0]) {
        wm.deco_bg = load_png_argb(env);
        if (wm.deco_bg)
            fprintf(stderr, "kiwm: decoration background loaded from '%s'\n", env);
    }
    if (!wm.deco_bg && find_theme_file("bg.png", path, sizeof(path))) {
        wm.deco_bg = load_png_argb(path);
        if (wm.deco_bg)
            fprintf(stderr, "kiwm: decoration background loaded from '%s'\n", path);
    }
    if (!wm.deco_bg) {
        fprintf(stderr, "kiwm: could not load %s/bg.png, falling back to flat color decoration\n",
                wm.theme_path);
        return;
    }

    if (find_theme_file("slice", path, sizeof(path)))
        load_bg_slice_file(path, &wm.bg_slice_l, &wm.bg_slice_t, &wm.bg_slice_r, &wm.bg_slice_b);
}

/* btns.png + btns.slice: a fixed 7-column (BTNCOL_*) x 3-row
 * (normal/hover/clicked) grid, not a 9-slice -- see wm.h's BTNCOL_*
 * comment and greenxp/btns.slice. */
static void load_btn_theme(void)
{
    char path[512];
    if (!find_theme_file("btns.png", path, sizeof(path)))
        return;
    wm.deco_btns = load_png_argb(path);
    if (!wm.deco_btns)
        return;

    wm.btn_cell_w = BUTTON_W;
    wm.btn_cell_h = TITLEBAR_H;
    if (find_theme_file("btns.slice", path, sizeof(path))) {
        FILE *f = fopen(path, "r");
        if (f) {
            char line[128];
            int v;
            while (fgets(line, sizeof(line), f)) {
                if (sscanf(line, "cell_width=%d", &v) == 1) wm.btn_cell_w = v;
                else if (sscanf(line, "cell_height=%d", &v) == 1) wm.btn_cell_h = v;
            }
            fclose(f);
        }
    }
    fprintf(stderr, "kiwm: button theme loaded from '%s' (%dx%d cells)\n",
            path, wm.btn_cell_w, wm.btn_cell_h);
}

static void load_colors_theme(void)
{
    char path[512];
    if (!find_theme_file("colors", path, sizeof(path)))
        return;
    FILE *f = fopen(path, "r");
    if (!f)
        return;

    wm.have_theme_colors = true;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        char *nl = strpbrk(line, "\r\n");
        if (nl) *nl = '\0';
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#')
            continue;
        *eq = '\0';
        const char *key = line;
        const char *val = eq + 1;

        if (strcmp(key, "bg_active") == 0)
            parse_hex_color(val, &wm.bg_active_r, &wm.bg_active_g, &wm.bg_active_b);
        else if (strcmp(key, "bg_inactive") == 0)
            parse_hex_color(val, &wm.bg_inactive_r, &wm.bg_inactive_g, &wm.bg_inactive_b);
        else if (strcmp(key, "fg_active") == 0)
            parse_hex_color(val, &wm.fg_active_r, &wm.fg_active_g, &wm.fg_active_b);
        else if (strcmp(key, "fg_inactive") == 0)
            parse_hex_color(val, &wm.fg_inactive_r, &wm.fg_inactive_g, &wm.fg_inactive_b);
        else if (strcmp(key, "border_active") == 0)
            parse_hex_color(val, &wm.border_active_r, &wm.border_active_g, &wm.border_active_b);
        else if (strcmp(key, "border_inactive") == 0)
            parse_hex_color(val, &wm.border_inactive_r, &wm.border_inactive_g, &wm.border_inactive_b);
        else if (strcmp(key, "border_radius") == 0) {
            int a = 0, b = 0, cc = 0, d = 0;
            int n = sscanf(val, "%d %d %d %d", &a, &b, &cc, &d);
            if (n == 1) {
                wm.radius_tl = wm.radius_tr = wm.radius_br = wm.radius_bl = a;
            } else if (n == 2) {
                wm.radius_tl = wm.radius_tr = a;
                wm.radius_bl = wm.radius_br = b;
            } else if (n == 4) {
                wm.radius_tl = a; wm.radius_tr = b; wm.radius_br = cc; wm.radius_bl = d;
            } else {
                fprintf(stderr, "kiwm: config: invalid border_radius '%s' "
                                "(expected 1, 2, or 4 numbers)\n", val);
            }
            if (wm.radius_tl < 0) wm.radius_tl = 0;
            if (wm.radius_tr < 0) wm.radius_tr = 0;
            if (wm.radius_br < 0) wm.radius_br = 0;
            if (wm.radius_bl < 0) wm.radius_bl = 0;
        } else if (strcmp(key, "round_maximized") == 0) {
            wm.round_maximized = atoi(val) != 0;
        }
    }
    fclose(f);
    fprintf(stderr, "kiwm: theme colors loaded from '%s'\n", path);
}

/* _NET_WM_ICON: one CARDINAL array, potentially holding *several*
 * "width,height,pixels..." icons back to back (pixels are 0xAARRGGBB,
 * one CARDINAL each, straight, not premultiplied -- same swizzle as
 * load_png_argb() above needs). Picks whichever available size is
 * closest to (preferring at least as big as) the titlebar icon slot. */
void load_client_icon(Client *c)
{
    if (c->icon) {
        cairo_surface_destroy(c->icon);
        c->icon = NULL;
    }

    xcb_get_property_reply_t *reply = xcb_get_property_reply(wm.conn,
        xcb_get_property(wm.conn, 0, c->window, wm.atoms.net_wm_icon, XCB_ATOM_CARDINAL, 0, 65536), NULL);
    if (!reply)
        return;
    if (reply->type != XCB_ATOM_CARDINAL || reply->format != 32) {
        free(reply);
        return;
    }

    uint32_t *data = xcb_get_property_value(reply);
    long len = xcb_get_property_value_length(reply) / 4;
    int target = BUTTON_W;

    long best_off = -1;
    int best_w = 0, best_h = 0;
    long i = 0;
    while (i + 2 <= len) {
        uint32_t w = data[i], h = data[i + 1];
        if (w == 0 || h == 0 || w > 512 || h > 512)
            break; /* malformed -- bail rather than read garbage as a size */
        long need = (long)w * (long)h;
        if (i + 2 + need > len)
            break;

        bool better;
        if (best_off < 0)
            better = true;
        else if ((int)w >= target && best_w >= target)
            better = (int)w < best_w;       /* both big enough: prefer the smaller one */
        else if ((int)w >= target)
            better = true;                  /* this one's big enough, current pick isn't */
        else if (best_w < target)
            better = (int)w > best_w;       /* neither is big enough: prefer the bigger one */
        else
            better = false;

        if (better) {
            best_off = i + 2;
            best_w = (int)w;
            best_h = (int)h;
        }
        i += 2 + need;
    }

    if (best_off < 0) {
        free(reply);
        return;
    }

    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, best_w, best_h);
    if (cairo_surface_status(surf) == CAIRO_STATUS_SUCCESS) {
        unsigned char *dst = cairo_image_surface_get_data(surf);
        int stride = cairo_image_surface_get_stride(surf);
        for (int y = 0; y < best_h; y++) {
            uint32_t *row = (uint32_t *)(void *)(dst + y * stride);
            for (int x = 0; x < best_w; x++) {
                uint32_t argb = data[best_off + (long)y * best_w + x];
                uint8_t a = (uint8_t)((argb >> 24) & 0xff);
                uint8_t r = (uint8_t)((argb >> 16) & 0xff);
                uint8_t g = (uint8_t)((argb >> 8) & 0xff);
                uint8_t b = (uint8_t)(argb & 0xff);
                r = (uint8_t)((r * a) / 255);
                g = (uint8_t)((g * a) / 255);
                b = (uint8_t)((b * a) / 255);
                row[x] = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
            }
        }
        cairo_surface_mark_dirty(surf);
        c->icon = surf;
    } else {
        cairo_surface_destroy(surf);
    }

    free(reply);
}

void load_decoration(void)
{
    /* Colors default to the plain kiwm.conf fallback fields (already set
     * by config_load()) until/unless the colors file overrides them --
     * so a theme missing some keys degrades to those, not to black. */
    wm.bg_active_r = wm.bg_inactive_r = wm.deco_bg_r;
    wm.bg_active_g = wm.bg_inactive_g = wm.deco_bg_g;
    wm.bg_active_b = wm.bg_inactive_b = wm.deco_bg_b;
    wm.fg_active_r = wm.fg_inactive_r = wm.deco_fg_r;
    wm.fg_active_g = wm.fg_inactive_g = wm.deco_fg_g;
    wm.fg_active_b = wm.fg_inactive_b = wm.deco_fg_b;
    wm.border_active_r = wm.border_inactive_r = wm.border_r;
    wm.border_active_g = wm.border_inactive_g = wm.border_g;
    wm.border_active_b = wm.border_inactive_b = wm.border_b;
    wm.radius_tl = wm.radius_tr = wm.radius_br = wm.radius_bl = 0;
    wm.round_maximized = true;
    wm.hover_btn = -1;

    load_bg_theme();
    load_btn_theme();
    load_colors_theme();
}

bool client_deco_visible(Client *c)
{
    return !(c->maximized && wm.hide_deco_on_maximize);
}

/* Paints one source sub-rectangle [sx,sy,sw,sh] of `src` into one
 * destination rectangle [dx,dy,dw,dh] of `cr`, scaling to fit -- the one
 * building block every corner/edge/center region of a 9-slice draw
 * reduces to (ported from xispanel's panel_draw_9slice/draw_slice_region,
 * same algorithm, same slice-file format, so greenxp/ themes work
 * identically in both). */
static void draw_slice_region(cairo_t *cr, cairo_surface_t *src, int sx, int sy, int sw, int sh,
                              double dx, double dy, double dw, double dh)
{
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0)
        return;
    cairo_save(cr);
    cairo_translate(cr, dx, dy);
    cairo_scale(cr, dw / (double)sw, dh / (double)sh);
    cairo_set_source_surface(cr, src, -sx, -sy);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
    cairo_rectangle(cr, 0, 0, sw, sh);
    cairo_clip(cr);
    cairo_paint(cr);
    cairo_restore(cr);
}

static void draw_9slice(cairo_t *cr, cairo_surface_t *src, int sw, int sh, int l, int t, int r, int b,
                        double dw, double dh)
{
    if (l + r > sw) l = r = 0;
    if (t + b > sh) t = b = 0;
    int cw = sw - l - r;
    int ch = sh - t - b;
    double dcw = dw - l - r;
    double dch = dh - t - b;
    if (dcw < 0) dcw = 0;
    if (dch < 0) dch = 0;

    draw_slice_region(cr, src, 0, 0, l, t, 0, 0, l, t);
    draw_slice_region(cr, src, sw - r, 0, r, t, dw - r, 0, r, t);
    draw_slice_region(cr, src, 0, sh - b, l, b, 0, dh - b, l, b);
    draw_slice_region(cr, src, sw - r, sh - b, r, b, dw - r, dh - b, r, b);
    draw_slice_region(cr, src, l, 0, cw, t, l, 0, dcw, t);
    draw_slice_region(cr, src, l, sh - b, cw, b, l, dh - b, dcw, b);
    draw_slice_region(cr, src, 0, t, l, ch, 0, t, l, dch);
    draw_slice_region(cr, src, sw - r, t, r, ch, dw - r, t, r, dch);
    draw_slice_region(cr, src, l, t, cw, ch, l, t, dcw, dch);
}

/* Sane upper bound on a configured corner radius -- purely to keep the
 * rectangle list (and the one-row-per-pixel loop building it) from
 * blowing up if a theme's colors file has a typo like border_radius=5000.
 * No real titlebar needs a rounder corner than this. */
#define MAX_CORNER_RADIUS 128

/* How many pixels of a corner's own r x r square, at row `y` (0 = the
 * very outer edge row, r-1 = the innermost row, closest to the flat
 * middle), lie *outside* the inscribed quarter-circle -- i.e. how far
 * from that edge the visible shape starts at this row. 0 once y >= r
 * (this corner's arc has already ended, no clipping needed here even if
 * a taller corner on the *other* side of the same row still needs it). */
static int corner_inset(int r, int y)
{
    if (r <= 0 || y >= r)
        return 0;
    double dy = r - y;
    double dx = sqrt((double)r * r - dy * dy);
    int inset = r - (int)(dx + 0.5);
    if (inset < 0) inset = 0;
    if (inset > r) inset = r;
    return inset;
}

/* Builds a rounded-rectangle region for a w x h frame with the given
 * per-corner radii, as a list of xcb_rectangle_t suitable for
 * xcb_shape_rectangles() -- kiwm has no compositor to alpha-blend real
 * rounded corners, so this clips the window's bounding shape instead: a
 * real rounded corner (if slightly stair-stepped at very small radii),
 * no compositor required. One rectangle per corner-arc row plus one for
 * the flat middle band; straight rows away from any corner cost nothing
 * extra. Returns the number of rectangles written (never more than
 * 2*MAX_CORNER_RADIUS + 1). */
static int build_rounded_rects(int w, int h, int tl, int tr, int br, int bl,
                               xcb_rectangle_t *out, int max_out)
{
    if (tl > MAX_CORNER_RADIUS) tl = MAX_CORNER_RADIUS;
    if (tr > MAX_CORNER_RADIUS) tr = MAX_CORNER_RADIUS;
    if (br > MAX_CORNER_RADIUS) br = MAX_CORNER_RADIUS;
    if (bl > MAX_CORNER_RADIUS) bl = MAX_CORNER_RADIUS;

    int top_h = tl > tr ? tl : tr;
    int bot_h = bl > br ? bl : br;
    /* Radii bigger than the window itself would overlap top/bottom --
     * clamp both bands down proportionally rather than producing
     * nonsensical (negative-height) middle band math. */
    if (top_h + bot_h > h) {
        int excess = top_h + bot_h - h;
        int half = excess / 2 + (excess % 2);
        top_h -= half;
        bot_h -= half;
        if (top_h < 0) top_h = 0;
        if (bot_h < 0) bot_h = 0;
    }

    int n = 0;
    for (int y = 0; y < top_h && n < max_out; y++) {
        int li = corner_inset(tl, y);
        int ri = corner_inset(tr, y);
        int x0 = li, x1 = w - ri;
        if (x1 > x0)
            out[n++] = (xcb_rectangle_t){ (int16_t)x0, (int16_t)y, (uint16_t)(x1 - x0), 1 };
    }
    if (h - top_h - bot_h > 0 && n < max_out)
        out[n++] = (xcb_rectangle_t){ 0, (int16_t)top_h, (uint16_t)w, (uint16_t)(h - top_h - bot_h) };
    for (int yy = 0; yy < bot_h && n < max_out; yy++) {
        int row_from_bottom = bot_h - 1 - yy;
        int li = corner_inset(bl, row_from_bottom);
        int ri = corner_inset(br, row_from_bottom);
        int x0 = li, x1 = w - ri;
        int y = h - bot_h + yy;
        if (x1 > x0)
            out[n++] = (xcb_rectangle_t){ (int16_t)x0, (int16_t)y, (uint16_t)(x1 - x0), 1 };
    }
    return n;
}

/* Clips c->frame's bounding shape (XCB SHAPE extension) to a rounded
 * rectangle per wm.radius_tl/tr/br/bl (theme's colors file,
 * border_radius=), or resets it back to the plain rectangle if all four
 * are 0 -- called from client.c's configure_frame() every time the
 * frame's size changes, since the shape has to match exactly. A no-op
 * (not even the reset) when the server has no SHAPE extension at all. */
/* A window whose frame exactly matches its output's full rectangle --
 * which is also exactly what a future real fullscreen state would look
 * like, kiwm has no such state yet -- should obviously never be rounded:
 * rounding the very corners of the screen itself would just show
 * whatever's behind (typically the desktop background) poking through
 * the corners of an otherwise edge-to-edge window. Not configurable, on
 * purpose, unlike round_maximized. */
static bool client_fills_output(Client *c)
{
    if (c->output < 0 || c->output >= wm.output_count)
        return false;
    XisOutput *o = &wm.outputs[c->output];
    return c->x == o->x && c->y == o->y &&
           c->frame_width == o->width && c->frame_height == o->height;
}

void apply_rounded_shape(Client *c)
{
    if (!wm.shape_ext_present)
        return;

    bool square = (wm.radius_tl == 0 && wm.radius_tr == 0 && wm.radius_br == 0 && wm.radius_bl == 0);
    if (!square && c->maximized && !wm.round_maximized)
        square = true;
    if (!square && client_fills_output(c))
        square = true;

    if (square) {
        xcb_shape_mask(wm.conn, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_BOUNDING, c->frame, 0, 0, XCB_PIXMAP_NONE);
        return;
    }

    int w = c->frame_width, h = c->frame_height;
    if (w <= 0 || h <= 0)
        return;

    xcb_rectangle_t rects[2 * MAX_CORNER_RADIUS + 1];
    int n = build_rounded_rects(w, h, wm.radius_tl, wm.radius_tr, wm.radius_br, wm.radius_bl,
                               rects, (int)(sizeof(rects) / sizeof(rects[0])));

    xcb_shape_rectangles(wm.conn, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_BOUNDING, XCB_CLIP_ORDERING_Y_SORTED,
                         c->frame, 0, 0, (uint32_t)n, rects);
}

int compute_deco_layout(int frame_width, DecoSlot *out, int max_out)
{
    int n = wm.deco_layout_count;
    if (n > max_out)
        n = max_out;

    int fixed_total = 0;
    int title_idx = -1;
    for (int i = 0; i < n; i++) {
        if (wm.deco_layout[i] == DECO_TITLE) {
            if (title_idx < 0)
                title_idx = i;
        } else {
            fixed_total += BUTTON_W;
        }
    }
    int title_w = frame_width - fixed_total;
    if (title_w < 0)
        title_w = 0;

    int x = 0;
    for (int i = 0; i < n; i++) {
        out[i].kind = wm.deco_layout[i];
        out[i].x = x;
        if (wm.deco_layout[i] == DECO_TITLE) {
            /* Only the first "title" token (if the config lists more than
             * one, which is nonsensical but shouldn't crash) gets the
             * flexible width; any further one just collapses to 0. */
            out[i].width = (i == title_idx) ? title_w : 0;
            x += out[i].width;
        } else {
            out[i].width = BUTTON_W;
            x += BUTTON_W;
        }
    }
    return n;
}

/* `col` is a BTNCOL_* sprite column; `glyph` is the hand-drawn fallback
 * used when no btns.png theme loaded. `hovered` selects btns.png's hover
 * row; toggle buttons (keep_above/keep_all_desktops) also use that same
 * row whenever `active`, in lieu of a dedicated "on" row the sprite
 * doesn't have -- there's no "clicked" row use at all yet, kiwm fires
 * button actions directly on press with no separate held-down moment to
 * show one during (see wm.h's BTNCOL_* comment). */
static void draw_button(cairo_t *cr, double x, int col, char glyph, bool hovered, bool active)
{
    if (wm.deco_btns) {
        int row = (hovered || active) ? 1 : 0;
        cairo_save(cr);
        cairo_translate(cr, x, 0);
        cairo_rectangle(cr, 0, 0, BUTTON_W, TITLEBAR_H);
        cairo_clip(cr);
        draw_slice_region(cr, wm.deco_btns, col * wm.btn_cell_w, row * wm.btn_cell_h,
                          wm.btn_cell_w, wm.btn_cell_h, 0, 0, BUTTON_W, TITLEBAR_H);
        cairo_restore(cr);
        return;
    }

    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, (hovered || active) ? 0.45 : 0.30);
    cairo_rectangle(cr, x, 0, BUTTON_W, TITLEBAR_H);
    cairo_fill(cr);

    cairo_set_source_rgba(cr, 0.92, 0.92, 0.95, 1.0);
    cairo_set_line_width(cr, 1.5);

    double cx = x + BUTTON_W / 2.0;
    double cy = TITLEBAR_H / 2.0;

    if (glyph == '-') {
        cairo_move_to(cr, cx - 5, cy);
        cairo_line_to(cr, cx + 5, cy);
        cairo_stroke(cr);
    } else if (glyph == '+') {
        cairo_rectangle(cr, cx - 5, cy - 5, 10, 10);
        cairo_stroke(cr);
    } else if (glyph == 'r') {
        cairo_rectangle(cr, cx - 5, cy - 3, 8, 8);
        cairo_stroke(cr);
        cairo_rectangle(cr, cx - 2, cy - 6, 8, 8);
        cairo_stroke(cr);
    } else if (glyph == 'x') {
        cairo_move_to(cr, cx - 4, cy - 4);
        cairo_line_to(cr, cx + 4, cy + 4);
        cairo_move_to(cr, cx + 4, cy - 4);
        cairo_line_to(cr, cx - 4, cy + 4);
        cairo_stroke(cr);
    } else if (glyph == '^') {
        cairo_move_to(cr, cx - 5, cy + 2);
        cairo_line_to(cr, cx, cy - 3);
        cairo_line_to(cr, cx + 5, cy + 2);
        cairo_stroke(cr);
    } else if (glyph == 'a') { /* keep_above: pin/arrow pointing up */
        cairo_move_to(cr, cx, cy - 5);
        cairo_line_to(cr, cx, cy + 5);
        cairo_move_to(cr, cx - 4, cy - 1);
        cairo_line_to(cr, cx, cy - 5);
        cairo_line_to(cr, cx + 4, cy - 1);
        cairo_stroke(cr);
    } else if (glyph == 'd') { /* keep_all_desktops: 2x2 grid */
        double gap = 1.5, s = 4;
        cairo_rectangle(cr, cx - gap - s, cy - gap - s, s, s);
        cairo_rectangle(cr, cx + gap, cy - gap - s, s, s);
        cairo_rectangle(cr, cx - gap - s, cy + gap, s, s);
        cairo_rectangle(cr, cx + gap, cy + gap, s, s);
        cairo_stroke(cr);
    }
}

void draw_decoration(Client *c)
{
    if (!client_deco_visible(c))
        return;
    if (!wm.visual)
        return;

    int w = c->frame_width;
    int h = c->frame_height;
    if (w <= 0 || h <= 0)
        return;

    bool focused = (c == wm.focused);

    /* Render into an off-screen pixmap, not the frame directly: every
     * paint call below (background, focus tint, title text, each button)
     * used to land on the actual window the instant it was sent, so a
     * fast sequence of redraws (dragging/resizing) could show those
     * layers appearing one at a time -- visible flicker. Blitting the
     * finished pixmap in one xcb_copy_area() at the end instead makes
     * the whole update atomic from the X server's point of view. */
    xcb_pixmap_t pixmap = xcb_generate_id(wm.conn);
    xcb_create_pixmap(wm.conn, wm.screen->root_depth, pixmap, c->frame, (uint16_t)w, (uint16_t)h);

    cairo_surface_t *surface = cairo_xcb_surface_create(wm.conn, pixmap, wm.visual, w, h);
    cairo_t *cr = cairo_create(surface);

    /* Everything below is the titlebar strip only -- clip to it so the
     * theme image/tint doesn't stretch down over the side/bottom border
     * area drawn separately afterward. */
    cairo_save(cr);
    cairo_rectangle(cr, 0, 0, w, TITLEBAR_H);
    cairo_clip(cr);

    if (wm.deco_bg) {
        int iw = cairo_image_surface_get_width(wm.deco_bg);
        int ih = cairo_image_surface_get_height(wm.deco_bg);
        draw_9slice(cr, wm.deco_bg, iw, ih, wm.bg_slice_l, wm.bg_slice_t, wm.bg_slice_r, wm.bg_slice_b,
                   w, TITLEBAR_H);
    } else {
        /* No theme PNG (missing file, or no theme configured yet): flat
         * fallback color from kiwm.conf's deco_bg= (default black). */
        cairo_set_source_rgb(cr, wm.deco_bg_r, wm.deco_bg_g, wm.deco_bg_b);
        cairo_paint(cr);
    }

    /* Focus tint on top of the theme image: a real per-focus color from
     * greenxp/colors when loaded, else the old plain white/black opacity
     * tint (so a from-scratch install with no theme at all still shows
     * *some* focus/unfocus difference). */
    if (wm.have_theme_colors) {
        if (focused)
            cairo_set_source_rgba(cr, wm.bg_active_r, wm.bg_active_g, wm.bg_active_b, 0.55);
        else
            cairo_set_source_rgba(cr, wm.bg_inactive_r, wm.bg_inactive_g, wm.bg_inactive_b, 0.55);
    } else {
        if (focused)
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.10);
        else
            cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.35);
    }
    cairo_paint(cr);

    cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 12.5);

    DecoSlot slots[MAX_DECO_ELEMS];
    int nslots = compute_deco_layout(w, slots, MAX_DECO_ELEMS);

    for (int i = 0; i < nslots; i++) {
        DecoSlot *s = &slots[i];
        bool hovered = (wm.hover_client == c && wm.hover_btn == i);

        switch (s->kind) {
        case DECO_TITLE: {
            /* Buttons drawn earlier in the layout order leave cr's source
             * set to whatever color they last used -- always re-apply the
             * title color here rather than once up front, since which
             * elements come "before" the title in draw order depends on
             * the configured titlebar_layout=. */
            if (focused)
                cairo_set_source_rgb(cr, wm.fg_active_r, wm.fg_active_g, wm.fg_active_b);
            else
                cairo_set_source_rgb(cr, wm.fg_inactive_r, wm.fg_inactive_g, wm.fg_inactive_b);

            cairo_text_extents_t ext;
            cairo_text_extents(cr, c->title, &ext);
            double title_y = (TITLEBAR_H - ext.height) / 2.0 - ext.y_bearing;
            cairo_save(cr);
            cairo_rectangle(cr, s->x, 0, s->width, TITLEBAR_H);
            cairo_clip(cr);
            cairo_move_to(cr, s->x + 8.0, title_y);
            cairo_show_text(cr, c->title);
            cairo_restore(cr);
            break;
        }
        case DECO_ICON:
            if (c->icon) {
                int iw = cairo_image_surface_get_width(c->icon);
                int ih = cairo_image_surface_get_height(c->icon);
                double pad = 5.0;
                double size = TITLEBAR_H - pad * 2;
                draw_slice_region(cr, c->icon, 0, 0, iw, ih,
                                  s->x + (BUTTON_W - size) / 2.0, pad, size, size);
            }
            break;
        case DECO_SHADE:
            draw_button(cr, s->x, BTNCOL_SHADE, '^', hovered, false);
            break;
        case DECO_MINIMIZE:
            draw_button(cr, s->x, BTNCOL_MINIMIZE, '-', hovered, false);
            break;
        case DECO_MAXIMIZE:
            draw_button(cr, s->x, c->maximized ? BTNCOL_RESTORE : BTNCOL_MAXIMIZE,
                       c->maximized ? 'r' : '+', hovered, false);
            break;
        case DECO_CLOSE:
            draw_button(cr, s->x, BTNCOL_CLOSE, 'x', hovered, false);
            break;
        case DECO_KEEP_ABOVE:
            draw_button(cr, s->x, BTNCOL_KEEP_ABOVE, 'a', hovered, c->keep_above);
            break;
        case DECO_KEEP_ALL_DESKTOPS:
            draw_button(cr, s->x, BTNCOL_KEEP_ALL_DESKTOPS, 'd', hovered, c->sticky);
            break;
        }
    }

    cairo_restore(cr);

    /* Flat-colored left/right/bottom border (kiwm.conf's border_thickness=,
     * default 0 = no border, just the titlebar; color from greenxp/colors
     * when loaded, else border_color=). */
    int bt = wm.border_thickness;
    if (bt > 0 && h > TITLEBAR_H) {
        if (wm.have_theme_colors) {
            if (focused)
                cairo_set_source_rgb(cr, wm.border_active_r, wm.border_active_g, wm.border_active_b);
            else
                cairo_set_source_rgb(cr, wm.border_inactive_r, wm.border_inactive_g, wm.border_inactive_b);
        } else {
            cairo_set_source_rgb(cr, wm.border_r, wm.border_g, wm.border_b);
        }
        cairo_rectangle(cr, 0, TITLEBAR_H, bt, h - TITLEBAR_H);          /* left */
        cairo_rectangle(cr, w - bt, TITLEBAR_H, bt, h - TITLEBAR_H);    /* right */
        cairo_rectangle(cr, 0, h - bt, w, bt);                          /* bottom */
        cairo_fill(cr);
    }

    cairo_destroy(cr);
    cairo_surface_flush(surface);
    cairo_surface_destroy(surface);

    xcb_copy_area(wm.conn, pixmap, c->frame, wm.deco_gc, 0, 0, 0, 0, (uint16_t)w, (uint16_t)h);
    xcb_free_pixmap(wm.conn, pixmap);
}
