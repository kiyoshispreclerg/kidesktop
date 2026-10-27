/* kf_tabs.c - tabs over the whole browsing area.
 *
 * There is a single toolbar, places panel and main paned (app->body); a tab
 * switch moves that body into the current notebook page, so every tab looks
 * like it holds all of it. What a tab really owns is its panes_paned (one or
 * two panes). Those all sit side by side in app->panes_box with only the
 * current one shown: hidden rather than unparented, so the views of
 * background tabs stay realized and keep loading. */
#include "kf.h"
#include "../shared/xis_i18n.h"

static KfTab *tab_of_page(GtkWidget *page) { return page ? g_object_get_data(G_OBJECT(page), "kf-tab") : NULL; }
static GtkNotebook *nb(KfApp *app) { return GTK_NOTEBOOK(app->notebook); }

static KfTab *nth_tab(KfApp *app, int n) { return tab_of_page(gtk_notebook_get_nth_page(nb(app), n)); }

/* Tabs opened in the background since the last switch, so a run of them
 * lines up after the current tab in the order they were opened. */
static int bg_run;

int kf_tabs_count(KfApp *app) { return gtk_notebook_get_n_pages(nb(app)); }

/* A lone tab has nothing to switch to: hide the strip. */
static void sync_tab_bar(KfApp *app)
{
    gtk_notebook_set_show_tabs(nb(app), kf_tabs_count(app) > 1);
}

GList *kf_tabs_all(KfApp *app)
{
    GList *out = NULL;
    for (int i = kf_tabs_count(app) - 1; i >= 0; i--)
        out = g_list_prepend(out, nth_tab(app, i));
    return out;
}

void kf_tabs_update_label(KfTab *t)
{
    KfPane *p = t->active;
    if (!p || !p->dir)
        return;
    char *name = kf_app_dir_name(p->dir);
    gtk_label_set_text(GTK_LABEL(t->label), name);
    /* An ellipsizing label asks for almost no width: size it by its text, up
     * to about 24 characters, so short names aren't cut and long ones don't
     * hog the bar. */
    PangoLayout *layout = gtk_widget_create_pango_layout(t->label, name);
    int w;
    pango_layout_get_pixel_size(layout, &w, NULL);
    g_object_unref(layout);
    PangoFontMetrics *m = pango_context_get_metrics(gtk_widget_get_pango_context(t->label),
                                                    gtk_widget_get_style(t->label)->font_desc, NULL);
    int cap = PANGO_PIXELS(pango_font_metrics_get_approximate_char_width(m)) * 24;
    pango_font_metrics_unref(m);
    gtk_widget_set_size_request(t->label, MIN(w, cap), -1);
    char *where = g_file_is_native(p->dir) ? g_file_get_path(p->dir) : g_file_get_uri(p->dir);
    gtk_widget_set_tooltip_text(gtk_notebook_get_tab_label(nb(t->app), t->page), where);
    g_free(where);
    g_free(name);
}

static void on_switch_page(GtkNotebook *n, gpointer page, guint num, gpointer d)
{
    (void)n; (void)page;
    KfApp *app = d;
    KfTab *t = nth_tab(app, num);
    if (!t || t == app->tab)
        return;
    if (app->tab)
        gtk_widget_hide(app->tab->panes_paned);
    gtk_widget_show(t->panes_paned);
    GtkWidget *parent = gtk_widget_get_parent(app->body);
    if (!parent)
        gtk_box_pack_start(GTK_BOX(t->page), app->body, TRUE, TRUE, 0);
    else if (parent != t->page)
        gtk_widget_reparent(app->body, t->page);
    app->tab = t;
    bg_run = 0;
    kf_app_tab_switched(app);
}

static void on_close_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    kf_tabs_close(d);
}

static GtkWidget *make_label(KfTab *t)
{
    GtkWidget *box = gtk_hbox_new(FALSE, 4);
    t->label = gtk_label_new("");
    gtk_label_set_ellipsize(GTK_LABEL(t->label), PANGO_ELLIPSIZE_END);
    gtk_misc_set_alignment(GTK_MISC(t->label), 0, 0.5);
    gtk_box_pack_start(GTK_BOX(box), t->label, TRUE, TRUE, 0);
    GtkWidget *close = gtk_button_new();
    gtk_widget_set_name(close, "kifiles-tab-close");
    gtk_button_set_relief(GTK_BUTTON(close), GTK_RELIEF_NONE);
    gtk_button_set_focus_on_click(GTK_BUTTON(close), FALSE);
    gtk_container_add(GTK_CONTAINER(close), gtk_image_new_from_stock(GTK_STOCK_CLOSE, GTK_ICON_SIZE_MENU));
    gtk_widget_set_tooltip_text(close, _("Close tab (Ctrl+W)"));
    g_signal_connect(close, "clicked", G_CALLBACK(on_close_clicked), t);
    gtk_box_pack_start(GTK_BOX(box), close, FALSE, FALSE, 0);
    gtk_widget_show_all(box);
    return box;
}

