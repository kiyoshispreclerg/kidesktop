/* libkitheme - the renderer.
 *
 * Every element is built from three primitives, one per KtStyle:
 *   soft     rounded body with a vertical gradient, a light inner edge on
 *            top, a soft dark border and an optional drop shadow; hover
 *            and focus are an accent-colored glow around it (Oxygen)
 *   classic  square two-pixel bevels from four derived colors
 *            (highlight, light, shadow, dark shadow -- Windows 9x)
 *   flat     rounded body, single border, no gradient
 * Callers pass integer rectangles; strokes are placed on half pixels so
 * one-pixel lines stay crisp. Nothing here allocates except Cairo's own
 * pattern objects. */
#include "kitheme.h"

#include <math.h>

static const KtColor WHITE = {1, 1, 1, 1};
static const KtColor BLACK = {0, 0, 0, 1};

static void set(cairo_t *cr, KtColor c)
{
    cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a);
}

static void rrect(cairo_t *cr, double x, double y, double w, double h, double r)
{
    if (w <= 0 || h <= 0)
        return;
    if (r > w / 2)
        r = w / 2;
    if (r > h / 2)
        r = h / 2;
    if (r <= 0.01) {
        cairo_rectangle(cr, x, y, w, h);
        return;
    }
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
}

static double radius(const KtTheme *t)
{
    return t->style == KT_STYLE_CLASSIC ? 0 : t->radius;
}

/* Two-stop linear gradient along y (or x when `horiz`). */
static void gradient(cairo_t *cr, double x, double y, double w, double h, int horiz, KtColor a, KtColor b)
{
    cairo_pattern_t *p = horiz ? cairo_pattern_create_linear(x, 0, x + w, 0)
                               : cairo_pattern_create_linear(0, y, 0, y + h);
    cairo_pattern_add_color_stop_rgba(p, 0, a.r, a.g, a.b, a.a);
    cairo_pattern_add_color_stop_rgba(p, 1, b.r, b.g, b.b, b.a);
    cairo_set_source(cr, p);
    cairo_pattern_destroy(p);
}

/* ---- classic bevels ---- */

typedef struct {
    KtColor hl, light, shadow, dark;
} Bevel;

static Bevel bevel_colors(const KtTheme *t, KtColor bg)
{
    double k = t->relief > 0 ? t->relief : 0.01;
    Bevel b = {
        kt_mix(bg, WHITE, 1.0 * k),
        kt_mix(bg, WHITE, 0.45 * k),
        kt_mix(bg, BLACK, 0.33 * k),
        kt_mix(bg, BLACK, 1.0 * k),
    };
    return b;
}

/* One ring of a bevel: top/left in `tl`, bottom/right in `br`. */
static void ring(cairo_t *cr, double x, double y, double w, double h, KtColor tl, KtColor br)
{
    if (w < 1 || h < 1)
        return;
    cairo_set_line_width(cr, 1);
    set(cr, tl);
    cairo_move_to(cr, x + 0.5, y + h - 1);
    cairo_line_to(cr, x + 0.5, y + 0.5);
    cairo_line_to(cr, x + w - 1, y + 0.5);
    cairo_stroke(cr);
    set(cr, br);
    cairo_move_to(cr, x + w - 0.5, y);
    cairo_line_to(cr, x + w - 0.5, y + h - 0.5);
    cairo_line_to(cr, x, y + h - 0.5);
    cairo_stroke(cr);
}

/* raised: highlight outside-light inside on top/left; sunken: reversed
 * (a field); `pressed_button`: Windows' pressed look, dark frame plus a
 * shadow line. */
static void classic_box(const KtTheme *t, cairo_t *cr, double x, double y, double w, double h,
                        KtColor bg, int sunken, int fill)
{
    Bevel b = bevel_colors(t, bg);
    if (fill) {
        set(cr, bg);
        cairo_rectangle(cr, x, y, w, h);
        cairo_fill(cr);
    }
    if (sunken > 1) {
        ring(cr, x, y, w, h, b.dark, b.dark);
        ring(cr, x + 1, y + 1, w - 2, h - 2, b.shadow, bg);
    } else if (sunken) {
        ring(cr, x, y, w, h, b.shadow, b.hl);
        ring(cr, x + 1, y + 1, w - 2, h - 2, b.dark, b.light);
    } else {
        ring(cr, x, y, w, h, b.light, b.dark);
        ring(cr, x + 1, y + 1, w - 2, h - 2, b.hl, b.shadow);
    }
}

static void dotted_rect(cairo_t *cr, double x, double y, double w, double h, KtColor c)
{
    static const double dash[] = {1, 1};
    cairo_save(cr);
    cairo_set_line_width(cr, 1);
    cairo_set_dash(cr, dash, 2, 0);
    set(cr, c);
    cairo_rectangle(cr, x + 0.5, y + 0.5, w - 1, h - 1);
    cairo_stroke(cr);
    cairo_restore(cr);
}

/* ---- soft / flat bodies ---- */

/* Glow ring just outside a body of radius r. */
static void glow(cairo_t *cr, double x, double y, double w, double h, double r, KtColor c, double a)
{
    cairo_set_line_width(cr, 1.5);
    set(cr, kt_alpha(c, a));
    rrect(cr, x - 0.25, y - 0.25, w + 0.5, h + 0.5, r + 0.25);
    cairo_stroke(cr);
}

