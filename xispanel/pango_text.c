/*
 * pango_text.c - Pango-backed text drawing, exploratory replacement for
 * the raw cairo_show_text()/cairo_text_extents() "toy font API" the rest
 * of the codebase still uses.
 *
 * Why: Cairo's toy font API renders through a single FreeType face with
 * no font-fallback of its own -- a codepoint missing from the system's
 * configured font (e.g. any CJK glyph in a decorative Latin font) just
 * doesn't draw. It also has no concept of ellipsizing itself; this
 * codebase's own trim_to_width() (ewmh.c) reimplements that by hand, one
 * removed codepoint at a time. Pango does per-glyph font substitution
 * (via the same Fontconfig database already linked) and has ellipsizing
 * built into PangoLayout, so migrating a call site to this replaces both
 * a manual truncation loop *and* the missing-glyph problem at once.
 *
 * Experiment (see branch xispanel-plus-pango): pulls Pango, and
 * transitively GLib, into xispanel's own process for the first time -- a
 * real departure from the "no toolkit" design this project otherwise
 * keeps to -- so this stays on its own branch until it's proven both
 * correct and not a regression (CPU/memory) before merging.
 */
#include "xispanel.h"

#include <pango/pangocairo.h>

static PangoFontDescription *g_desc = NULL;

void pango_text_init(const char *family)
{
    if (g_desc) {
        pango_font_description_free(g_desc);
    }
    g_desc = pango_font_description_new();
    pango_font_description_set_family(g_desc, family && family[0] ? family : "sans-serif");
}

/* Shared setup for both the measuring and drawing entry points below --
 * a PangoLayout carrying `text` at `size_px`, ellipsized to max_width_px
 * if positive. Caller owns the returned layout (g_object_unref() it). */
static PangoLayout *build_layout(cairo_t *cr, const char *text, double size_px, double max_width_px, int weight)
{
    pango_font_description_set_absolute_size(g_desc, size_px * PANGO_SCALE);
    pango_font_description_set_weight(g_desc, (PangoWeight)weight);
    PangoLayout *layout = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(layout, g_desc);
    pango_layout_set_single_paragraph_mode(layout, TRUE);
    if (max_width_px > 0) {
        pango_layout_set_width(layout, (int)(max_width_px * PANGO_SCALE));
        pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
    }
    pango_layout_set_text(layout, text, -1);
    return layout;
}

/* Measures `text` as it would be rendered at `size_px` -- including
 * ellipsizing to max_width_px first, if positive -- without drawing
 * anything. Used ahead of a draw call when the caller needs the final
 * (possibly-ellipsized) pixel size to position the text itself (e.g.
 * centering), since that isn't known until the layout is built. `cr`
 * only supplies font-rendering options (antialiasing etc.); a throwaway
 * probe surface's cr works fine, same as this codebase's existing
 * probe_cr pattern for cairo_text_extents(). */
void pango_text_extents_ellipsized(cairo_t *cr, const char *text, double size_px, double max_width_px, double *out_w,
                                    double *out_h)
{
    PangoLayout *layout = build_layout(cr, text, size_px, max_width_px, PANGO_WEIGHT_NORMAL);
    int lw, lh;
    pango_layout_get_pixel_size(layout, &lw, &lh);
    if (out_w) {
        *out_w = lw;
    }
    if (out_h) {
        *out_h = lh;
    }
    g_object_unref(layout);
}

/* Draws `text` (any valid UTF-8, no manual truncation needed -- Pango
 * handles glyph fallback and, when max_width_px > 0, ellipsizing on its
 * own) at `x`, vertically centered within [top_y, top_y+box_h). Reports
 * the rendered pixel width via *out_w (NULL if not needed, e.g. when the
 * caller doesn't need to know how much room the text actually used, or
 * already got it from pango_text_extents_ellipsized() to position `x`
 * itself for centering). */
void pango_show_text_boxed(cairo_t *cr, double x, double top_y, double box_h, double max_width_px, double size_px,
                            const char *text, double *out_w, const Panel *p)
{
    pango_show_text_boxed_bold(cr, x, top_y, box_h, max_width_px, size_px, text, 0, out_w, p);
}

/* pango_show_text_boxed(), with the title drawn bold when `bold` is set --
 * tasklist.c's urgent-window look (see ewmh_get_urgent()). Its own
 * function rather than a param on pango_show_text_boxed() so none of that
 * function's other ~15 call sites need touching. `bold` forces
 * PANGO_WEIGHT_BOLD regardless of `p`'s own font_weight= -- urgent is its
 * own distinct look, not a theme setting.
 *
 * `p`'s title_shadow and title_outline (see xispanel.c's
 * panel_load_theme_colors() doc comment) draw first, in that order --
 * same technique and draw order (shadow, outline, fill) as kiwm's own
 * pango_show_title_text() for window titles. The real fill always goes
 * last, on top, at whatever color the caller already set on `cr`. */
void pango_show_text_boxed_bold(cairo_t *cr, double x, double top_y, double box_h, double max_width_px,
                                 double size_px, const char *text, int bold, double *out_w, const Panel *p)
{
    int weight = bold ? PANGO_WEIGHT_BOLD : (p && p->font_weight ? p->font_weight : PANGO_WEIGHT_NORMAL);
    PangoLayout *layout = build_layout(cr, text, size_px, max_width_px, weight);
    int lw, lh;
    pango_layout_get_pixel_size(layout, &lw, &lh);
    if (out_w) {
        *out_w = lw;
    }
    double y = top_y + (box_h - lh) / 2.0;

    double fr, fg, fb, fa;
    cairo_pattern_get_rgba(cairo_get_source(cr), &fr, &fg, &fb, &fa);

    if (p && p->text_shadow) {
        cairo_set_source_rgba(cr, p->text_shadow_r, p->text_shadow_g, p->text_shadow_b, p->text_shadow_a);
        cairo_move_to(cr, x + p->text_shadow_dx, y + p->text_shadow_dy);
        pango_cairo_show_layout(cr, layout);
    }

    if (p && p->text_outline && p->text_outline_width > 0.0) {
        cairo_set_source_rgba(cr, p->text_outline_r, p->text_outline_g, p->text_outline_b, p->text_outline_a);
        cairo_set_line_width(cr, p->text_outline_width);
        /* Round joins/caps: a miter join on a glyph's sharp corners spikes
         * out well past the stroke width at panel text sizes. */
        cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_move_to(cr, x, y);
        pango_cairo_layout_path(cr, layout);
        cairo_stroke(cr);
    }

    cairo_set_source_rgba(cr, fr, fg, fb, fa);
    cairo_move_to(cr, x, y);
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);
}
