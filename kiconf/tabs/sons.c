/* kiconf - Sons tab: the desktop-wide sound theme (shared/xis_sound.h).
 *
 * Writes kidesktop-sound.conf: theme=, enabled=, volume= and one
 * sound.<event>= line for each event given its own file (or "none").
 * Every program that plays sounds re-reads that file on its own within
 * a couple of seconds, so there is nothing to signal on save.
 *
 * Picking a theme sets every event at once; the list below it overrides
 * single events. "Testar" previews with the tab's current, unsaved
 * choices, through xis_sound_play_file(). */
#include "../common.h"
#include "../tabs.h"
#include "../../shared/xis_sound.h"

#include <dirent.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SOUND_CONF "kidesktop-sound.conf"
#define MAX_THEMES 128

/* Event ids are the ones shared/xis_sound.h documents. */
static const struct { const char *id, *label; } EVENTS[] = {
    { "window-minimize",         N_("Minimizar janela") },
    { "window-close",            N_("Fechar janela") },
    { "window-maximize",         N_("Maximizar janela") },
    { "window-shade",            N_("Enrolar janela") },
    { "window-unshade",          N_("Desenrolar janela") },
    { "window-all-desktops",     N_("Janela em todas as areas de trabalho") },
    { "window-all-desktops-not", N_("Janela so nesta area de trabalho") },
    { "window-move",             N_("Comecar a mover janela") },
    { "window-move-stop",        N_("Terminar de mover janela") },
    { "login",                   N_("Entrar na sessao") },
    { "logout",                  N_("Sair da sessao") },
    { "bell",                    N_("Campainha do sistema") },
    { "trash-empty",             N_("Esvaziar a lixeira") },
    { "folder-open",             N_("Abrir pasta no gerenciador de arquivos") },
    { "folder-back",             N_("Voltar no gerenciador de arquivos") },
    { "folder-forward",          N_("Avancar no gerenciador de arquivos") },
    { "folder-up",               N_("Subir para a pasta acima") },
    { "file-trash",              N_("Mover arquivos para a lixeira") },
    { "complete",                N_("Operacao concluida / programa terminou no terminal") },
    { "dialog-error",            N_("Erro numa operacao de arquivos") },
    { "device-removed",          N_("Dispositivo removido com seguranca") },
};
#define N_EVENTS ((int)(sizeof(EVENTS) / sizeof(EVENTS[0])))

enum { COL_LABEL, COL_SOURCE, COL_ID, COL_OVERRIDE, N_COLS };

static GtkWidget *g_enabled_chk, *g_volume_scale, *g_theme_combo, *g_view, *g_status;
static GtkListStore *g_store;
static char g_themes[MAX_THEMES][64];
static int g_ntheme;

static void add_theme(const char *name)
{
    if (!*name || strlen(name) >= sizeof(g_themes[0]) || g_ntheme >= MAX_THEMES)
        return;
    for (int i = 0; i < g_ntheme; i++)
        if (!strcmp(g_themes[i], name))
            return;
    snprintf(g_themes[g_ntheme++], sizeof(g_themes[0]), "%s", name);
}

static void scan_dir_themes(const char *dir, const char *marker)
{
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        char path[PATH_MAX];
        if (!fmt_fits(path, sizeof(path), "%s/%s/%s", dir, e->d_name, marker))
            continue;
        if (access(path, R_OK) == 0)
            add_theme(e->d_name);
    }
    closedir(d);
}

/* /usr/share/sounds/<Theme>-Window-Minimize.ogg -> "<Theme>". */
static void scan_kde_themes(void)
{
    static const char suffix[] = "-Window-Minimize.ogg";
    DIR *d = opendir("/usr/share/sounds");
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t n = strlen(e->d_name), sl = sizeof(suffix) - 1;
        if (n > sl && !strcmp(e->d_name + n - sl, suffix)) {
            char name[64];
            snprintf(name, sizeof(name), "%.*s", (int)(n - sl), e->d_name);
            add_theme(name);
        }
    }
    closedir(d);
}

