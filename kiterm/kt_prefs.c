/* kt_prefs.c - preferences dialog. Every change applies to the open tabs
 * right away and is written to kiterm.conf, so there is no OK/Apply step
 * (same behavior as kiconf). Opacity needs the window to have an RGBA
 * colormap, which can only be chosen before it is shown. */
#include "kiterm.h"
#include "../shared/xis_i18n.h"
#include <string.h>

static GtkWidget *dialog;   /* one at a time */

static void changed(KtWindow *w)
{
    kt_window_apply_config(w);
    kt_config_save(&w->cfg);
}

static void on_font(GtkFontButton *b, gpointer d)
{
    KtWindow *w = d;
    g_strlcpy(w->cfg.font, gtk_font_button_get_font_name(b), sizeof(w->cfg.font));
    changed(w);
}

static void on_color(GtkColorButton *b, gpointer d)
{
    KtWindow *w = d;
    GdkColor *dst = g_object_get_data(G_OBJECT(b), "kt-dst");
    gtk_color_button_get_color(b, dst);
    changed(w);
}

static void on_scheme(GtkComboBox *c, gpointer d)
{
    KtWindow *w = d;
    int i = gtk_combo_box_get_active(c) - 1;   /* entry 0 is the "Choose..." hint */
    if (i < 0 || i >= kt_n_schemes)
        return;
    kt_config_apply_scheme(&w->cfg, &kt_schemes[i]);
    /* Mirror into the swatches without re-triggering on_color. */
    GSList *l = g_object_get_data(G_OBJECT(c), "kt-swatches");
    for (; l; l = l->next) {
        GtkColorButton *b = l->data;
        g_signal_handlers_block_by_func(b, on_color, w);
        gtk_color_button_set_color(b, g_object_get_data(G_OBJECT(b), "kt-dst"));
        g_signal_handlers_unblock_by_func(b, on_color, w);
    }
    changed(w);
}

static void on_opacity(GtkRange *r, gpointer d)
{
    KtWindow *w = d;
    w->cfg.opacity = gtk_range_get_value(r) / 100.0;
    changed(w);
    GtkWidget *note = g_object_get_data(G_OBJECT(r), "kt-note");
    if (note && w->cfg.opacity < 1.0 && !w->argb)
        gtk_widget_show(note);
}

static void on_spin_scroll(GtkSpinButton *s, gpointer d)
{
    KtWindow *w = d;
    w->cfg.scrollback = gtk_spin_button_get_value_as_int(s);
    changed(w);
}

static void on_spin_cols(GtkSpinButton *s, gpointer d)
{
    KtWindow *w = d;
    w->cfg.cols = gtk_spin_button_get_value_as_int(s);
    kt_config_save(&w->cfg);    /* size applies to new tabs only */
}

static void on_spin_rows(GtkSpinButton *s, gpointer d)
{
    KtWindow *w = d;
    w->cfg.rows = gtk_spin_button_get_value_as_int(s);
    kt_config_save(&w->cfg);
}

static void on_spin_secs(GtkSpinButton *s, gpointer d)
{
    KtWindow *w = d;
    w->cfg.notify_done_secs = gtk_spin_button_get_value_as_int(s);
    kt_config_save(&w->cfg);
}

static void on_toggle(GtkToggleButton *b, gpointer d)
{
    KtWindow *w = d;
    gboolean *dst = g_object_get_data(G_OBJECT(b), "kt-dst");
    *dst = gtk_toggle_button_get_active(b);
    changed(w);
}

static void on_combo_int(GtkComboBox *c, gpointer d)
{
    KtWindow *w = d;
    int *dst = g_object_get_data(G_OBJECT(c), "kt-dst");
    *dst = gtk_combo_box_get_active(c);
    changed(w);
}

static void on_shell(GtkEditable *e, gpointer d)
{
    KtWindow *w = d;
    g_strlcpy(w->cfg.shell, gtk_entry_get_text(GTK_ENTRY(e)), sizeof(w->cfg.shell));
    kt_config_save(&w->cfg);
}

static void on_wordchars(GtkEditable *e, gpointer d)
{
    KtWindow *w = d;
    g_strlcpy(w->cfg.word_chars, gtk_entry_get_text(GTK_ENTRY(e)), sizeof(w->cfg.word_chars));
    changed(w);
}

static void row(GtkWidget *table, int r, const char *label, GtkWidget *w)
{
    GtkWidget *l = gtk_label_new_with_mnemonic(label);
    gtk_misc_set_alignment(GTK_MISC(l), 0, 0.5);
    gtk_label_set_mnemonic_widget(GTK_LABEL(l), w);
    gtk_table_attach(GTK_TABLE(table), l, 0, 1, r, r + 1, GTK_FILL, 0, 0, 0);
    gtk_table_attach(GTK_TABLE(table), w, 1, 2, r, r + 1, GTK_FILL | GTK_EXPAND, 0, 0, 0);
}

