/* kiconf - Iniciar automaticamente tab: kisession's own services (live
 * toggle, kisession.conf's SERVICE lines) plus XDG autostart entries
 * (installed apps like fcitx/opensnitch, takes effect at the next
 * login -- see the file doc comment on why). See kiconf.c's top doc
 * comment for the overall design.
 *
 * The XDG-autostart half mirrors kisession.c's own reader exactly
 * (autostart_entry_wanted()/autostart_scan_dir()/run_autostart(), down to
 * the ~/.config/autostart-wins-over-$XDG_CONFIG_DIRS precedence and the
 * OnlyShowIn/NotShowIn "KiDesktop" check) so this tab lists precisely
 * what would actually try to start, nothing kisession itself would skip.
 * Unlike that reader, entries already Hidden are listed too (so they can
 * be turned back on) -- only the harder eligibility checks (Type,
 * OnlyShowIn/NotShowIn, TryExec) drop a row entirely, since those would
 * never autostart regardless of Hidden.
 *
 * Toggling never edits a system .desktop file (usually root-owned, and
 * about to be overwritten by the next package update anyway): it always
 * patches Hidden= in a ~/.config/autostart/<basename>.desktop override
 * (desktop_entry_set_key(), same helper kisession.conf's SERVICE list
 * has no need for but this and Programas Padrao's xdg-mime-adjacent bits
 * both do), creating a minimal one if none exists yet. Re-enabling a
 * system entry patches Hidden=false in that override rather than
 * deleting it, so a hand-written override with other fields never loses
 * them -- see desktop_entry_set_key()'s own doc comment in common.h.
 *
 * autostart_scan_dir() gates this only on services picking up new state
 * at the *next* login (g_autostart_done in kisession.c never re-fires
 * once the session is up), unlike the SERVICE list above it, which
 * applies immediately over SIGHUP -- the status label says as much. */
#include "../common.h"
#include "../tabs.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- kisession's own services ------------------------------------------ */

static GtkWidget *g_service_chk[N_KISESSION_SERVICES];

static void apply_services_cb(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    KisessionConfig cfg;
    kisession_load(&cfg); /* fresh, so Programas Padrao's wm= survives */
    for (int i = 0; i < N_KISESSION_SERVICES; i++) {
        cfg.enabled[i] = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_service_chk[i]));
    }
    kisession_save(&cfg);
}

/* ---- XDG autostart ------------------------------------------------------ */

#define MAX_AUTOSTART_ENTRIES 256
#define MAX_SEEN 256

typedef struct {
    char basename[NAME_LEN]; /* "fcitx.desktop" -- identity across a reload */
    char name[NAME_LEN];
    int hidden;
    int has_user_override; /* ~/.config/autostart/<basename> already exists */
} AutostartEntry;

static AutostartEntry g_entries[MAX_AUTOSTART_ENTRIES];
static int g_n_entries = 0;
static GtkListStore *g_autostart_store;
enum { COL_AS_ENABLED = 0, COL_AS_NAME, COL_AS_ROW, N_AS_COLS };

static void autostart_userdir(char *out, size_t outsz)
{
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) {
        snprintf(out, outsz, "%s/autostart", xdg);
    } else {
        snprintf(out, outsz, "%s/.config/autostart", getenv("HOME") ? getenv("HOME") : "/tmp");
    }
}

static int list_has_kidesktop(const char *list)
{
    const char *p = list;
    while (*p) {
        const char *semi = strchr(p, ';');
        size_t len = semi ? (size_t)(semi - p) : strlen(p);
        if (len == strlen("KiDesktop") && !strncmp(p, "KiDesktop", len)) {
            return 1;
        }
        if (!semi) {
            break;
        }
        p = semi + 1;
    }
    return 0;
}

static int command_exists(const char *cmd)
{
    char out[PATH_MAX];
    char *argv[] = {"which", (char *)cmd, NULL};
    return run_capture(argv, out, sizeof(out)) && out[0];
}

/* Everything autostart_entry_wanted() in kisession.c checks *except*
 * Hidden/X-GNOME-Autostart-enabled -- those become this tab's checkbox
 * instead of a reason to hide the row entirely. */
static int autostart_eligible_ignoring_hidden(const char *path)
{
    char buf[1024];
    if (desktop_entry_get(path, "Type", buf, sizeof(buf)) && strcmp(buf, "Application")) {
        return 0;
    }
    if (desktop_entry_get(path, "OnlyShowIn", buf, sizeof(buf)) && !list_has_kidesktop(buf)) {
        return 0;
    }
    if (desktop_entry_get(path, "NotShowIn", buf, sizeof(buf)) && list_has_kidesktop(buf)) {
        return 0;
    }
    if (desktop_entry_get(path, "TryExec", buf, sizeof(buf)) && !command_exists(buf)) {
        return 0;
    }
    return 1;
}

