/* kifind - find files by name under a folder (GTK2).
 *
 * 0.0.1 is only the window: the folder to search comes from the command line
 * (kifiles starts it with Ctrl+F in the folder it shows); the search itself
 * is still to be written. */
#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <stdio.h>
#include <string.h>
#include "../shared/xis_i18n.h"

#define KIFIND_VERSION "0.0.1"

typedef struct {
    GtkWidget *win, *name, *folder, *subfolders, *status;
} Find;

static void on_find(GtkWidget *w, gpointer d)
{
    (void)w;
    Find *f = d;
    gtk_label_set_text(GTK_LABEL(f->status), _("Searching is not implemented yet."));
}

static gboolean on_key(GtkWidget *w, GdkEventKey *ev, gpointer d)
{
    (void)d;
    if (ev->keyval == GDK_KEY_Escape) {
        gtk_widget_destroy(w);
        return TRUE;
    }
    return FALSE;
}

/* A path, a file:// URI or nothing (the current folder). */
static char *start_folder(const char *arg)
{
    if (!arg)
        return g_get_current_dir();
    GFile *f = g_file_new_for_commandline_arg(arg);
    char *path = g_file_get_path(f);
    g_object_unref(f);
    if (path && g_file_test(path, G_FILE_TEST_IS_DIR))
        return path;
    g_free(path);
    return g_strdup(g_get_home_dir());
}

static GtkWidget *row_label(const char *text, GtkWidget *target)
{
    GtkWidget *l = gtk_label_new_with_mnemonic(text);
    gtk_misc_set_alignment(GTK_MISC(l), 0, 0.5);
    gtk_label_set_mnemonic_widget(GTK_LABEL(l), target);
    return l;
}

int main(int argc, char **argv)
{
    xis_i18n_init("kifind");
    const char *arg = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--version")) {
            printf("kifind %s\n", KIFIND_VERSION);
            return 0;
        }
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            printf(_("Usage: kifind [FOLDER]\n"));
            return 0;
        }
        if (!arg && argv[i][0] != '-')
            arg = argv[i];
    }
    gtk_init(&argc, &argv);
    gtk_window_set_default_icon_name("system-search");

    Find *f = g_new0(Find, 1);
    f->win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(f->win), _("Find Files"));
    gtk_window_set_role(GTK_WINDOW(f->win), "kifind");
    gtk_window_set_default_size(GTK_WINDOW(f->win), 520, 360);
    g_signal_connect(f->win, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    g_signal_connect(f->win, "key-press-event", G_CALLBACK(on_key), f);

    GtkWidget *vbox = gtk_vbox_new(FALSE, 6);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 8);
    gtk_container_add(GTK_CONTAINER(f->win), vbox);

    GtkWidget *grid = gtk_table_new(3, 2, FALSE);
    gtk_table_set_row_spacings(GTK_TABLE(grid), 4);
    gtk_table_set_col_spacings(GTK_TABLE(grid), 8);
    f->name = gtk_entry_new();
    gtk_entry_set_activates_default(GTK_ENTRY(f->name), TRUE);
    gtk_table_attach(GTK_TABLE(grid), row_label(_("_Named:"), f->name), 0, 1, 0, 1, GTK_FILL, 0, 0, 0);
    gtk_table_attach(GTK_TABLE(grid), f->name, 1, 2, 0, 1, GTK_EXPAND | GTK_FILL, 0, 0, 0);
    f->folder = gtk_file_chooser_button_new(_("Look In"), GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER);
    char *dir = start_folder(arg);
    gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(f->folder), dir);
    gtk_table_attach(GTK_TABLE(grid), row_label(_("_Look in:"), f->folder), 0, 1, 1, 2, GTK_FILL, 0, 0, 0);
    gtk_table_attach(GTK_TABLE(grid), f->folder, 1, 2, 1, 2, GTK_EXPAND | GTK_FILL, 0, 0, 0);
    f->subfolders = gtk_check_button_new_with_mnemonic(_("Include _subfolders"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(f->subfolders), TRUE);
    gtk_table_attach(GTK_TABLE(grid), f->subfolders, 1, 2, 2, 3, GTK_FILL, 0, 0, 0);
    gtk_box_pack_start(GTK_BOX(vbox), grid, FALSE, FALSE, 0);

    /* results: empty until the search exists */
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sw), GTK_SHADOW_IN);
    GtkListStore *store = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_STRING);
    GtkWidget *tv = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    g_object_unref(store);
    gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(tv), -1, _("Name"), gtk_cell_renderer_text_new(), "text", 0, NULL);
    gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(tv), -1, _("In Folder"), gtk_cell_renderer_text_new(), "text", 1, NULL);
    gtk_container_add(GTK_CONTAINER(sw), tv);
    gtk_box_pack_start(GTK_BOX(vbox), sw, TRUE, TRUE, 0);

    GtkWidget *bottom = gtk_hbox_new(FALSE, 6);
    f->status = gtk_label_new("");
    gtk_misc_set_alignment(GTK_MISC(f->status), 0, 0.5);
    gtk_label_set_ellipsize(GTK_LABEL(f->status), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(bottom), f->status, TRUE, TRUE, 0);
    GtkWidget *close = gtk_button_new_from_stock(GTK_STOCK_CLOSE);
    g_signal_connect_swapped(close, "clicked", G_CALLBACK(gtk_widget_destroy), f->win);
    gtk_box_pack_end(GTK_BOX(bottom), close, FALSE, FALSE, 0);
    GtkWidget *find = gtk_button_new_from_stock(GTK_STOCK_FIND);
    gtk_widget_set_can_default(find, TRUE);
    g_signal_connect(find, "clicked", G_CALLBACK(on_find), f);
    gtk_box_pack_end(GTK_BOX(bottom), find, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), bottom, FALSE, FALSE, 0);

    gtk_widget_show_all(f->win);
    gtk_widget_grab_default(find);
    gtk_widget_grab_focus(f->name);
    g_free(dir);
    gtk_main();
    g_free(f);
    return 0;
}