/* A raised, bevelled body: buttons, scrollbar sliders, scale handles.
 * `horiz` turns the gradient sideways (vertical scrollbar sliders). */
static void raised_body(const KtTheme *t, cairo_t *cr, double x, double y, double w, double h,
                        unsigned st, KtColor bg, int horiz, double r)
{
    if (w < 4 || h < 4)
        return;
    int pressed = st & (KT_PRESSED | KT_CHECKED);
    int disabled = st & KT_DISABLED;
    if (disabled)
        bg = kt_mix(bg, t->window_bg, 0.5);
    else if (st & KT_HOVER)
        bg = kt_shade(bg, 1.04);

    if (t->style == KT_STYLE_CLASSIC) {
        if (st & KT_DEFAULT) {
            ring(cr, x, y, w, h, BLACK, BLACK);
            x++, y++, w -= 2, h -= 2;
        }
        classic_box(t, cr, x, y, w, h, bg, pressed ? 2 : 0, 1);
        return;
    }

    /* Room for the glow/shadow: the body sits one pixel inside. */
    double bx = x + 1, by = y + 1, bw = w - 2, bh = h - 2;
    double g = t->gradient;
    KtColor border = kt_alpha(kt_shade(bg, 0.55), disabled ? 0.35 : 0.55 + 0.35 * t->relief);
    if (st & KT_DEFAULT)
        border = kt_mix(border, kt_alpha(t->accent, border.a), 0.5);

    if (t->style == KT_STYLE_FLAT) {
        set(cr, pressed ? kt_shade(bg, 0.9) : bg);
        rrect(cr, bx, by, bw, bh, r);
        cairo_fill(cr);
        cairo_set_line_width(cr, 1);
        set(cr, (st & (KT_HOVER | KT_FOCUSED)) && t->hover_glow && !disabled ? t->accent : border);
        rrect(cr, bx + 0.5, by + 0.5, bw - 1, bh - 1, r - 0.5);
        cairo_stroke(cr);
        return;
    }

    if (t->shadow > 0 && !pressed) {
        set(cr, kt_alpha(BLACK, 0.10 * t->shadow));
        rrect(cr, bx - 0.5, by + 0.5, bw + 1, bh + 1, r + 0.5);
        cairo_fill(cr);
        set(cr, kt_alpha(BLACK, 0.12 * t->shadow));
        rrect(cr, bx, by + 1, bw, bh, r);
        cairo_fill(cr);
    }

    if (pressed)
        gradient(cr, bx, by, bw, bh, horiz, kt_shade(bg, 1 - 0.14 * g - 0.06), kt_shade(bg, 1 + 0.04 * g));
    else
        gradient(cr, bx, by, bw, bh, horiz, kt_shade(bg, 1 + 0.10 * g), kt_shade(bg, 1 - 0.09 * g));
    rrect(cr, bx, by, bw, bh, r);
    cairo_fill(cr);

    cairo_set_line_width(cr, 1);
    if (t->relief > 0 && bw > 4 && bh > 4) {
        /* Light inner edge fading out toward the middle, or, pressed, a
         * dark one: what makes the body read as raised or sunken. */
        KtColor edge = pressed ? kt_alpha(BLACK, 0.18 * t->relief) : kt_alpha(WHITE, 0.75 * t->relief);
        KtColor clear = kt_alpha(edge, 0);
        gradient(cr, bx, by, bw, bh, horiz, edge, clear);
        rrect(cr, bx + 1.5, by + 1.5, bw - 3, bh - 3, r - 1.5);
        cairo_stroke(cr);
    }
    set(cr, border);
    rrect(cr, bx + 0.5, by + 0.5, bw - 1, bh - 1, r - 0.5);
    cairo_stroke(cr);

    if (disabled)
        return;
    if ((st & KT_FOCUSED) && t->focus == KT_FOCUS_GLOW)
        glow(cr, bx, by, bw, bh, r, t->accent, 0.85);
    else if ((st & KT_HOVER) && t->hover_glow)
        glow(cr, bx, by, bw, bh, r, kt_shade(t->accent, 1.2), 0.6);
}

