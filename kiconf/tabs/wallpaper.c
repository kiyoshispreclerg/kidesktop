/* kiconf - Wallpaper tab: xisback socket client.
 * See kiconf.c's top doc comment for the overall design. */
#include "../common.h"
#include "../tabs.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

/* Wallpaper tab (xisback) widgets + state */
#define MAX_WP_LAYERS 64
typedef struct {
    char output[NAME_LEN], desktop[16], mode[16];
    int interval, shuffle, fade_ms;
    char path[PATH_MAX];
} WpLayer;
static WpLayer g_wp_layers[MAX_WP_LAYERS];
static int g_n_wp_layers = 0;
static GtkListStore *g_wp_layers_store;
static GtkWidget *g_wp_layers_view;
static GtkWidget *g_wp_output_entry, *g_wp_desktop_entry, *g_wp_mode_combo;
static GtkWidget *g_wp_interval_spin, *g_wp_shuffle_chk, *g_wp_fade_spin, *g_wp_path_entry;
static GtkWidget *g_wp_action_left, *g_wp_action_right, *g_wp_action_middle, *g_wp_action_double;
static GtkWidget *g_wp_status_label;

enum { COL_WP_OUTPUT = 0, COL_WP_DESKTOP, COL_WP_MODE, COL_WP_INTERVAL, COL_WP_SHUFFLE, COL_WP_FADE, COL_WP_PATH, N_WP_COLS };

/* ---- Wallpaper tab: xisback socket client ----------------------------- */

static void xisback_socket_path(char *out, size_t outsz)
{
    const char *rundir = getenv("XDG_RUNTIME_DIR");
    snprintf(out, outsz, "%s/xisback.sock", (rundir && *rundir) ? rundir : "/tmp");
}

/* Sends one line, reads until EOF, same wire format as PROTOCOL.md's
 * Python example. If the daemon isn't reachable, starts it (no args ->
 * comes up as an empty daemon, see xisback's own doc comment) and retries
 * once, same as xisconf.py's _xisback_send(). */
static int xisback_send(const char *cmd, char *resp, size_t respsz)
{
    resp[0] = '\0';
    char path[PATH_MAX];
    xisback_socket_path(path, sizeof(path));

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return 0;
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        /* xisback doesn't daemonize/detach itself (same as kiwm/kicomp --
         * see spawn_replace()'s own doc comment) -- it just runs its event
         * loop forever in this child, so waitpid()ing on it here would
         * block until xisback eventually exits, i.e. forever. setsid()
         * detaches it from kiconf the same way spawn_replace() does, so
         * it outlives kiconf instead of dying with it. */
        pid_t pid = fork();
        if (pid == 0) {
            setsid();
            execlp("xisback", "xisback", (char *)NULL);
            _exit(127);
        }
        usleep(300000);
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0 || connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
            if (fd >= 0) {
                close(fd);
            }
            return 0;
        }
    }
    char line[PATH_MAX + 128];
    snprintf(line, sizeof(line), "%s\n", cmd);
    if (write(fd, line, strlen(line)) < 0) {
        close(fd);
        return 0;
    }
    shutdown(fd, SHUT_WR);
    size_t total = 0;
    ssize_t n;
    while (total + 1 < respsz && (n = read(fd, resp + total, respsz - 1 - total)) > 0) {
        total += (size_t)n;
    }
    resp[total] = '\0';
    close(fd);
    return 1;
}

