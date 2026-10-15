/* kf_crumbs.c - breadcrumb location bar for one pane.
 *
 * Buttons for every folder from the root (or Home) down to the current one;
 * when they don't fit, the oldest are hidden behind a "..." menu. Clicking the
 * empty part of the bar (or Ctrl+L) turns it into a location entry with
 * folder completion. */
#include "kf.h"
#include "../shared/xis_i18n.h"
#include <string.h>

typedef struct {
    KfPane *pane;
    GtkWidget *bar;             /* the hbox returned to the pane */
    GtkWidget *btns;            /* hbox of crumb buttons + filler */
    GtkWidget *more;            /* the "..." menu button */
    GtkWidget *entry;
    GtkListStore *completions;
    GFile *dir;
    GPtrArray *items;           /* Crumb* */
    int first_visible;
    guint relayout_id;
} Crumbs;

typedef struct {
    GFile *file;
    GtkWidget *sep;             /* the "›" before this crumb (NULL for the first) */
    GtkWidget *btn;
    int width;                  /* button + separator requisition */
} Crumb;

static void crumb_free(gpointer data)
{
    Crumb *c = data;
    g_object_unref(c->file);
    g_free(c);
}

static void crumbs_free(gpointer data)
{
    Crumbs *c = data;
    if (c->relayout_id)
        g_source_remove(c->relayout_id);
    g_ptr_array_free(c->items, TRUE);
    if (c->dir)
        g_object_unref(c->dir);
    g_free(c);
}

static Crumbs *get(GtkWidget *w) { return g_object_get_data(G_OBJECT(w), "kf-crumbs"); }

static char *crumb_label(GFile *f, gboolean is_first, gboolean under_home)
{
    if (is_first && under_home)
        return g_strdup(_("Home"));
    if (g_file_has_uri_scheme(f, "trash")) {
        GFile *par = g_file_get_parent(f);
        if (!par)
            return g_strdup(_("Trash"));
        g_object_unref(par);
    }
    char *base = g_file_get_basename(f);
    if (!base || !strcmp(base, "/") || !*base) {
        g_free(base);
        if (g_file_is_native(f))
            return g_strdup("/");
        return g_file_get_parse_name(f);
    }
    char *disp = g_filename_display_name(base);
    g_free(base);
    return disp;
}

static void on_crumb_clicked(GtkButton *b, gpointer data)
{
    Crumbs *c = data;
    GFile *f = g_object_get_data(G_OBJECT(b), "kf-file");
    kf_pane_navigate(c->pane, f, TRUE, NULL);
}

static void on_more_item(GtkMenuItem *mi, gpointer data)
{
    Crumbs *c = data;
    kf_pane_navigate(c->pane, g_object_get_data(G_OBJECT(mi), "kf-file"), TRUE, NULL);
}

static void on_more_clicked(GtkButton *b, gpointer data)
{
    Crumbs *c = data;
    GtkWidget *menu = gtk_menu_new();
    for (int i = c->first_visible - 1; i >= 0; i--) {
        Crumb *cr = g_ptr_array_index(c->items, i);
        const char *txt = gtk_button_get_label(GTK_BUTTON(cr->btn));
        GtkWidget *mi = gtk_menu_item_new_with_label(txt);
        g_object_set_data(G_OBJECT(mi), "kf-file", cr->file);
        g_signal_connect(mi, "activate", G_CALLBACK(on_more_item), c);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    }
    gtk_widget_show_all(menu);
    g_signal_connect(menu, "selection-done", G_CALLBACK(gtk_widget_destroy), NULL);
    gtk_menu_popup(GTK_MENU(menu), NULL, NULL, NULL, NULL, 1, gtk_get_current_event_time());
    (void)b;
}

/* Show as many of the newest crumbs as fit, in an idle so that hiding
 * widgets never happens inside size-allocate. */