static void scan_themes(void)
{
    g_ntheme = 0;
    const char *xdg = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    char base[PATH_MAX], dir[PATH_MAX + 32];
    if (xdg && *xdg)
        snprintf(base, sizeof(base), "%s", xdg);
    else
        snprintf(base, sizeof(base), "%s/.local/share", home ? home : "");
    const char *roots[] = { base, "/usr/local/share", "/usr/share" };
    for (int r = 0; r < 3; r++) {
        if (fmt_fits(dir, sizeof(dir), "%s/kidesktop/sounds", roots[r]))
            scan_dir_themes(dir, "sounds.conf");
    }
    for (int r = 0; r < 3; r++) {
        if (fmt_fits(dir, sizeof(dir), "%s/sounds", roots[r]))
            scan_dir_themes(dir, "index.theme");
    }
    scan_kde_themes();
}

static const char *selected_theme(void)
{
    int i = gtk_combo_box_get_active(GTK_COMBO_BOX(g_theme_combo));
    return i >= 0 && i < g_ntheme ? g_themes[i] : "";
}

static void sound_source_text(const char *theme, const char *id, const char *override,
                              char *out, size_t sz)
{
    char file[PATH_MAX];
    if (override && !strcmp(override, "none"))
        snprintf(out, sz, "%s", _("Silencioso"));
    else if (override && *override)
        snprintf(out, sz, "%s: %s", _("Arquivo"), strrchr(override, '/') ? strrchr(override, '/') + 1 : override);
    else if (xis_sound_theme_file(theme, id, file, sizeof(file)))
        snprintf(out, sz, "%s", strrchr(file, '/') ? strrchr(file, '/') + 1 : file);
    else
        snprintf(out, sz, "%s", _("(o tema nao define)"));
}

static void refresh_sources(void)
{
    const char *theme = selected_theme();
    GtkTreeIter it;
    gboolean ok = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(g_store), &it);
    while (ok) {
        char *id, *ovr;
        gtk_tree_model_get(GTK_TREE_MODEL(g_store), &it, COL_ID, &id, COL_OVERRIDE, &ovr, -1);
        char text[PATH_MAX];
        sound_source_text(theme, id, ovr, text, sizeof(text));
        gtk_list_store_set(g_store, &it, COL_SOURCE, text, -1);
        g_free(id);
        g_free(ovr);
        ok = gtk_tree_model_iter_next(GTK_TREE_MODEL(g_store), &it);
    }
}

static gboolean selected_row(GtkTreeIter *it)
{
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(g_view));
    GtkTreeModel *model;
    return gtk_tree_selection_get_selected(sel, &model, it);
}

static void set_override(const char *value)
{
    GtkTreeIter it;
    if (!selected_row(&it))
        return;
    gtk_list_store_set(g_store, &it, COL_OVERRIDE, value, -1);
    refresh_sources();
}

static void on_theme_changed(GtkWidget *w, gpointer data)
{
    (void)w;
    (void)data;
    refresh_sources();
}

static void on_test(GtkWidget *w, gpointer data)
{
    (void)w;
    (void)data;
    GtkTreeIter it;
    if (!selected_row(&it))
        return;
    char *id, *ovr;
    gtk_tree_model_get(GTK_TREE_MODEL(g_store), &it, COL_ID, &id, COL_OVERRIDE, &ovr, -1);
    char file[PATH_MAX] = "";
    if (ovr && !strcmp(ovr, "none"))
        ;
    else if (ovr && *ovr)
        snprintf(file, sizeof(file), "%s", ovr);
    else
        xis_sound_theme_file(selected_theme(), id, file, sizeof(file));
    if (*file)
        xis_sound_play_file(file, (int)gtk_range_get_value(GTK_RANGE(g_volume_scale)));
    g_free(id);
    g_free(ovr);
}

static void on_choose(GtkWidget *w, gpointer data)
{
    (void)w;
    (void)data;
    GtkTreeIter it;
    if (!selected_row(&it))
        return;
    GtkWidget *dlg = gtk_file_chooser_dialog_new(_("Escolher som"),
        GTK_WINDOW(gtk_widget_get_toplevel(g_view)), GTK_FILE_CHOOSER_ACTION_OPEN,
        GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL, GTK_STOCK_OPEN, GTK_RESPONSE_ACCEPT, NULL);
    GtkFileFilter *flt = gtk_file_filter_new();
    gtk_file_filter_set_name(flt, _("Audio (ogg, oga, wav)"));
    gtk_file_filter_add_pattern(flt, "*.ogg");
    gtk_file_filter_add_pattern(flt, "*.oga");
    gtk_file_filter_add_pattern(flt, "*.wav");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), flt);
    gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dlg), "/usr/share/sounds");
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *f = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        if (f)
            set_override(f);
        g_free(f);
    }
    gtk_widget_destroy(dlg);
}