/* A sunken field: entries, check boxes, progress troughs. */
static void sunken_body(const KtTheme *t, cairo_t *cr, double x, double y, double w, double h,
                        unsigned st, KtColor fill, double r)
{
    if (w < 4 || h < 4)
        return;
    int disabled = st & KT_DISABLED;
    if (disabled)
        fill = kt_mix(fill, t->window_bg, 0.6);

    if (t->style == KT_STYLE_CLASSIC) {
        classic_box(t, cr, x, y, w, h, t->window_bg, 1, 0);
        set(cr, fill);
        cairo_rectangle(cr, x + 2, y + 2, w - 4, h - 4);
        cairo_fill(cr);
        return;
    }

    double bx = x + 1, by = y + 1, bw = w - 2, bh = h - 2;
    KtColor border = kt_alpha(kt_shade(t->window_bg, 0.5), disabled ? 0.3 : 0.45 + 0.35 * t->relief);
    set(cr, fill);
    rrect(cr, bx, by, bw, bh, r);
    cairo_fill(cr);
    cairo_set_line_width(cr, 1);
    if (t->style == KT_STYLE_SOFT && t->relief > 0) {
        /* Inner shadow along the top. */
        double sh = bh < 8 ? bh / 2 : 4;
        gradient(cr, bx, by, bw, sh, 0, kt_alpha(BLACK, 0.16 * t->relief), kt_alpha(BLACK, 0));
        rrect(cr, bx, by, bw, bh, r);
        cairo_fill(cr);
        /* And a light lip along the bottom, outside the border. */
        set(cr, kt_alpha(WHITE, 0.6 * t->relief));
        cairo_move_to(cr, bx + r, by + bh + 0.5);
        cairo_line_to(cr, bx + bw - r, by + bh + 0.5);
        cairo_stroke(cr);
    }
    int lit = !disabled && ((st & KT_FOCUSED) || ((st & KT_HOVER) && t->hover_glow));
    set(cr, lit && t->style == KT_STYLE_FLAT ? t->accent : border);
    rrect(cr, bx + 0.5, by + 0.5, bw - 1, bh - 1, r - 0.5);
    cairo_stroke(cr);
    if (t->style == KT_STYLE_SOFT && !disabled) {
        if ((st & KT_FOCUSED) && t->focus == KT_FOCUS_GLOW)
            glow(cr, bx, by, bw, bh, r, t->accent, 0.85);
        else if ((st & KT_HOVER) && t->hover_glow)
            glow(cr, bx, by, bw, bh, r, kt_shade(t->accent, 1.2), 0.5);
    }
}

/* ---- public elements ---- */

void kt_draw_button(const KtTheme *t, cairo_t *cr, double x, double y, double w, double h, unsigned st, KtColor bg)
{
    cairo_save(cr);
    raised_body(t, cr, x, y, w, h, st, bg, 0, radius(t));
    if (t->style == KT_STYLE_CLASSIC && (st & KT_FOCUSED) && t->focus != KT_FOCUS_NONE && w > 10 && h > 10)
        dotted_rect(cr, x + 4, y + 4, w - 8, h - 8, t->button_fg);
    cairo_restore(cr);
}

void kt_draw_entry(const KtTheme *t, cairo_t *cr, double x, double y, double w, double h, unsigned st, KtColor bg)
{
    cairo_save(cr);
    sunken_body(t, cr, x, y, w, h, st, bg, radius(t));
    cairo_restore(cr);
}

void kt_draw_frame(const KtTheme *t, cairo_t *cr, double x, double y, double w, double h, int sunken, KtColor bg)
{
    if (w < 2 || h < 2)
        return;
    cairo_save(cr);
    cairo_set_line_width(cr, 1);
    if (t->style == KT_STYLE_CLASSIC) {
        Bevel b = bevel_colors(t, bg);
        if (sunken > 0)
            classic_box(t, cr, x, y, w, h, bg, 1, 0);
        else if (sunken < 0)
            classic_box(t, cr, x, y, w, h, bg, 0, 0);
        else {
            ring(cr, x, y, w - 1, h - 1, b.shadow, b.shadow);
            ring(cr, x + 1, y + 1, w - 1, h - 1, b.hl, b.hl);
        }
    } else {
        double r = radius(t);
        KtColor border = kt_alpha(kt_shade(bg, 0.55), 0.35 + 0.35 * t->relief);
        set(cr, border);
        rrect(cr, x + 0.5, y + 0.5, w - 1, h - 1, r);
        cairo_stroke(cr);
        if (t->style == KT_STYLE_SOFT && t->relief > 0 && w > 4 && h > 4) {
            set(cr, kt_alpha(sunken > 0 ? BLACK : WHITE, (sunken > 0 ? 0.08 : 0.5) * t->relief));
            rrect(cr, x + 1.5, y + 1.5, w - 3, h - 3, r - 1);
            cairo_stroke(cr);
        }
    }
    cairo_restore(cr);
}

static void check_mark(cairo_t *cr, double x, double y, double s, KtColor c, double width)
{
    set(cr, c);
    cairo_set_line_width(cr, width);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_move_to(cr, x + s * 0.22, y + s * 0.52);
    cairo_line_to(cr, x + s * 0.42, y + s * 0.72);
    cairo_line_to(cr, x + s * 0.78, y + s * 0.28);
    cairo_stroke(cr);
}

/* The box is square, centered in the rectangle. */
static void square_in(double *x, double *y, double *w, double *h)
{
    double s = *w < *h ? *w : *h;
    *x = floor(*x + (*w - s) / 2);
    *y = floor(*y + (*h - s) / 2);
    *w = *h = s;
}

void kt_draw_check(const KtTheme *t, cairo_t *cr, double x, double y, double w, double h, unsigned st)
{
    square_in(&x, &y, &w, &h);
    cairo_save(cr);
    int on = st & (KT_CHECKED | KT_MIXED);
    KtColor mark = st & KT_DISABLED ? kt_mix(t->base_fg, t->base_bg, 0.6) : t->base_fg;
    if (st & KT_NOBOX) {
        mark = t->base_fg;
    } else if (t->style == KT_STYLE_FLAT && on) {
        KtColor fill = st & KT_DISABLED ? kt_mix(t->selection_bg, t->window_bg, 0.6) : t->selection_bg;
        set(cr, fill);
        rrect(cr, x + 1, y + 1, w - 2, h - 2, radius(t) * 0.75);
        cairo_fill(cr);
        mark = t->selection_fg;
    } else {
        sunken_body(t, cr, x, y, w, h, st & ~KT_PRESSED, t->base_bg, radius(t) * 0.75);
        if (st & KT_PRESSED) {
            set(cr, kt_alpha(BLACK, 0.08));
            rrect(cr, x + 2, y + 2, w - 4, h - 4, 1);
            cairo_fill(cr);
        }
    }
    if (st & KT_MIXED) {
        set(cr, mark);
        cairo_rectangle(cr, x + w * 0.25, y + h / 2 - w * 0.08, w * 0.5, w * 0.16 > 2 ? w * 0.16 : 2);
        cairo_fill(cr);
    } else if (st & KT_CHECKED) {
        double width = t->style == KT_STYLE_CLASSIC ? (w >= 13 ? 2.2 : 1.6) : w / 7.5;
        check_mark(cr, x, y, w, mark, width < 1.5 ? 1.5 : width);
    }
    cairo_restore(cr);
}

