/* kiconf - Menu de programas tab: a kmenuedit-equivalent for the apps
 * this session's menus (xispanel's launcher/menu, xismenu's global menu)
 * pull their entries from. See kiconf.c's top doc comment for the
 * overall design.
 *
 * Flat list grouped by top-level XDG category (no submenu/.menu-file
 * structure) -- every row is one installed .desktop app, from
 * scan_all_apps() (common.c). Editing/hiding a *system* entry (normally
 * root-owned, about to be overwritten by the next package update anyway)
 * never touches it: it materializes an override in
 * $XDG_DATA_HOME/applications/<id>.desktop instead, same precedence trick
 * tabs/autostart.c already uses for Hidden=. The very first edit of a
 * system entry writes a *complete* override (every field, not just the
 * one that changed) so it stands on its own once it starts shadowing the
 * original -- unlike autostart.c's Hidden-only override, here the whole
 * entry (Exec=, Icon=, ...) is what a user might want to change.
 *
 * An entry already under the user's own applications dir (is_user) is
 * edited/deleted in place -- that's the only case "Excluir" ever acts on:
 * a system entry has nothing of the user's to remove, so the button is a
 * silent no-op for it (same guard as autostart.c's remove_custom_cb()). */
#include "../common.h"
#include "../tabs.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    const char *xdg_name; /* Categories= token */
    const char *label;    /* shown group heading */
} MenuCategory;

/* Kept by hand for the same reason common.h's KISESSION_SERVICES[] is --
 * no machine-readable source to scan this from. Order matters: an app
 * lands in the first one of these its Categories= mentions. */
static const MenuCategory MENU_CATEGORIES[] = {
    {"AudioVideo", "Audio e video"}, {"Development", "Desenvolvimento"},
    {"Education", "Educacao"},       {"Game", "Jogos"},
    {"Graphics", "Graficos"},        {"Network", "Internet e rede"},
    {"Office", "Escritorio"},        {"Science", "Ciencia"},
    {"Settings", "Configuracoes"},   {"System", "Sistema"},
    {"Utility", "Utilitarios"},
};
#define N_MENU_CATEGORIES ((int)(sizeof(MENU_CATEGORIES) / sizeof(MENU_CATEGORIES[0])))

static DesktopApp g_apps[MAX_DESKTOP_APPS];
static int g_n_apps = 0;

static GtkTreeStore *g_menu_store;
static GtkWidget *g_menu_view;
enum { COL_M_VISIBLE = 0, COL_M_NAME, COL_M_ROW, N_M_COLS };

static void user_apps_dir(char *out, size_t outsz)
{
    const char *xdg = getenv("XDG_DATA_HOME");
    if (xdg && *xdg) {
        snprintf(out, outsz, "%s/applications", xdg);
    } else {
        snprintf(out, outsz, "%s/.local/share/applications", getenv("HOME") ? getenv("HOME") : "/tmp");
    }
}

static int categories_has(const char *list, const char *category)
{
    const char *p = list;
    size_t catlen = strlen(category);
    while (*p) {
        const char *semi = strchr(p, ';');
        size_t len = semi ? (size_t)(semi - p) : strlen(p);
        if (len == catlen && !strncmp(p, category, len)) {
            return 1;
        }
        if (!semi) {
            break;
        }
        p = semi + 1;
    }
    return 0;
}

static int app_category_index(const DesktopApp *app)
{
    for (int i = 0; i < N_MENU_CATEGORIES; i++) {
        if (categories_has(app->categories, MENU_CATEGORIES[i].xdg_name)) {
            return i;
        }
    }
    return -1; /* "Outros" */
}

static int app_cmp(const void *pa, const void *pb)
{
    return strcmp(((const DesktopApp *)pa)->name, ((const DesktopApp *)pb)->name);
}