static GtkWidget *new_table(void)
{
    GtkWidget *t = gtk_table_new(1, 2, FALSE);
    gtk_table_set_row_spacings(GTK_TABLE(t), 6);
    gtk_table_set_col_spacings(GTK_TABLE(t), 12);
    gtk_container_set_border_width(GTK_CONTAINER(t), 12);
    return t;
}

static GtkWidget *check(KtWindow *w, const char *label, gboolean *dst)
{
    GtkWidget *c = gtk_check_button_new_with_mnemonic(label);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(c), *dst);
    g_object_set_data(G_OBJECT(c), "kt-dst", dst);
    g_signal_connect(c, "toggled", G_CALLBACK(on_toggle), w);
    return c;
}

static GtkWidget *swatch(KtWindow *w, GdkColor *dst, GSList **list)
{
    GtkWidget *b = gtk_color_button_new_with_color(dst);
    g_object_set_data(G_OBJECT(b), "kt-dst", dst);
    g_signal_connect(b, "color-set", G_CALLBACK(on_color), w);
    *list = g_slist_prepend(*list, b);
    return b;
}

static GtkWidget *page_general(KtWindow *w)
{
    GtkWidget *t = new_table();
    int r = 0;
    GtkWidget *font = gtk_font_button_new_with_font(w->cfg.font);
    gtk_font_button_set_show_style(GTK_FONT_BUTTON(font), TRUE);
    g_signal_connect(font, "font-set", G_CALLBACK(on_font), w);
    gtk_table_resize(GTK_TABLE(t), 8, 2);
    row(t, r++, _("_Font:"), font);

    GtkWidget *scroll = gtk_spin_button_new_with_range(0, 1000000, 1000);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(scroll), w->cfg.scrollback);
    g_signal_connect(scroll, "value-changed", G_CALLBACK(on_spin_scroll), w);
    row(t, r++, _("_Scrollback lines:"), scroll);

    GtkWidget *cols = gtk_spin_button_new_with_range(20, 500, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(cols), w->cfg.cols);
    g_signal_connect(cols, "value-changed", G_CALLBACK(on_spin_cols), w);
    row(t, r++, _("Initial _columns:"), cols);
    GtkWidget *rows = gtk_spin_button_new_with_range(5, 200, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(rows), w->cfg.rows);
    g_signal_connect(rows, "value-changed", G_CALLBACK(on_spin_rows), w);
    row(t, r++, _("Initial _rows:"), rows);

    GtkWidget *shell = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(shell), w->cfg.shell);
    g_signal_connect(shell, "changed", G_CALLBACK(on_shell), w);
    gtk_widget_set_tooltip_text(shell, _("Empty uses $SHELL. Applies to new tabs."));
    row(t, r++, _("S_hell:"), shell);

    GtkWidget *words = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(words), w->cfg.word_chars);
    g_signal_connect(words, "changed", G_CALLBACK(on_wordchars), w);
    gtk_widget_set_tooltip_text(words, _("Characters that count as part of a word when double-clicking."));
    row(t, r++, _("_Word characters:"), words);

    GtkWidget *tabs = gtk_combo_box_new_text();
    gtk_combo_box_append_text(GTK_COMBO_BOX(tabs), _("Top"));
    gtk_combo_box_append_text(GTK_COMBO_BOX(tabs), _("Bottom"));
    gtk_combo_box_append_text(GTK_COMBO_BOX(tabs), _("Left"));
    gtk_combo_box_append_text(GTK_COMBO_BOX(tabs), _("Right"));
    gtk_combo_box_set_active(GTK_COMBO_BOX(tabs), w->cfg.tabs_pos);
    g_object_set_data(G_OBJECT(tabs), "kt-dst", &w->cfg.tabs_pos);
    g_signal_connect(tabs, "changed", G_CALLBACK(on_combo_int), w);
    row(t, r++, _("_Tab bar:"), tabs);

    GtkWidget *conf = check(w, _("Ask before closing a tab that is running a program"), &w->cfg.confirm_close);
    gtk_table_attach(GTK_TABLE(t), conf, 0, 2, r, r + 1, GTK_FILL, 0, 0, 0);
    return t;
}