void kt_draw_radio(const KtTheme *t, cairo_t *cr, double x, double y, double w, double h, unsigned st)
{
    square_in(&x, &y, &w, &h);
    cairo_save(cr);
    double cx = x + w / 2, cy = y + h / 2, r = w / 2;
    KtColor dot = st & KT_DISABLED ? kt_mix(t->base_fg, t->base_bg, 0.6) : t->base_fg;
    if (st & KT_NOBOX) {
        dot = t->base_fg;
    } else if (t->style == KT_STYLE_CLASSIC) {
        Bevel b = bevel_colors(t, t->window_bg);
        cairo_set_line_width(cr, 1);
        set(cr, st & KT_DISABLED ? t->window_bg : t->base_bg);
        cairo_arc(cr, cx, cy, r - 1.5, 0, 2 * M_PI);
        cairo_fill(cr);
        /* Two half rings, like the bevel's two colors. */
        set(cr, b.shadow);
        cairo_arc(cr, cx, cy, r - 0.5, 3 * M_PI / 4, 7 * M_PI / 4);
        cairo_stroke(cr);
        set(cr, b.hl);
        cairo_arc(cr, cx, cy, r - 0.5, -M_PI / 4, 3 * M_PI / 4);
        cairo_stroke(cr);
        set(cr, b.dark);
        cairo_arc(cr, cx, cy, r - 1.5, 3 * M_PI / 4, 7 * M_PI / 4);
        cairo_stroke(cr);
        set(cr, b.light);
        cairo_arc(cr, cx, cy, r - 1.5, -M_PI / 4, 3 * M_PI / 4);
        cairo_stroke(cr);
    } else if (t->style == KT_STYLE_FLAT && (st & KT_CHECKED)) {
        set(cr, st & KT_DISABLED ? kt_mix(t->selection_bg, t->window_bg, 0.6) : t->selection_bg);
        cairo_arc(cr, cx, cy, r - 1, 0, 2 * M_PI);
        cairo_fill(cr);
        dot = t->selection_fg;
    } else {
        sunken_body(t, cr, x, y, w, h, st & ~KT_PRESSED, t->base_bg, w / 2);
    }
    if (st & KT_CHECKED) {
        set(cr, dot);
        cairo_arc(cr, cx, cy, w * 0.19 > 1.5 ? w * 0.19 : 1.5, 0, 2 * M_PI);
        cairo_fill(cr);
    } else if (st & KT_MIXED) {
        set(cr, dot);
        cairo_rectangle(cr, x + w * 0.3, cy - 1, w * 0.4, 2);
        cairo_fill(cr);
    }
    cairo_restore(cr);
}

void kt_draw_arrow(const KtTheme *t, cairo_t *cr, double x, double y, double w, double h, KtDir dir, KtColor fg)
{
    (void)t;
    double s = (w < h ? w : h);
    /* Large areas are whole buttons (combo boxes): 60% of them, never
     * wider than 9 px. Small ones are already just the arrow's own box
     * (GTK2's steppers pass half the button): fill those. */
    double base = s < 11 ? s : floor(s * 0.6);
    if (base > 9)
        base = 9;
    if (base < 3)
        base = 3;
    double depth = ceil(base / 2);
    double cx = x + w / 2, cy = y + h / 2;
    cairo_save(cr);
    set(cr, fg);
    switch (dir) {
    case KT_UP:
        cy = floor(cy - depth / 2);
        cairo_move_to(cr, cx - base / 2, cy + depth);
        cairo_line_to(cr, cx + base / 2, cy + depth);
        cairo_line_to(cr, cx, cy);
        break;
    case KT_DOWN:
        cy = floor(cy - depth / 2);
        cairo_move_to(cr, cx - base / 2, cy);
        cairo_line_to(cr, cx + base / 2, cy);
        cairo_line_to(cr, cx, cy + depth);
        break;
    case KT_LEFT:
        cx = floor(cx - depth / 2);
        cairo_move_to(cr, cx + depth, cy - base / 2);
        cairo_line_to(cr, cx + depth, cy + base / 2);
        cairo_line_to(cr, cx, cy);
        break;
    case KT_RIGHT:
        cx = floor(cx - depth / 2);
        cairo_move_to(cr, cx, cy - base / 2);
        cairo_line_to(cr, cx, cy + base / 2);
        cairo_line_to(cr, cx + depth, cy);
        break;
    }
    cairo_close_path(cr);
    cairo_fill(cr);
    cairo_restore(cr);
}

