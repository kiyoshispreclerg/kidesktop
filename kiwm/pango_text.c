/* pango_text.c - Pango-backed title text drawing, ported from xispanel's
 * pango_text.c (xispanel-plus-pango branch).
 *
 * Why: Cairo's toy font API (cairo_select_font_face/cairo_show_text, what
 * draw_decoration() used before this) renders through a single FreeType
 * face with no font-fallback of its own -- a codepoint missing from
 * whatever font that face happens to be (e.g. any CJK/Cyrillic/Arabic
 * glyph in a Latin-only sans font) just doesn't draw, showing as a blank
 * gap in the titlebar. Pango does per-glyph font substitution (via the
 * same Fontconfig database already on the system) and has ellipsizing
 * built into PangoLayout, so this replaces both the missing-glyph problem
 * and the old plain cairo_rectangle()+cairo_clip() hard-cutoff (which just
 * truncated mid-glyph rather than showing "...").
 */
#include "decoration.h"

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

/* Shared setup: a PangoLayout carrying `text` at `size_px`, ellipsized to
 * max_width_px if positive. `center` (only meaningful when max_width_px >
 * 0) horizontally centers the text within that width instead of the usual
 * left alignment -- used for kiwm's title_center= theme option, where the
 * title element is already the layout's full slot width, so centering is
 * just a Pango alignment flag, no separate spacer element needed. Caller
 * owns the returned layout (g_object_unref() it). */
static PangoLayout *build_layout(cairo_t *cr, const char *text, double size_px, double max_width_px, bool center)
{
    pango_font_description_set_absolute_size(g_desc, size_px * PANGO_SCALE);
    PangoLayout *layout = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(layout, g_desc);
    pango_layout_set_single_paragraph_mode(layout, TRUE);
    if (max_width_px > 0) {
        pango_layout_set_width(layout, (int)(max_width_px * PANGO_SCALE));
        pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
        if (center)
            pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
    }
    pango_layout_set_text(layout, text, -1);
    return layout;
}

/* Draws `text` (any valid UTF-8 -- Pango handles glyph fallback and, when
 * max_width_px > 0, ellipsizing on its own, no manual truncation needed)
 * at `x`, vertically centered within [top_y, top_y+box_h) and, if `center`
 * is set, horizontally centered within [x, x+max_width_px) too (left-
 * aligned at `x` otherwise). Reports the rendered pixel width via *out_w
 * (NULL if the caller doesn't need it). */
void pango_show_text_boxed(cairo_t *cr, double x, double top_y, double box_h, double max_width_px, double size_px,
                            const char *text, bool center, double *out_w)
{
    PangoLayout *layout = build_layout(cr, text, size_px, max_width_px, center);
    int lw, lh;
    pango_layout_get_pixel_size(layout, &lw, &lh);
    if (out_w) {
        *out_w = lw;
    }
    /* With centering, Pango's own alignment already positions the text
     * horizontally within the layout's full width -- cairo_move_to just
     * needs to land at that width's left edge (`x`), not lw's. */
    cairo_move_to(cr, x, top_y + (box_h - lh) / 2.0);
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);
}
