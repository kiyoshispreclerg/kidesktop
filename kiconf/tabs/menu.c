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
 * silent no-op for it (same guard as autostart.c's remove_custom_cb()).
 *
 * "Somente no KiDesktop" (extra env vars/args, like kmenuedit's) never
 * touches either of those: it writes a copy of the winning entry to
 * session_apps_dir(), with Exec= rewritten and the raw values kept in
 * X-KiDesktop-Env=/X-KiDesktop-Args= to read back and regenerate from.
 * kisession puts that dir in XDG_DATA_DIRS ahead of /usr/share, so every
 * XDG launcher in the session (pcmanfm and gio included) picks it up, and
 * nothing outside the session does. The one thing it can't beat is a
 * copy in $XDG_DATA_HOME/applications (the spec searches that first) --
 * the dialog says so when the app has one. Every copy is regenerated
 * from its base on each refill, so a package update or an edit above
 * isn't left behind by a stale snapshot. */
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
static GtkTreeModel *g_menu_filter; /* what g_menu_view actually shows */
static GtkWidget *g_menu_view;
static char *g_filter_folded; /* NULL/"" = show everything */
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

#define SESSION_VAL_LEN 512
static void session_get(const DesktopApp *app, char *env, char *args);
static void session_write(const DesktopApp *app, const char *env, const char *args);

static void refill_menu_store(void)
{
    g_n_apps = scan_all_apps(g_apps, MAX_DESKTOP_APPS);
    qsort(g_apps, (size_t)g_n_apps, sizeof(DesktopApp), app_cmp);
    for (int i = 0; i < g_n_apps; i++) {
        char env[SESSION_VAL_LEN], args[SESSION_VAL_LEN];
        session_get(&g_apps[i], env, args);
        if (env[0] || args[0]) {
            session_write(&g_apps[i], env, args);
        }
    }

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
    /* A heading is filtered when inserted, before it has any children --
     * re-run the filter now that they're all in. */
    gtk_tree_model_filter_refilter(GTK_TREE_MODEL_FILTER(g_menu_filter));
    gtk_tree_view_expand_all(GTK_TREE_VIEW(g_menu_view));
}

/* Lowercased and accent-stripped ("Configuracoes" matches "configurações"),
 * so the filter works however the user types pt_BR names. Caller frees. */
static char *fold_text(const char *s)
{
    char *nfd = g_utf8_normalize(s, -1, G_NORMALIZE_NFKD);
    if (!nfd) {
        return g_utf8_casefold(s, -1);
    }
    GString *bare = g_string_sized_new(strlen(nfd));
    for (const char *p = nfd; *p; p = g_utf8_next_char(p)) {
        gunichar c = g_utf8_get_char(p);
        GUnicodeType t = g_unichar_type(c);
        if (t != G_UNICODE_NON_SPACING_MARK && t != G_UNICODE_COMBINING_MARK && t != G_UNICODE_ENCLOSING_MARK) {
            g_string_append_unichar(bare, c);
        }
    }
    g_free(nfd);
    char *folded = g_utf8_casefold(bare->str, -1);
    g_string_free(bare, TRUE);
    return folded;
}

static gboolean app_matches_filter(const DesktopApp *app)
{
    const char *fields[] = {app->name, app->comment, app->id};
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        char *f = fold_text(fields[i]);
        gboolean hit = strstr(f, g_filter_folded) != NULL;
        g_free(f);
        if (hit) {
            return TRUE;
        }
    }
    return FALSE;
}

/* An app row is shown if it matches; a category heading if any of its
 * apps does, so a match never ends up orphaned or under an empty group. */
static gboolean menu_row_visible(GtkTreeModel *model, GtkTreeIter *it, gpointer data)
{
    (void)data;
    if (!g_filter_folded || !*g_filter_folded) {
        return TRUE;
    }
    gint row;
    gtk_tree_model_get(model, it, COL_M_ROW, &row, -1);
    if (row >= 0 && row < g_n_apps) {
        return app_matches_filter(&g_apps[row]);
    }
    GtkTreeIter child;
    gboolean ok = gtk_tree_model_iter_children(model, &child, it);
    while (ok) {
        gtk_tree_model_get(model, &child, COL_M_ROW, &row, -1);
        if (row >= 0 && row < g_n_apps && app_matches_filter(&g_apps[row])) {
            return TRUE;
        }
        ok = gtk_tree_model_iter_next(model, &child);
    }
    return FALSE;
}

static void filter_changed_cb(GtkEditable *editable, gpointer data)
{
    (void)data;
    g_free(g_filter_folded);
    g_filter_folded = fold_text(gtk_entry_get_text(GTK_ENTRY(editable)));
    gtk_tree_model_filter_refilter(GTK_TREE_MODEL_FILTER(g_menu_filter));
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

static int session_path(const DesktopApp *app, char *out, size_t outsz)
{
    char dir[512];
    session_apps_dir(dir, sizeof(dir));
    return fmt_fits(out, outsz, "%s/%s", dir, app->id);
}

/* Reads the env/args an app's session copy was generated from ("" if it
 * has none). */
static void session_get(const DesktopApp *app, char *env, char *args)
{
    env[0] = args[0] = '\0';
    char path[512];
    if (session_path(app, path, sizeof(path))) {
        desktop_entry_get(path, "X-KiDesktop-Env", env, SESSION_VAL_LEN);
        desktop_entry_get(path, "X-KiDesktop-Args", args, SESSION_VAL_LEN);
    }
}

/* `args` goes before the first field code (%f %F %u %U ...), so files
 * passed in by a launcher still come last; at the end if there's none. */
static void exec_insert_args(const char *exec, const char *args, char *out, size_t outsz)
{
    const char *at = NULL;
    for (const char *p = exec; (p = strchr(p, '%'));) {
        if (p[1] && strchr("fFuUick", p[1]) && (p == exec || isspace((unsigned char)p[-1])) &&
            (!p[2] || isspace((unsigned char)p[2]))) {
            at = p;
            break;
        }
        p += p[1] ? 2 : 1; /* also skips a literal "%%" */
    }
    if (!args[0]) {
        snprintf(out, outsz, "%s", exec);
    } else if (at) {
        snprintf(out, outsz, "%.*s%s %s", (int)(at - exec), exec, args, at);
    } else {
        snprintf(out, outsz, "%s %s", exec, args);
    }
}

/* (Re)writes the session copy of `app` from its current base entry, or
 * removes it when both values are empty. The copy is the base file
 * verbatim (translations, actions, MimeType= all kept) plus a new Exec=.
 * DBusActivatable= is dropped: with it, gio activates the app over D-Bus
 * and never looks at Exec=. */
static void session_write(const DesktopApp *app, const char *env, const char *args)
{
    char path[512];
    if (!session_path(app, path, sizeof(path))) {
        return;
    }
    if (!env[0] && !args[0]) {
        unlink(path);
        return;
    }
    char dir[512];
    session_apps_dir(dir, sizeof(dir));
    g_mkdir_with_parents(dir, 0700);

    gchar *contents = NULL;
    gsize len = 0;
    if (!g_file_get_contents(app->path, &contents, &len, NULL) || !g_file_set_contents(path, contents, (gssize)len, NULL)) {
        g_warning("kiconf: could not copy '%s' to '%s'", app->path, path);
        g_free(contents);
        return;
    }
    g_free(contents);

    char with_args[1100], exec[1700];
    exec_insert_args(app->exec, args, with_args, sizeof(with_args));
    if (env[0]) {
        snprintf(exec, sizeof(exec), "env %s %s", env, with_args);
    } else {
        snprintf(exec, sizeof(exec), "%s", with_args);
    }
    desktop_entry_set_key(path, "Exec", exec);
    desktop_entry_set_key(path, "DBusActivatable", NULL);
    desktop_entry_set_key(path, "X-KiDesktop-Env", env[0] ? env : NULL);
    desktop_entry_set_key(path, "X-KiDesktop-Args", args[0] ? args : NULL);
}

/* Splits `s` like Exec= does (whitespace, "double quotes" with \
 * escapes) and checks each word is NAME=VALUE with a valid NAME.
 * Returns the first offending word in `bad` (0) or 1 if all are fine. */
static int env_valid(const char *s, char *bad, size_t badsz)
{
    const char *p = s;
    while (*p) {
        while (isspace((unsigned char)*p)) {
            p++;
        }
        if (!*p) {
            break;
        }
        const char *start = p;
        int inq = 0;
        while (*p && (inq || !isspace((unsigned char)*p))) {
            if (*p == '\\' && p[1]) {
                p++;
            } else if (*p == '"') {
                inq = !inq;
            }
            p++;
        }
        const char *n = start + (*start == '"');
        int ok = isalpha((unsigned char)*n) || *n == '_';
        while (ok && (isalnum((unsigned char)*n) || *n == '_')) {
            n++;
        }
        if (!ok || *n != '=') {
            snprintf(bad, badsz, "%.*s", (int)(p - start), start);
            return 0;
        }
    }
    return 1;
}

static void toggle_visible_cb(GtkCellRendererToggle *cell, gchar *path_str, gpointer data)
{
    (void)cell;
    (void)data;
    GtkTreePath *path = gtk_tree_path_new_from_string(path_str);
    GtkTreeIter it;
    if (!gtk_tree_model_get_iter(g_menu_filter, &it, path)) {
        gtk_tree_path_free(path);
        return;
    }
    gtk_tree_path_free(path);

    gint row;
    gtk_tree_model_get(g_menu_filter, &it, COL_M_ROW, &row, -1);
    if (row < 0 || row >= g_n_apps) {
        return; /* a category heading, not an app */
    }
    DesktopApp *app = &g_apps[row];
    int new_nodisplay = !app->nodisplay;

    char userdir[512], userpath[512];
    user_apps_dir(userdir, sizeof(userdir));
    mkdir(userdir, 0700);
    if (!fmt_fits(userpath, sizeof(userpath), "%s/%s", userdir, app->id)) {
        return;
    }

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
 * `seed`, may be NULL). Returns TRUE and fills `out` if the user hit OK.
 *
 * With `env`/`args` non-NULL (Editar only -- a brand-new entry is a user
 * copy, which a session copy could never shadow) it also shows the
 * "Somente no KiDesktop" fields, pre-filled from and written back to
 * those SESSION_VAL_LEN buffers. */
static gboolean run_entry_dialog(GtkWidget *window, const char *title, const DesktopApp *seed, DesktopApp *out,
                                 char *env, char *args)
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

    GtkWidget *env_entry = NULL, *args_entry = NULL;
    if (env && args) {
        GtkWidget *sbox = gtk_vbox_new(FALSE, 6);
        gtk_container_set_border_width(GTK_CONTAINER(sbox), 8);
        GtkWidget *stable = gtk_table_new(2, 2, FALSE);
        env_entry = gtk_entry_new();
        args_entry = gtk_entry_new();
        gtk_entry_set_text(GTK_ENTRY(env_entry), env);
        gtk_entry_set_text(GTK_ENTRY(args_entry), args);
        labeled_row(stable, 0, _("Variaveis de ambiente:"), env_entry);
        labeled_row(stable, 1, _("Argumentos extras:"), args_entry);
        gtk_box_pack_start(GTK_BOX(sbox), stable, FALSE, FALSE, 0);

        GtkWidget *hint = gtk_label_new(_("Aplicados so ao abrir o programa dentro da sessao KiDesktop "
                                          "(painel, lancador, gerenciador de arquivos...). Variaveis no formato "
                                          "NOME=valor separadas por espaco; os argumentos entram antes dos "
                                          "arquivos abertos. Alterar os campos de cima de um programa do sistema cria uma "
                                          "copia pessoal dele, que tem prioridade sobre estas opcoes."));
        gtk_label_set_line_wrap(GTK_LABEL(hint), TRUE);
        gtk_misc_set_alignment(GTK_MISC(hint), 0.0, 0.5);
        gtk_box_pack_start(GTK_BOX(sbox), hint, FALSE, FALSE, 0);
        if (seed && seed->is_user) {
            GtkWidget *warn = gtk_label_new(_("Atencao: este programa tem uma copia pessoal em "
                                              "~/.local/share/applications, que tem prioridade sobre estas "
                                              "opcoes. Elas so valem se essa copia deixar de existir."));
            gtk_label_set_line_wrap(GTK_LABEL(warn), TRUE);
            gtk_misc_set_alignment(GTK_MISC(warn), 0.0, 0.5);
            gtk_box_pack_start(GTK_BOX(sbox), warn, FALSE, FALSE, 0);
        }
        GtkWidget *frame = frame_with(_("Somente no KiDesktop"), sbox);
        gtk_container_set_border_width(GTK_CONTAINER(frame), 8);
        gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dialog)->vbox), frame, FALSE, FALSE, 0);
        gtk_widget_show_all(frame);
    }

    gboolean ok = FALSE;
    gint resp;
    while ((resp = gtk_dialog_run(GTK_DIALOG(dialog))) == GTK_RESPONSE_OK && env_entry) {
        char bad[256];
        if (env_valid(gtk_entry_get_text(GTK_ENTRY(env_entry)), bad, sizeof(bad))) {
            break;
        }
        GtkWidget *msg = gtk_message_dialog_new(GTK_WINDOW(dialog), GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR,
                                                GTK_BUTTONS_OK, _("Variavel de ambiente invalida: %s"), bad);
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(msg), "%s", _("Use NOME=valor, separadas por espaco."));
        gtk_dialog_run(GTK_DIALOG(msg));
        gtk_widget_destroy(msg);
    }
    if (resp == GTK_RESPONSE_OK) {
        const gchar *name = gtk_entry_get_text(GTK_ENTRY(name_entry));
        const gchar *exec = gtk_entry_get_text(GTK_ENTRY(exec_entry));
        if (name && *name && exec && *exec) {
            snprintf(out->name, sizeof(out->name), "%s", name);
            snprintf(out->comment, sizeof(out->comment), "%s", gtk_entry_get_text(GTK_ENTRY(comment_entry)));
            snprintf(out->exec, sizeof(out->exec), "%s", exec);
            snprintf(out->icon, sizeof(out->icon), "%s", gtk_entry_get_text(GTK_ENTRY(icon_entry)));
            out->terminal = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(terminal_chk));
            int cidx = gtk_combo_box_get_active(GTK_COMBO_BOX(cat_combo));
            int seed_cat = seed ? app_category_index(seed) : -1;
            if (seed && cidx == (seed_cat < 0 ? N_MENU_CATEGORIES : seed_cat)) {
                /* Unchanged: keep the full list (sub-categories too), so
                 * it doesn't read as an edit of a system entry. */
                snprintf(out->categories, sizeof(out->categories), "%s", seed->categories);
            } else if (cidx >= 0 && cidx < N_MENU_CATEGORIES) {
                snprintf(out->categories, sizeof(out->categories), "%s", MENU_CATEGORIES[cidx].xdg_name);
            }
            if (env_entry) {
                snprintf(env, SESSION_VAL_LEN, "%s", gtk_entry_get_text(GTK_ENTRY(env_entry)));
                snprintf(args, SESSION_VAL_LEN, "%s", gtk_entry_get_text(GTK_ENTRY(args_entry)));
                g_strstrip(env);
                g_strstrip(args);
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
    GtkTreeModel *model;
    GtkTreeIter it;
    if (!gtk_tree_selection_get_selected(sel, &model, &it)) {
        return FALSE;
    }
    gint row;
    gtk_tree_model_get(model, &it, COL_M_ROW, &row, -1);
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
    if (!run_entry_dialog(window, "Novo programa", NULL, &entry, NULL, NULL)) {
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
    int fits;
    do {
        if (n == 0) {
            fits = fmt_fits(path, sizeof(path), "%s/%s.desktop", userdir, slug);
        } else {
            fits = fmt_fits(path, sizeof(path), "%s/%s-%d.desktop", userdir, slug, n + 1);
        }
        n++;
    } while (fits && access(path, F_OK) == 0 && n < 100);
    if (!fits) {
        return;
    }

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
    char env[SESSION_VAL_LEN], args[SESSION_VAL_LEN];
    session_get(app, env, args);
    if (!run_entry_dialog(window, "Editar programa", app, &edited, env, args)) {
        return;
    }
    session_write(app, env, args);

    int global_changed = strcmp(edited.name, app->name) || strcmp(edited.comment, app->comment) ||
                         strcmp(edited.exec, app->exec) || strcmp(edited.icon, app->icon) ||
                         strcmp(edited.categories, app->categories) || edited.terminal != app->terminal;
    if (!global_changed) {
        /* Only the session fields changed: no user copy, which would
         * shadow the session one it was just asked for. */
    } else if (app->is_user) {
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
        if (fmt_fits(path, sizeof(path), "%s/%s", userdir, app->id)) {
            write_full_override(path, &edited);
        }
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
    DesktopApp gone = g_apps[row];
    unlink(gone.path);
    refill_menu_store();
    for (int i = 0; i < g_n_apps; i++) {
        if (!strcmp(g_apps[i].id, gone.id)) {
            return; /* a system entry is back underneath; refill regenerated its session copy */
        }
    }
    session_write(&gone, "", ""); /* nothing left to generate it from */
}

GtkWidget *build_menu_tab(void)
{
    GtkWidget *outer = gtk_vbox_new(FALSE, 8);
    gtk_container_set_border_width(GTK_CONTAINER(outer), 12);

    GtkWidget *filter_entry = gtk_entry_new();
    a11y_name(filter_entry, _("Filtrar programas"));
    GtkWidget *filter_row = gtk_hbox_new(FALSE, 6);
    gtk_box_pack_start(GTK_BOX(filter_row), gtk_label_new(_("Filtrar:")), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(filter_row), filter_entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(outer), filter_row, FALSE, FALSE, 0);

    g_menu_store = gtk_tree_store_new(N_M_COLS, G_TYPE_BOOLEAN, G_TYPE_STRING, G_TYPE_INT);
    g_menu_filter = gtk_tree_model_filter_new(GTK_TREE_MODEL(g_menu_store), NULL);
    gtk_tree_model_filter_set_visible_func(GTK_TREE_MODEL_FILTER(g_menu_filter), menu_row_visible, NULL, NULL);
    g_menu_view = gtk_tree_view_new_with_model(g_menu_filter);
    a11y_name(g_menu_view, _("Menu de programas"));
    refill_menu_store();
    g_signal_connect(filter_entry, "changed", G_CALLBACK(filter_changed_cb), NULL);

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

    GtkWidget *note = gtk_label_new(_("Ocultar/editar uma entrada do sistema cria uma copia em "
                                       "~/.local/share/applications -- o arquivo original nunca e alterado. "
                                       "\"Excluir\" so funciona em entradas ja criadas pelo usuario. "
                                       "As opcoes \"Somente no KiDesktop\" ficam em ~/.local/share/kidesktop."));
    gtk_label_set_line_wrap(GTK_LABEL(note), TRUE);
    gtk_misc_set_alignment(GTK_MISC(note), 0.0, 0.5);
    gtk_box_pack_start(GTK_BOX(outer), note, FALSE, FALSE, 0);

    GtkWidget *btnbox = gtk_hbox_new(FALSE, 4);
    GtkWidget *new_btn = gtk_button_new_with_label(_("Novo..."));
    g_signal_connect(new_btn, "clicked", G_CALLBACK(new_entry_cb), NULL);
    GtkWidget *edit_btn = gtk_button_new_with_label(_("Editar..."));
    g_signal_connect(edit_btn, "clicked", G_CALLBACK(edit_entry_cb), g_menu_view);
    GtkWidget *del_btn = gtk_button_new_with_label(_("Excluir"));
    g_signal_connect(del_btn, "clicked", G_CALLBACK(delete_entry_cb), g_menu_view);
    gtk_box_pack_start(GTK_BOX(btnbox), new_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(btnbox), edit_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(btnbox), del_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), btnbox, FALSE, FALSE, 0);

    return outer;
}