static void scan_autostart_dir(const char *dir, int is_user_dir, char seen[][NAME_LEN], int *n_seen)
{
    DIR *d = opendir(dir);
    if (!d) {
        return;
    }
    struct dirent *ent;
    while (g_n_entries < MAX_AUTOSTART_ENTRIES && (ent = readdir(d))) {
        size_t len = strlen(ent->d_name);
        if (len < 9 || strcmp(ent->d_name + len - 8, ".desktop")) {
            continue;
        }
        int already = 0;
        for (int i = 0; i < *n_seen; i++) {
            if (!strcmp(seen[i], ent->d_name)) {
                already = 1;
                break;
            }
        }
        if (already) {
            continue;
        }
        if (*n_seen < MAX_SEEN) {
            snprintf(seen[*n_seen], NAME_LEN, "%s", ent->d_name);
            (*n_seen)++;
        }

        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        if (!autostart_eligible_ignoring_hidden(path)) {
            continue;
        }

        AutostartEntry *e = &g_entries[g_n_entries];
        snprintf(e->basename, sizeof(e->basename), "%s", ent->d_name);
        if (!desktop_entry_get(path, "Name", e->name, sizeof(e->name))) {
            snprintf(e->name, sizeof(e->name), "%s", ent->d_name);
        }
        char hbuf[16], gbuf[16];
        e->hidden = (desktop_entry_get(path, "Hidden", hbuf, sizeof(hbuf)) && !strcmp(hbuf, "true")) ||
                    (desktop_entry_get(path, "X-GNOME-Autostart-enabled", gbuf, sizeof(gbuf)) && !strcmp(gbuf, "false"));
        e->has_user_override = is_user_dir;
        g_n_entries++;
    }
    closedir(d);
}

static void scan_autostart_entries(void)
{
    g_n_entries = 0;
    char seen[MAX_SEEN][NAME_LEN];
    int n_seen = 0;

    char userdir[PATH_MAX];
    autostart_userdir(userdir, sizeof(userdir));
    scan_autostart_dir(userdir, 1, seen, &n_seen);

    char dirs[2048];
    const char *cdirs = getenv("XDG_CONFIG_DIRS");
    snprintf(dirs, sizeof(dirs), "%s", (cdirs && *cdirs) ? cdirs : "/etc/xdg");
    char *save = NULL;
    for (char *tok = strtok_r(dirs, ":", &save); tok; tok = strtok_r(NULL, ":", &save)) {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/autostart", tok);
        scan_autostart_dir(path, 0, seen, &n_seen);
    }
}

static void as_cell_toggled(GtkCellRendererToggle *cell, gchar *path_str, gpointer data)
{
    (void)cell;
    (void)data;
    GtkTreePath *path = gtk_tree_path_new_from_string(path_str);
    GtkTreeIter it;
    if (gtk_tree_model_get_iter(GTK_TREE_MODEL(g_autostart_store), &it, path)) {
        gboolean cur;
        gtk_tree_model_get(GTK_TREE_MODEL(g_autostart_store), &it, COL_AS_ENABLED, &cur, -1);
        gtk_list_store_set(g_autostart_store, &it, COL_AS_ENABLED, !cur, -1);
    }
    gtk_tree_path_free(path);
}

static void refill_autostart_store(void)
{
    scan_autostart_entries();
    gtk_list_store_clear(g_autostart_store);
    for (int i = 0; i < g_n_entries; i++) {
        GtkTreeIter it;
        gtk_list_store_append(g_autostart_store, &it);
        gtk_list_store_set(g_autostart_store, &it,
                            COL_AS_ENABLED, !g_entries[i].hidden, COL_AS_NAME, g_entries[i].name,
                            COL_AS_ROW, i, -1);
    }
}

