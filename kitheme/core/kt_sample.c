/* libkitheme - a gallery of every element, drawn straight from a KtTheme.
 *
 * kiconf's KiTheme tab shows it as the live preview (so edits are visible
 * before anything is exported or applied) and `kitheme-export --sample`
 * writes it to a PNG. Text uses Cairo's own simple text API: good enough
 * for labels in a preview, and keeps Pango out of the library. */
#include "kitheme.h"

#include <stdio.h>

static void label(const KtTheme *t, cairo_t *cr, double x, double y, double w, double h, const char *s, KtColor c)
{
    (void)t;
    cairo_text_extents_t e;
    cairo_text_extents(cr, s, &e);
    cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a);
    cairo_move_to(cr, x + (w - e.width) / 2 - e.x_bearing, y + (h - e.height) / 2 - e.y_bearing);
    cairo_show_text(cr, s);
}

static void left_label(cairo_t *cr, double x, double y, double h, const char *s, KtColor c)
{
    cairo_text_extents_t e;
    cairo_text_extents(cr, "Xg", &e);
    cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a);
    cairo_move_to(cr, x, y + (h - e.height) / 2 - e.y_bearing);
    cairo_show_text(cr, s);
}

/* Progress bar with the two-color label, the way the adapters draw it. */
static void progress(const KtTheme *t, cairo_t *cr, double x, double y, double w, double h, double frac)
{
    kt_draw_progress_trough(t, cr, x, y, w, h, 0);
    double fw = (w - 2) * frac;
    kt_draw_progress_bar(t, cr, x + 1, y + 1, fw, h - 2, 0, 0);
    if (t->progress_text != KT_PTEXT_ALWAYS)
        return;
    char txt[16];
    snprintf(txt, sizeof(txt), "%d%%", (int)(frac * 100 + 0.5));
    cairo_save(cr);
    cairo_rectangle(cr, x + 1 + fw, y, w - fw, h);
    cairo_clip(cr);
    label(t, cr, x, y, w, h, txt, t->window_fg);
    cairo_restore(cr);
    cairo_save(cr);
    cairo_rectangle(cr, x + 1, y, fw, h);
    cairo_clip(cr);
    label(t, cr, x, y, w, h, txt, t->selection_fg);
    cairo_restore(cr);
}