void kt_draw_progress_trough(const KtTheme *t, cairo_t *cr, double x, double y, double w, double h, int vertical)
{
    (void)vertical;
    cairo_save(cr);
    if (t->style == KT_STYLE_CLASSIC) {
        Bevel b = bevel_colors(t, t->window_bg);
        set(cr, t->window_bg);
        cairo_rectangle(cr, x, y, w, h);
        cairo_fill(cr);
        ring(cr, x, y, w, h, b.shadow, b.hl);
    } else if (t->style == KT_STYLE_FLAT) {
        set(cr, kt_shade(t->window_bg, 0.88));
        rrect(cr, x, y, w, h, radius(t));
        cairo_fill(cr);
    } else {
        sunken_body(t, cr, x, y, w, h, 0, kt_mix(t->window_bg, BLACK, 0.04), radius(t) + 1);
    }
    cairo_restore(cr);
}

/* Diagonal stripes, `period` px apart, shifted by phase*period. */
static void stripes(cairo_t *cr, double x, double y, double w, double h, int vertical, double phase, KtColor c)
{
    double len = vertical ? h : w, th = vertical ? w : h;
    double period = th * 1.6 > 8 ? th * 1.6 : 8;
    set(cr, c);
    for (double o = -period * 2 + phase * period; o < len + period; o += period) {
        if (vertical) {
            cairo_move_to(cr, x, y + o);
            cairo_line_to(cr, x + w, y + o + th);
            cairo_line_to(cr, x + w, y + o + th + period / 2);
            cairo_line_to(cr, x, y + o + period / 2);
        } else {
            cairo_move_to(cr, x + o, y + h);
            cairo_line_to(cr, x + o + th, y);
            cairo_line_to(cr, x + o + th + period / 2, y);
            cairo_line_to(cr, x + o + period / 2, y + h);
        }
        cairo_close_path(cr);
    }
    cairo_fill(cr);
}

void kt_draw_progress_bar(const KtTheme *t, cairo_t *cr, double x, double y, double w, double h, int vertical, double phase)
{
    if (w < 1 || h < 1)
        return;
    cairo_save(cr);
    KtColor c = t->progress;
    if (t->style == KT_STYLE_CLASSIC) {
        /* Windows draws blocks; "stripes" here means those blocks. */
        set(cr, c);
        if (t->progress_stripes) {
            double th = vertical ? w : h, len = vertical ? h : w;
            double block = floor(th * 0.66) > 4 ? floor(th * 0.66) : 4;
            for (double o = 0; o < len; o += block + 2) {
                double l = o + block > len ? len - o : block;
                if (vertical)
                    cairo_rectangle(cr, x + 1, y + h - o - l, w - 2, l);
                else
                    cairo_rectangle(cr, x + o, y + 1, l, h - 2);
            }
        } else {
            cairo_rectangle(cr, x, y, w, h);
        }
        cairo_fill(cr);
        cairo_restore(cr);
        return;
    }
    double r = radius(t) + (t->style == KT_STYLE_SOFT ? 0.5 : 0);
    rrect(cr, x, y, w, h, r);
    cairo_clip(cr);
    if (t->style == KT_STYLE_FLAT) {
        set(cr, c);
        cairo_paint(cr);
    } else {
        double g = t->gradient;
        gradient(cr, x, y, w, h, vertical, kt_shade(c, 1 + 0.18 * g), kt_shade(c, 1 - 0.15 * g));
        cairo_paint(cr);
    }
    if (t->progress_stripes)
        stripes(cr, x, y, w, h, vertical, phase, kt_alpha(WHITE, 0.16));
    if (t->style == KT_STYLE_SOFT) {
        cairo_set_line_width(cr, 1);
        if (t->relief > 0 && w > 3 && h > 3) {
            /* Gloss on the upper half. */
            gradient(cr, x, y, w, vertical ? w : h / 2, vertical, kt_alpha(WHITE, 0.35 * t->relief), kt_alpha(WHITE, 0.05));
            if (vertical)
                cairo_rectangle(cr, x, y, w / 2, h);
            else
                cairo_rectangle(cr, x, y, w, h / 2);
            cairo_fill(cr);
        }
        set(cr, kt_alpha(kt_shade(c, 0.6), 0.8));
        rrect(cr, x + 0.5, y + 0.5, w - 1, h - 1, r - 0.5);
        cairo_stroke(cr);
    }
    cairo_restore(cr);
}

/* 50% checkerboard of the background and its highlight: Windows' trough. */
static void dither(cairo_t *cr, double x, double y, double w, double h, KtColor a, KtColor b)
{
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, 2, 2);
    cairo_t *pc = cairo_create(s);
    set(pc, a);
    cairo_paint(pc);
    set(pc, b);
    cairo_rectangle(pc, 0, 0, 1, 1);
    cairo_rectangle(pc, 1, 1, 1, 1);
    cairo_fill(pc);
    cairo_destroy(pc);
    cairo_pattern_t *p = cairo_pattern_create_for_surface(s);
    cairo_pattern_set_extend(p, CAIRO_EXTEND_REPEAT);
    cairo_pattern_set_filter(p, CAIRO_FILTER_NEAREST);
    cairo_set_source(cr, p);
    cairo_rectangle(cr, x, y, w, h);
    cairo_fill(cr);
    cairo_pattern_destroy(p);
    cairo_surface_destroy(s);
}