static gboolean relayout(gpointer data)
{
    Crumbs *c = data;
    c->relayout_id = 0;
    GtkAllocation a;
    gtk_widget_get_allocation(c->btns, &a);
    int n = c->items->len;
    if (n == 0 || a.width <= 1)
        return FALSE;
    GtkRequisition mr;
    gtk_widget_size_request(c->more, &mr);
    int avail = a.width - 8, used = 0, first = n;
    for (int i = n - 1; i >= 0; i--) {
        Crumb *cr = g_ptr_array_index(c->items, i);
        int need = used + cr->width + (i > 0 ? mr.width : 0);
        if (need > avail && first < n)
            break;
        used += cr->width;
        first = i;
    }
    c->first_visible = first;
    for (int i = 0; i < n; i++) {
        Crumb *cr = g_ptr_array_index(c->items, i);
        gtk_widget_set_visible(cr->btn, i >= first);
        if (cr->sep)
            gtk_widget_set_visible(cr->sep, i > first);
    }
    gtk_widget_set_visible(c->more, first > 0);
    return FALSE;
}

static void queue_relayout(Crumbs *c)
{
    if (!c->relayout_id)
        c->relayout_id = g_idle_add(relayout, c);
}

static void on_btns_allocate(GtkWidget *w, GtkAllocation *a, gpointer data)
{
    (void)w; (void)a;
    queue_relayout(data);
}

static gboolean on_filler_press(GtkWidget *w, GdkEventButton *ev, gpointer data)
{
    (void)w;
    if (ev->button == 1) {
        kf_crumbs_edit(((Crumbs *)data)->bar);
        return TRUE;
    }
    return FALSE;
}

void kf_crumbs_set_location(GtkWidget *crumbs, GFile *dir)
{
    Crumbs *c = get(crumbs);
    if (c->dir)
        g_object_unref(c->dir);
    c->dir = g_object_ref(dir);

    /* Drop the old buttons (the filler and the "..." stay). */
    for (guint i = 0; i < c->items->len; i++) {
        Crumb *cr = g_ptr_array_index(c->items, i);
        if (cr->sep)
            gtk_widget_destroy(cr->sep);
        gtk_widget_destroy(cr->btn);
    }
    g_ptr_array_set_size(c->items, 0);

    /* Chain from the folder up to the root, or to Home when inside it. */
    GFile *home = g_file_new_for_path(g_get_home_dir());
    gboolean under_home = g_file_equal(dir, home) || g_file_has_prefix(dir, home);
    GList *chain = NULL;
    GFile *cur = g_object_ref(dir);
    while (cur) {
        chain = g_list_prepend(chain, cur);
        if (under_home && g_file_equal(cur, home))
            break;
        cur = g_file_get_parent(cur);
    }
    g_object_unref(home);

    int pos = 0;
    for (GList *l = chain; l; l = l->next, pos++) {
        GFile *f = l->data;                 /* takes over the chain's ref */
        gboolean last = !l->next;
        Crumb *cr = g_new0(Crumb, 1);
        cr->file = f;
        if (pos > 0) {
            cr->sep = gtk_label_new("›");
            gtk_box_pack_start(GTK_BOX(c->btns), cr->sep, FALSE, FALSE, 0);
        }
        char *txt = crumb_label(f, pos == 0, under_home);
        cr->btn = gtk_button_new();
        GtkWidget *lab = gtk_label_new(NULL);
        char *esc = g_markup_escape_text(txt, -1);
        char *mk = last ? g_strdup_printf("<b>%s</b>", esc) : g_strdup(esc);
        gtk_label_set_markup(GTK_LABEL(lab), mk);
        g_free(mk);
        g_free(esc);
        gtk_container_add(GTK_CONTAINER(cr->btn), lab);
        gtk_button_set_relief(GTK_BUTTON(cr->btn), GTK_RELIEF_NONE);
        gtk_button_set_focus_on_click(GTK_BUTTON(cr->btn), FALSE);
        gtk_widget_set_tooltip_text(cr->btn, txt);
        g_free(txt);
        g_object_set_data(G_OBJECT(cr->btn), "kf-file", f);
        g_signal_connect(cr->btn, "clicked", G_CALLBACK(on_crumb_clicked), c);
        gtk_box_pack_start(GTK_BOX(c->btns), cr->btn, FALSE, FALSE, 0);
        gtk_widget_show_all(cr->btn);
        if (cr->sep)
            gtk_widget_show(cr->sep);
        GtkRequisition r1 = { 0, 0 }, r2 = { 0, 0 };
        gtk_widget_size_request(cr->btn, &r1);
        if (cr->sep)
            gtk_widget_size_request(cr->sep, &r2);
        cr->width = r1.width + r2.width;
        g_ptr_array_add(c->items, cr);
    }
    g_list_free(chain);

    /* Keep the buttons before the "..." / filler in packing order: both were
     * packed first, so push them to the right positions again. */
    gtk_box_reorder_child(GTK_BOX(c->btns), c->more, 0);
    queue_relayout(c);
    /* A fresh location replaces any half-typed entry. */
    gtk_widget_hide(c->entry);
    gtk_widget_show(c->btns);
}

