/* kiconf - Programas padrao tab: XDG default applications + which WM
 * kisession starts. See kiconf.c's top doc comment for the overall design.
 *
 * Browser/Arquivos/Editor/E-mail are plain `xdg-mime default <id>
 * <mimetype...>` -- the standard mechanism (writes ~/.config/mimeapps.list),
 * applies immediately, nothing kiconf-specific about it. Terminal has no
 * MIME type to hang a default off of; xispanel's folder widget and
 * xisserve already try, in order, xdg-terminal-exec (which reads
 * ~/.config/xdg-terminals.list, one desktop-file-id per line, first
 * usable one wins), then $TERMINAL, then Debian's x-terminal-emulator
 * alternative (see xispanel/widgets/folder.c) -- this tab only writes the
 * first of those (a single-line xdg-terminals.list), since it's the one
 * actual per-user preference file in that chain; $TERMINAL is a shell
 * environment concern kisession doesn't expose a config knob for yet.
 *
 * Every combo here is populated by scanning installed .desktop files for
 * a Categories= token (TerminalEmulator/WebBrowser/FileManager/
 * TextEditor/Email) -- only what's actually installed shows up, same
 * spirit as Aparencia's theme scanning.
 *
 * "WM a carregar" is kisession.conf's `wm=` (see common.h's
 * KisessionConfig) -- applies live, kisession restarts just the WM on
 * SIGHUP when it changes (kisession.c's reload_config()). */
#include "../common.h"
#include "../tabs.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define MAX_APPS 256

typedef struct {
    char id[NAME_LEN];   /* .desktop basename, what xdg-mime/xdg-terminals.list want */
    char name[NAME_LEN]; /* Name=, what the combo shows */
} DesktopApp;

/* True iff `list` (a ';'-separated Categories= value) has `category` as
 * one of its entries (not just a substring -- "TerminalEmulator" must
 * not match inside some longer, unrelated category name). */
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

static void scan_apps_dir(const char *dir, const char *category, DesktopApp *out, int *n, int max)
{
    DIR *d = opendir(dir);
    if (!d) {
        return;
    }
    struct dirent *ent;
    while (*n < max && (ent = readdir(d))) {
        size_t len = strlen(ent->d_name);
        if (len < 9 || strcmp(ent->d_name + len - 8, ".desktop")) {
            continue;
        }
        for (int i = 0; i < *n; i++) {
            if (!strcmp(out[i].id, ent->d_name)) {
                goto next; /* a higher-priority dir already listed this id */
            }
        }
        {
            char path[PATH_MAX];
            snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
            char cats[1024];
            if (!desktop_entry_get(path, "Categories", cats, sizeof(cats)) || !categories_has(cats, category)) {
                continue;
            }
            char name[NAME_LEN];
            if (!desktop_entry_get(path, "Name", name, sizeof(name))) {
                snprintf(name, sizeof(name), "%s", ent->d_name);
            }
            snprintf(out[*n].id, sizeof(out[*n].id), "%s", ent->d_name);
            snprintf(out[*n].name, sizeof(out[*n].name), "%s", name);
            (*n)++;
        }
    next:;
    }
    closedir(d);
}

/* $XDG_DATA_HOME/applications, then each $XDG_DATA_DIRS/applications --
 * same precedence order (first found wins an id) real .desktop lookups
 * use, though for listing purposes here a duplicate id just gets skipped
 * rather than mattering which copy "won". */
static int scan_apps_by_category(const char *category, DesktopApp *out, int max)
{
    int n = 0;
    char userdir[PATH_MAX];
    const char *xdg_data = getenv("XDG_DATA_HOME");
    if (xdg_data && *xdg_data) {
        snprintf(userdir, sizeof(userdir), "%s/applications", xdg_data);
    } else {
        snprintf(userdir, sizeof(userdir), "%s/.local/share/applications", getenv("HOME") ? getenv("HOME") : "/tmp");
    }
    scan_apps_dir(userdir, category, out, &n, max);

    char dirs[2048];
    const char *xdg_dirs = getenv("XDG_DATA_DIRS");
    snprintf(dirs, sizeof(dirs), "%s", (xdg_dirs && *xdg_dirs) ? xdg_dirs : "/usr/local/share:/usr/share");
    char *save = NULL;
    for (char *tok = strtok_r(dirs, ":", &save); tok; tok = strtok_r(NULL, ":", &save)) {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/applications", tok);
        scan_apps_dir(path, category, out, &n, max);
    }
    return n;
}