static void on_mute(GtkWidget *w, gpointer data)  { (void)w; (void)data; set_override("none"); }
static void on_reset(GtkWidget *w, gpointer data) { (void)w; (void)data; set_override(""); }

static void on_save(GtkWidget *w, gpointer data)
{
    (void)w;
    (void)data;
    char path[PATH_MAX];
    resolve_path(SOUND_CONF, path, sizeof(path));
    FILE *f = fopen(path, "w");
    if (!f) {
        gtk_label_set_text(GTK_LABEL(g_status), _("Nao foi possivel gravar o arquivo de sons."));
        return;
    }
    fprintf(f, "# Sound theme, written by kiconf (Sons tab); see shared/xis_sound.h.\n");
    fprintf(f, "theme=%s\n", selected_theme());
    fprintf(f, "enabled=%d\n", gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_enabled_chk)) ? 1 : 0);
    fprintf(f, "volume=%d\n", (int)gtk_range_get_value(GTK_RANGE(g_volume_scale)));
    GtkTreeIter it;
    gboolean ok = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(g_store), &it);
    while (ok) {
        char *id, *ovr;
        gtk_tree_model_get(GTK_TREE_MODEL(g_store), &it, COL_ID, &id, COL_OVERRIDE, &ovr, -1);
        if (ovr && *ovr)
            fprintf(f, "sound.%s=%s\n", id, ovr);
        g_free(id);
        g_free(ovr);
        ok = gtk_tree_model_iter_next(GTK_TREE_MODEL(g_store), &it);
    }
    fclose(f);
    signal_daemon("kiconfd"); /* republishes the theme over XSETTINGS */
    gtk_label_set_text(GTK_LABEL(g_status), _("Salvo. Os programas aplicam em ate 2 segundos."));
}

static void load_settings(void)
{
    char path[PATH_MAX];
    resolve_path(SOUND_CONF, path, sizeof(path));
    char theme[64] = "";
    int enabled = 0, volume = 80;
    char over[N_EVENTS][PATH_MAX];
    memset(over, 0, sizeof(over));

    FILE *f = fopen(path, "r");
    if (f) {
        char line[PATH_MAX + 80];
        while (fgets(line, sizeof(line), f)) {
            char *l = trim(line), *eq = strchr(l, '=');
            if (*l == '#' || !eq)
                continue;
            *eq = '\0';
            char *k = trim(l), *v = trim(eq + 1);
            if (!strcmp(k, "theme"))
                snprintf(theme, sizeof(theme), "%s", v);
            else if (!strcmp(k, "enabled"))
                enabled = atoi(v) != 0;
            else if (!strcmp(k, "volume"))
                volume = atoi(v);
            else if (!strncmp(k, "sound.", 6))
                for (int i = 0; i < N_EVENTS; i++)
                    if (!strcmp(k + 6, EVENTS[i].id))
                        snprintf(over[i], sizeof(over[i]), "%s", v);
        }
        fclose(f);
    }

    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_enabled_chk), enabled);
    gtk_range_set_value(GTK_RANGE(g_volume_scale), volume < 0 ? 0 : volume > 100 ? 100 : volume);
    int active = g_ntheme > 0 ? 0 : -1;
    for (int i = 0; i < g_ntheme; i++)
        if (!strcmp(g_themes[i], theme))
            active = i;
    gtk_combo_box_set_active(GTK_COMBO_BOX(g_theme_combo), active);

    gtk_list_store_clear(g_store);
    for (int i = 0; i < N_EVENTS; i++) {
        GtkTreeIter it;
        gtk_list_store_append(g_store, &it);
        gtk_list_store_set(g_store, &it, COL_LABEL, _(EVENTS[i].label), COL_ID, EVENTS[i].id,
                           COL_OVERRIDE, over[i], -1);
    }
    refresh_sources();
}