/* ---- entry mode -------------------------------------------------------- */

static char *entry_text_for(GFile *dir)
{
    if (g_file_is_native(dir))
        return g_file_get_path(dir);
    return g_file_get_uri(dir);
}

static void leave_edit(Crumbs *c)
{
    gtk_widget_hide(c->entry);
    gtk_widget_show(c->btns);
    kf_pane_focus_view(c->pane);
}

static void on_entry_activate(GtkEntry *e, gpointer data)
{
    Crumbs *c = data;
    const char *txt = g_strstrip((char *)gtk_entry_get_text(e));
    if (!*txt) {
        leave_edit(c);
        return;
    }
    char *expanded = NULL;
    if (txt[0] == '~' && (txt[1] == 0 || txt[1] == '/'))
        expanded = g_strconcat(g_get_home_dir(), txt + 1, NULL);
    GFile *f = g_file_new_for_commandline_arg_and_cwd(expanded ? expanded : txt,
                                                      g_get_home_dir());
    g_free(expanded);
    GFileInfo *info = g_file_query_info(f, "standard::type", G_FILE_QUERY_INFO_NONE, NULL, NULL);
    if (info && g_file_info_get_file_type(info) != G_FILE_TYPE_DIRECTORY) {
        kf_app_open(c->pane->app, c->pane, f);
        leave_edit(c);
    } else if (info || !g_file_is_native(f)) {
        /* Non-native locations (smb://, trash://) may need mounting first;
         * the pane handles that when loading. */
        leave_edit(c);
        kf_pane_navigate(c->pane, f, TRUE, NULL);
    } else {
        kf_app_message(c->pane->app, GTK_MESSAGE_ERROR, _("Folder not found"), txt);
    }
    if (info)
        g_object_unref(info);
    g_object_unref(f);
}

static gboolean on_entry_key(GtkWidget *w, GdkEventKey *ev, gpointer data)
{
    (void)w;
    if (ev->keyval == GDK_KEY_Escape) {
        leave_edit(data);
        return TRUE;
    }
    return FALSE;
}

static gboolean on_entry_focus_out(GtkWidget *w, GdkEventFocus *ev, gpointer data)
{
    (void)w; (void)ev;
    Crumbs *c = data;
    if (gtk_widget_get_visible(c->entry)) {
        gtk_widget_hide(c->entry);
        gtk_widget_show(c->btns);
    }
    return FALSE;
}

/* Subfolders of the folder named by what has been typed so far. */
static void on_entry_changed(GtkEditable *e, gpointer data)
{
    Crumbs *c = data;
    const char *txt = gtk_entry_get_text(GTK_ENTRY(e));
    char *expanded = (txt[0] == '~' && (txt[1] == 0 || txt[1] == '/'))
        ? g_strconcat(g_get_home_dir(), txt + 1, NULL) : g_strdup(txt);
    if (expanded[0] != '/') {
        g_free(expanded);
        return;
    }
    char *slash = strrchr(expanded, '/');
    char *parent = g_strndup(expanded, slash == expanded ? 1 : (gsize)(slash - expanded));
    if (g_strcmp0(parent, g_object_get_data(G_OBJECT(c->completions), "kf-parent")) != 0) {
        gtk_list_store_clear(c->completions);
        g_object_set_data_full(G_OBJECT(c->completions), "kf-parent", g_strdup(parent), g_free);
        GDir *d = g_dir_open(parent, 0, NULL);
        if (d) {
            const char *name;
            int count = 0;
            while ((name = g_dir_read_name(d)) && count < 2000) {
                char *full = g_build_filename(parent, name, NULL);
                if (g_file_test(full, G_FILE_TEST_IS_DIR)) {
                    char *with = g_strconcat(full, "/", NULL);
                    GtkTreeIter it;
                    gtk_list_store_append(c->completions, &it);
                    gtk_list_store_set(c->completions, &it, 0, with, -1);
                    g_free(with);
                    count++;
                }
                g_free(full);
            }
            g_dir_close(d);
        }
    }
    g_free(parent);
    g_free(expanded);
}