static void refill_menu_store(void)
{
    g_n_apps = scan_all_apps(g_apps, MAX_DESKTOP_APPS);
    qsort(g_apps, (size_t)g_n_apps, sizeof(DesktopApp), app_cmp);

    gtk_tree_store_clear(g_menu_store);
    GtkTreeIter cat_iters[N_MENU_CATEGORIES + 1]; /* last slot: "Outros" */
    int cat_used[N_MENU_CATEGORIES + 1];
    memset(cat_used, 0, sizeof(cat_used));

    for (int i = 0; i < g_n_apps; i++) {
        int cat = app_category_index(&g_apps[i]);
        int slot = cat < 0 ? N_MENU_CATEGORIES : cat;
        if (!cat_used[slot]) {
            gtk_tree_store_append(g_menu_store, &cat_iters[slot], NULL);
            gtk_tree_store_set(g_menu_store, &cat_iters[slot], COL_M_NAME,
                                cat < 0 ? "Outros" : MENU_CATEGORIES[cat].label, COL_M_ROW, -1, -1);
            cat_used[slot] = 1;
        }
        GtkTreeIter child;
        gtk_tree_store_append(g_menu_store, &child, &cat_iters[slot]);
        gtk_tree_store_set(g_menu_store, &child, COL_M_VISIBLE, !g_apps[i].nodisplay, COL_M_NAME, g_apps[i].name,
                            COL_M_ROW, i, -1);
    }
    gtk_tree_view_expand_all(GTK_TREE_VIEW(g_menu_view));
}

/* Writes a full, self-sufficient .desktop for `app` at `path` -- used the
 * first time a system entry is edited/hidden, so the override doesn't
 * depend on any field it didn't explicitly set (see file doc comment). */
static void write_full_override(const char *path, const DesktopApp *app)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        g_warning("kiconf: could not write '%s': %s", path, strerror(errno));
        return;
    }
    fprintf(f, "[Desktop Entry]\n");
    fprintf(f, "Type=Application\n");
    fprintf(f, "Name=%s\n", app->name);
    if (app->comment[0]) {
        fprintf(f, "Comment=%s\n", app->comment);
    }
    fprintf(f, "Exec=%s\n", app->exec);
    if (app->icon[0]) {
        fprintf(f, "Icon=%s\n", app->icon);
    }
    if (app->categories[0]) {
        fprintf(f, "Categories=%s\n", app->categories);
    }
    fprintf(f, "Terminal=%s\n", app->terminal ? "true" : "false");
    if (app->nodisplay) {
        fprintf(f, "NoDisplay=true\n");
    }
    fclose(f);
}

static void toggle_visible_cb(GtkCellRendererToggle *cell, gchar *path_str, gpointer data)
{
    (void)cell;
    (void)data;
    GtkTreePath *path = gtk_tree_path_new_from_string(path_str);
    GtkTreeIter it;
    if (!gtk_tree_model_get_iter(GTK_TREE_MODEL(g_menu_store), &it, path)) {
        gtk_tree_path_free(path);
        return;
    }
    gtk_tree_path_free(path);

    gint row;
    gtk_tree_model_get(GTK_TREE_MODEL(g_menu_store), &it, COL_M_ROW, &row, -1);
    if (row < 0 || row >= g_n_apps) {
        return; /* a category heading, not an app */
    }
    DesktopApp *app = &g_apps[row];
    int new_nodisplay = !app->nodisplay;

    char userdir[512], userpath[512];
    user_apps_dir(userdir, sizeof(userdir));
    mkdir(userdir, 0700);
    snprintf(userpath, sizeof(userpath), "%s/%s", userdir, app->id);

    if (app->is_user) {
        desktop_entry_set_key(app->path, "NoDisplay", new_nodisplay ? "true" : "false");
    } else if (access(userpath, F_OK) == 0) {
        desktop_entry_set_key(userpath, "NoDisplay", new_nodisplay ? "true" : "false");
    } else {
        app->nodisplay = new_nodisplay;
        write_full_override(userpath, app);
    }
    refill_menu_store();
}

/* Nome/Comentario/Comando/Icone/Categoria/Terminal dialog, shared by
 * "Novo..." (fields start blank) and "Editar..." (fields pre-filled from
 * `seed`, may be NULL). Returns TRUE and fills `out` if the user hit OK. */
