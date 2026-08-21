/* Decoration: Cairo + Imlib2, hardcoded to greenxp/bg.png (same PNG
 * xispanel's theme backgrounds use), cached once at startup. No
 * theme.conf/9-slice system yet (see kiwm-kicomp-projeto.md section 10). */
#include "decoration.h"
#include "wm.h"

#include <cairo/cairo-xcb.h>
#include <Imlib2.h>

#include <stdio.h>
#include <stdlib.h>

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

void load_decoration(void)
{
    const char *env = getenv("KIWM_DECO_BG");
    const char *candidates[] = {
        "../greenxp/bg.png",
        "./greenxp/bg.png",
        "greenxp/bg.png",
    };

    if (env && env[0]) {
        wm.deco_bg = load_png_argb(env);
        if (wm.deco_bg) {
            fprintf(stderr, "kiwm: decoration background loaded from '%s'\n", env);
            return;
        }
    }

    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        wm.deco_bg = load_png_argb(candidates[i]);
        if (wm.deco_bg) {
            fprintf(stderr, "kiwm: decoration background loaded from '%s'\n", candidates[i]);
            return;
        }
    }

    fprintf(stderr, "kiwm: could not load greenxp/bg.png, falling back to flat color decoration\n");
}

bool client_deco_visible(Client *c)
{
    return !(c->maximized && wm.hide_deco_on_maximize);
}

static void draw_button(cairo_t *cr, double x, char glyph)
{
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.30);
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
    } else if (glyph == 'x') {
        cairo_move_to(cr, cx - 4, cy - 4);
        cairo_line_to(cr, cx + 4, cy + 4);
        cairo_move_to(cr, cx + 4, cy - 4);
        cairo_line_to(cr, cx - 4, cy + 4);
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
    int h = TITLEBAR_H;
    if (w <= 0 || h <= 0)
        return;

    cairo_surface_t *surface = cairo_xcb_surface_create(wm.conn, c->frame, wm.visual, w, h);
    cairo_t *cr = cairo_create(surface);

    if (wm.deco_bg) {
        int iw = cairo_image_surface_get_width(wm.deco_bg);
        int ih = cairo_image_surface_get_height(wm.deco_bg);
        cairo_save(cr);
        cairo_scale(cr, (double)w / (double)iw, (double)h / (double)ih);
        cairo_set_source_surface(cr, wm.deco_bg, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
        cairo_paint(cr);
        cairo_restore(cr);
    } else {
        /* No theme PNG (missing file, or no theme configured yet): flat
         * fallback color from kiwm.conf's deco_bg= (default black). */
        cairo_set_source_rgb(cr, wm.deco_bg_r, wm.deco_bg_g, wm.deco_bg_b);
        cairo_paint(cr);
    }

    /* Focus tint on top of the theme image. */
    if (c == wm.focused)
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.10);
    else
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.35);
    cairo_paint(cr);

    cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 12.5);
    cairo_set_source_rgb(cr, wm.deco_fg_r, wm.deco_fg_g, wm.deco_fg_b);

    cairo_text_extents_t ext;
    cairo_text_extents(cr, c->title, &ext);
    double title_x = 8.0;
    double title_y = (TITLEBAR_H - ext.height) / 2.0 - ext.y_bearing;
    cairo_move_to(cr, title_x, title_y);
    cairo_show_text(cr, c->title);

    draw_button(cr, w - BUTTON_W * 3, '-');
    draw_button(cr, w - BUTTON_W * 2, '+');
    draw_button(cr, w - BUTTON_W,     'x');

    cairo_destroy(cr);
    cairo_surface_flush(surface);
    cairo_surface_destroy(surface);
}
