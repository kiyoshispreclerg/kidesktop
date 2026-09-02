/* Decoration: Cairo + Imlib2. Loads an optional theme from wm.theme_path
 * (kiwm.conf's theme=, default "greenxp" -- the same folder xispanel's own
 * theme backgrounds use, sharing its bg.png + 9-slice "slice" sidecar
 * convention) -- bg.png/slice, btns.png/btns.slice (window-control button
 * sprite sheet) and colors (per-focus titlebar/border colors). Every piece
 * loads independently; whatever isn't found just falls back to a plain
 * flat kiwm.conf-configured look, there's no all-or-nothing theme
 * requirement. */
#include "decoration.h"
#include "density.h"
#include "wm.h"

#include <cairo/cairo-xcb.h>
#include <xcb/shape.h>
#include <Imlib2.h>

#include <math.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

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

/* "#rrggbb" or "#rrggbbaa" (leading '#' optional) -> 0..1 doubles. Same
 * format as kiwm.conf's deco_bg=/deco_fg=/border_color=. */
static bool parse_hex_color(const char *s, double *r, double *g, double *b, double *a)
{
    if (s[0] == '#')
        s++;

    /* #rrggbb or #rrggbbaa -- the alpha is optional and defaults to fully
     * opaque, so every color written before it existed keeps meaning
     * exactly what it did. */
    unsigned int ri, gi, bi, ai = 255;
    int n = sscanf(s, "%2x%2x%2x%2x", &ri, &gi, &bi, &ai);
    if (n < 3)
        return false;
    if (n == 3)
        ai = 255;

    *r = ri / 255.0;
    *g = gi / 255.0;
    *b = bi / 255.0;
    if (a)
        *a = ai / 255.0;
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

/* "bold", "semibold", "light", ... or a raw Pango weight number
 * (100..1000) -> PangoWeight. Named after the CSS/Pango names a theme
 * author would reach for; anything unrecognized keeps normal, with a
 * warning, like every other bad value in this file. */
static int parse_font_weight(const char *val)
{
    static const struct { const char *name; int weight; } weights[] = {
        { "thin", 100 },       { "ultralight", 200 }, { "light", 300 },
        { "semilight", 350 },  { "book", 380 },       { "normal", 400 },
        { "regular", 400 },    { "medium", 500 },     { "semibold", 600 },
        { "bold", 700 },       { "ultrabold", 800 },  { "heavy", 900 },
        { "black", 900 },
    };
    for (size_t i = 0; i < sizeof(weights) / sizeof(weights[0]); i++)
        if (strcasecmp(val, weights[i].name) == 0)
            return weights[i].weight;

    int n = atoi(val);
    if (n >= 100 && n <= 1000)
        return n;

    fprintf(stderr, "kiwm: theme: unknown font_weight '%s' (expected e.g. normal, bold, "
                    "or 100-1000), keeping normal\n", val);
    return 400;
}

/* normal / italic / oblique -> PangoStyle (0/2/1 -- the enum's own order,
 * spelled out here so this file doesn't have to include Pango just for
 * three integers; pango_text.c casts them back). */
static int parse_font_style(const char *val)
{
    if (strcasecmp(val, "normal") == 0)  return 0;
    if (strcasecmp(val, "oblique") == 0) return 1;
    if (strcasecmp(val, "italic") == 0)  return 2;
    fprintf(stderr, "kiwm: theme: unknown font_style '%s' (expected normal, italic or oblique), "
                    "keeping normal\n", val);
    return 0;
}

/* A color that also acts as its effect's on/off switch: "none" (or an
 * empty value) turns the effect off, a parseable color turns it on. That
 * way a theme disables a shadow by clearing the line rather than needing a
 * separate title_shadow_enabled= key next to it. */
static bool parse_effect_color(const char *val, double *r, double *g, double *b, double *a)
{
    if (!val[0] || strcasecmp(val, "none") == 0 || strcasecmp(val, "off") == 0)
        return false;
    if (!parse_hex_color(val, r, g, b, a)) {
        fprintf(stderr, "kiwm: theme: invalid color '%s' (expected #rrggbb, #rrggbbaa or none)\n", val);
        return false;
    }
    return true;
}

/* "dx dy", or a single number used for both axes. Negatives are fine --
 * that's a shadow cast up and/or to the left. */
static void parse_offset(const char *val, double *dx, double *dy)
{
    double x = 0, y = 0;
    int n = sscanf(val, "%lf %lf", &x, &y);
    if (n < 1) {
        fprintf(stderr, "kiwm: theme: invalid offset '%s' (expected \"dx dy\")\n", val);
        return;
    }
    *dx = x;
    *dy = (n == 2) ? y : x;
}

/* "close_button_tint" / "maximize_button_tint" / ... -> the matching
 * DecoElemKind slot in wm.btn_tint_*. Returns false when `key` isn't one
 * of these at all, so the caller's key chain just carries on. The names
 * are the titlebar_layout= element names plus "_button_tint", so a theme
 * writes the same word it already uses to place the button. */
static bool parse_button_tint(const char *key, const char *val)
{
    static const struct { const char *name; DecoElemKind kind; } tints[] = {
        { "close_button_tint",             DECO_CLOSE },
        { "maximize_button_tint",          DECO_MAXIMIZE },
        { "minimize_button_tint",          DECO_MINIMIZE },
        { "shade_button_tint",             DECO_SHADE },
        { "keep_above_button_tint",        DECO_KEEP_ABOVE },
        { "keep_all_desktops_button_tint", DECO_KEEP_ALL_DESKTOPS },
    };
    for (size_t i = 0; i < sizeof(tints) / sizeof(tints[0]); i++) {
        if (strcmp(key, tints[i].name) != 0)
            continue;
        int k = (int)tints[i].kind;
        /* Same "the color is the switch" rule the title effects use:
         * clearing the line (or "none") turns this button's tint off. */
        wm.btn_tint_set[k] = parse_effect_color(val, &wm.btn_tint_r[k], &wm.btn_tint_g[k],
                                                &wm.btn_tint_b[k], &wm.btn_tint_a[k]);
        return true;
    }
    return false;
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
    /* Opaque unless the file says otherwise: a colors file written before
     * alpha existed, or one that only gives some of its colors an alpha
     * channel, keeps meaning exactly what it did. */
    wm.bg_active_a = wm.bg_inactive_a = 1.0;
    wm.fg_active_a = wm.fg_inactive_a = 1.0;
    wm.border_active_a = wm.border_inactive_a = 1.0;

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
            parse_hex_color(val, &wm.bg_active_r, &wm.bg_active_g, &wm.bg_active_b, &wm.bg_active_a);
        else if (strcmp(key, "bg_inactive") == 0)
            parse_hex_color(val, &wm.bg_inactive_r, &wm.bg_inactive_g, &wm.bg_inactive_b, &wm.bg_inactive_a);
        else if (strcmp(key, "fg_active") == 0)
            parse_hex_color(val, &wm.fg_active_r, &wm.fg_active_g, &wm.fg_active_b, &wm.fg_active_a);
        else if (strcmp(key, "fg_inactive") == 0)
            parse_hex_color(val, &wm.fg_inactive_r, &wm.fg_inactive_g, &wm.fg_inactive_b, &wm.fg_inactive_a);
        else if (strcmp(key, "border_active") == 0)
            parse_hex_color(val, &wm.border_active_r, &wm.border_active_g, &wm.border_active_b, &wm.border_active_a);
        else if (strcmp(key, "border_inactive") == 0)
            parse_hex_color(val, &wm.border_inactive_r, &wm.border_inactive_g, &wm.border_inactive_b, &wm.border_inactive_a);
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
        } else if (strcmp(key, "font") == 0) {
            snprintf(wm.title_font, sizeof(wm.title_font), "%s", val);
        } else if (strcmp(key, "font_size") == 0) {
            double v = atof(val);
            if (v > 0)
                wm.title_font_size = v;
        } else if (strcmp(key, "title_center") == 0) {
            wm.title_center = atoi(val) != 0;
        } else if (strcmp(key, "font_weight") == 0) {
            wm.title_weight = parse_font_weight(val);
        } else if (strcmp(key, "font_style") == 0) {
            wm.title_style = parse_font_style(val);
        } else if (strcmp(key, "title_shadow") == 0) {
            wm.title_shadow = parse_effect_color(val, &wm.title_shadow_r, &wm.title_shadow_g,
                                                 &wm.title_shadow_b, &wm.title_shadow_a);
        } else if (strcmp(key, "title_shadow_offset") == 0) {
            parse_offset(val, &wm.title_shadow_dx, &wm.title_shadow_dy);
        } else if (strcmp(key, "title_outline") == 0) {
            wm.title_outline = parse_effect_color(val, &wm.title_outline_r, &wm.title_outline_g,
                                                  &wm.title_outline_b, &wm.title_outline_a);
        } else if (strcmp(key, "button_tinting") == 0) {
            if (strcasecmp(val, "none") == 0)         wm.btn_tinting = BTN_TINT_NONE;
            else if (strcasecmp(val, "over") == 0)    wm.btn_tinting = BTN_TINT_OVER;
            else if (strcasecmp(val, "replace") == 0) wm.btn_tinting = BTN_TINT_REPLACE;
            else
                fprintf(stderr, "kiwm: theme: unknown button_tinting '%s' (expected none, over "
                                "or replace), keeping current value\n", val);
        } else if (strcmp(key, "button_tint_scope") == 0) {
            if (strcasecmp(val, "button") == 0)          wm.btn_tint_scope = BTN_SCOPE_BUTTON;
            else if (strcasecmp(val, "decoration") == 0) wm.btn_tint_scope = BTN_SCOPE_DECORATION;
            else if (strcasecmp(val, "both") == 0)       wm.btn_tint_scope = BTN_SCOPE_BOTH;
            else
                fprintf(stderr, "kiwm: theme: unknown button_tint_scope '%s' (expected button, "
                                "decoration or both), keeping current value\n", val);
        } else if (parse_button_tint(key, val)) {
            /* <button>_button_tint= for each DecoElemKind -- handled by
             * name inside the helper, since there is one key per button
             * and they differ only in which slot they land in. */
        } else if (strcmp(key, "title_outline_width") == 0) {
            double v = atof(val);
            if (v < 0) v = 0;
            /* Past a few pixels the stroke stops being an outline and
             * starts being a blob with a letter somewhere inside it. */
            if (v > 8.0) v = 8.0;
            wm.title_outline_width = v;
        }
    }
    fclose(f);
    fprintf(stderr, "kiwm: theme colors loaded from '%s'\n", path);

    /* font= only takes effect once pango_text_init() (re-)builds its
     * PangoFontDescription from it -- see load_decoration()'s caller in
     * main.c, which calls that right after load_decoration() returns. */
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
    wm.round_maximized = false;
    wm.title_font[0] = '\0';   /* empty -- pango_text_init() falls back to "sans-serif" */
    wm.title_font_size = 12.5; /* the old hardcoded cairo_set_font_size() value */
    wm.title_center = false;
    wm.title_weight = 400;     /* PANGO_WEIGHT_NORMAL */
    wm.title_style = 0;        /* PANGO_STYLE_NORMAL */
    wm.title_shadow = false;
    wm.title_shadow_dx = wm.title_shadow_dy = 1.0;
    wm.title_outline = false;
    wm.title_outline_width = 1.0;
    for (int i = 0; i < DECO_KIND_COUNT; i++)
        wm.btn_tint_set[i] = false;
    wm.btn_tinting = BTN_TINT_OVER;
    wm.btn_tint_scope = BTN_SCOPE_BUTTON;
    wm.hover_btn = -1;

    load_bg_theme();
    load_btn_theme();
    load_colors_theme();
}

