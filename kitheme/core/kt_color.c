/* libkitheme - color parsing and shading. */
#include "kitheme.h"

#include <stdio.h>
#include <string.h>

static int hexval(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* "#rgb", "#rrggbb" or "#rrggbbaa". */
int kt_color_parse(const char *s, KtColor *out)
{
    if (!s || *s != '#')
        return 0;
    s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t'))
        n--;
    int v[8];
    for (size_t i = 0; i < n && i < 8; i++) {
        v[i] = hexval(s[i]);
        if (v[i] < 0)
            return 0;
    }
    if (n == 3) {
        out->r = v[0] / 15.0;
        out->g = v[1] / 15.0;
        out->b = v[2] / 15.0;
        out->a = 1.0;
        return 1;
    }
    if (n != 6 && n != 8)
        return 0;
    out->r = (v[0] * 16 + v[1]) / 255.0;
    out->g = (v[2] * 16 + v[3]) / 255.0;
    out->b = (v[4] * 16 + v[5]) / 255.0;
    out->a = n == 8 ? (v[6] * 16 + v[7]) / 255.0 : 1.0;
    return 1;
}

static int byte(double v)
{
    if (v < 0)
        v = 0;
    if (v > 1)
        v = 1;
    return (int)(v * 255.0 + 0.5);
}

void kt_color_hex(KtColor c, char out[8])
{
    snprintf(out, 8, "#%02x%02x%02x", byte(c.r), byte(c.g), byte(c.b));
}

void kt_color_css(KtColor c, char *out, size_t sz)
{
    if (c.a >= 1.0) {
        char hex[8];
        kt_color_hex(c, hex);
        snprintf(out, sz, "%s", hex);
        return;
    }
    /* Not %f: a comma locale would turn 0.5 into "0,5" and break the CSS. */
    int a = byte(c.a) * 1000 / 255;
    snprintf(out, sz, "rgba(%d,%d,%d,0.%03d)", byte(c.r), byte(c.g), byte(c.b), a > 999 ? 999 : a);
}

static void rgb_to_hls(double r, double g, double b, double *h, double *l, double *s)
{
    double max = r > g ? (r > b ? r : b) : (g > b ? g : b);
    double min = r < g ? (r < b ? r : b) : (g < b ? g : b);
    *l = (max + min) / 2;
    *h = *s = 0;
    if (max == min)
        return;
    double d = max - min;
    *s = *l <= 0.5 ? d / (max + min) : d / (2 - max - min);
    if (r == max)
        *h = (g - b) / d;
    else if (g == max)
        *h = 2 + (b - r) / d;
    else
        *h = 4 + (r - g) / d;
    *h *= 60;
    if (*h < 0)
        *h += 360;
}

static double hue(double m1, double m2, double h)
{
    while (h > 360)
        h -= 360;
    while (h < 0)
        h += 360;
    if (h < 60)
        return m1 + (m2 - m1) * h / 60;
    if (h < 180)
        return m2;
    if (h < 240)
        return m1 + (m2 - m1) * (240 - h) / 60;
    return m1;
}

static void hls_to_rgb(double h, double l, double s, double *r, double *g, double *b)
{
    if (s == 0) {
        *r = *g = *b = l;
        return;
    }
    double m2 = l <= 0.5 ? l * (1 + s) : l + s - l * s;
    double m1 = 2 * l - m2;
    *r = hue(m1, m2, h + 120);
    *g = hue(m1, m2, h);
    *b = hue(m1, m2, h - 120);
}

/* Same algorithm as gtk_style_shade(), so GTK2 colors this engine derives
 * match the ones GTK2 itself derives for light/dark/mid. */
KtColor kt_shade(KtColor c, double k)
{
    double h, l, s;
    rgb_to_hls(c.r, c.g, c.b, &h, &l, &s);
    l *= k;
    if (l > 1)
        l = 1;
    if (l < 0)
        l = 0;
    s *= k;
    if (s > 1)
        s = 1;
    if (s < 0)
        s = 0;
    KtColor o = {0, 0, 0, c.a};
    hls_to_rgb(h, l, s, &o.r, &o.g, &o.b);
    return o;
}

KtColor kt_mix(KtColor a, KtColor b, double t)
{
    KtColor o = {
        a.r + (b.r - a.r) * t,
        a.g + (b.g - a.g) * t,
        a.b + (b.b - a.b) * t,
        a.a + (b.a - a.a) * t,
    };
    return o;
}

KtColor kt_alpha(KtColor c, double a)
{
    c.a = a;
    return c;
}

double kt_luma(KtColor c)
{
    return 0.299 * c.r + 0.587 * c.g + 0.114 * c.b;
}

void kt_colors_derive(KtTheme *t, KtColor accent, KtColor bg)
{
    const KtColor white = {1, 1, 1, 1}, black = {0, 0, 0, 1};
    accent.a = bg.a = 1;
    int dark = kt_luma(bg) < 0.5;
    /* Text goes toward `ink`; tooltips sit on the opposite side, so their
     * text goes toward `paper`. */
    KtColor ink = dark ? white : black, paper = dark ? black : white;
    t->simple_accent = accent;
    t->simple_bg = bg;
    t->window_bg = bg;
    t->window_fg = kt_mix(bg, ink, 0.9);
    t->base_bg = dark ? kt_mix(bg, black, 0.3) : kt_mix(bg, white, 0.85);
    t->base_fg = kt_mix(t->base_bg, ink, 0.88);
    t->button_bg = kt_mix(bg, white, dark ? 0.08 : 0.25);
    t->button_fg = t->window_fg;
    t->selection_bg = accent;
    t->selection_fg = kt_luma(accent) > 0.6 ? kt_mix(accent, black, 0.85) : white;
    t->accent = kt_mix(accent, ink, dark ? 0.15 : 0.06);
    t->progress = accent;
    t->menu_bg = bg;
    t->menu_fg = t->window_fg;
    t->tooltip_bg = kt_mix(bg, ink, 0.88);
    t->tooltip_fg = kt_mix(accent, paper, dark ? 0.75 : 0.8);
}