static int xisback_list_layers(WpLayer *layers, int max)
{
    char resp[16384];
    if (!xisback_send("LIST", resp, sizeof(resp))) {
        return 0;
    }
    int n = 0;
    char *save = NULL;
    char *line = strtok_r(resp, "\n", &save);
    while (line && n < max) {
        char *save2 = NULL;
        char *output = strtok_r(line, "\t", &save2);
        char *desktop = output ? strtok_r(NULL, "\t", &save2) : NULL;
        char *mode = desktop ? strtok_r(NULL, "\t", &save2) : NULL;
        char *interval = mode ? strtok_r(NULL, "\t", &save2) : NULL;
        char *shuffle = interval ? strtok_r(NULL, "\t", &save2) : NULL;
        char *fade = shuffle ? strtok_r(NULL, "\t", &save2) : NULL;
        char *path = fade ? strtok_r(NULL, "", &save2) : NULL;
        if (output && desktop && mode && interval && shuffle && fade && path) {
            WpLayer *l = &layers[n++];
            snprintf(l->output, sizeof(l->output), "%s", output);
            snprintf(l->desktop, sizeof(l->desktop), "%s", desktop);
            snprintf(l->mode, sizeof(l->mode), "%s", mode);
            l->interval = atoi(interval);
            l->shuffle = atoi(shuffle);
            l->fade_ms = atoi(fade);
            snprintf(l->path, sizeof(l->path), "%s", path);
        }
        line = strtok_r(NULL, "\n", &save);
    }
    return n;
}

static void refresh_wp_layers(void)
{
    gtk_list_store_clear(g_wp_layers_store);
    g_n_wp_layers = xisback_list_layers(g_wp_layers, MAX_WP_LAYERS);
    for (int i = 0; i < g_n_wp_layers; i++) {
        WpLayer *l = &g_wp_layers[i];
        char interval_s[16], shuffle_s[8], fade_s[16];
        snprintf(interval_s, sizeof(interval_s), "%d", l->interval);
        snprintf(shuffle_s, sizeof(shuffle_s), "%s", l->shuffle ? "sim" : "nao");
        snprintf(fade_s, sizeof(fade_s), "%.1fs", l->fade_ms / 1000.0);
        GtkTreeIter it;
        gtk_list_store_append(g_wp_layers_store, &it);
        gtk_list_store_set(g_wp_layers_store, &it,
                            COL_WP_OUTPUT, l->output, COL_WP_DESKTOP, l->desktop, COL_WP_MODE, l->mode,
                            COL_WP_INTERVAL, interval_s, COL_WP_SHUFFLE, shuffle_s, COL_WP_FADE, fade_s,
                            COL_WP_PATH, l->path, -1);
    }
    char status[64];
    snprintf(status, sizeof(status), "%d camada(s) ativa(s).", g_n_wp_layers);
    gtk_label_set_text(GTK_LABEL(g_wp_status_label), status);
}

static void wp_load_layer_into_form(const WpLayer *l)
{
    gtk_entry_set_text(GTK_ENTRY(g_wp_output_entry), l->output);
    gtk_entry_set_text(GTK_ENTRY(g_wp_desktop_entry), l->desktop);
    gtk_combo_box_set_active(GTK_COMBO_BOX(g_wp_mode_combo), strcmp(l->mode, "stretch") == 0 ? 1 : 0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_wp_interval_spin), l->interval);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_wp_shuffle_chk), l->shuffle);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_wp_fade_spin), l->fade_ms / 1000.0);
    gtk_entry_set_text(GTK_ENTRY(g_wp_path_entry), l->path);
}

static void on_wp_row_selected(GtkTreeSelection *sel, gpointer data)
{
    (void)data;
    GtkTreeIter it;
    GtkTreeModel *model;
    if (!gtk_tree_selection_get_selected(sel, &model, &it)) {
        return;
    }
    GtkTreePath *path = gtk_tree_model_get_path(model, &it);
    int idx = gtk_tree_path_get_indices(path)[0];
    gtk_tree_path_free(path);
    if (idx >= 0 && idx < g_n_wp_layers) {
        wp_load_layer_into_form(&g_wp_layers[idx]);
    }
}

static int wp_selected_index(void)
{
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(g_wp_layers_view));
    GtkTreeIter it;
    GtkTreeModel *model;
    if (!gtk_tree_selection_get_selected(sel, &model, &it)) {
        return -1;
    }
    GtkTreePath *path = gtk_tree_model_get_path(model, &it);
    int idx = gtk_tree_path_get_indices(path)[0];
    gtk_tree_path_free(path);
    return idx;
}