void kt_draw_scroll_trough(const KtTheme *t, cairo_t *cr, double x, double y, double w, double h, int vertical)
{
    cairo_save(cr);
    if (t->style == KT_STYLE_CLASSIC) {
        dither(cr, x, y, w, h, t->window_bg, bevel_colors(t, t->window_bg).hl);
    } else if (t->style == KT_STYLE_FLAT) {
        set(cr, kt_shade(t->window_bg, 0.96));
        cairo_rectangle(cr, x, y, w, h);
        cairo_fill(cr);
    } else {
        set(cr, t->window_bg);
        cairo_rectangle(cr, x, y, w, h);
        cairo_fill(cr);
        /* A narrow groove in the middle. */
        double gw = (vertical ? w : h) * 0.5;
        if (vertical)
            sunken_body(t, cr, x + (w - gw) / 2, y + 1, gw, h - 2, 0, kt_mix(t->window_bg, BLACK, 0.05), gw / 2);
        else
            sunken_body(t, cr, x + 1, y + (h - gw) / 2, w - 2, gw, 0, kt_mix(t->window_bg, BLACK, 0.05), gw / 2);
    }
    cairo_restore(cr);
}

void kt_draw_scroll_slider(const KtTheme *t, cairo_t *cr, double x, double y, double w, double h, int vertical, unsigned st)
{
    cairo_save(cr);
    if (t->style == KT_STYLE_FLAT) {
        double m = 2;
        KtColor c = kt_shade(t->window_bg, st & KT_PRESSED ? 0.55 : st & KT_HOVER ? 0.65 : 0.75);
        set(cr, c);
        rrect(cr, x + m, y + m, w - 2 * m, h - 2 * m, (vertical ? w : h) / 2 - m);
        cairo_fill(cr);
    } else {
        double r = t->style == KT_STYLE_SOFT ? ((vertical ? w : h) - 2) / 2 : 0;
        raised_body(t, cr, x, y, w, h, st & ~KT_PRESSED, t->button_bg, vertical, r);
    }
    cairo_restore(cr);
}

void kt_draw_scale_groove(const KtTheme *t, cairo_t *cr, double x, double y, double w, double h, int vertical, double fill_to)
{
    double th = t->style == KT_STYLE_CLASSIC ? 4 : 6;
    double gx = vertical ? floor(x + (w - th) / 2) : x, gy = vertical ? y : floor(y + (h - th) / 2);
    double gw = vertical ? th : w, gh = vertical ? h : th;
    cairo_save(cr);
    if (t->style == KT_STYLE_CLASSIC)
        classic_box(t, cr, gx, gy, gw, gh, t->window_bg, 1, 0);
    else
        sunken_body(t, cr, gx, gy, gw, gh, 0, kt_mix(t->window_bg, BLACK, 0.06), th / 2);
    if (fill_to > 0 && t->style != KT_STYLE_CLASSIC) {
        double fx = gx + 1, fy = gy + 1, fw = gw - 2, fh = gh - 2;
        if (vertical) {
            fy = gy + gh - 1 - fill_to;
            fh = fill_to;
        } else {
            fw = fill_to;
        }
        if (fw > 0 && fh > 0) {
            set(cr, t->progress);
            rrect(cr, fx, fy, fw, fh, th / 2 - 1);
            cairo_fill(cr);
        }
    }
    cairo_restore(cr);
}

void kt_draw_scale_handle(const KtTheme *t, cairo_t *cr, double x, double y, double w, double h, unsigned st)
{
    cairo_save(cr);
    if (t->style == KT_STYLE_CLASSIC) {
        raised_body(t, cr, x, y, w, h, st & ~KT_PRESSED, t->button_bg, 0, 0);
    } else {
        double s = w < h ? w : h;
        double sx = floor(x + (w - s) / 2), sy = floor(y + (h - s) / 2);
        if (t->style == KT_STYLE_FLAT) {
            set(cr, st & KT_DISABLED ? kt_mix(t->progress, t->window_bg, 0.6) : st & KT_HOVER ? kt_shade(t->progress, 1.1) : t->progress);
            cairo_arc(cr, sx + s / 2, sy + s / 2, s / 2 - 1, 0, 2 * M_PI);
            cairo_fill(cr);
        } else {
            raised_body(t, cr, sx, sy, s, s, st & ~KT_PRESSED, t->button_bg, 0, s / 2);
        }
    }
    cairo_restore(cr);
}

