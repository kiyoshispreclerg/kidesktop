/* See text.h. */
#include "text.h"

#include <pango/pangocairo.h>
#include <xcb/render.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct CompTextImage {
    int w, h;
    unsigned char *pixels;          /* premultiplied BGRA, w*4 stride */
    xcb_pixmap_t pixmap;
    xcb_render_picture_t picture;
    unsigned int texture;           /* the GL backend's, 0 until uploaded */
};

/* ------------------------------------------------------------------ */
/* the decoration's style                                              */
/* ------------------------------------------------------------------ */

static CompTextStyle theme;
static bool theme_read;

static bool parse_color(const char *s, float out[4])
{
    if (!s)
        return false;
    while (*s == ' ' || *s == '\t')
        s++;
    if (!strcasecmp(s, "none"))
        return false;
    if (*s == '#')
        s++;

    unsigned r = 0, g = 0, b = 0, a = 255;
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\n' || s[n - 1] == '\r'))
        n--;

    if (n >= 8) {
        if (sscanf(s, "%2x%2x%2x%2x", &r, &g, &b, &a) != 4)
            return false;
    } else if (n >= 6) {
        if (sscanf(s, "%2x%2x%2x", &r, &g, &b) != 3)
            return false;
    } else {
        return false;
    }

    out[0] = (float)r / 255.0f;
    out[1] = (float)g / 255.0f;
    out[2] = (float)b / 255.0f;
    out[3] = (float)a / 255.0f;
    return true;
}

static int parse_weight(const char *v)
{
    static const struct { const char *name; int weight; } names[] = {
        {"thin",100},{"ultralight",200},{"light",300},{"semilight",350},
        {"book",380},{"normal",400},{"medium",500},{"semibold",600},
        {"bold",700},{"ultrabold",800},{"heavy",900},{"black",900},
    };
    for (size_t i = 0; i < sizeof(names)/sizeof(names[0]); i++)
        if (!strcasecmp(v, names[i].name))
            return names[i].weight;
    int n = atoi(v);
    return (n >= 100 && n <= 1000) ? n : 400;
}

/* One `key=value` line from someone else's config file. Whitespace and
 * comments are the only syntax; anything unrecognised is skipped rather
 * than complained about, because this file belongs to another program. */
static bool split_line(char *line, char **key, char **val)
{
    char *hash = strchr(line, '#');
    if (hash)
        *hash = '\0';

    char *eq = strchr(line, '=');
    if (!eq)
        return false;
    *eq = '\0';

    char *k = line;
    while (*k == ' ' || *k == '\t')
        k++;
    char *ke = k + strlen(k);
    while (ke > k && (ke[-1] == ' ' || ke[-1] == '\t'))
        *--ke = '\0';

    char *v = eq + 1;
    while (*v == ' ' || *v == '\t')
        v++;
    char *ve = v + strlen(v);
    while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t' || ve[-1] == '\n' || ve[-1] == '\r'))
        *--ve = '\0';

    if (!*k)
        return false;
    *key = k;
    *val = v;
    return true;
}

static void read_theme_colors(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *k, *v;
        if (!split_line(line, &k, &v))
            continue;

        if (!strcmp(k, "font"))
            snprintf(theme.font, sizeof(theme.font), "%s", v);
        else if (!strcmp(k, "font_size"))
            theme.size = atof(v);
        else if (!strcmp(k, "font_weight"))
            theme.weight = parse_weight(v);
        else if (!strcmp(k, "font_style"))
            theme.slant = !strcasecmp(v, "italic") ? 1 :
                          !strcasecmp(v, "oblique") ? 2 : 0;
        else if (!strcmp(k, "fg_active"))
            parse_color(v, theme.fg);
        else if (!strcmp(k, "bg_active"))
            parse_color(v, theme.bg);
        else if (!strcmp(k, "title_shadow"))
            theme.shadow = parse_color(v, theme.shadow_color);
        else if (!strcmp(k, "title_shadow_offset")) {
            int dx = 1, dy = 1;
            if (sscanf(v, "%d %d", &dx, &dy) == 1)
                dy = dx;
            theme.shadow_dx = dx;
            theme.shadow_dy = dy;
        } else if (!strcmp(k, "title_outline"))
            theme.outline = parse_color(v, theme.outline_color);
        else if (!strcmp(k, "title_outline_width"))
            theme.outline_width = atof(v);
    }
    fclose(f);
}