static gboolean run_entry_dialog(GtkWidget *window, const char *title, const DesktopApp *seed, DesktopApp *out)
{
    memset(out, 0, sizeof(*out));

    GtkWidget *dialog = gtk_dialog_new_with_buttons(title, GTK_WINDOW(window), GTK_DIALOG_MODAL, GTK_STOCK_CANCEL,
                                                     GTK_RESPONSE_CANCEL, GTK_STOCK_OK, GTK_RESPONSE_OK, NULL);
    GtkWidget *table = gtk_table_new(6, 2, FALSE);
    gtk_container_set_border_width(GTK_CONTAINER(table), 8);

    GtkWidget *name_entry = gtk_entry_new();
    GtkWidget *comment_entry = gtk_entry_new();
    GtkWidget *exec_entry = gtk_entry_new();
    GtkWidget *icon_entry = gtk_entry_new();
    GtkWidget *terminal_chk = gtk_check_button_new();

    GtkWidget *cat_combo = gtk_combo_box_new_text();
    for (int i = 0; i < N_MENU_CATEGORIES; i++) {
        gtk_combo_box_append_text(GTK_COMBO_BOX(cat_combo), MENU_CATEGORIES[i].label);
    }
    gtk_combo_box_append_text(GTK_COMBO_BOX(cat_combo), "Outros");

    if (seed) {
        gtk_entry_set_text(GTK_ENTRY(name_entry), seed->name);
        gtk_entry_set_text(GTK_ENTRY(comment_entry), seed->comment);
        gtk_entry_set_text(GTK_ENTRY(exec_entry), seed->exec);
        gtk_entry_set_text(GTK_ENTRY(icon_entry), seed->icon);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(terminal_chk), seed->terminal);
        int cat = app_category_index(seed);
        gtk_combo_box_set_active(GTK_COMBO_BOX(cat_combo), cat < 0 ? N_MENU_CATEGORIES : cat);
    } else {
        gtk_combo_box_set_active(GTK_COMBO_BOX(cat_combo), N_MENU_CATEGORIES);
    }

    labeled_row(table, 0, "Nome:", name_entry);
    labeled_row(table, 1, "Comentario:", comment_entry);
    labeled_row(table, 2, "Comando:", exec_entry);
    labeled_row(table, 3, "Icone:", icon_entry);
    labeled_row(table, 4, "Categoria:", cat_combo);
    labeled_row(table, 5, "Executar em terminal:", terminal_chk);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dialog)->vbox), table, TRUE, TRUE, 0);
    gtk_widget_show_all(table);

    gboolean ok = FALSE;
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK) {
        const gchar *name = gtk_entry_get_text(GTK_ENTRY(name_entry));
        const gchar *exec = gtk_entry_get_text(GTK_ENTRY(exec_entry));
        if (name && *name && exec && *exec) {
            snprintf(out->name, sizeof(out->name), "%s", name);
            snprintf(out->comment, sizeof(out->comment), "%s", gtk_entry_get_text(GTK_ENTRY(comment_entry)));
            snprintf(out->exec, sizeof(out->exec), "%s", exec);
            snprintf(out->icon, sizeof(out->icon), "%s", gtk_entry_get_text(GTK_ENTRY(icon_entry)));
            out->terminal = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(terminal_chk));
            int cidx = gtk_combo_box_get_active(GTK_COMBO_BOX(cat_combo));
            if (cidx >= 0 && cidx < N_MENU_CATEGORIES) {
                snprintf(out->categories, sizeof(out->categories), "%s", MENU_CATEGORIES[cidx].xdg_name);
            }
            ok = TRUE;
        }
    }
    gtk_widget_destroy(dialog);
    return ok;
}

static gboolean selected_app_row(GtkTreeView *view, gint *row_out)
{
    GtkTreeSelection *sel = gtk_tree_view_get_selection(view);
    GtkTreeIter it;
    if (!gtk_tree_selection_get_selected(sel, NULL, &it)) {
        return FALSE;
    }
    gint row;
    gtk_tree_model_get(GTK_TREE_MODEL(g_menu_store), &it, COL_M_ROW, &row, -1);
    if (row < 0 || row >= g_n_apps) {
        return FALSE; /* a category heading */
    }
    *row_out = row;
    return TRUE;
}

static void new_entry_cb(GtkWidget *widget, gpointer data)
{
    (void)data;
    GtkWidget *window = gtk_widget_get_toplevel(widget);
    DesktopApp entry;
    if (!run_entry_dialog(window, "Novo programa", NULL, &entry)) {
        return;
    }

    char slug[NAME_LEN];
    int j = 0;
    for (const char *p = entry.name; *p && j < (int)sizeof(slug) - 1; p++) {
        slug[j++] = isalnum((unsigned char)*p) ? (char)tolower((unsigned char)*p) : '-';
    }
    slug[j] = '\0';
    if (!slug[0]) {
        snprintf(slug, sizeof(slug), "custom");
    }

    char userdir[512];
    user_apps_dir(userdir, sizeof(userdir));
    mkdir(userdir, 0700);

    char path[512];
    int n = 0;
    do {
        if (n == 0) {
            snprintf(path, sizeof(path), "%s/%s.desktop", userdir, slug);
        } else {
            snprintf(path, sizeof(path), "%s/%s-%d.desktop", userdir, slug, n + 1);
        }
        n++;
    } while (access(path, F_OK) == 0 && n < 100);

    write_full_override(path, &entry);
    refill_menu_store();
}