static GtkWidget *page_colors(KtWindow *w)
{
    GtkWidget *t = new_table();
    gtk_table_resize(GTK_TABLE(t), 10, 2);
    GSList *swatches = NULL;
    int r = 0;

    GtkWidget *scheme = gtk_combo_box_new_text();
    gtk_combo_box_append_text(GTK_COMBO_BOX(scheme), _("Choose a preset..."));
    for (int i = 0; i < kt_n_schemes; i++)
        gtk_combo_box_append_text(GTK_COMBO_BOX(scheme), _(kt_schemes[i].label));
    gtk_combo_box_set_active(GTK_COMBO_BOX(scheme), 0);
    row(t, r++, _("_Color scheme:"), scheme);

    row(t, r++, _("_Text:"), swatch(w, &w->cfg.fg, &swatches));
    row(t, r++, _("_Background:"), swatch(w, &w->cfg.bg, &swatches));
    row(t, r++, _("C_ursor:"), swatch(w, &w->cfg.cursor, &swatches));

    GtkWidget *pal = gtk_table_new(2, 8, TRUE);
    gtk_table_set_col_spacings(GTK_TABLE(pal), 2);
    gtk_table_set_row_spacings(GTK_TABLE(pal), 2);
    for (int i = 0; i < 16; i++)
        gtk_table_attach_defaults(GTK_TABLE(pal), swatch(w, &w->cfg.palette[i], &swatches),
                                  i % 8, i % 8 + 1, i / 8, i / 8 + 1);
    row(t, r++, _("_Palette:"), pal);

    GtkWidget *op = gtk_hscale_new_with_range(10, 100, 1);
    gtk_scale_set_value_pos(GTK_SCALE(op), GTK_POS_RIGHT);
    gtk_range_set_value(GTK_RANGE(op), w->cfg.opacity * 100.0);
    GtkWidget *note = gtk_label_new(_("Transparency starts working in new windows."));
    gtk_widget_set_no_show_all(note, TRUE);
    g_object_set_data(G_OBJECT(op), "kt-note", note);
    g_signal_connect(op, "value-changed", G_CALLBACK(on_opacity), w);
    row(t, r++, _("_Opacity (%):"), op);
    gtk_table_attach(GTK_TABLE(t), note, 1, 2, r, r + 1, GTK_FILL, 0, 0, 0);
    r++;

    g_object_set_data(G_OBJECT(scheme), "kt-swatches", swatches);
    g_object_set_data(G_OBJECT(scheme), "kt-w", w);
    g_signal_connect(scheme, "changed", G_CALLBACK(on_scheme), w);
    return t;
}

static GtkWidget *page_behavior(KtWindow *w)
{
    GtkWidget *t = new_table();
    gtk_table_resize(GTK_TABLE(t), 9, 2);
    int r = 0;

    GtkWidget *shape = gtk_combo_box_new_text();
    gtk_combo_box_append_text(GTK_COMBO_BOX(shape), _("Block"));
    gtk_combo_box_append_text(GTK_COMBO_BOX(shape), _("I-beam"));
    gtk_combo_box_append_text(GTK_COMBO_BOX(shape), _("Underline"));
    gtk_combo_box_set_active(GTK_COMBO_BOX(shape), w->cfg.cursor_shape);
    g_object_set_data(G_OBJECT(shape), "kt-dst", &w->cfg.cursor_shape);
    g_signal_connect(shape, "changed", G_CALLBACK(on_combo_int), w);
    row(t, r++, _("Cursor _shape:"), shape);

    gboolean *flags[] = { &w->cfg.cursor_blink, &w->cfg.scroll_on_output, &w->cfg.scroll_on_keystroke,
                          &w->cfg.audible_bell, &w->cfg.urgent_on_bell, &w->cfg.notify_done };
    const char *labels[] = { _("Blinking cursor"), _("Scroll to the bottom on output"),
                             _("Scroll to the bottom on keystroke"), _("Play the bell sound"),
                             _("Flag the window when the bell rings in the background"),
                             _("Notify when a program finishes in another tab or an unfocused window") };
    for (int i = 0; i < 6; i++, r++)
        gtk_table_attach(GTK_TABLE(t), check(w, labels[i], flags[i]), 0, 2, r, r + 1, GTK_FILL, 0, 0, 0);

    GtkWidget *secs = gtk_spin_button_new_with_range(1, 3600, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(secs), w->cfg.notify_done_secs);
    g_signal_connect(secs, "value-changed", G_CALLBACK(on_spin_secs), w);
    row(t, r++, _("...only if it ran for at least (seconds):"), secs);
    return t;
}

static void on_response(GtkDialog *d, gint resp, gpointer data)
{
    (void)resp; (void)data;
    gtk_widget_destroy(GTK_WIDGET(d));
}

static void on_destroy(GtkWidget *d, gpointer data)
{
    (void)d; (void)data;
    dialog = NULL;
}

void kt_prefs_show(KtWindow *w)
{
    if (dialog) {
        gtk_window_present(GTK_WINDOW(dialog));
        return;
    }
    dialog = gtk_dialog_new_with_buttons(_("kiterm preferences"), GTK_WINDOW(w->win), 0,
                                         GTK_STOCK_CLOSE, GTK_RESPONSE_CLOSE, NULL);
    GtkWidget *nb = gtk_notebook_new();
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), page_general(w), gtk_label_new(_("General")));
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), page_colors(w), gtk_label_new(_("Appearance")));
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), page_behavior(w), gtk_label_new(_("Behavior")));
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dialog)->vbox), nb, TRUE, TRUE, 0);
    g_signal_connect(dialog, "response", G_CALLBACK(on_response), NULL);
    g_signal_connect(dialog, "destroy", G_CALLBACK(on_destroy), NULL);
    gtk_widget_show_all(dialog);
}