static void add_custom_cb(GtkWidget *widget, gpointer data)
{
    GtkWidget *window = gtk_widget_get_toplevel(widget);
    (void)data;

    GtkWidget *dialog = gtk_dialog_new_with_buttons(
        "Adicionar programa de inicio automatico", GTK_WINDOW(window), GTK_DIALOG_MODAL,
        GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL, GTK_STOCK_OK, GTK_RESPONSE_OK, NULL);

    GtkWidget *table = gtk_table_new(2, 2, FALSE);
    gtk_container_set_border_width(GTK_CONTAINER(table), 8);
    GtkWidget *name_entry = gtk_entry_new();
    labeled_row(table, 0, "Nome:", name_entry);
    GtkWidget *cmd_entry = gtk_entry_new();
    labeled_row(table, 1, "Comando:", cmd_entry);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dialog)->vbox), table, TRUE, TRUE, 0);
    gtk_widget_show_all(table);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK) {
        const gchar *name = gtk_entry_get_text(GTK_ENTRY(name_entry));
        const gchar *cmd = gtk_entry_get_text(GTK_ENTRY(cmd_entry));
        if (name && *name && cmd && *cmd) {
            char slug[NAME_LEN];
            int j = 0;
            for (const char *p = name; *p && j < (int)sizeof(slug) - 1; p++) {
                slug[j++] = (isalnum((unsigned char)*p)) ? (char)tolower((unsigned char)*p) : '-';
            }
            slug[j] = '\0';
            if (!slug[0]) {
                snprintf(slug, sizeof(slug), "custom");
            }

            char userdir[PATH_MAX];
            autostart_userdir(userdir, sizeof(userdir));
            mkdir(userdir, 0700);

            char path[PATH_MAX];
            int n = 0;
            do {
                if (n == 0) {
                    snprintf(path, sizeof(path), "%s/%s.desktop", userdir, slug);
                } else {
                    snprintf(path, sizeof(path), "%s/%s-%d.desktop", userdir, slug, n + 1);
                }
                n++;
            } while (access(path, F_OK) == 0 && n < 100);

            FILE *f = fopen(path, "w");
            if (f) {
                fprintf(f, "[Desktop Entry]\nType=Application\nName=%s\nExec=%s\nNoDisplay=true\n", name, cmd);
                fclose(f);
            } else {
                g_warning("kiconf: could not write '%s': %s", path, strerror(errno));
            }
            refill_autostart_store();
        }
    }
    gtk_widget_destroy(dialog);
}

static void remove_custom_cb(GtkWidget *widget, gpointer data)
{
    GtkTreeView *view = GTK_TREE_VIEW(data);
    (void)widget;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(view);
    GtkTreeIter it;
    if (!gtk_tree_selection_get_selected(sel, NULL, &it)) {
        return;
    }
    gint row;
    gtk_tree_model_get(GTK_TREE_MODEL(g_autostart_store), &it, COL_AS_ROW, &row, -1);
    if (row < 0 || row >= g_n_entries || !g_entries[row].has_user_override) {
        return; /* nothing of ours to remove -- it's a plain system entry */
    }
    char userdir[PATH_MAX], path[PATH_MAX];
    autostart_userdir(userdir, sizeof(userdir));
    snprintf(path, sizeof(path), "%s/%s", userdir, g_entries[row].basename);
    unlink(path);
    refill_autostart_store();
}

static void apply_autostart_cb(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;

    char userdir[PATH_MAX];
    autostart_userdir(userdir, sizeof(userdir));
    mkdir(userdir, 0700);

    GtkTreeIter it;
    gboolean valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(g_autostart_store), &it);
    while (valid) {
        gboolean enabled;
        gint row;
        gtk_tree_model_get(GTK_TREE_MODEL(g_autostart_store), &it, COL_AS_ENABLED, &enabled, COL_AS_ROW, &row, -1);
        /* Only touch a row the user actually flipped -- writing Hidden=
         * into every row unconditionally would create a minimal
         * override stub for every untouched system entry too, which
         * then shadows its Name=/Icon=/etc. on the next scan (the
         * override, now existing, wins XDG precedence) for no reason:
         * nothing about it actually changed. */
        if (row >= 0 && row < g_n_entries && (gboolean)!g_entries[row].hidden != enabled) {
            char path[PATH_MAX];
            snprintf(path, sizeof(path), "%s/%s", userdir, g_entries[row].basename);
            if (!g_entries[row].has_user_override) {
                /* First override for this entry: carry the display name
                 * over too, so the row doesn't degrade to its raw
                 * basename on the next scan just because *this* file
                 * (now the one XDG precedence picks) never had a Name=
                 * of its own -- everything else about the entry (Icon=,
                 * Comment=, ...) still comes from the shadowed original,
                 * kisession doesn't look at those anyway. */
                desktop_entry_set_key(path, "Name", g_entries[row].name);
            }
            desktop_entry_set_key(path, "Hidden", enabled ? "false" : "true");
        }
        valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(g_autostart_store), &it);
    }
    refill_autostart_store();
}