static void edit_entry_cb(GtkWidget *widget, gpointer data)
{
    GtkTreeView *view = GTK_TREE_VIEW(data);
    GtkWidget *window = gtk_widget_get_toplevel(widget);
    gint row;
    if (!selected_app_row(view, &row)) {
        return;
    }
    DesktopApp *app = &g_apps[row];

    DesktopApp edited;
    if (!run_entry_dialog(window, "Editar programa", app, &edited)) {
        return;
    }

    if (app->is_user) {
        desktop_entry_set_key(app->path, "Name", edited.name);
        desktop_entry_set_key(app->path, "Comment", edited.comment[0] ? edited.comment : NULL);
        desktop_entry_set_key(app->path, "Exec", edited.exec);
        desktop_entry_set_key(app->path, "Icon", edited.icon[0] ? edited.icon : NULL);
        desktop_entry_set_key(app->path, "Categories", edited.categories[0] ? edited.categories : NULL);
        desktop_entry_set_key(app->path, "Terminal", edited.terminal ? "true" : "false");
    } else {
        edited.nodisplay = app->nodisplay;
        char userdir[512], path[512];
        user_apps_dir(userdir, sizeof(userdir));
        mkdir(userdir, 0700);
        snprintf(path, sizeof(path), "%s/%s", userdir, app->id);
        write_full_override(path, &edited);
    }
    refill_menu_store();
}

static void delete_entry_cb(GtkWidget *widget, gpointer data)
{
    (void)widget;
    GtkTreeView *view = GTK_TREE_VIEW(data);
    gint row;
    if (!selected_app_row(view, &row)) {
        return;
    }
    if (!g_apps[row].is_user) {
        return; /* nothing of ours to remove -- it's a plain system entry */
    }
    unlink(g_apps[row].path);
    refill_menu_store();
}

GtkWidget *build_menu_tab(void)
{
    GtkWidget *outer = gtk_vbox_new(FALSE, 8);
    gtk_container_set_border_width(GTK_CONTAINER(outer), 12);

    g_menu_store = gtk_tree_store_new(N_M_COLS, G_TYPE_BOOLEAN, G_TYPE_STRING, G_TYPE_INT);
    g_menu_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(g_menu_store));
    refill_menu_store();

    GtkCellRenderer *vis_r = gtk_cell_renderer_toggle_new();
    g_signal_connect(vis_r, "toggled", G_CALLBACK(toggle_visible_cb), NULL);
    gtk_tree_view_append_column(
        GTK_TREE_VIEW(g_menu_view),
        gtk_tree_view_column_new_with_attributes("Visivel", vis_r, "active", COL_M_VISIBLE, NULL));
    GtkTreeViewColumn *name_col = gtk_tree_view_column_new_with_attributes(
        "Programa", gtk_cell_renderer_text_new(), "text", COL_M_NAME, NULL);
    gtk_tree_view_column_set_expand(name_col, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(g_menu_view), name_col);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), g_menu_view);
    gtk_box_pack_start(GTK_BOX(outer), scroll, TRUE, TRUE, 0);

    GtkWidget *note = gtk_label_new("Ocultar/editar uma entrada do sistema cria uma copia em "
                                     "~/.local/share/applications -- o arquivo original nunca e alterado. "
                                     "\"Excluir\" so funciona em entradas ja criadas pelo usuario.");
    gtk_label_set_line_wrap(GTK_LABEL(note), TRUE);
    gtk_misc_set_alignment(GTK_MISC(note), 0.0, 0.5);
    gtk_box_pack_start(GTK_BOX(outer), note, FALSE, FALSE, 0);

    GtkWidget *btnbox = gtk_hbox_new(FALSE, 4);
    GtkWidget *new_btn = gtk_button_new_with_label("Novo...");
    g_signal_connect(new_btn, "clicked", G_CALLBACK(new_entry_cb), NULL);
    GtkWidget *edit_btn = gtk_button_new_with_label("Editar...");
    g_signal_connect(edit_btn, "clicked", G_CALLBACK(edit_entry_cb), g_menu_view);
    GtkWidget *del_btn = gtk_button_new_with_label("Excluir");
    g_signal_connect(del_btn, "clicked", G_CALLBACK(delete_entry_cb), g_menu_view);
    gtk_box_pack_start(GTK_BOX(btnbox), new_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(btnbox), edit_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(btnbox), del_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), btnbox, FALSE, FALSE, 0);

    return outer;
}