static void on_wp_set(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    const char *output = gtk_entry_get_text(GTK_ENTRY(g_wp_output_entry));
    const char *desktop = gtk_entry_get_text(GTK_ENTRY(g_wp_desktop_entry));
    gchar *mode = gtk_combo_box_get_active_text(GTK_COMBO_BOX(g_wp_mode_combo));
    int interval = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_wp_interval_spin));
    int shuffle = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_wp_shuffle_chk));
    int fade_ms = (int)(gtk_spin_button_get_value(GTK_SPIN_BUTTON(g_wp_fade_spin)) * 1000);
    const char *path = gtk_entry_get_text(GTK_ENTRY(g_wp_path_entry));

    char cmd[PATH_MAX + 256];
    snprintf(cmd, sizeof(cmd), "SET\t%s\t%s\t%s\t%d\t%d\t%d\t%s",
              output[0] ? output : "*", desktop[0] ? desktop : "*", mode ? mode : "fill",
              interval, shuffle, fade_ms, path);
    char resp[256];
    xisback_send(cmd, resp, sizeof(resp));
    g_free(mode);
    refresh_wp_layers();
}

static void on_wp_next(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    int idx = wp_selected_index();
    if (idx < 0 || idx >= g_n_wp_layers) {
        return;
    }
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "NEXT\t%s\t%s", g_wp_layers[idx].output, g_wp_layers[idx].desktop);
    char resp[256];
    xisback_send(cmd, resp, sizeof(resp));
    refresh_wp_layers();
}

static void on_wp_clear_selected(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    int idx = wp_selected_index();
    if (idx < 0 || idx >= g_n_wp_layers) {
        return;
    }
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "CLEAR\t%s\t%s", g_wp_layers[idx].output, g_wp_layers[idx].desktop);
    char resp[256];
    xisback_send(cmd, resp, sizeof(resp));
    refresh_wp_layers();
}

static void on_wp_clear_all(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    char resp[256];
    xisback_send("CLEARALL", resp, sizeof(resp));
    refresh_wp_layers();
}

static void on_wp_refresh(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    refresh_wp_layers();
}

static void on_wp_global(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    char resp[256];
    xisback_send("CLEARALL", resp, sizeof(resp));
    on_wp_set(NULL, NULL);
}

static void on_wp_pick_file(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    GtkWidget *dlg = gtk_file_chooser_dialog_new("Escolher imagem", NULL, GTK_FILE_CHOOSER_ACTION_OPEN,
                                                   GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
                                                   GTK_STOCK_OPEN, GTK_RESPONSE_ACCEPT, NULL);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        gtk_entry_set_text(GTK_ENTRY(g_wp_path_entry), filename);
        g_free(filename);
    }
    gtk_widget_destroy(dlg);
}

static void on_wp_pick_folder(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    GtkWidget *dlg = gtk_file_chooser_dialog_new("Escolher pasta de slideshow", NULL,
                                                   GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
                                                   GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
                                                   GTK_STOCK_OPEN, GTK_RESPONSE_ACCEPT, NULL);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        gtk_entry_set_text(GTK_ENTRY(g_wp_path_entry), filename);
        g_free(filename);
    }
    gtk_widget_destroy(dlg);
}

static void on_wp_actions_save(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    const char *left = gtk_entry_get_text(GTK_ENTRY(g_wp_action_left));
    const char *right = gtk_entry_get_text(GTK_ENTRY(g_wp_action_right));
    const char *middle = gtk_entry_get_text(GTK_ENTRY(g_wp_action_middle));
    const char *dbl = gtk_entry_get_text(GTK_ENTRY(g_wp_action_double));
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "SETACTIONS\t%s\t%s\t%s\t%s", left, right, middle, dbl);
    char resp[256];
    xisback_send(cmd, resp, sizeof(resp));
}

static void load_wp_actions(void)
{
    char resp[1024];
    if (!xisback_send("ACTIONS", resp, sizeof(resp))) {
        return;
    }
    char *save = NULL;
    char *left = strtok_r(resp, "\t", &save);
    char *right = left ? strtok_r(NULL, "\t", &save) : NULL;
    char *middle = right ? strtok_r(NULL, "\t", &save) : NULL;
    char *dbl = middle ? strtok_r(NULL, "\n", &save) : NULL;
    gtk_entry_set_text(GTK_ENTRY(g_wp_action_left), left ? left : "");
    gtk_entry_set_text(GTK_ENTRY(g_wp_action_right), right ? right : "");
    gtk_entry_set_text(GTK_ENTRY(g_wp_action_middle), middle ? middle : "");
    gtk_entry_set_text(GTK_ENTRY(g_wp_action_double), dbl ? dbl : "");
}