static const char *xdg_mime_query_default(const char *mimetype, char *out, size_t outsz)
{
    char *argv[] = {"xdg-mime", "query", "default", (char *)mimetype, NULL};
    if (!run_capture(argv, out, outsz)) {
        out[0] = '\0';
    }
    size_t len = strlen(out);
    while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r')) {
        out[--len] = '\0';
    }
    return out;
}

/* One "default app for a category" row: a combo of every installed app
 * in that Categories= bucket, applying via xdg-mime default <id>
 * <mimetypes...> (mimetypes is a single pre-built space-separated string). */
typedef struct {
    GtkWidget *combo;
    DesktopApp apps[MAX_APPS];
    int n_apps;
    char mimetypes[256]; /* space-separated, passed to xdg-mime as-is */
} MimeDefaultRow;

#define MAX_MIME_ROWS 8
static MimeDefaultRow g_mime_rows[MAX_MIME_ROWS];
static int g_n_mime_rows = 0;

static GtkWidget *add_mime_default_row(GtkWidget *table, int row, const char *label, const char *category,
                                        const char *mimetypes)
{
    MimeDefaultRow *r = &g_mime_rows[g_n_mime_rows++];
    snprintf(r->mimetypes, sizeof(r->mimetypes), "%s", mimetypes);
    r->n_apps = scan_apps_by_category(category, r->apps, MAX_APPS);

    char current[NAME_LEN];
    char first_mime[64];
    snprintf(first_mime, sizeof(first_mime), "%s", mimetypes);
    char *sp = strchr(first_mime, ' ');
    if (sp) {
        *sp = '\0';
    }
    xdg_mime_query_default(first_mime, current, sizeof(current));

    r->combo = gtk_combo_box_new_text();
    int idx = -1;
    for (int i = 0; i < r->n_apps; i++) {
        gtk_combo_box_append_text(GTK_COMBO_BOX(r->combo), r->apps[i].name);
        if (!strcmp(r->apps[i].id, current)) {
            idx = i;
        }
    }
    if (idx < 0 && current[0] && r->n_apps < MAX_APPS) {
        snprintf(r->apps[r->n_apps].id, sizeof(r->apps[r->n_apps].id), "%s", current);
        snprintf(r->apps[r->n_apps].name, sizeof(r->apps[r->n_apps].name), "%s", current);
        gtk_combo_box_append_text(GTK_COMBO_BOX(r->combo), current);
        idx = r->n_apps;
        r->n_apps++;
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(r->combo), idx >= 0 ? idx : (r->n_apps > 0 ? 0 : -1));

    labeled_row(table, row, label, r->combo);
    return r->combo;
}

/* Terminal: not a MIME default -- see the file doc comment. */
static GtkWidget *g_terminal_combo;
static DesktopApp g_terminal_apps[MAX_APPS];
static int g_n_terminal_apps;

static void terminal_list_path(char *out, size_t outsz)
{
    resolve_path("xdg-terminals.list", out, outsz);
}

static GtkWidget *g_wm_combo;
static const char *const WM_CANDIDATES[] = {"kiwm", "compiz", "kwin_x11", "kwin", "openbox", NULL};

static void apply_programs_cb(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;

    for (int i = 0; i < g_n_mime_rows; i++) {
        MimeDefaultRow *r = &g_mime_rows[i];
        int idx = gtk_combo_box_get_active(GTK_COMBO_BOX(r->combo));
        if (idx < 0 || idx >= r->n_apps) {
            continue;
        }
        char argv_line[512];
        snprintf(argv_line, sizeof(argv_line), "%s", r->mimetypes);
        char *argv[8] = {"xdg-mime", "default", r->apps[idx].id};
        int argc = 3;
        char *save = NULL;
        for (char *tok = strtok_r(argv_line, " ", &save); tok && argc < 7; tok = strtok_r(NULL, " ", &save)) {
            argv[argc++] = tok;
        }
        argv[argc] = NULL;
        run_fire(argv);
    }

    int tidx = gtk_combo_box_get_active(GTK_COMBO_BOX(g_terminal_combo));
    if (tidx >= 0 && tidx < g_n_terminal_apps) {
        char path[PATH_MAX];
        terminal_list_path(path, sizeof(path));
        FILE *f = fopen(path, "w");
        if (f) {
            fprintf(f, "%s\n", g_terminal_apps[tidx].id);
            fclose(f);
        } else {
            g_warning("kiconf: could not write '%s': %s", path, strerror(errno));
        }
    }

    gchar *wm = gtk_combo_box_get_active_text(GTK_COMBO_BOX(g_wm_combo));
    KisessionConfig cfg;
    kisession_load(&cfg); /* fresh, so Iniciar automaticamente's service toggles survive */
    snprintf(cfg.wm, sizeof(cfg.wm), "%s", (wm && strcmp(wm, "(automatico)")) ? wm : "");
    g_free(wm);
    kisession_save(&cfg);
}