void kt_draw_tab(const KtTheme *t, cairo_t *cr, double x, double y, double w, double h, KtDir gap, unsigned st)
{
    int sel = st & KT_SELECTED;
    cairo_save(cr);
    cairo_rectangle(cr, x, y, w, h);
    cairo_clip(cr);
    /* Unselected tabs are shorter, away from the page. */
    if (!sel) {
        double d = t->style == KT_STYLE_CLASSIC ? 2 : 3;
        switch (gap) {
        case KT_DOWN: y += d, h -= d; break;
        case KT_UP: h -= d; break;
        case KT_RIGHT: x += d, w -= d; break;
        case KT_LEFT: w -= d; break;
        }
    }
    /* Draw a shape extended past the gap side, so its corners and border
     * on that side fall outside the clip. */
    double ext = radius(t) + 3;
    double ex = x, ey = y, ew = w, eh = h;
    switch (gap) {
    case KT_DOWN: eh += ext; break;
    case KT_UP: ey -= ext, eh += ext; break;
    case KT_RIGHT: ew += ext; break;
    case KT_LEFT: ex -= ext, ew += ext; break;
    }
    KtColor bg = sel ? t->window_bg : kt_shade(t->window_bg, st & KT_HOVER ? 0.97 : 0.91);
    if (t->style == KT_STYLE_CLASSIC) {
        classic_box(t, cr, ex, ey, ew, eh, bg, 0, 1);
    } else if (t->style == KT_STYLE_FLAT) {
        set(cr, bg);
        rrect(cr, ex, ey, ew, eh, radius(t));
        cairo_fill(cr);
        cairo_set_line_width(cr, 1);
        set(cr, kt_alpha(kt_shade(t->window_bg, 0.55), 0.5));
        rrect(cr, ex + 0.5, ey + 0.5, ew - 1, eh - 1, radius(t));
        cairo_stroke(cr);
        if (sel) {
            /* Accent line on the side away from the page. */
            set(cr, t->accent);
            switch (gap) {
            case KT_DOWN: cairo_rectangle(cr, x + 1, y, w - 2, 2); break;
            case KT_UP: cairo_rectangle(cr, x + 1, y + h - 2, w - 2, 2); break;
            case KT_RIGHT: cairo_rectangle(cr, x, y + 1, 2, h - 2); break;
            case KT_LEFT: cairo_rectangle(cr, x + w - 2, y + 1, 2, h - 2); break;
            }
            cairo_fill(cr);
        }
    } else {
        unsigned bst = (st & (KT_HOVER | KT_DISABLED)) & ~(sel ? KT_HOVER : 0);
        KtTheme flat = *t;
        flat.shadow = sel ? t->shadow * 0.5 : 0;
        flat.hover_glow = 0;
        raised_body(&flat, cr, ex, ey, ew, eh, bst, bg, gap == KT_LEFT || gap == KT_RIGHT, radius(t) + 1);
        if (!sel) {
            /* Unselected tabs sit behind the pane: shade them slightly
             * toward it. */
            gradient(cr, x, y, w, h, gap == KT_LEFT || gap == KT_RIGHT,
                     kt_alpha(BLACK, gap == KT_DOWN || gap == KT_RIGHT ? 0 : 0.06),
                     kt_alpha(BLACK, gap == KT_DOWN || gap == KT_RIGHT ? 0.06 : 0));
            cairo_paint(cr);
        }
    }
    cairo_restore(cr);
}

void kt_draw_tab_pane(const KtTheme *t, cairo_t *cr, double x, double y, double w, double h)
{
    cairo_save(cr);
    if (t->style == KT_STYLE_CLASSIC) {
        classic_box(t, cr, x, y, w, h, t->window_bg, 0, 0);
    } else {
        KtTheme flat = *t;
        flat.shadow = t->shadow * 0.5;
        flat.gradient = 0;
        raised_body(&flat, cr, x, y, w, h, 0, t->window_bg, 0, radius(t) + 1);
    }
    cairo_restore(cr);
}

double kt_menu_radius(const KtTheme *t)
{
    if (t->style == KT_STYLE_CLASSIC || t->menu_radius < 1)
        return 0;
    return t->menu_radius > 16 ? 16 : t->menu_radius;
}

void kt_draw_menu(const KtTheme *t, cairo_t *cr, double x, double y, double w, double h)
{
    cairo_save(cr);
    if (t->style == KT_STYLE_CLASSIC) {
        classic_box(t, cr, x, y, w, h, t->menu_bg, 0, 1);
    } else {
        /* A menu is a rectangular window: the adapters clip it to the same
         * radius (kt_menu_radius), so what is outside the curve is never
         * shown. */
        double r = kt_menu_radius(t);
        if (t->style == KT_STYLE_SOFT && t->gradient > 0)
            gradient(cr, x, y, w, h, 0, kt_shade(t->menu_bg, 1 + 0.05 * t->gradient), kt_shade(t->menu_bg, 1 - 0.04 * t->gradient));
        else
            set(cr, t->menu_bg);
        if (r > 0) {
            rrect(cr, x, y, w, h, r);
            cairo_fill(cr);
        } else {
            cairo_rectangle(cr, x, y, w, h);
            cairo_fill(cr);
        }
        cairo_set_line_width(cr, 1);
        set(cr, kt_shade(t->menu_bg, 0.6));
        if (r > 0)
            rrect(cr, x + 0.5, y + 0.5, w - 1, h - 1, r - 0.5);
        else
            cairo_rectangle(cr, x + 0.5, y + 0.5, w - 1, h - 1);
        cairo_stroke(cr);
    }
    cairo_restore(cr);
}

void kt_draw_menuitem(const KtTheme *t, cairo_t *cr, double x, double y, double w, double h, int in_menubar)
{
    (void)in_menubar;
    cairo_save(cr);
    KtColor c = t->selection_bg;
    if (t->style == KT_STYLE_CLASSIC) {
        set(cr, c);
        cairo_rectangle(cr, x, y, w, h);
        cairo_fill(cr);
    } else if (t->style == KT_STYLE_FLAT) {
        set(cr, c);
        rrect(cr, x, y, w, h, radius(t));
        cairo_fill(cr);
    } else {
        double r = radius(t);
        gradient(cr, x, y, w, h, 0, kt_shade(c, 1 + 0.12 * t->gradient), kt_shade(c, 1 - 0.10 * t->gradient));
        rrect(cr, x, y, w, h, r);
        cairo_fill(cr);
        cairo_set_line_width(cr, 1);
        set(cr, kt_alpha(kt_shade(c, 0.7), 0.8));
        rrect(cr, x + 0.5, y + 0.5, w - 1, h - 1, r - 0.5);
        cairo_stroke(cr);
    }
    cairo_restore(cr);
}