GtkWidget *build_autostart_tab(void)
{
    GtkWidget *outer = gtk_vbox_new(FALSE, 8);
    gtk_container_set_border_width(GTK_CONTAINER(outer), 12);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    GtkWidget *content = gtk_vbox_new(FALSE, 8);
    gtk_scrolled_window_add_with_viewport(GTK_SCROLLED_WINDOW(scroll), content);
    gtk_box_pack_start(GTK_BOX(outer), scroll, TRUE, TRUE, 0);

    /* kisession's own services */
    KisessionConfig cfg;
    kisession_load(&cfg);
    GtkWidget *svc_table = gtk_table_new(N_KISESSION_SERVICES, 2, FALSE);
    for (int i = 0; i < N_KISESSION_SERVICES; i++) {
        g_service_chk[i] = gtk_check_button_new();
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_service_chk[i]), cfg.enabled[i]);
        labeled_row(svc_table, i, KISESSION_SERVICES[i].doc, g_service_chk[i]);
    }
    GtkWidget *svc_box = gtk_vbox_new(FALSE, 4);
    gtk_box_pack_start(GTK_BOX(svc_box), svc_table, FALSE, FALSE, 0);
    GtkWidget *svc_apply = gtk_button_new_with_label("Aplicar servicos (recarrega kisession na hora)");
    g_signal_connect(svc_apply, "clicked", G_CALLBACK(apply_services_cb), NULL);
    GtkWidget *svc_btnbox = gtk_hbox_new(FALSE, 0);
    gtk_box_pack_end(GTK_BOX(svc_btnbox), svc_apply, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(svc_box), svc_btnbox, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content), frame_with("Servicos do KiDesktop (kisession.conf)", svc_box), FALSE, FALSE, 0);

    /* XDG autostart (installed apps) */
    g_autostart_store = gtk_list_store_new(N_AS_COLS, G_TYPE_BOOLEAN, G_TYPE_STRING, G_TYPE_INT);
    refill_autostart_store();

    GtkWidget *as_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(g_autostart_store));
    GtkCellRenderer *en_r = gtk_cell_renderer_toggle_new();
    g_signal_connect(en_r, "toggled", G_CALLBACK(as_cell_toggled), NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(as_view),
        gtk_tree_view_column_new_with_attributes("Ativo", en_r, "active", COL_AS_ENABLED, NULL));
    GtkCellRenderer *name_r = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *name_col = gtk_tree_view_column_new_with_attributes("Programa", name_r, "text", COL_AS_NAME, NULL);
    gtk_tree_view_column_set_expand(name_col, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(as_view), name_col);

    GtkWidget *as_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(as_scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(as_scroll, -1, 220);
    gtk_container_add(GTK_CONTAINER(as_scroll), as_view);

    GtkWidget *as_box = gtk_vbox_new(FALSE, 4);
    gtk_box_pack_start(GTK_BOX(as_box), as_scroll, TRUE, TRUE, 0);

    GtkWidget *note = gtk_label_new(
        "Aplicativos instalados que se registram para iniciar com a sessao (fcitx, "
        "opensnitch, ...), mais os que voce adicionar. Valem a partir do proximo login.");
    gtk_misc_set_alignment(GTK_MISC(note), 0.0, 0.5);
    gtk_label_set_line_wrap(GTK_LABEL(note), TRUE);
    gtk_box_pack_start(GTK_BOX(as_box), note, FALSE, FALSE, 0);

    GtkWidget *as_btnbox = gtk_hbox_new(FALSE, 6);
    GtkWidget *add_btn = gtk_button_new_with_label("Adicionar...");
    GtkWidget *remove_btn = gtk_button_new_with_label("Remover");
    GtkWidget *as_apply = gtk_button_new_with_label("Aplicar");
    g_signal_connect(add_btn, "clicked", G_CALLBACK(add_custom_cb), NULL);
    g_signal_connect(remove_btn, "clicked", G_CALLBACK(remove_custom_cb), as_view);
    g_signal_connect(as_apply, "clicked", G_CALLBACK(apply_autostart_cb), NULL);
    gtk_box_pack_start(GTK_BOX(as_btnbox), add_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(as_btnbox), remove_btn, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(as_btnbox), as_apply, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(as_box), as_btnbox, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(content), frame_with("Inicio automatico (XDG autostart)", as_box), FALSE, FALSE, 0);

    return outer;
}