GtkWidget *build_programas_tab(void)
{
    GtkWidget *outer = gtk_vbox_new(FALSE, 8);
    gtk_container_set_border_width(GTK_CONTAINER(outer), 12);

    g_n_mime_rows = 0;
    GtkWidget *apps_table = gtk_table_new(5, 2, FALSE);
    add_mime_default_row(apps_table, 0, "Navegador:", "WebBrowser", "x-scheme-handler/http x-scheme-handler/https text/html");
    add_mime_default_row(apps_table, 1, "Gerenciador de arquivos:", "FileManager", "inode/directory");
    add_mime_default_row(apps_table, 2, "Editor de texto:", "TextEditor", "text/plain");
    add_mime_default_row(apps_table, 3, "E-mail:", "Email", "x-scheme-handler/mailto");

    g_n_terminal_apps = scan_apps_by_category("TerminalEmulator", g_terminal_apps, MAX_APPS);
    char term_path[PATH_MAX];
    terminal_list_path(term_path, sizeof(term_path));
    char cur_term[NAME_LEN] = "";
    FILE *tf = fopen(term_path, "r");
    if (tf) {
        if (fgets(cur_term, sizeof(cur_term), tf)) {
            size_t len = strlen(cur_term);
            while (len > 0 && (cur_term[len - 1] == '\n' || cur_term[len - 1] == '\r')) {
                cur_term[--len] = '\0';
            }
        }
        fclose(tf);
    }
    g_terminal_combo = gtk_combo_box_new_text();
    int tidx = -1;
    for (int i = 0; i < g_n_terminal_apps; i++) {
        gtk_combo_box_append_text(GTK_COMBO_BOX(g_terminal_combo), g_terminal_apps[i].name);
        if (!strcmp(g_terminal_apps[i].id, cur_term)) {
            tidx = i;
        }
    }
    if (tidx < 0 && cur_term[0] && g_n_terminal_apps < MAX_APPS) {
        snprintf(g_terminal_apps[g_n_terminal_apps].id, sizeof(g_terminal_apps[0].id), "%s", cur_term);
        snprintf(g_terminal_apps[g_n_terminal_apps].name, sizeof(g_terminal_apps[0].name), "%s", cur_term);
        gtk_combo_box_append_text(GTK_COMBO_BOX(g_terminal_combo), cur_term);
        tidx = g_n_terminal_apps;
        g_n_terminal_apps++;
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(g_terminal_combo), tidx >= 0 ? tidx : (g_n_terminal_apps > 0 ? 0 : -1));
    labeled_row(apps_table, 4, "Terminal:", g_terminal_combo);

    gtk_box_pack_start(GTK_BOX(outer), frame_with("Aplicativos padrao", apps_table), FALSE, FALSE, 0);

    GtkWidget *wm_table = gtk_table_new(1, 2, FALSE);
    KisessionConfig cfg;
    kisession_load(&cfg);
    g_wm_combo = gtk_combo_box_new_text();
    gtk_combo_box_append_text(GTK_COMBO_BOX(g_wm_combo), "(automatico)");
    int widx = 0;
    for (int i = 0; WM_CANDIDATES[i]; i++) {
        gtk_combo_box_append_text(GTK_COMBO_BOX(g_wm_combo), WM_CANDIDATES[i]);
        if (!strcmp(cfg.wm, WM_CANDIDATES[i])) {
            widx = i + 1;
        }
    }
    if (cfg.wm[0] && widx == 0) {
        gtk_combo_box_append_text(GTK_COMBO_BOX(g_wm_combo), cfg.wm);
        widx = gtk_tree_model_iter_n_children(gtk_combo_box_get_model(GTK_COMBO_BOX(g_wm_combo)), NULL) - 1;
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(g_wm_combo), widx);
    labeled_row(wm_table, 0, "Gerenciador de janelas a carregar:", g_wm_combo);
    gtk_box_pack_start(GTK_BOX(outer),
                        frame_with("Sessao (kisession.conf -- \"(automatico)\" usa o primeiro instalado)", wm_table),
                        FALSE, FALSE, 0);

    GtkWidget *btnbox = gtk_hbox_new(FALSE, 0);
    GtkWidget *apply_btn = gtk_button_new_with_label("Aplicar");
    g_signal_connect(apply_btn, "clicked", G_CALLBACK(apply_programs_cb), NULL);
    gtk_box_pack_end(GTK_BOX(btnbox), apply_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), btnbox, FALSE, FALSE, 0);

    return outer;
}