GtkWidget *build_sons_tab(void)
{
    GtkWidget *outer = gtk_vbox_new(FALSE, 8);
    gtk_container_set_border_width(GTK_CONTAINER(outer), 8);

    GtkWidget *info = gtk_label_new(_(
        "Sons dos eventos do desktop. Escolher um tema muda todos de uma vez; "
        "na lista abaixo cada evento pode ter um arquivo proprio ou ficar silencioso."));
    gtk_misc_set_alignment(GTK_MISC(info), 0.0, 0.5);
    gtk_label_set_line_wrap(GTK_LABEL(info), TRUE);
    gtk_box_pack_start(GTK_BOX(outer), info, FALSE, FALSE, 0);

    scan_themes();

    GtkWidget *table = gtk_table_new(3, 2, FALSE);
    gtk_table_set_row_spacings(GTK_TABLE(table), 6);
    gtk_table_set_col_spacings(GTK_TABLE(table), 8);

    g_enabled_chk = gtk_check_button_new_with_label(_("Ativar sons"));
    gtk_table_attach(GTK_TABLE(table), g_enabled_chk, 0, 2, 0, 1, GTK_FILL, GTK_FILL, 0, 0);

    g_theme_combo = gtk_combo_box_new_text();
    for (int i = 0; i < g_ntheme; i++)
        gtk_combo_box_append_text(GTK_COMBO_BOX(g_theme_combo), g_themes[i]);
    g_signal_connect(g_theme_combo, "changed", G_CALLBACK(on_theme_changed), NULL);
    labeled_row(table, 1, _("Tema sonoro:"), g_theme_combo);

    g_volume_scale = gtk_hscale_new_with_range(0, 100, 1);
    gtk_scale_set_value_pos(GTK_SCALE(g_volume_scale), GTK_POS_RIGHT);
    labeled_row(table, 2, _("Volume:"), g_volume_scale);
    gtk_box_pack_start(GTK_BOX(outer), table, FALSE, FALSE, 0);

    g_store = gtk_list_store_new(N_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    g_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(g_store));
    a11y_name(g_view, _("Eventos sonoros"));
    gtk_tree_view_append_column(GTK_TREE_VIEW(g_view),
        gtk_tree_view_column_new_with_attributes(_("Evento"), gtk_cell_renderer_text_new(), "text", COL_LABEL, NULL));
    GtkTreeViewColumn *src = gtk_tree_view_column_new_with_attributes(
        _("Som"), gtk_cell_renderer_text_new(), "text", COL_SOURCE, NULL);
    gtk_tree_view_column_set_expand(src, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(g_view), src);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(scroll, -1, 240);
    gtk_container_add(GTK_CONTAINER(scroll), g_view);
    gtk_box_pack_start(GTK_BOX(outer), scroll, TRUE, TRUE, 0);

    GtkWidget *row = gtk_hbox_new(FALSE, 6);
    static const struct { const char *label; GCallback cb; } BTNS[] = {
        { N_("Testar"), G_CALLBACK(on_test) },
        { N_("Escolher arquivo..."), G_CALLBACK(on_choose) },
        { N_("Silenciar"), G_CALLBACK(on_mute) },
        { N_("Usar o do tema"), G_CALLBACK(on_reset) },
    };
    for (int i = 0; i < 4; i++) {
        GtkWidget *b = gtk_button_new_with_label(_(BTNS[i].label));
        g_signal_connect(b, "clicked", BTNS[i].cb, NULL);
        gtk_box_pack_start(GTK_BOX(row), b, FALSE, FALSE, 0);
    }
    GtkWidget *save = gtk_button_new_with_label(_("Salvar"));
    g_signal_connect(save, "clicked", G_CALLBACK(on_save), NULL);
    gtk_box_pack_end(GTK_BOX(row), save, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), row, FALSE, FALSE, 0);

    g_status = gtk_label_new("");
    gtk_misc_set_alignment(GTK_MISC(g_status), 0.0, 0.5);
    gtk_box_pack_start(GTK_BOX(outer), g_status, FALSE, FALSE, 0);

    load_settings();
    return outer;
}