static void config_path(const char *name, char *out, size_t outsz)
{
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) {
        snprintf(out, outsz, "%s/%s", xdg, name);
        return;
    }
    const char *home = getenv("HOME");
    snprintf(out, outsz, "%s/.config/%s", home ? home : ".", name);
}

const CompTextStyle *text_theme_style(void)
{
    if (theme_read)
        return &theme;
    theme_read = true;

    /* Plain and legible, for a session with no kiwm to ask. */
    theme.font[0] = '\0';
    theme.size = 12.5;
    theme.weight = 400;
    theme.slant = 0;
    theme.fg[0] = theme.fg[1] = theme.fg[2] = theme.fg[3] = 1.0f;
    theme.bg[0] = theme.bg[1] = theme.bg[2] = 0.0f;
    theme.bg[3] = 0.55f;
    theme.shadow_dx = theme.shadow_dy = 1;
    theme.outline_width = 1.0;
    theme.pad_x = 8;
    theme.pad_y = 4;

    char path[PATH_MAX];
    config_path("kiwm.conf", path, sizeof(path));

    char theme_dir[PATH_MAX];
    theme_dir[0] = '\0';

    FILE *f = fopen(path, "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            char *k, *v;
            if (!split_line(line, &k, &v))
                continue;
            if (!strcmp(k, "theme"))
                snprintf(theme_dir, sizeof(theme_dir), "%s", v);
            else if (!strcmp(k, "deco_fg"))
                parse_color(v, theme.fg);
            else if (!strcmp(k, "deco_bg"))
                parse_color(v, theme.bg);
        }
        fclose(f);
    }

    /* The theme's own file wins: it is the more specific statement, and
     * it is where a theme puts the font it was designed around. */
    if (theme_dir[0]) {
        char colors[PATH_MAX + 16];
        snprintf(colors, sizeof(colors), "%s/colors", theme_dir);
        read_theme_colors(colors);
    }

    /* A titlebar's background is usually translucent over the window
     * behind it; a label floating over a thumbnail has nothing behind it
     * to blend with, so it needs to be readable on its own. */
    if (theme.bg[3] < 0.5f)
        theme.bg[3] = 0.72f;

    comp_info("text style: %s %.1fpx weight %d%s%s",
              theme.font[0] ? theme.font : "sans-serif", theme.size,
              theme.weight, theme.shadow ? ", shadow" : "",
              theme.outline ? ", outline" : "");
    return &theme;
}

/* ------------------------------------------------------------------ */
/* rasterising                                                         */
/* ------------------------------------------------------------------ */

/* A GC on a 32-bit drawable, kept because every label wants the same one.
 * It has to be created against a depth-32 drawable, not the root, which
 * is why it is made lazily against the first pixmap of that depth. */
static xcb_gcontext_t gc;
static xcb_pixmap_t gc_owner;

static xcb_gcontext_t text_gc_for(xcb_pixmap_t pixmap)
{
    if (gc)
        return gc;
    gc = xcb_generate_id(comp.conn);
    xcb_create_gc(comp.conn, gc, pixmap, 0, NULL);
    gc_owner = pixmap;
    return gc;
}

