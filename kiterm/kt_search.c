/* kt_search.c - the find bar under the terminal (Ctrl+Shift+F). */
#include "kiterm.h"
#include "../shared/xis_i18n.h"
#include <string.h>

static void apply_pattern(KtWindow *w)
{
    VteTerminal *t = kt_window_current(w);
    if (!t)
        return;
    const char *text = gtk_entry_get_text(GTK_ENTRY(w->search_entry));
    if (!*text) {
        vte_terminal_search_set_gregex(t, NULL);
        vte_terminal_select_none(t);
        gtk_widget_modify_base(w->search_entry, GTK_STATE_NORMAL, NULL);
        return;
    }
    gboolean regex = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->search_regex));
    gboolean match_case = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->search_case));
    char *pat = regex ? g_strdup(text) : g_regex_escape_string(text, -1);
    GRegex *re = g_regex_new(pat, match_case ? 0 : G_REGEX_CASELESS, 0, NULL);
    g_free(pat);
    if (!re) {
        /* Half-typed regex: red field, keep the previous pattern. */
        GdkColor bad;
        gdk_color_parse("#ffb3b3", &bad);
        gtk_widget_modify_base(w->search_entry, GTK_STATE_NORMAL, &bad);
        return;
    }
    gtk_widget_modify_base(w->search_entry, GTK_STATE_NORMAL, NULL);
    vte_terminal_search_set_gregex(t, re);
    g_regex_unref(re);
    vte_terminal_search_set_wrap_around(t, TRUE);
    vte_terminal_select_none(t);
    vte_terminal_search_find_next(t);
}

void kt_search_step(KtWindow *w, gboolean forward)
{
    VteTerminal *t = kt_window_current(w);
    if (!t)
        return;
    if (!vte_terminal_search_get_gregex(t))
        apply_pattern(w);
    else if (forward)
        vte_terminal_search_find_next(t);
    else
        vte_terminal_search_find_previous(t);
}

static void on_changed(GtkEditable *e, gpointer d) { (void)e; apply_pattern(d); }
static void on_toggled(GtkToggleButton *b, gpointer d) { (void)b; apply_pattern(d); }
static void on_next(GtkButton *b, gpointer d) { (void)b; kt_search_step(d, TRUE); }
static void on_prev(GtkButton *b, gpointer d) { (void)b; kt_search_step(d, FALSE); }
static void on_close(GtkButton *b, gpointer d) { (void)b; kt_search_hide(d); }

static gboolean on_entry_key(GtkWidget *e, GdkEventKey *ev, gpointer d)
{
    (void)e;
    if (ev->keyval == GDK_KEY_Escape) {
        kt_search_hide(d);
        return TRUE;
    }
    if (ev->keyval == GDK_KEY_Return || ev->keyval == GDK_KEY_KP_Enter) {
        kt_search_step(d, !(ev->state & GDK_SHIFT_MASK));
        return TRUE;
    }
    return FALSE;
}

void kt_search_init(KtWindow *w, GtkWidget *vbox)
{
    GtkWidget *bar = gtk_hbox_new(FALSE, 4);
    gtk_container_set_border_width(GTK_CONTAINER(bar), 3);

    GtkWidget *close = gtk_button_new();
    gtk_button_set_relief(GTK_BUTTON(close), GTK_RELIEF_NONE);
    gtk_container_add(GTK_CONTAINER(close), gtk_image_new_from_stock(GTK_STOCK_CLOSE, GTK_ICON_SIZE_MENU));
    gtk_widget_set_tooltip_text(close, _("Close the search bar"));
    g_signal_connect(close, "clicked", G_CALLBACK(on_close), w);
    gtk_box_pack_start(GTK_BOX(bar), close, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(bar), gtk_label_new(_("Find:")), FALSE, FALSE, 0);
    w->search_entry = gtk_entry_new();
    g_signal_connect(w->search_entry, "changed", G_CALLBACK(on_changed), w);
    g_signal_connect(w->search_entry, "key-press-event", G_CALLBACK(on_entry_key), w);
    gtk_box_pack_start(GTK_BOX(bar), w->search_entry, TRUE, TRUE, 0);

    GtkWidget *prev = gtk_button_new();
    gtk_container_add(GTK_CONTAINER(prev), gtk_image_new_from_stock(GTK_STOCK_GO_UP, GTK_ICON_SIZE_MENU));
    gtk_widget_set_tooltip_text(prev, _("Previous match (Shift+Enter)"));
    g_signal_connect(prev, "clicked", G_CALLBACK(on_prev), w);
    GtkWidget *next = gtk_button_new();
    gtk_container_add(GTK_CONTAINER(next), gtk_image_new_from_stock(GTK_STOCK_GO_DOWN, GTK_ICON_SIZE_MENU));
    gtk_widget_set_tooltip_text(next, _("Next match (Enter)"));
    g_signal_connect(next, "clicked", G_CALLBACK(on_next), w);
    gtk_box_pack_start(GTK_BOX(bar), prev, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), next, FALSE, FALSE, 0);

    w->search_case = gtk_check_button_new_with_mnemonic(_("Match _case"));
    w->search_regex = gtk_check_button_new_with_mnemonic(_("_Regular expression"));
    g_signal_connect(w->search_case, "toggled", G_CALLBACK(on_toggled), w);
    g_signal_connect(w->search_regex, "toggled", G_CALLBACK(on_toggled), w);
    gtk_box_pack_start(GTK_BOX(bar), w->search_case, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), w->search_regex, FALSE, FALSE, 0);

    w->search_bar = bar;
    gtk_box_pack_end(GTK_BOX(vbox), bar, FALSE, FALSE, 0);
    gtk_widget_show_all(bar);
    gtk_widget_set_no_show_all(bar, TRUE);
    gtk_widget_hide(bar);
}

void kt_search_show(KtWindow *w)
{
    gtk_widget_show(w->search_bar);
    gtk_widget_grab_focus(w->search_entry);
    /* Pre-fill with the selection, like most terminals. */
    VteTerminal *t = kt_window_current(w);
    if (t && vte_terminal_get_has_selection(t)) {
        GtkClipboard *cb = gtk_widget_get_clipboard(GTK_WIDGET(t), GDK_SELECTION_PRIMARY);
        char *sel = gtk_clipboard_wait_for_text(cb);
        if (sel && *sel && !strchr(sel, '\n'))
            gtk_entry_set_text(GTK_ENTRY(w->search_entry), sel);
        g_free(sel);
    }
    gtk_editable_select_region(GTK_EDITABLE(w->search_entry), 0, -1);
}

void kt_search_hide(KtWindow *w)
{
    gtk_widget_hide(w->search_bar);
    VteTerminal *t = kt_window_current(w);
    if (t) {
        vte_terminal_search_set_gregex(t, NULL);
        gtk_widget_grab_focus(GTK_WIDGET(t));
    }
}