KfTab *kf_tabs_open(KfApp *app, GFile *dir, gboolean switch_to)
{
    KfTab *t = g_new0(KfTab, 1);
    t->app = app;
    t->page = gtk_vbox_new(FALSE, 0);
    g_object_set_data_full(G_OBJECT(t->page), "kf-tab", t, g_free);
    t->panes_paned = gtk_hpaned_new();
    gtk_widget_set_no_show_all(t->panes_paned, TRUE);      /* on_switch_page shows the current one */
    gtk_box_pack_start(GTK_BOX(app->panes_box), t->panes_paned, TRUE, TRUE, 0);
    KfPane *p = kf_pane_new(app);
    p->tab = t;
    t->panes[0] = t->active = p;
    gtk_paned_pack1(GTK_PANED(t->panes_paned), p->box, TRUE, FALSE);
    kf_app_setup_pane(app, p);

    int pos = app->tab ? gtk_notebook_page_num(nb(app), app->tab->page) + 1 + (switch_to ? 0 : bg_run++) : -1;
    gtk_widget_show(t->page);
    int n = gtk_notebook_insert_page(nb(app), t->page, make_label(t), pos);
    gtk_notebook_set_tab_reorderable(nb(app), t->page, TRUE);
    sync_tab_bar(app);
    kf_pane_navigate(p, dir, FALSE, NULL);
    if (switch_to)
        gtk_notebook_set_current_page(nb(app), n);
    kf_app_pane_changed(app, app->active, KF_CHANGE_SELECTION);    /* tab actions' sensitivity */
    return t;
}

void kf_tabs_close(KfTab *t)
{
    KfApp *app = t->app;
    int count = kf_tabs_count(app);
    if (count <= 1) {
        kf_app_quit(app);
        return;
    }
    int n = gtk_notebook_page_num(nb(app), t->page);
    if (t == app->tab)      /* move the body out first: removing the page destroys what it holds */
        gtk_notebook_set_current_page(nb(app), n + 1 < count ? n + 1 : n - 1);
    for (int i = 0; i < 2; i++)
        if (t->panes[i])
            kf_pane_destroy(t->panes[i]);
    gtk_widget_destroy(t->panes_paned);
    gtk_notebook_remove_page(nb(app), gtk_notebook_page_num(nb(app), t->page));   /* frees t */
    sync_tab_bar(app);
    kf_app_pane_changed(app, app->active, KF_CHANGE_SELECTION);
}

void kf_tabs_close_others(KfTab *keep)
{
    KfApp *app = keep->app;
    gtk_notebook_set_current_page(nb(app), gtk_notebook_page_num(nb(app), keep->page));
    GList *all = kf_tabs_all(app);
    for (GList *l = all; l; l = l->next)
        if (l->data != keep)
            kf_tabs_close(l->data);
    g_list_free(all);
}

void kf_tabs_detach(KfTab *t)
{
    if (t->active && t->active->dir)
        kf_app_open_window(t->active->dir);
    kf_tabs_close(t);
}

void kf_tabs_step(KfApp *app, int dir)
{
    int count = kf_tabs_count(app);
    if (count > 1)
        gtk_notebook_set_current_page(nb(app), (gtk_notebook_get_current_page(nb(app)) + dir + count) % count);
}

/* ---- tab bar mouse ------------------------------------------------------------ */

/* The tab nearest to the pointer: the notebook only reports a press on its
 * tab strip, not which tab it hit. */
static KfTab *tab_at_pointer(KfApp *app)
{
    GtkAllocation na;
    gtk_widget_get_allocation(app->notebook, &na);
    int px, py;
    gtk_widget_get_pointer(app->notebook, &px, &py);    /* relative to the allocation: no own window */
    KfTab *best = NULL;
    int best_d = G_MAXINT;
    for (int i = 0; i < kf_tabs_count(app); i++) {
        KfTab *t = nth_tab(app, i);
        GtkWidget *lb = gtk_notebook_get_tab_label(nb(app), t->page);
        if (!gtk_widget_get_mapped(lb))         /* scrolled out of the strip */
            continue;
        GtkAllocation a;
        gtk_widget_get_allocation(lb, &a);
        int x0 = a.x - na.x, x1 = x0 + a.width;
        int d = px < x0 ? x0 - px : px > x1 ? px - x1 : 0;
        if (d < best_d) {
            best_d = d;
            best = t;
        }
    }
    return best;
}

static KfTab *menu_tab(GtkMenuItem *mi) { return g_object_get_data(G_OBJECT(mi), "kf-tab"); }