void kt_draw_sample(const KtTheme *t, cairo_t *cr, double w, double h)
{
    cairo_save(cr);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 12);
    cairo_set_source_rgb(cr, t->window_bg.r, t->window_bg.g, t->window_bg.b);
    cairo_paint(cr);

    /* Menu bar with an open menu. */
    double mbh = 24;
    cairo_set_source_rgb(cr, t->window_bg.r, t->window_bg.g, t->window_bg.b);
    cairo_rectangle(cr, 0, 0, w, mbh);
    cairo_fill(cr);
    kt_draw_menuitem(t, cr, 4, 2, 52, mbh - 4, 1);
    label(t, cr, 4, 2, 52, mbh - 4, "Arquivo", t->selection_fg);
    label(t, cr, 60, 2, 52, mbh - 4, "Editar", t->window_fg);
    label(t, cr, 116, 2, 52, mbh - 4, "Ajuda", t->window_fg);
    kt_draw_separator(t, cr, 0, mbh - 1, w, 2, 0);

    /* Row 1: buttons. */
    double y = mbh + 12, bw = 82, bh = 28;
    static const struct {
        const char *text;
        unsigned st;
    } buttons[] = {
        {"Normal", 0}, {"Hover", KT_HOVER}, {"Pressed", KT_PRESSED}, {"Padrao", KT_DEFAULT | KT_FOCUSED}, {"Desativ.", KT_DISABLED},
    };
    for (int i = 0; i < 5; i++) {
        double bx = 12 + i * (bw + 6);
        kt_draw_button(t, cr, bx, y, bw, bh, buttons[i].st, t->button_bg);
        KtColor fg = buttons[i].st & KT_DISABLED ? kt_mix(t->button_fg, t->button_bg, 0.55) : t->button_fg;
        label(t, cr, bx, y + (buttons[i].st & KT_PRESSED && t->style == KT_STYLE_CLASSIC ? 1 : 0), bw, bh, buttons[i].text, fg);
    }

    /* Row 2: entry, combo, spin. */
    y += bh + 12;
    kt_draw_entry(t, cr, 12, y, 170, 26, KT_FOCUSED, t->base_bg);
    left_label(cr, 20, y, 26, "Texto digitado|", t->base_fg);
    kt_draw_button(t, cr, 190, y, 120, 26, 0, t->button_bg);
    left_label(cr, 200, y, 26, "Opcao 1", t->button_fg);
    kt_draw_arrow(t, cr, 290, y, 14, 26, KT_DOWN, t->button_fg);
    kt_draw_entry(t, cr, 318, y, 90, 26, 0, t->base_bg);
    left_label(cr, 326, y, 26, "42", t->base_fg);
    kt_draw_button(t, cr, 390, y + 2, 16, 11, 0, t->button_bg);
    kt_draw_arrow(t, cr, 390, y + 2, 16, 11, KT_UP, t->button_fg);
    kt_draw_button(t, cr, 390, y + 13, 16, 11, 0, t->button_bg);
    kt_draw_arrow(t, cr, 390, y + 13, 16, 11, KT_DOWN, t->button_fg);

    /* Row 3: checks and radios. */
    y += 26 + 12;
    double cs = t->check_size;
    static const struct {
        const char *text;
        unsigned st;
    } checks[] = {{"Marcado", KT_CHECKED}, {"Vazio", 0}, {"Misto", KT_MIXED}, {"Hover", KT_HOVER | KT_CHECKED}};
    for (int i = 0; i < 4; i++) {
        double cx = 12 + i * 100;
        kt_draw_check(t, cr, cx, y, cs, cs, checks[i].st);
        left_label(cr, cx + cs + 6, y, cs, checks[i].text, t->window_fg);
    }
    y += cs + 8;
    for (int i = 0; i < 3; i++) {
        double cx = 12 + i * 100;
        kt_draw_radio(t, cr, cx, y, cs, cs, i == 0 ? KT_CHECKED : i == 2 ? KT_DISABLED : 0);
        left_label(cr, cx + cs + 6, y, cs, i == 0 ? "Opcao A" : i == 1 ? "Opcao B" : "Desativada", t->window_fg);
    }

    /* Row 4: progress bars and a scale. */
    y += cs + 14;
    double pt = t->progress_thickness;
    progress(t, cr, 12, y, 300, pt, 0.62);
    kt_draw_scale_groove(t, cr, 330, y, 120, pt, 0, 70);
    kt_draw_scale_handle(t, cr, 330 + 70 - 8, y + pt / 2 - 8, 16, 16, 0);

    /* Row 5: tabs + pane, scrollbar, list, tooltip. */
    y += pt + 14;
    double th = 26, pane_h = h - y - th - 12;
    if (pane_h > 40) {
        kt_draw_tab_pane(t, cr, 12, y + th - 1, 300, pane_h);
        kt_draw_tab(t, cr, 14, y, 80, th, KT_DOWN, 0);
        label(t, cr, 14, y + 2, 80, th - 2, "Geral", t->window_fg);
        kt_draw_tab(t, cr, 92, y, 80, th, KT_DOWN, KT_SELECTED);
        label(t, cr, 92, y, 80, th, "Avancado", t->window_fg);
        kt_draw_tab(t, cr, 170, y, 80, th, KT_DOWN, KT_HOVER);
        label(t, cr, 170, y + 2, 80, th - 2, "Sobre", t->window_fg);

        double lx = 24, ly = y + th + 10, lw = 250, lh = pane_h - 22;
        kt_draw_entry(t, cr, lx, ly, lw, lh, 0, t->base_bg);
        kt_draw_header(t, cr, lx + 2, ly + 2, lw - 4 - t->scrollbar_width, 22, 0);
        left_label(cr, lx + 10, ly + 2, 22, "Nome", t->window_fg);
        static const char *rows[] = {"documento.txt", "foto.png", "musica.ogg", "video.mkv"};
        for (int i = 0; i < 4 && ly + 26 + i * 22 + 22 < ly + lh - 2; i++) {
            double ry = ly + 26 + i * 22;
            if (i == 1)
                kt_draw_row_selection(t, cr, lx + 2, ry, lw - 4 - t->scrollbar_width, 22, 1);
            left_label(cr, lx + 10, ry, 22, rows[i], i == 1 ? t->selection_fg : t->base_fg);
        }
        double sw = t->scrollbar_width, sx = lx + lw - 2 - sw, sy = ly + 2, sh = lh - 4;
        kt_draw_scroll_trough(t, cr, sx, sy, sw, sh, 1);
        double arrows = t->scrollbar_arrows ? sw : 0;
        if (arrows) {
            kt_draw_button(t, cr, sx, sy, sw, sw, 0, t->button_bg);
            kt_draw_arrow(t, cr, sx, sy, sw, sw, KT_UP, t->button_fg);
            kt_draw_button(t, cr, sx, sy + sh - sw, sw, sw, 0, t->button_bg);
            kt_draw_arrow(t, cr, sx, sy + sh - sw, sw, sw, KT_DOWN, t->button_fg);
        }
        kt_draw_scroll_slider(t, cr, sx, sy + arrows + 6, sw, (sh - 2 * arrows) * 0.4, 1, KT_HOVER);
    }

    /* An open menu and a tooltip on the right. */
    double mx = 330, my = y - 6, mw = 140;
    if (my + 120 < h) {
        kt_draw_menu(t, cr, mx, my, mw, 104);
        static const char *items[] = {"Novo", "Abrir...", "", "Sair"};
        double iy = my + 4;
        for (int i = 0; i < 4; i++) {
            if (!items[i][0]) {
                kt_draw_separator(t, cr, mx + 6, iy, mw - 12, 8, 0);
                iy += 8;
                continue;
            }
            if (i == 1)
                kt_draw_menuitem(t, cr, mx + 3, iy, mw - 6, 24, 0);
            left_label(cr, mx + 14, iy, 24, items[i], i == 1 ? t->selection_fg : t->menu_fg);
            iy += 28;
        }
        kt_draw_tooltip(t, cr, mx + 10, my + 114, 110, 24);
        label(t, cr, mx + 10, my + 114, 110, 24, "Dica de ferramenta", t->tooltip_fg);
    }
    cairo_restore(cr);
}
