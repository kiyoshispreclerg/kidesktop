/*
 * appmenu.c - `xisserve --menu <window> <x> <y>`: pop up one window's
 * exported application menu (File/Edit/View...) as a real cascading GTK
 * menu, at the coordinates the caller asks for.
 *
 * This exists so a window manager can offer an application-menu button in
 * its decoration without becoming a DBus client itself. kiwm's appmenu
 * titlebar element does exactly that: it draws the button, checks only
 * that the window carries the _KDE_NET_WM_APPMENU_* properties, and runs
 * kiwm.conf's appmenu_command= with %w/%x/%y substituted -- typically
 * `xisserve --menu %w %x %y`. All the DBusMenu work stays here, in a
 * process that is allowed to block on a bus round-trip; a compositing WM
 * is not.
 *
 * Everything about the launcher is bypassed: no singleton lock, no
 * control socket, no launcher window. A menu popup is a one-shot thing
 * per click, and relaying it into a running launcher instance would mean
 * that instance's window fighting for the position and the grab. So this
 * mode builds a menu, pops it, runs its own gtk_main(), and exits when
 * the menu goes away -- the process lives for as long as the menu is on
 * screen and not a moment longer.
 *
 * The whole tree is fetched in one GetLayout(0, -1) call, same as
 * plugins/globalmenu.c's search does, so submenus are already populated
 * when they open. Applications that only fill a submenu in response to
 * AboutToShow would show it empty; no app tested so far does that, and
 * neither this nor xispanel's own cascading menus send that call yet.
 */
#include <gtk/gtk.h>

#include "xisserve.h"
#include "plugins/dbusmenu.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    char busname[128];
    char objpath[128];
    int id;
} ItemCtx;

static void on_item_activate(GtkMenuItem *item, gpointer data)
{
    (void)item;
    ItemCtx *ctx = data;
    /* Fire-and-forget, exactly what activating the item in the app's own
     * menu does -- the app is the one that acts on it. */
    dbusmenu_send_event(ctx->busname, ctx->objpath, ctx->id);
}

static void on_selection_done(GtkMenuShell *shell, gpointer data)
{
    (void)shell;
    (void)data;
    /* Dismissed (Escape, a click outside) *or* an item chosen -- the
     * activate handler above has already run by then, so quitting here
     * covers both endings with one signal. */
    gtk_main_quit();
}

/* gtk_menu_popup()'s position callback: put the menu exactly where the
 * caller asked, which for a titlebar button is directly under it. GTK
 * still keeps the menu on screen by itself if that would run it off the
 * bottom or the right edge. */
static void position_menu(GtkMenu *menu, gint *x, gint *y, gboolean *push_in, gpointer data)
{
    (void)menu;
    const gint *pos = data;
    *x = pos[0];
    *y = pos[1];
    *push_in = TRUE;
}

/* Builds the GtkMenu tree from dbusmenu_fetch()'s flattened arrays.
 *
 * The arrays are a depth-first walk with each item's nesting depth
 * alongside it, so one pass rebuilds the tree: `shells[d]` is the menu
 * currently open at depth d, an item goes into `shells[its depth]`, and
 * an item whose *next* sibling is one level deeper is the one that owns
 * that deeper menu. */
static GtkWidget *build_menu(DbusMenuItem *items, const int *ids, const int *depths, int n,
                             const char *busname, const char *objpath, GPtrArray *ctxs)
{
    enum { MAX_DEPTH = 16 };
    GtkWidget *shells[MAX_DEPTH];
    memset(shells, 0, sizeof(shells));
    shells[0] = gtk_menu_new();

    for (int i = 0; i < n; i++) {
        int d = depths[i];
        if (d < 0 || d >= MAX_DEPTH || !shells[d])
            continue;

        GtkWidget *item;
        if (items[i].is_separator) {
            item = gtk_separator_menu_item_new();
        } else {
            /* DBusMenu labels carry '_' mnemonics ("_File"), which is
             * exactly what GTK's own mnemonic constructor expects. */
            item = items[i].icon ? gtk_image_menu_item_new_with_mnemonic(items[i].label)
                                 : gtk_menu_item_new_with_mnemonic(items[i].label);
            if (items[i].icon)
                gtk_image_menu_item_set_image(GTK_IMAGE_MENU_ITEM(item),
                                              gtk_image_new_from_pixbuf(items[i].icon));
            if (!items[i].enabled)
                gtk_widget_set_sensitive(item, FALSE);
        }
        gtk_menu_shell_append(GTK_MENU_SHELL(shells[d]), item);

        /* A deeper next item means this one is a submenu's parent. Its
         * own activate is then GTK's business (opening the submenu), not
         * a DBusMenu Event. */
        gboolean has_children = (i + 1 < n) && depths[i + 1] == d + 1 && d + 1 < MAX_DEPTH;
        if (has_children) {
            GtkWidget *sub = gtk_menu_new();
            gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), sub);
            shells[d + 1] = sub;
        } else if (!items[i].is_separator && items[i].enabled) {
            ItemCtx *ctx = g_new0(ItemCtx, 1);
            snprintf(ctx->busname, sizeof(ctx->busname), "%s", busname);
            snprintf(ctx->objpath, sizeof(ctx->objpath), "%s", objpath);
            ctx->id = ids[i];
            g_ptr_array_add(ctxs, ctx);
            g_signal_connect(item, "activate", G_CALLBACK(on_item_activate), ctx);
        }
    }

    gtk_widget_show_all(shells[0]);
    return shells[0];
}

int appmenu_run(unsigned long window, int x, int y)
{
    char busname[128], objpath[128];
    if (!xisserve_window_appmenu(window, busname, sizeof(busname), objpath, sizeof(objpath))) {
        fprintf(stderr, "xisserve: window 0x%lx exports no application menu\n", window);
        return 1;
    }

    static DbusMenuItem items[DBUSMENU_MAX_ITEMS];
    static int ids[DBUSMENU_MAX_ITEMS];
    static int depths[DBUSMENU_MAX_ITEMS];
    int n = dbusmenu_fetch(busname, objpath, 0, -1, items, ids, depths, DBUSMENU_MAX_ITEMS);
    if (n <= 0) {
        /* -1 is "no DBus at all" (including the dbusmenu_stub.c build),
         * 0 an empty menu -- nothing to show either way, and nothing
         * worth popping an empty box for. */
        fprintf(stderr, "xisserve: no menu returned by %s%s\n", busname, objpath);
        return 1;
    }

    GPtrArray *ctxs = g_ptr_array_new_with_free_func(g_free);
    GtkWidget *menu = build_menu(items, ids, depths, n, busname, objpath, ctxs);
    g_signal_connect(menu, "selection-done", G_CALLBACK(on_selection_done), NULL);

    gint pos[2] = { x, y };
    gtk_menu_popup(GTK_MENU(menu), NULL, NULL, position_menu, pos, 0, gtk_get_current_event_time());
    gtk_main();

    /* Freed together, after the menu is gone: an item's handler can only
     * run while the menu is up. */
    dbusmenu_free_item_icons(items, n);
    g_ptr_array_free(ctxs, TRUE);
    return 0;
}