static void m_new(GtkMenuItem *mi, gpointer d)
{
    (void)d;
    KfTab *t = menu_tab(mi);
    if (t->active && t->active->dir)
        kf_tabs_open(t->app, t->active->dir, TRUE);
}

static void m_detach(GtkMenuItem *mi, gpointer d) { (void)d; kf_tabs_detach(menu_tab(mi)); }
static void m_close_others(GtkMenuItem *mi, gpointer d) { (void)d; kf_tabs_close_others(menu_tab(mi)); }
static void m_close(GtkMenuItem *mi, gpointer d) { (void)d; kf_tabs_close(menu_tab(mi)); }

static void add_item(GtkWidget *menu, const char *label, GCallback cb, KfTab *t, gboolean sensitive)
{
    GtkWidget *mi = gtk_menu_item_new_with_mnemonic(label);
    g_object_set_data(G_OBJECT(mi), "kf-tab", t);
    g_signal_connect(mi, "activate", cb, NULL);
    gtk_widget_set_sensitive(mi, sensitive);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
}

static void popup_menu(KfTab *t, GdkEventButton *ev)
{
    gboolean many = kf_tabs_count(t->app) > 1;
    GtkWidget *menu = gtk_menu_new();
    add_item(menu, _("New _Tab"), G_CALLBACK(m_new), t, TRUE);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    add_item(menu, _("_Detach Tab"), G_CALLBACK(m_detach), t, many);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    add_item(menu, _("Close _Other Tabs"), G_CALLBACK(m_close_others), t, many);
    add_item(menu, _("_Close Tab"), G_CALLBACK(m_close), t, TRUE);
    gtk_widget_show_all(menu);
    g_signal_connect(menu, "selection-done", G_CALLBACK(gtk_widget_destroy), NULL);
    gtk_menu_popup(GTK_MENU(menu), NULL, NULL, NULL, NULL, ev->button, ev->time);
}

/* Right button: the tab's menu; middle button closes it. Runs before the
 * notebook's own handler, which only cares about the left button. */
static gboolean on_notebook_button(GtkWidget *w, GdkEventButton *ev, gpointer d)
{
    KfApp *app = d;
    if (ev->type != GDK_BUTTON_PRESS || (ev->button != 2 && ev->button != 3))
        return FALSE;
    if (gtk_get_event_widget((GdkEvent *)ev) != w)     /* a press inside a page, bubbling up */
        return FALSE;
    KfTab *t = tab_at_pointer(app);
    if (!t)
        return FALSE;
    if (ev->button == 2)
        kf_tabs_close(t);
    else
        popup_menu(t, ev);
    return TRUE;
}

/* The notebook's side frame is dropped (xthickness 0, see kf_tabs_setup) so
 * the views reach the window's edges; the tabs get that padding back as
 * tab-hborder, from the theme's own notebook style. */
static void on_notebook_style(GtkWidget *w, GtkStyle *prev, gpointer d)
{
    (void)prev; (void)d;
    GtkStyle *theme = gtk_rc_get_style_by_paths(gtk_widget_get_settings(w), NULL, NULL, GTK_TYPE_NOTEBOOK);
    guint hb = 2 + (theme ? theme->xthickness : 0);
    guint cur = 0;
    g_object_get(w, "tab-hborder", &cur, NULL);
    if (cur != hb)
        g_object_set(w, "tab-hborder", hb, NULL);
}

void kf_tabs_setup(KfApp *app)
{
    gtk_rc_parse_string("style \"kifiles-tabs\" { xthickness = 0 }\n"
                        "widget \"*.kifiles-tabs\" style \"kifiles-tabs\"\n"
                        "style \"kifiles-tab-close\" {\n"
                        "  xthickness = 0\n  ythickness = 0\n"
                        "  GtkButton::inner-border = { 0, 0, 0, 0 }\n"
                        "  GtkWidget::focus-padding = 0\n  GtkWidget::focus-line-width = 0\n"
                        "}\n"
                        "widget \"*.kifiles-tab-close\" style \"kifiles-tab-close\"\n");
    app->notebook = gtk_notebook_new();
    gtk_widget_set_name(app->notebook, "kifiles-tabs");
    gtk_notebook_set_show_border(nb(app), FALSE);      /* a lone tab: no frame at all */
    g_signal_connect(app->notebook, "style-set", G_CALLBACK(on_notebook_style), NULL);
    gtk_notebook_set_scrollable(nb(app), TRUE);
    gtk_widget_set_can_focus(app->notebook, FALSE);    /* a click on a tab leaves focus in the view */
    g_signal_connect_after(app->notebook, "switch-page", G_CALLBACK(on_switch_page), app);
    g_signal_connect(app->notebook, "button-press-event", G_CALLBACK(on_notebook_button), app);
}
