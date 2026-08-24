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
#include <Imlib2.h>

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
    }
    fclose(f);
    fprintf(stderr, "kiwm: theme colors loaded from '%s'\n", path);
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

/* `col` is a BTNCOL_* sprite column; `glyph` is the hand-drawn fallback
 * used when no btns.png theme loaded. `hovered` selects btns.png's hover
 * row -- there's no "clicked" row use yet, kiwm fires button actions
 * directly on press with no separate held-down moment to show one during
 * (see wm.h's BTNCOL_* comment). */
static void draw_button(cairo_t *cr, double x, int col, char glyph, bool hovered)
{
    if (wm.deco_btns) {
        int row = hovered ? 1 : 0;
        cairo_save(cr);
        cairo_translate(cr, x, 0);
        cairo_rectangle(cr, 0, 0, BUTTON_W, TITLEBAR_H);
        cairo_clip(cr);
        draw_slice_region(cr, wm.deco_btns, col * wm.btn_cell_w, row * wm.btn_cell_h,
                          wm.btn_cell_w, wm.btn_cell_h, 0, 0, BUTTON_W, TITLEBAR_H);
        cairo_restore(cr);
        return;
    }

    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, hovered ? 0.45 : 0.30);
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

    cairo_surface_t *surface = cairo_xcb_surface_create(wm.conn, c->frame, wm.visual, w, h);
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
    if (focused)
        cairo_set_source_rgb(cr, wm.fg_active_r, wm.fg_active_g, wm.fg_active_b);
    else
        cairo_set_source_rgb(cr, wm.fg_inactive_r, wm.fg_inactive_g, wm.fg_inactive_b);

    cairo_text_extents_t ext;
    cairo_text_extents(cr, c->title, &ext);
    double title_x = 8.0;
    double title_y = (TITLEBAR_H - ext.height) / 2.0 - ext.y_bearing;
    cairo_move_to(cr, title_x, title_y);
    cairo_show_text(cr, c->title);

    bool hover_shade = (wm.hover_client == c && wm.hover_btn == BTNSLOT_SHADE);
    bool hover_min = (wm.hover_client == c && wm.hover_btn == BTNSLOT_MINIMIZE);
    bool hover_max = (wm.hover_client == c && wm.hover_btn == BTNSLOT_MAXIMIZE);
    bool hover_close = (wm.hover_client == c && wm.hover_btn == BTNSLOT_CLOSE);

    draw_button(cr, w - BUTTON_W * 4, BTNCOL_SHADE, '^', hover_shade);
    draw_button(cr, w - BUTTON_W * 3, BTNCOL_MINIMIZE, '-', hover_min);
    draw_button(cr, w - BUTTON_W * 2, c->maximized ? BTNCOL_RESTORE : BTNCOL_MAXIMIZE,
               c->maximized ? 'r' : '+', hover_max);
    draw_button(cr, w - BUTTON_W,     BTNCOL_CLOSE, 'x', hover_close);

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
}