GtkWidget *build_wallpaper_tab(void)
{
    GtkWidget *outer = gtk_vbox_new(FALSE, 8);
    gtk_container_set_border_width(GTK_CONTAINER(outer), 12);

    g_wp_status_label = gtk_label_new("-");
    gtk_misc_set_alignment(GTK_MISC(g_wp_status_label), 0.0, 0.5);
    gtk_box_pack_start(GTK_BOX(outer), g_wp_status_label, FALSE, FALSE, 0);

    g_wp_layers_store = gtk_list_store_new(N_WP_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                             G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    g_wp_layers_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(g_wp_layers_store));
    const char *wp_titles[N_WP_COLS] = {"Output", "Desktop", "Modo", "Intervalo", "Shuffle", "Fade", "Caminho"};
    for (int col = 0; col < N_WP_COLS; col++) {
        GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
        GtkTreeViewColumn *tvcol = gtk_tree_view_column_new_with_attributes(wp_titles[col], renderer, "text", col, NULL);
        gtk_tree_view_column_set_expand(tvcol, col == N_WP_COLS - 1);
        gtk_tree_view_append_column(GTK_TREE_VIEW(g_wp_layers_view), tvcol);
    }
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(g_wp_layers_view)), "changed",
                       G_CALLBACK(on_wp_row_selected), NULL);
    GtkWidget *wp_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(wp_scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(wp_scroll, -1, 140);
    gtk_container_add(GTK_CONTAINER(wp_scroll), g_wp_layers_view);

    GtkWidget *layers_box = gtk_vbox_new(FALSE, 4);
    gtk_box_pack_start(GTK_BOX(layers_box), wp_scroll, TRUE, TRUE, 0);
    GtkWidget *layers_btnbox = gtk_hbox_new(FALSE, 6);
    GtkWidget *next_btn = gtk_button_new_with_label("Avancar slide");
    GtkWidget *clear_sel_btn = gtk_button_new_with_label("Limpar selecionada");
    GtkWidget *clear_all_btn = gtk_button_new_with_label("Limpar todas");
    GtkWidget *refresh_btn = gtk_button_new_with_label("Atualizar");
    GtkWidget *global_btn = gtk_button_new_with_label("Mesmo papel de parede pra tudo");
    g_signal_connect(next_btn, "clicked", G_CALLBACK(on_wp_next), NULL);
    g_signal_connect(clear_sel_btn, "clicked", G_CALLBACK(on_wp_clear_selected), NULL);
    g_signal_connect(clear_all_btn, "clicked", G_CALLBACK(on_wp_clear_all), NULL);
    g_signal_connect(refresh_btn, "clicked", G_CALLBACK(on_wp_refresh), NULL);
    g_signal_connect(global_btn, "clicked", G_CALLBACK(on_wp_global), NULL);
    gtk_box_pack_start(GTK_BOX(layers_btnbox), next_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(layers_btnbox), clear_sel_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(layers_btnbox), clear_all_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(layers_btnbox), refresh_btn, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(layers_btnbox), global_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(layers_box), layers_btnbox, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), frame_with("Camadas ativas (clique numa linha pra editar)", layers_box), TRUE, TRUE, 0);

    GtkWidget *set_table = gtk_table_new(6, 2, FALSE);
    g_wp_output_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(g_wp_output_entry), "*");
    labeled_row(set_table, 0, "Output ('*' = tudo):", g_wp_output_entry);
    g_wp_desktop_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(g_wp_desktop_entry), "*");
    labeled_row(set_table, 1, "Desktop ('*' = todos):", g_wp_desktop_entry);
    g_wp_mode_combo = gtk_combo_box_new_text();
    gtk_combo_box_append_text(GTK_COMBO_BOX(g_wp_mode_combo), "fill");
    gtk_combo_box_append_text(GTK_COMBO_BOX(g_wp_mode_combo), "stretch");
    gtk_combo_box_set_active(GTK_COMBO_BOX(g_wp_mode_combo), 0);
    labeled_row(set_table, 2, "Modo:", g_wp_mode_combo);
    g_wp_interval_spin = gtk_spin_button_new_with_range(5, 86400, 5);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_wp_interval_spin), 300);
    labeled_row(set_table, 3, "Intervalo do slideshow (s):", g_wp_interval_spin);
    g_wp_shuffle_chk = gtk_check_button_new_with_label("Ordem aleatoria (em vez de alfabetica)");
    gtk_table_attach(GTK_TABLE(set_table), g_wp_shuffle_chk, 0, 2, 4, 5, GTK_FILL, GTK_FILL, 4, 2);
    g_wp_fade_spin = gtk_spin_button_new_with_range(0.0, 5.0, 0.1);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(g_wp_fade_spin), 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_wp_fade_spin), 1.0);
    labeled_row(set_table, 5, "Crossfade (s, 0=instantaneo):", g_wp_fade_spin);

    GtkWidget *path_row = gtk_hbox_new(FALSE, 4);
    g_wp_path_entry = gtk_entry_new();
    gtk_box_pack_start(GTK_BOX(path_row), g_wp_path_entry, TRUE, TRUE, 0);
    GtkWidget *file_btn = gtk_button_new_with_label("Arquivo...");
    GtkWidget *folder_btn = gtk_button_new_with_label("Pasta...");
    g_signal_connect(file_btn, "clicked", G_CALLBACK(on_wp_pick_file), NULL);
    g_signal_connect(folder_btn, "clicked", G_CALLBACK(on_wp_pick_folder), NULL);
    gtk_box_pack_start(GTK_BOX(path_row), file_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(path_row), folder_btn, FALSE, FALSE, 0);

    GtkWidget *set_box = gtk_vbox_new(FALSE, 4);
    gtk_box_pack_start(GTK_BOX(set_box), set_table, FALSE, FALSE, 0);
    GtkWidget *path_label = gtk_label_new("Imagem ou pasta:");
    gtk_misc_set_alignment(GTK_MISC(path_label), 0.0, 0.5);
    gtk_box_pack_start(GTK_BOX(set_box), path_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(set_box), path_row, FALSE, FALSE, 0);
    GtkWidget *set_btn = gtk_button_new_with_label("Definir camada");
    g_signal_connect(set_btn, "clicked", G_CALLBACK(on_wp_set), NULL);
    GtkWidget *set_btnbox = gtk_hbox_new(FALSE, 0);
    gtk_box_pack_end(GTK_BOX(set_btnbox), set_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(set_box), set_btnbox, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), frame_with("Definir/substituir uma camada", set_box), FALSE, FALSE, 0);

    GtkWidget *actions_table = gtk_table_new(4, 2, FALSE);
    g_wp_action_left = gtk_entry_new();
    labeled_row(actions_table, 0, "Clique esquerdo:", g_wp_action_left);
    g_wp_action_right = gtk_entry_new();
    labeled_row(actions_table, 1, "Clique direito:", g_wp_action_right);
    g_wp_action_middle = gtk_entry_new();
    labeled_row(actions_table, 2, "Clique do meio:", g_wp_action_middle);
    g_wp_action_double = gtk_entry_new();
    labeled_row(actions_table, 3, "Clique duplo:", g_wp_action_double);
    GtkWidget *actions_box = gtk_vbox_new(FALSE, 4);
    gtk_box_pack_start(GTK_BOX(actions_box), actions_table, FALSE, FALSE, 0);
    GtkWidget *actions_save_btn = gtk_button_new_with_label("Salvar acoes de clique");
    g_signal_connect(actions_save_btn, "clicked", G_CALLBACK(on_wp_actions_save), NULL);
    gtk_box_pack_start(GTK_BOX(actions_box), actions_save_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), frame_with("Acoes de clique", actions_box), FALSE, FALSE, 0);

    load_wp_actions();
    refresh_wp_layers();

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_add_with_viewport(GTK_SCROLLED_WINDOW(scroll), outer);
    return scroll;
}