/* The standard 32-bit ARGB picture format, which is what Cairo produced. */
static xcb_render_pictformat_t text_argb_format(void)
{
    static xcb_render_pictformat_t fmt;
    static bool asked;
    if (asked)
        return fmt;
    asked = true;

    xcb_render_query_pict_formats_reply_t *r =
        xcb_render_query_pict_formats_reply(comp.conn,
            xcb_render_query_pict_formats(comp.conn), NULL);
    if (!r)
        return 0;

    xcb_render_pictforminfo_iterator_t it =
        xcb_render_query_pict_formats_formats_iterator(r);
    for (; it.rem; xcb_render_pictforminfo_next(&it)) {
        xcb_render_pictforminfo_t *f = it.data;
        if (f->type == XCB_RENDER_PICT_TYPE_DIRECT && f->depth == 32 &&
            f->direct.alpha_mask == 0xff && f->direct.red_mask == 0xff &&
            f->direct.red_shift == 16 && f->direct.green_shift == 8 &&
            f->direct.blue_shift == 0) {
            fmt = f->id;
            break;
        }
    }
    free(r);
    return fmt;
}

static void set_source(cairo_t *cr, const float c[4])
{
    cairo_set_source_rgba(cr, c[0], c[1], c[2], c[3]);
}

CompTextImage *text_render(const char *utf8, const CompTextStyle *style,
                           int max_width)
{
    if (!utf8 || !*utf8 || !style)
        return NULL;

    /* Laid out once on a throwaway surface to find the size, then drawn
     * on a surface that size: a label is exactly as big as its text plus
     * its padding, and that is what the caller places. */
    cairo_surface_t *probe = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t *pcr = cairo_create(probe);
    PangoLayout *layout = pango_cairo_create_layout(pcr);

    PangoFontDescription *desc =
        pango_font_description_from_string(style->font[0] ? style->font : "sans-serif");
    pango_font_description_set_absolute_size(desc, style->size * PANGO_SCALE);
    pango_font_description_set_weight(desc, (PangoWeight)style->weight);
    pango_font_description_set_style(desc, style->slant == 1 ? PANGO_STYLE_ITALIC :
                                           style->slant == 2 ? PANGO_STYLE_OBLIQUE :
                                                               PANGO_STYLE_NORMAL);
    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);

    pango_layout_set_text(layout, utf8, -1);
    if (max_width > 0) {
        int room = max_width - style->pad_x * 2;
        if (room < 16)
            room = 16;
        pango_layout_set_width(layout, room * PANGO_SCALE);
        pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
        pango_layout_set_single_paragraph_mode(layout, TRUE);
    }

    int tw = 0, th = 0;
    pango_layout_get_pixel_size(layout, &tw, &th);

    int pad_extra = 0;
    if (style->outline)
        pad_extra = (int)style->outline_width + 1;
    if (style->shadow) {
        int dx = style->shadow_dx < 0 ? -style->shadow_dx : style->shadow_dx;
        int dy = style->shadow_dy < 0 ? -style->shadow_dy : style->shadow_dy;
        int m = dx > dy ? dx : dy;
        if (m > pad_extra)
            pad_extra = m;
    }

    int w = tw + style->pad_x * 2 + pad_extra * 2;
    int h = th + style->pad_y * 2 + pad_extra * 2;
    if (w < 1 || h < 1 || w > 4096 || h > 512) {
        g_object_unref(layout);
        cairo_destroy(pcr);
        cairo_surface_destroy(probe);
        return NULL;
    }

    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t *cr = cairo_create(surface);
    PangoLayout *final = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(final, pango_layout_get_font_description(layout)
        ? pango_font_description_copy(pango_layout_get_font_description(layout))
        : NULL);
    pango_layout_set_text(final, utf8, -1);
    if (max_width > 0) {
        pango_layout_set_width(final, pango_layout_get_width(layout));
        pango_layout_set_ellipsize(final, PANGO_ELLIPSIZE_END);
        pango_layout_set_single_paragraph_mode(final, TRUE);
    }

    g_object_unref(layout);
    cairo_destroy(pcr);
    cairo_surface_destroy(probe);

    /* The plate. Rounded by the same amount the padding gives, so it
     * reads as a label rather than as a box someone forgot to style. */
    if (style->bg[3] > 0.0f) {
        double r = 4.0;
        cairo_new_path(cr);
        cairo_arc(cr, r, r, r, 3.1415926, 4.712389);
        cairo_arc(cr, w - r, r, r, 4.712389, 6.283185);
        cairo_arc(cr, w - r, h - r, r, 0.0, 1.570796);
        cairo_arc(cr, r, h - r, r, 1.570796, 3.1415926);
        cairo_close_path(cr);
        set_source(cr, style->bg);
        cairo_fill(cr);
    }

    double tx = style->pad_x + pad_extra;
    double ty = style->pad_y + pad_extra;

    if (style->shadow) {
        cairo_move_to(cr, tx + style->shadow_dx, ty + style->shadow_dy);
        set_source(cr, style->shadow_color);
        pango_cairo_show_layout(cr, final);
    }

    if (style->outline) {
        cairo_move_to(cr, tx, ty);
        pango_cairo_layout_path(cr, final);
        set_source(cr, style->outline_color);
        cairo_set_line_width(cr, style->outline_width > 0 ? style->outline_width : 1.0);
        cairo_stroke(cr);
    }

    cairo_move_to(cr, tx, ty);
    set_source(cr, style->fg);
    pango_cairo_show_layout(cr, final);

    cairo_surface_flush(surface);

    CompTextImage *img = calloc(1, sizeof(*img));
    if (!img) {
        g_object_unref(final);
        cairo_destroy(cr);
        cairo_surface_destroy(surface);
        return NULL;
    }

    img->w = w;
    img->h = h;

    size_t stride = (size_t)cairo_image_surface_get_stride(surface);
    const unsigned char *src = cairo_image_surface_get_data(surface);
    img->pixels = malloc((size_t)w * (size_t)h * 4);
    if (img->pixels) {
        for (int y = 0; y < h; y++)
            memcpy(img->pixels + (size_t)y * (size_t)w * 4,
                   src + (size_t)y * stride, (size_t)w * 4);
    }

    /* And the same pixels as an X pixmap, for the XRender backend.
     * Cairo's ARGB32 is premultiplied in native byte order, which is
     * exactly what a 32-bit XRender picture wants on the same machine. */
    if (img->pixels && comp.caps.render) {
        img->pixmap = xcb_generate_id(comp.conn);
        xcb_create_pixmap(comp.conn, 32, img->pixmap, comp.root,
                          (uint16_t)w, (uint16_t)h);

        /* One row at a time: a single PutImage can exceed the server's
         * maximum request length for a wide label. */
        xcb_gcontext_t g = text_gc_for(img->pixmap);
        for (int y = 0; y < h; y++)
            xcb_put_image(comp.conn, XCB_IMAGE_FORMAT_Z_PIXMAP, img->pixmap,
                          g, (uint16_t)w, 1, 0, (int16_t)y, 0, 32,
                          (uint32_t)w * 4, img->pixels + (size_t)y * (size_t)w * 4);

        xcb_render_pictformat_t fmt = text_argb_format();
        if (fmt != 0) {
            img->picture = xcb_generate_id(comp.conn);
            xcb_render_create_picture(comp.conn, img->picture, img->pixmap, fmt, 0, NULL);
        }
    }

    g_object_unref(final);
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    return img;
}

void text_free(CompTextImage *img)
{
    if (!img)
        return;
    if (img->picture)
        xcb_render_free_picture(comp.conn, img->picture);
    if (img->pixmap)
        xcb_free_pixmap(comp.conn, img->pixmap);
    free(img->pixels);
    free(img);
}

int text_width(const CompTextImage *img) { return img ? img->w : 0; }
int text_height(const CompTextImage *img) { return img ? img->h : 0; }

xcb_render_picture_t text_picture(const CompTextImage *img)
{
    return img ? img->picture : XCB_NONE;
}

const unsigned char *text_pixels(const CompTextImage *img)
{
    return img ? img->pixels : NULL;
}

unsigned int *text_gl_texture(CompTextImage *img)
{
    return img ? &img->texture : NULL;
}

void text_shutdown(void)
{
    if (gc) {
        xcb_free_gc(comp.conn, gc);
        gc = 0;
        gc_owner = 0;
    }
}