void kf_crumbs_edit(GtkWidget *crumbs)
{
    Crumbs *c = get(crumbs);
    char *txt = c->dir ? entry_text_for(c->dir) : g_strdup("");
    gtk_entry_set_text(GTK_ENTRY(c->entry), txt);
    g_free(txt);
    gtk_widget_hide(c->btns);
    gtk_widget_show(c->entry);
    gtk_widget_grab_focus(c->entry);
    gtk_editable_select_region(GTK_EDITABLE(c->entry), 0, -1);
}

GtkWidget *kf_crumbs_new(KfPane *p)
{
    Crumbs *c = g_new0(Crumbs, 1);
    c->pane = p;
    c->items = g_ptr_array_new_with_free_func(crumb_free);

    c->bar = gtk_hbox_new(FALSE, 0);
    g_object_set_data_full(G_OBJECT(c->bar), "kf-crumbs", c, crumbs_free);

    c->btns = gtk_hbox_new(FALSE, 0);
    /* The explicit request keeps a long path from widening the window; the
     * relayout above hides what doesn't fit instead. */
    gtk_widget_set_size_request(c->btns, 20, -1);
    c->more = gtk_button_new_with_label("…");
    gtk_button_set_relief(GTK_BUTTON(c->more), GTK_RELIEF_NONE);
    gtk_button_set_focus_on_click(GTK_BUTTON(c->more), FALSE);
    gtk_widget_set_tooltip_text(c->more, _("Parent folders"));
    g_signal_connect(c->more, "clicked", G_CALLBACK(on_more_clicked), c);
    gtk_box_pack_start(GTK_BOX(c->btns), c->more, FALSE, FALSE, 0);
    gtk_widget_set_no_show_all(c->more, TRUE);

    GtkWidget *filler = gtk_event_box_new();
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(filler), FALSE);
    gtk_widget_add_events(filler, GDK_BUTTON_PRESS_MASK);
    g_signal_connect(filler, "button-press-event", G_CALLBACK(on_filler_press), c);
    gtk_box_pack_end(GTK_BOX(c->btns), filler, TRUE, TRUE, 0);
    gtk_widget_show(filler);
    gtk_widget_set_tooltip_text(filler, _("Click to type a location"));
    g_signal_connect(c->btns, "size-allocate", G_CALLBACK(on_btns_allocate), c);
    gtk_box_pack_start(GTK_BOX(c->bar), c->btns, TRUE, TRUE, 0);
    gtk_widget_show(c->btns);

    c->entry = gtk_entry_new();
    gtk_widget_set_no_show_all(c->entry, TRUE);
    c->completions = gtk_list_store_new(1, G_TYPE_STRING);
    GtkEntryCompletion *comp = gtk_entry_completion_new();
    gtk_entry_completion_set_model(comp, GTK_TREE_MODEL(c->completions));
    gtk_entry_completion_set_text_column(comp, 0);
    gtk_entry_completion_set_inline_completion(comp, TRUE);
    gtk_entry_set_completion(GTK_ENTRY(c->entry), comp);
    g_object_unref(comp);
    g_object_unref(c->completions);
    g_signal_connect(c->entry, "activate", G_CALLBACK(on_entry_activate), c);
    g_signal_connect(c->entry, "changed", G_CALLBACK(on_entry_changed), c);
    g_signal_connect(c->entry, "key-press-event", G_CALLBACK(on_entry_key), c);
    g_signal_connect(c->entry, "focus-out-event", G_CALLBACK(on_entry_focus_out), c);
    gtk_box_pack_start(GTK_BOX(c->bar), c->entry, TRUE, TRUE, 0);
    return c->bar;
}