bool client_deco_visible(Client *c)
{
    /* Fullscreen always hides the decoration entirely -- not gated behind
     * wm.hide_deco_on_maximize the way maximize is, since a titlebar left
     * showing over a fullscreen video/game/browser would defeat the point
     * of the state regardless of theme preference. */
    if (c->fullscreen)
        return false;
    /* The client asked for no decoration at all (_MOTIF_WM_HINTS
     * decorations=0 / _KDE_NET_WM_WINDOW_TYPE_OVERRIDE, see wm.h's
     * Client::undecorated) -- honored unconditionally, same as fullscreen:
     * an app that draws its own window chrome ends up with two stacked
     * titlebars otherwise. */
    if (c->undecorated)
        return false;
    return !(client_maximized(c) && wm.hide_deco_on_maximize);
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

/* draw_slice_region()'s twin, painting the *current source* through that
 * region's alpha instead of the region's own pixels -- i.e. the sprite as
 * a stencil. That's what makes a button tint follow the button's actual
 * shape (a round Klassy-ish button stays round, and the transparent
 * corners of the cell stay transparent) rather than washing a rectangle
 * of color across the cell. */
static void mask_slice_region(cairo_t *cr, cairo_surface_t *src, int sx, int sy, int sw, int sh,
                              double dx, double dy, double dw, double dh)
{
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0)
        return;
    cairo_save(cr);
    cairo_translate(cr, dx, dy);
    cairo_scale(cr, dw / (double)sw, dh / (double)sh);

    cairo_pattern_t *p = cairo_pattern_create_for_surface(src);
    cairo_matrix_t m;
    /* Pattern space is source space: this is the same placement
     * cairo_set_source_surface(src, -sx, -sy) would give. */
    cairo_matrix_init_translate(&m, sx, sy);
    cairo_pattern_set_matrix(p, &m);
    cairo_pattern_set_filter(p, CAIRO_FILTER_BILINEAR);

    cairo_rectangle(cr, 0, 0, sw, sh);
    cairo_clip(cr);
    cairo_mask(cr, p);
    cairo_pattern_destroy(p);
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
int build_rounded_rects(int w, int h, int tl, int tr, int br, int bl,
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
 * which is also exactly the geometry client.c's toggle_fullscreen() sets --
 * should obviously never be rounded: rounding the very corners of the
 * screen itself would just show whatever's behind (typically the desktop
 * background) poking through the corners of an otherwise edge-to-edge
 * window. Not configurable, on purpose, unlike round_maximized. */
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
    if (!square && client_maximized(c) && !wm.round_maximized)
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

/* Whether this client permits the action a given titlebar element invokes
 * (see wm.h's Client::allow_*). An element for something the window
 * doesn't allow isn't drawn at all -- a fixed-size dialog showing a
 * maximize button that does nothing when clicked is worse than showing no
 * button, and it's what every other WM does with these hints. The elements
 * not listed here (title, icon, shade, keep-above, keep-all-desktops) are
 * always available: kiwm never restricts them. */
static bool deco_elem_allowed(const Client *c, DecoElemKind kind)
{
    if (!c)
        return true;
    switch (kind) {
    case DECO_MINIMIZE: return c->allow_minimize;
    case DECO_MAXIMIZE: return c->allow_maximize;
    case DECO_CLOSE:    return c->allow_close;
    /* Only where there is a menu to show *and* something configured to
     * show it with -- otherwise the slot would be a dead button on every
     * window that exports no menu (anything not Qt/KF5, most of the time)
     * and on a session with no appmenu_command= at all. */
    case DECO_APPMENU:  return wm.appmenu_command[0] && c->has_appmenu;
    default:            return true;
    }
}

int compute_deco_layout(const Client *c, int frame_width, DecoSlot *out, int max_out)
{
    DecoElemKind kinds[MAX_DECO_ELEMS];
    int n = 0;
    for (int i = 0; i < wm.deco_layout_count && n < max_out; i++)
        if (deco_elem_allowed(c, wm.deco_layout[i]))
            kinds[n++] = wm.deco_layout[i];

    int fixed_total = 0;
    int title_idx = -1;
    for (int i = 0; i < n; i++) {
        if (kinds[i] == DECO_TITLE) {
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
        out[i].kind = kinds[i];
        out[i].x = x;
        if (kinds[i] == DECO_TITLE) {
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
 * used when no btns.png theme loaded. `pressed` selects btns.png's third
 * ("clicked") row, `hovered` its second; see the row order in wm.h's
 * BTNCOL_* comment. `hovered` selects btns.png's hover
 * row; toggle buttons (keep_above/keep_all_desktops) also use that same
 * row whenever `active`, in lieu of a dedicated "on" row the sprite
 * doesn't have -- there's no "clicked" row use at all yet, kiwm fires
 * button actions directly on press with no separate held-down moment to
 * show one during (see wm.h's BTNCOL_* comment). */
static void draw_button(cairo_t *cr, double x, DecoElemKind kind, int col, char glyph,
                        bool hovered, bool active, bool pressed)
{
    /* The theme's per-button hover tint (see wm.h's btn_tint_*): applied
     * only while the pointer is on this button -- `active` is a toggle
     * being on, not a pointer state, so it keeps the theme's own look. */
    int k = (int)kind;
    bool tint = wm.btn_tinting != BTN_TINT_NONE && wm.btn_tint_scope != BTN_SCOPE_DECORATION &&
                (hovered || pressed) &&
                k >= 0 && k < DECO_KIND_COUNT && wm.btn_tint_set[k];
    bool tint_only = tint && wm.btn_tinting == BTN_TINT_REPLACE;

    if (wm.deco_btns) {
        /* Rows are normal=0, hover=1, clicked=2 top to bottom, but a
         * sheet is allowed to ship fewer than three -- fall back to the
         * last one it has rather than sampling past its bottom edge. */
        int rows = 1;
        if (wm.btn_cell_h > 0) {
            int sheet_h = cairo_image_surface_get_height(wm.deco_btns);
            rows = sheet_h / wm.btn_cell_h;
            if (rows < 1)
                rows = 1;
        }
        int row = pressed ? 2 : ((hovered || active) ? 1 : 0);
        if (row >= rows)
            row = rows - 1;

        /* A sheet drawn before this column existed (every theme, for the
         * appmenu button) simply doesn't have it -- sampling past its
         * right edge would draw whatever is at the sheet's edge, so fall
         * through to the hand-drawn glyph below instead. */
        int cols = wm.btn_cell_w > 0 ? cairo_image_surface_get_width(wm.deco_btns) / wm.btn_cell_w : 0;
        if (col >= cols)
            goto fallback_glyph;

        cairo_save(cr);
        cairo_translate(cr, x, 0);
        cairo_rectangle(cr, 0, 0, BUTTON_W, TITLEBAR_H);
        cairo_clip(cr);
        if (!tint_only)
            draw_slice_region(cr, wm.deco_btns, col * wm.btn_cell_w, row * wm.btn_cell_h,
                              wm.btn_cell_w, wm.btn_cell_h, 0, 0, BUTTON_W, TITLEBAR_H);
        if (tint) {
            cairo_set_source_rgba(cr, wm.btn_tint_r[k], wm.btn_tint_g[k],
                                  wm.btn_tint_b[k], wm.btn_tint_a[k]);
            mask_slice_region(cr, wm.deco_btns, col * wm.btn_cell_w, row * wm.btn_cell_h,
                              wm.btn_cell_w, wm.btn_cell_h, 0, 0, BUTTON_W, TITLEBAR_H);
        }
        cairo_restore(cr);
        return;
    }

fallback_glyph:
    /* No sprite sheet (or none covering this button): the button is a flat
     * block, so the tint is just that block's color -- over the plain one,
     * or instead of it. */
    if (!tint_only) {
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, pressed ? 0.60 : ((hovered || active) ? 0.45 : 0.30));
        cairo_rectangle(cr, x, 0, BUTTON_W, TITLEBAR_H);
        cairo_fill(cr);
    }
    if (tint) {
        cairo_set_source_rgba(cr, wm.btn_tint_r[k], wm.btn_tint_g[k],
                              wm.btn_tint_b[k], wm.btn_tint_a[k]);
        cairo_rectangle(cr, x, 0, BUTTON_W, TITLEBAR_H);
        cairo_fill(cr);
    }

    cairo_set_source_rgba(cr, 0.92, 0.92, 0.95, 1.0);
    cairo_set_line_width(cr, 1.5);

    double cx = x + BUTTON_W / 2.0;
    double cy = TITLEBAR_H / 2.0;

    if (glyph == 'm') {
        /* Hamburger: the appmenu button's fallback, for the themes (all of
         * them so far) whose sprite sheet has no column for it. */
        for (int i = -1; i <= 1; i++) {
            cairo_move_to(cr, cx - 5, cy + i * 4);
            cairo_line_to(cr, cx + 5, cy + i * 4);
        }
        cairo_stroke(cr);
    } else if (glyph == '-') {
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

/* The tint of whichever button of `c` the pointer is currently on, when
 * the theme asked for it to reach past that button (button_tint_scope=).
 * Returns false when nothing applies -- no pointer on this client, no
 * button under it, or that button has no tint of its own.
 *
 * This is what turns the whole frame red while the pointer sits on Close:
 * the color is the *button's*, the surface it lands on is the
 * decoration's. */
static bool decoration_hover_tint(Client *c, const DecoSlot *slots, int nslots,
                                  double *r, double *g, double *b, double *a)
{
    if (wm.btn_tinting == BTN_TINT_NONE || wm.btn_tint_scope == BTN_SCOPE_BUTTON)
        return false;
    if (wm.hover_client != c || wm.hover_btn < 0 || wm.hover_btn >= nslots)
        return false;

    int k = (int)slots[wm.hover_btn].kind;
    if (k < 0 || k >= DECO_KIND_COUNT || !wm.btn_tint_set[k])
        return false;

    *r = wm.btn_tint_r[k];
    *g = wm.btn_tint_g[k];
    *b = wm.btn_tint_b[k];
    *a = wm.btn_tint_a[k];
    return true;
}

/* Everything the decoration *is*, painted into whatever Cairo context it
 * is handed at whatever size that context implies.
 *
 * Split out of draw_decoration() so it can be run twice: once at the
 * frame's real size, for the frame itself, and once into a
 * density-scaled pixmap for a compositor doing per-monitor HiDPI scaling
 * (density.c). The second call gets a context with cairo_scale() already
 * applied, so the text is re-shaped and the shapes re-rasterized at the
 * bigger size instead of being magnified afterwards -- which is the whole
 * difference between a sharp titlebar and a blurry one.
 *
 * `argb` is whether the surface has a real alpha channel to clear. */
void paint_deco(Client *c, cairo_t *cr, int w, int h, bool focused, bool argb)
{
    /* A depth-32 pixmap starts as undefined *including* its alpha channel,
     * and everything painted below is composited OVER what's there -- so
     * on an ARGB frame the garbage would show through anywhere the theme
     * doesn't paint fully opaque. Start from honest transparency instead:
     * with a compositor the untouched parts (a themed titlebar's own
     * translucency, the area behind the client) are then genuinely
     * transparent, and without one the server just ignores the alpha as
     * it always has. Not done for root-depth frames: they have no alpha
     * channel and this would only be an extra full-surface paint. */
    if (argb) {
        cairo_save(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
        cairo_set_source_rgba(cr, 0, 0, 0, 0);
        cairo_paint(cr);
        cairo_restore(cr);
    }

    /* Needed before the background is painted, not just to place the
     * elements: which button the pointer is on decides whether the whole
     * decoration takes that button's tint (button_tint_scope=). */
    DecoSlot slots[MAX_DECO_ELEMS];
    int nslots = compute_deco_layout(c, w, slots, MAX_DECO_ELEMS);
    double dtr = 0, dtg = 0, dtb = 0, dta = 0;
    bool deco_tint = decoration_hover_tint(c, slots, nslots, &dtr, &dtg, &dtb, &dta);
    bool deco_tint_replace = deco_tint && wm.btn_tinting == BTN_TINT_REPLACE;

    /* Everything below is the titlebar strip only -- clip to it so the
     * theme image/tint doesn't stretch down over the side/bottom border
     * area drawn separately afterward. */
    cairo_save(cr);
    cairo_rectangle(cr, 0, 0, w, TITLEBAR_H);
    cairo_clip(cr);

    if (deco_tint_replace) {
        /* replace: the tint *is* the titlebar for as long as the pointer
         * is on that button -- no theme image, no focus tint under it.
         * The title and the buttons are still drawn on top afterwards, so
         * this recolors the frame rather than blanking it. */
        cairo_set_source_rgba(cr, dtr, dtg, dtb, dta);
        cairo_paint(cr);
    } else if (wm.deco_bg) {
        int iw = cairo_image_surface_get_width(wm.deco_bg);
        int ih = cairo_image_surface_get_height(wm.deco_bg);
        draw_9slice(cr, wm.deco_bg, iw, ih, wm.bg_slice_l, wm.bg_slice_t, wm.bg_slice_r, wm.bg_slice_b,
                   w, TITLEBAR_H);
    } else {
        /* No theme PNG (missing file, or no theme configured yet): flat
         * fallback color from kiwm.conf's deco_bg= (default black). */
        cairo_set_source_rgba(cr, wm.deco_bg_r, wm.deco_bg_g, wm.deco_bg_b, wm.deco_bg_a);
        cairo_paint(cr);
    }

    /* Focus tint on top of the theme image: a real per-focus color from
     * greenxp/colors when loaded, else the old plain white/black opacity
     * tint (so a from-scratch install with no theme at all still shows
     * *some* focus/unfocus difference). */
    if (deco_tint_replace) {
        /* Already the tint's own color -- the focus tint would only mud
         * it back towards the theme. */
    } else if (wm.have_theme_colors) {
        /* The color's own alpha multiplies the tint's: #rrggbb (opaque)
         * tints exactly as before, and a color given an alpha channel
         * tints proportionally less. */
        if (focused)
            cairo_set_source_rgba(cr, wm.bg_active_r, wm.bg_active_g, wm.bg_active_b,
                                  0.55 * wm.bg_active_a);
        else
            cairo_set_source_rgba(cr, wm.bg_inactive_r, wm.bg_inactive_g, wm.bg_inactive_b,
                                  0.55 * wm.bg_inactive_a);
    } else {
        if (focused)
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.10);
        else
            cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.35);
    }
    if (!deco_tint_replace)
        cairo_paint(cr);

    /* over: the theme is still there underneath, washed in the hovered
     * button's color. Painted before the title and the buttons so both
     * stay legible on top of it. */
    if (deco_tint && !deco_tint_replace) {
        cairo_set_source_rgba(cr, dtr, dtg, dtb, dta);
        cairo_paint(cr);
    }

    for (int i = 0; i < nslots; i++) {
        DecoSlot *s = &slots[i];
        bool hovered = (wm.hover_client == c && wm.hover_btn == i);
        /* Held down *and* the pointer still on it: dragging off an armed
         * button un-presses it (and moving back re-presses), the same way
         * every toolkit's buttons behave -- and the same rule that decides
         * whether releasing there actually fires the action (see events.c's
         * handle_button_release()). The hover_client check keeps this
         * working when there's no sprite theme to track hover for. */
        bool pressed = (wm.pressed_client == c && wm.pressed_btn == i) &&
                       (wm.hover_client != c || wm.hover_btn == i);

        switch (s->kind) {
        case DECO_TITLE: {
            /* Buttons drawn earlier in the layout order leave cr's source
             * set to whatever color they last used, and the title's own
             * shadow/outline set two more of their own -- so the fill
             * color is handed to pango_show_title_text() rather than left
             * on cr, and applied there last of the three.
             *
             * Always fully opaque, whatever alpha fg_active=/fg_inactive=
             * carry: an alpha channel on those is about the titlebar's own
             * translucency, and the window's name has to stay readable
             * over whatever shows through it. (The shadow and outline
             * colors do keep their alpha -- being able to lay a 50%-black
             * shadow over the titlebar is the point of having one.) */
            double tr, tg, tb;
            if (focused) {
                tr = wm.fg_active_r; tg = wm.fg_active_g; tb = wm.fg_active_b;
            } else {
                tr = wm.fg_inactive_r; tg = wm.fg_inactive_g; tb = wm.fg_inactive_b;
            }

            /* Pango handles both missing-glyph fallback (any script, not
             * just whatever the toy font API's single face covers) and
             * ellipsizing to the slot's width on its own -- see
             * pango_text.c's file comment. Centered (title_center=) uses
             * the slot's full width with no left pad, since Pango's own
             * alignment already balances the space on both sides. */
            if (wm.title_center)
                pango_show_title_text(cr, s->x, 0, TITLEBAR_H, s->width, wm.title_font_size,
                                      c->title, true, tr, tg, tb);
            else
                pango_show_title_text(cr, s->x + 8.0, 0, TITLEBAR_H, s->width - 8.0, wm.title_font_size,
                                      c->title, false, tr, tg, tb);
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
            draw_button(cr, s->x, DECO_SHADE, BTNCOL_SHADE, '^', hovered, false, pressed);
            break;
        case DECO_MINIMIZE:
            draw_button(cr, s->x, DECO_MINIMIZE, BTNCOL_MINIMIZE, '-', hovered, false, pressed);
            break;
        case DECO_MAXIMIZE:
            /* The "restore" look stands for any maximization, including a
             * single-axis one: in all of them a plain click is about
             * giving the state up rather than taking more of it. */
            draw_button(cr, s->x, DECO_MAXIMIZE,
                       (c->max_horz || c->max_vert) ? BTNCOL_RESTORE : BTNCOL_MAXIMIZE,
                       (c->max_horz || c->max_vert) ? 'r' : '+', hovered, false, pressed);
            break;
        case DECO_CLOSE:
            draw_button(cr, s->x, DECO_CLOSE, BTNCOL_CLOSE, 'x', hovered, false, pressed);
            break;
        case DECO_KEEP_ABOVE:
            draw_button(cr, s->x, DECO_KEEP_ABOVE, BTNCOL_KEEP_ABOVE, 'a', hovered, c->keep_above, pressed);
            break;
        case DECO_KEEP_ALL_DESKTOPS:
            draw_button(cr, s->x, DECO_KEEP_ALL_DESKTOPS, BTNCOL_KEEP_ALL_DESKTOPS, 'd', hovered, c->sticky, pressed);
            break;
        case DECO_APPMENU:
            draw_button(cr, s->x, DECO_APPMENU, BTNCOL_APPMENU, 'm', hovered, false, pressed);
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
                cairo_set_source_rgba(cr, wm.border_active_r, wm.border_active_g, wm.border_active_b,
                                      wm.border_active_a);
            else
                cairo_set_source_rgba(cr, wm.border_inactive_r, wm.border_inactive_g,
                                      wm.border_inactive_b, wm.border_inactive_a);
        } else {
            cairo_set_source_rgba(cr, wm.border_r, wm.border_g, wm.border_b, wm.border_a);
        }
        if (deco_tint_replace)
            cairo_set_source_rgba(cr, dtr, dtg, dtb, dta);
        cairo_rectangle(cr, 0, TITLEBAR_H, bt, h - TITLEBAR_H);          /* left */
        cairo_rectangle(cr, w - bt, TITLEBAR_H, bt, h - TITLEBAR_H);    /* right */
        cairo_rectangle(cr, 0, h - bt, w, bt);                          /* bottom */
        cairo_fill(cr);

        /* The border is part of the decoration, so it washes with it. */
        if (deco_tint && !deco_tint_replace) {
            cairo_set_source_rgba(cr, dtr, dtg, dtb, dta);
            cairo_rectangle(cr, 0, TITLEBAR_H, bt, h - TITLEBAR_H);
            cairo_rectangle(cr, w - bt, TITLEBAR_H, bt, h - TITLEBAR_H);
            cairo_rectangle(cr, 0, h - bt, w, bt);
            cairo_fill(cr);
        }
    }

}

void draw_decoration(Client *c)
{
    if (!client_deco_visible(c)) {
        /* Nothing to draw -- and nothing to *keep published* either. The
         * dense copy of the decoration is a pixmap of its own, so unlike
         * the frame it is not replaced by anything when this window is
         * maximized: left alone it stays on screen, at its old size, over
         * a window that is supposed to have no decoration at all
         * (density.h). */
        deco_density_hide(c);
        return;
    }
    /* Set in manage() -- the root visual for a normal client, the screen's
     * 32-bit one for an ARGB client (see wm.h's Client::frame_visual). */
    if (!c->frame_visual)
        return;

    int w = c->frame_width;
    int h = c->frame_height;
    if (w <= 0 || h <= 0)
        return;

    bool focused = (c == wm.focused);
    bool dbg = wm.debug_resize && wm.drag_mode == DRAG_RESIZE;
    double t_start = dbg ? monotonic_ms() : 0;

    /* Render into an off-screen pixmap, not the frame directly: every
     * paint call below (background, focus tint, title text, each button)
     * used to land on the actual window the instant it was sent, so a
     * fast sequence of redraws (dragging/resizing) could show those
     * layers appearing one at a time -- visible flicker. Blitting the
     * finished pixmap in one xcb_copy_area() at the end instead makes
     * the whole update atomic from the X server's point of view. */
    xcb_pixmap_t pixmap = xcb_generate_id(wm.conn);
    xcb_create_pixmap(wm.conn, c->frame_depth, pixmap, c->frame, (uint16_t)w, (uint16_t)h);
    double t_pixmap = dbg ? monotonic_ms() : 0;

    cairo_surface_t *surface = cairo_xcb_surface_create(wm.conn, pixmap, c->frame_visual, w, h);
    cairo_t *cr = cairo_create(surface);

    paint_deco(c, cr, w, h, focused, c->frame_depth == 32);

    double t_paint = dbg ? monotonic_ms() : 0;

    cairo_destroy(cr);
    cairo_surface_flush(surface);
    cairo_surface_destroy(surface);
    double t_flush = dbg ? monotonic_ms() : 0;

    /* CopyArea requires source and destination to share a depth, and the
     * GC is bound to one too -- so an ARGB frame is blitted with the
     * depth-32 GC main.c made for exactly this (wm.deco_gc_argb). */
    xcb_gcontext_t gc = (c->frame_depth == 32 && wm.deco_gc_argb) ? wm.deco_gc_argb : wm.deco_gc;
    xcb_copy_area(wm.conn, pixmap, c->frame, gc, 0, 0, 0, 0, (uint16_t)w, (uint16_t)h);
    xcb_free_pixmap(wm.conn, pixmap);

    /* And the same decoration again, at whatever density a compositor
     * asked for (density.h). A no-op -- not even a branch's worth of work
     * -- unless one did. */
    deco_density_publish(c, w, h, focused);

    if (dbg) {
        double t_end = monotonic_ms();
        fprintf(stderr, "kiwm: [resize-debug] draw_decoration: create_pixmap=%.2fms paint=%.2fms "
                        "cairo_flush=%.2fms copy_area=%.2fms total=%.2fms\n",
                t_pixmap - t_start, t_paint - t_pixmap, t_flush - t_paint, t_end - t_flush,
                t_end - t_start);
    }
}
