/*
 * applications.c - `xisserve --applications [<x> <y>]`: pop up a
 * cascading menu listing every installed application, grouped by
 * freedesktop category, at the coordinates the caller asks for.
 *
 * This is appmenu.c's sibling: same one-shot-popup shape (no singleton
 * lock, no control socket, no launcher window -- build a menu, pop it,
 * run its own gtk_main(), exit when it goes away), but listing every
 * scanned .desktop entry instead of one window's own DBusMenu tree.
 *
 * It exists to be bound as an xisback click action: xisback's
 * run_action() already sets XISBACK_CLICK_X/XISBACK_CLICK_Y on the child
 * it execs (see xisback.c), so `xisback --on-right-click 'xisserve
 * --applications'` pops the list right where the desktop was clicked --
 * the same "right-click bare desktop space for an Applications menu"
 * escape hatch Plasma/kickoff-style shells offer, without needing
 * per-binding coordinate wiring. --apps-x/--apps-y or two positional
 * arguments (same spelling convention as --menu) override the env vars
 * when given.
 */
#include <gtk/gtk.h>

#include "xisserve.h"

#include <stdio.h>
#include <stdlib.h>

static void on_app_activate(GtkMenuItem *item, gpointer data)
{
    (void)item;
    ResultEntry *e = data;
    run_detached(e->exec);
}

static void on_selection_done(GtkMenuShell *shell, gpointer data)
{
    (void)shell;
    (void)data;
    /* Dismissed (Escape, a click outside) *or* an item chosen -- the
     * activate handler above has already run by then, so quitting here
     * covers both endings with one signal, same as appmenu.c. */
    gtk_main_quit();
}

/* gtk_menu_popup()'s position callback -- put the top-level menu exactly
 * where the click happened; GTK keeps it (and each submenu) on screen by
 * itself if that would run off an edge. */
static void position_menu(GtkMenu *menu, gint *x, gint *y, gboolean *push_in, gpointer data)
{
    (void)menu;
    const gint *pos = data;
    *x = pos[0];
    *y = pos[1];
    *push_in = TRUE;
}

/* Icons land on items that are already built and (for the top-level
 * menu) already on screen -- so unlike the show_all() pass, the image
 * itself has to be shown by hand. Items are created as image menu items
 * up front whenever the .desktop had an Icon= at all, precisely so
 * there's something here to hang the image on later. */
static void apply_menu_icon(gpointer target, GdkPixbuf *icon, gpointer user_data)
{
    (void)user_data;
    GtkWidget *img = gtk_image_new_from_pixbuf(icon);
    gtk_widget_show(img);
    gtk_image_menu_item_set_image(GTK_IMAGE_MENU_ITEM(target), img);
}

static gint compare_keys_by_label(gconstpointer a, gconstpointer b)
{
    const char *ka = *(const char **)a;
    const char *kb = *(const char **)b;
    return g_utf8_collate(xisserve_category_label(ka), xisserve_category_label(kb));
}

int applications_run(int x, int y)
{
    GPtrArray *apps = xisserve_scan_apps();
    if (apps->len == 0) {
        fprintf(stderr, "xisserve: no applications found\n");
        g_ptr_array_free(apps, TRUE);
        return 1;
    }

    /* Bucket by category_key -- apps is already name-sorted, so each
     * bucket comes out name-sorted too. Buckets borrow their entries
     * from `apps`; only the GPtrArray wrappers are hash-table-owned. */
    GHashTable *buckets = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
                                                 (GDestroyNotify)g_ptr_array_unref);
    GPtrArray *keys = g_ptr_array_new();
    for (guint i = 0; i < apps->len; i++) {
        ResultEntry *e = g_ptr_array_index(apps, i);
        GPtrArray *bucket = g_hash_table_lookup(buckets, e->category_key);
        if (!bucket) {
            bucket = g_ptr_array_new();
            g_hash_table_insert(buckets, e->category_key, bucket);
            g_ptr_array_add(keys, e->category_key);
        }
        g_ptr_array_add(bucket, e);
    }
    g_ptr_array_sort(keys, compare_keys_by_label);

    /* A fresh process resolves every icon from cold, which measured at
     * 570-950ms for ~550 apps -- an order of magnitude more than
     * everything else this program does put together (see icons.c). So
     * the menu is built and popped up with only the icons already in
     * hand, and the rest fill in from an idle while it's on screen. */
    XisserveIconJob *job = xisserve_icon_job_new(apply_menu_icon, NULL);

    GtkWidget *top = gtk_menu_new();
    for (guint i = 0; i < keys->len; i++) {
        const char *key = g_ptr_array_index(keys, i);
        GPtrArray *bucket = g_hash_table_lookup(buckets, key);

        GtkWidget *cat_item = gtk_menu_item_new_with_label(xisserve_category_label(key));
        GtkWidget *submenu = gtk_menu_new();
        gtk_menu_item_set_submenu(GTK_MENU_ITEM(cat_item), submenu);

        for (guint j = 0; j < bucket->len; j++) {
            ResultEntry *e = g_ptr_array_index(bucket, j);
            gboolean have_icon = xisserve_icon_resolved(e->icon_spec, &e->icon);
            GtkWidget *item = e->icon_spec[0] ? gtk_image_menu_item_new_with_label(e->name)
                                              : gtk_menu_item_new_with_label(e->name);
            if (e->icon)
                gtk_image_menu_item_set_image(GTK_IMAGE_MENU_ITEM(item),
                                              gtk_image_new_from_pixbuf(e->icon));
            else if (!have_icon)
                xisserve_icon_job_add(job, e, item);
            g_signal_connect(item, "activate", G_CALLBACK(on_app_activate), e);
            gtk_menu_shell_append(GTK_MENU_SHELL(submenu), item);
        }
        gtk_menu_shell_append(GTK_MENU_SHELL(top), cat_item);
    }
    gtk_widget_show_all(top);
    g_signal_connect(top, "selection-done", G_CALLBACK(on_selection_done), NULL);

    gint pos[2] = { x, y };
    gtk_menu_popup(GTK_MENU(top), NULL, NULL, position_menu, pos, 0, gtk_get_current_event_time());
    xisserve_icon_job_start(&job);
    gtk_main();

    xisserve_icon_job_cancel(&job); /* it borrows the entries freed just below */
    g_hash_table_destroy(buckets);
    g_ptr_array_free(keys, TRUE);
    for (guint i = 0; i < apps->len; i++) result_entry_free(g_ptr_array_index(apps, i));
    g_ptr_array_free(apps, TRUE);
    return 0;
}