void kt_draw_tooltip(const KtTheme *t, cairo_t *cr, double x, double y, double w, double h)
{
    cairo_save(cr);
    set(cr, t->tooltip_bg);
    cairo_rectangle(cr, x, y, w, h);
    cairo_fill(cr);
    cairo_set_line_width(cr, 1);
    set(cr, kt_luma(t->tooltip_bg) < 0.5 ? kt_shade(t->tooltip_bg, 2.2) : kt_shade(t->tooltip_bg, 0.5));
    cairo_rectangle(cr, x + 0.5, y + 0.5, w - 1, h - 1);
    cairo_stroke(cr);
    cairo_restore(cr);
}

void kt_draw_header(const KtTheme *t, cairo_t *cr, double x, double y, double w, double h, unsigned st)
{
    cairo_save(cr);
    if (t->style == KT_STYLE_CLASSIC) {
        classic_box(t, cr, x, y, w, h, t->button_bg, st & KT_PRESSED ? 2 : 0, 1);
        cairo_restore(cr);
        return;
    }
    KtColor bg = t->window_bg;
    if (st & KT_PRESSED)
        bg = kt_shade(bg, 0.92);
    else if (st & KT_HOVER)
        bg = kt_shade(bg, 1.04);
    if (t->style == KT_STYLE_SOFT)
        gradient(cr, x, y, w, h, 0, kt_shade(bg, 1 + 0.08 * t->gradient), kt_shade(bg, 1 - 0.06 * t->gradient));
    else
        set(cr, bg);
    cairo_rectangle(cr, x, y, w, h);
    cairo_fill(cr);
    cairo_set_line_width(cr, 1);
    set(cr, kt_alpha(kt_shade(t->window_bg, 0.55), 0.5));
    cairo_move_to(cr, x, y + h - 0.5);
    cairo_line_to(cr, x + w, y + h - 0.5);
    cairo_move_to(cr, x + w - 0.5, y + 3);
    cairo_line_to(cr, x + w - 0.5, y + h - 3);
    cairo_stroke(cr);
    cairo_restore(cr);
}

void kt_draw_row_selection(const KtTheme *t, cairo_t *cr, double x, double y, double w, double h, int focused)
{
    cairo_save(cr);
    KtColor c = focused ? t->selection_bg : kt_mix(t->selection_bg, t->base_bg, 0.45);
    if (t->style == KT_STYLE_SOFT && t->gradient > 0)
        gradient(cr, x, y, w, h, 0, kt_shade(c, 1 + 0.10 * t->gradient), kt_shade(c, 1 - 0.08 * t->gradient));
    else
        set(cr, c);
    cairo_rectangle(cr, x, y, w, h);
    cairo_fill(cr);
    cairo_restore(cr);
}

void kt_draw_separator(const KtTheme *t, cairo_t *cr, double x, double y, double w, double h, int vertical)
{
    cairo_save(cr);
    cairo_set_line_width(cr, 1);
    Bevel b = bevel_colors(t, t->window_bg);
    KtColor dark = t->style == KT_STYLE_CLASSIC ? b.shadow : kt_alpha(kt_shade(t->window_bg, 0.55), 0.6);
    KtColor lite = t->style == KT_STYLE_CLASSIC ? b.hl : kt_alpha(WHITE, 0.6 * t->relief);
    if (vertical) {
        double cx = floor(x + w / 2);
        set(cr, dark);
        cairo_move_to(cr, cx - 0.5, y);
        cairo_line_to(cr, cx - 0.5, y + h);
        cairo_stroke(cr);
        if (t->style != KT_STYLE_FLAT) {
            set(cr, lite);
            cairo_move_to(cr, cx + 0.5, y);
            cairo_line_to(cr, cx + 0.5, y + h);
            cairo_stroke(cr);
        }
    } else {
        double cy = floor(y + h / 2);
        set(cr, dark);
        cairo_move_to(cr, x, cy - 0.5);
        cairo_line_to(cr, x + w, cy - 0.5);
        cairo_stroke(cr);
        if (t->style != KT_STYLE_FLAT) {
            set(cr, lite);
            cairo_move_to(cr, x, cy + 0.5);
            cairo_line_to(cr, x + w, cy + 0.5);
            cairo_stroke(cr);
        }
    }
    cairo_restore(cr);
}

void kt_draw_focus(const KtTheme *t, cairo_t *cr, double x, double y, double w, double h)
{
    if (t->focus == KT_FOCUS_NONE || w < 2 || h < 2)
        return;
    cairo_save(cr);
    if (t->focus == KT_FOCUS_DOTTED || t->style == KT_STYLE_CLASSIC) {
        dotted_rect(cr, x, y, w, h, t->window_fg);
    } else {
        cairo_set_line_width(cr, 1);
        set(cr, kt_alpha(t->accent, 0.7));
        rrect(cr, x + 0.5, y + 0.5, w - 1, h - 1, radius(t));
        cairo_stroke(cr);
    }
    cairo_restore(cr);
}
