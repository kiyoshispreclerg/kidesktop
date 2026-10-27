/* kfd_multi.c - a combo-like button whose menu ticks any number of choices.
 *
 * Clicking an item ticks it and leaves the menu open, so several can be
 * picked in one go. The button shows the ticked labels, or none_label when
 * nothing is ticked (which means "no restriction"). Items added as secondary
 * go into a submenu, for long lists with a few usual choices. */
#include "kifind.h"
#include "../shared/xis_i18n.h"

typedef struct {
    GtkWidget *button, *label, *menu, *sub, *sub_item;
    char *none_label;
    GList *items;               /* GtkCheckMenuItem*, in order */
    gboolean updating;
} Multi;

static Multi *get(GtkWidget *b) { return g_object_get_data(G_OBJECT(b), "kfd-multi"); }

static void update_label(Multi *m)
{
    GString *s = g_string_new(NULL);
    for (GList *l = m->items; l; l = l->next)
        if (gtk_check_menu_item_get_active(l->data)) {
            if (s->len)
                g_string_append(s, ", ");
            g_string_append(s, g_object_get_data(G_OBJECT(l->data), "kfd-label"));
        }
    gtk_label_set_text(GTK_LABEL(m->label), s->len ? s->str : m->none_label);
    gtk_widget_set_tooltip_text(m->button, s->len ? s->str : NULL);
    g_string_free(s, TRUE);
}

static void on_toggled(GtkCheckMenuItem *i, gpointer d)
{
    (void)i;
    Multi *m = d;
    if (!m->updating)
        update_label(m);
}

/* Toggle by hand and keep the menu up; keyboard activation still closes it. */
static gboolean on_item_release(GtkWidget *w, GdkEventButton *ev, gpointer d)
{
    (void)ev; (void)d;
    GtkCheckMenuItem *c = GTK_CHECK_MENU_ITEM(w);
    gtk_check_menu_item_set_active(c, !gtk_check_menu_item_get_active(c));
    return TRUE;
}

void kfd_multi_clear(GtkWidget *b)
{
    Multi *m = get(b);
    m->updating = TRUE;
    for (GList *l = m->items; l; l = l->next)
        gtk_check_menu_item_set_active(l->data, FALSE);
    m->updating = FALSE;
    update_label(m);
}

static void on_clear(GtkMenuItem *i, gpointer d)
{
    (void)i;
    kfd_multi_clear(d);
}

static void position_below(GtkMenu *menu, gint *x, gint *y, gboolean *push_in, gpointer d)
{
    GtkWidget *b = d;
    GtkAllocation a;
    gtk_widget_get_allocation(b, &a);
    gdk_window_get_origin(gtk_widget_get_window(b), x, y);
    *x += a.x;
    *y += a.y + a.height;
    gtk_widget_set_size_request(GTK_WIDGET(menu), -1, -1);
    GtkRequisition r;
    gtk_widget_size_request(GTK_WIDGET(menu), &r);
    if (r.width < a.width)
        gtk_widget_set_size_request(GTK_WIDGET(menu), a.width, -1);
    *push_in = TRUE;
}

static void on_clicked(GtkButton *btn, gpointer d)
{
    Multi *m = d;
    gtk_widget_show_all(m->menu);
    if (m->sub) {
        GList *kids = gtk_container_get_children(GTK_CONTAINER(m->sub));
        if (!kids)
            gtk_widget_hide(m->sub_item);
        g_list_free(kids);
    }
    gtk_menu_popup(GTK_MENU(m->menu), NULL, NULL, position_below, btn, 0, gtk_get_current_event_time());
}

static void free_multi(gpointer d)
{
    Multi *m = d;
    gtk_widget_destroy(m->menu);
    g_list_free(m->items);
    g_free(m->none_label);
    g_free(m);
}

GtkWidget *kfd_multi_new(const char *none_label)
{
    Multi *m = g_new0(Multi, 1);
    m->none_label = g_strdup(none_label);
    m->button = gtk_button_new();
    GtkWidget *box = gtk_hbox_new(FALSE, 4);
    m->label = gtk_label_new(none_label);
    gtk_label_set_ellipsize(GTK_LABEL(m->label), PANGO_ELLIPSIZE_END);
    gtk_misc_set_alignment(GTK_MISC(m->label), 0, 0.5);
    gtk_box_pack_start(GTK_BOX(box), m->label, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), gtk_arrow_new(GTK_ARROW_DOWN, GTK_SHADOW_NONE), FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(m->button), box);
    gtk_widget_set_size_request(m->label, 60, -1);
    m->menu = gtk_menu_new();
    GtkWidget *clear = gtk_menu_item_new_with_label(none_label);
    g_signal_connect(clear, "activate", G_CALLBACK(on_clear), m->button);
    gtk_menu_shell_append(GTK_MENU_SHELL(m->menu), clear);
    gtk_menu_shell_append(GTK_MENU_SHELL(m->menu), gtk_separator_menu_item_new());
    g_object_set_data_full(G_OBJECT(m->button), "kfd-multi", m, free_multi);
    g_signal_connect(m->button, "clicked", G_CALLBACK(on_clicked), m);
    return m->button;
}

void kfd_multi_set_secondary_label(GtkWidget *b, const char *label)
{
    Multi *m = get(b);
    if (!m->sub) {
        m->sub = gtk_menu_new();
        m->sub_item = gtk_menu_item_new_with_label(label);
        gtk_menu_item_set_submenu(GTK_MENU_ITEM(m->sub_item), m->sub);
        gtk_menu_shell_append(GTK_MENU_SHELL(m->menu), m->sub_item);
    } else {
        gtk_menu_item_set_label(GTK_MENU_ITEM(m->sub_item), label);
    }
}

void kfd_multi_add(GtkWidget *b, const char *label, int id, gboolean secondary)
{
    Multi *m = get(b);
    if (secondary && !m->sub)
        kfd_multi_set_secondary_label(b, _("Others"));
    GtkWidget *i = gtk_check_menu_item_new_with_label(label);
    g_object_set_data_full(G_OBJECT(i), "kfd-label", g_strdup(label), g_free);
    g_object_set_data(G_OBJECT(i), "kfd-id", GINT_TO_POINTER(id));
    g_signal_connect(i, "toggled", G_CALLBACK(on_toggled), m);
    g_signal_connect(i, "button-release-event", G_CALLBACK(on_item_release), m);
    if (secondary) {
        gtk_menu_shell_append(GTK_MENU_SHELL(m->sub), i);
    } else if (m->sub_item) {
        /* primary items stay above the submenu */
        GList *kids = gtk_container_get_children(GTK_CONTAINER(m->menu));
        int pos = g_list_index(kids, m->sub_item);
        g_list_free(kids);
        gtk_menu_shell_insert(GTK_MENU_SHELL(m->menu), i, pos);
    } else {
        gtk_menu_shell_append(GTK_MENU_SHELL(m->menu), i);
    }
    m->items = g_list_append(m->items, i);
}

GArray *kfd_multi_selected(GtkWidget *b)
{
    Multi *m = get(b);
    GArray *a = NULL;
    for (GList *l = m->items; l; l = l->next)
        if (gtk_check_menu_item_get_active(l->data)) {
            if (!a)
                a = g_array_new(FALSE, FALSE, sizeof(int));
            int id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(l->data), "kfd-id"));
            g_array_append_val(a, id);
        }
    return a;
}
