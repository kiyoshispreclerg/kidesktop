/* kiconf - Permissoes tab: xisguard control socket.
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

/* Permissoes tab widgets + baseline */
static GtkWidget *g_xg_status_label;
static GtkWidget *g_xg_no_pause_chk, *g_xg_quiet_chk, *g_xg_always_kill_chk, *g_xg_log_level_spin;
static GtkWidget *g_xg_rules_view;
static GtkListStore *g_xg_rules_store;
static GtkWidget *g_xg_action_combo, *g_xg_type_combo, *g_xg_pattern_entry;

typedef struct {
    int online;
    int no_pause, quiet, always_kill, log_level;
} XgStatus;
static XgStatus g_xg_baseline;

enum { COL_XG_TYPE = 0, COL_XG_ACTION, COL_XG_PATTERN, N_XG_COLS };

static const char *const XNOTIFY_ACTIONS[][2] = {
    {"ALL", "Todas as acoes"},
    {"ATTACH", "Usar memoria compartilhada"},
    {"SELECTION", "Acessar area de transferencia"},
    {"COMPOSITE", "Acessar outras janelas"},
    {"SCREEN", "Capturar e desenhar na tela"},
    {"RECORD", "Gravar eventos - como teclas"},
    {"CURSOR", "Acessar imagem/posicao do cursor"},
    {"INPUT_GRAB", "Capturar mouse ou teclado"},
    {"INPUT_INJECT", "Inserir eventos de teclado"},
    {"HOTKEY", "Registrar atalhos globais"},
    {"INPUT", "Capturar entrada mesmo sem foco"},
    {"MANAGE", "Listar/ler propriedades de outras janelas"},
    {"GRAB_OVERRIDE", "Permitir roubar um grab (telas de bloqueio)"},
    {"WARP", "Mover o cursor do mouse"},
    {"FOCUS", "Roubar o foco de entrada"},
    {"RANDR", "Mudar configuracao de tela"},
    {"OVERLAY", "Criar janela overlay (transparente)"},
    {NULL, NULL},
};

/* Ad hoc scanning for the small flat JSON objects these two control
 * sockets speak -- no library linked for it, see the file doc comment. */
static int json_get_bool(const char *json, const char *key, int deflt)
{
    char pat[80];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) {
        return deflt;
    }
    p += strlen(pat);
    while (*p == ' ' || *p == ':') {
        p++;
    }
    return strncmp(p, "true", 4) == 0;
}

static int json_get_int(const char *json, const char *key, int deflt)
{
    char pat[80];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) {
        return deflt;
    }
    p += strlen(pat);
    while (*p == ' ' || *p == ':') {
        p++;
    }
    return atoi(p);
}

static void json_get_str(const char *json, const char *key, char *out, size_t outsz)
{
    char pat[80];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    out[0] = '\0';
    const char *p = strstr(json, pat);
    if (!p) {
        return;
    }
    p += strlen(pat);
    while (*p == ' ' || *p == ':') {
        p++;
    }
    if (*p != '"') {
        return;
    }
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < outsz) {
        out[i++] = *p++;
    }
    out[i] = '\0';
}

static int json_ok(const char *json)
{
    return json_get_bool(json, "ok", 0);
}

/* Escapes '"' and '\\' for embedding `s` as a JSON string value -- the
 * only characters that appear in practice here (exe patterns, action
 * names) that would otherwise break the wire format. */
static void json_escape(const char *s, char *out, size_t outsz)
{
    size_t i = 0;
    for (; *s && i + 2 < outsz; s++) {
        if (*s == '"' || *s == '\\') {
            out[i++] = '\\';
        }
        out[i++] = *s;
    }
    out[i] = '\0';
}

/* ---- Permissoes tab: xisguard control socket -------------------------- */

static void xisguard_ctl_path(char *out, size_t outsz)
{
    const char *rundir = getenv("XDG_RUNTIME_DIR");
    if (!rundir || !*rundir) {
        rundir = "/tmp";
    }
    const char *disp = getenv("DISPLAY");
    int dispnum = 0;
    if (disp && disp[0] == ':') {
        dispnum = atoi(disp + 1);
    }
    snprintf(out, outsz, "%s/xisguard-ctl.%d.sock", rundir, dispnum);
}

static void xisguard_get_status(XgStatus *st)
{
    memset(st, 0, sizeof(*st));
    char path[PATH_MAX];
    xisguard_ctl_path(path, sizeof(path));
    char resp[JSON_BUF_LEN];
    if (json_line_send(path, "{\"cmd\":\"GET_STATUS\"}", resp, sizeof(resp)) && json_ok(resp)) {
        st->online = 1;
        st->no_pause = json_get_bool(resp, "no_pause", 0);
        st->quiet = json_get_bool(resp, "quiet", 0);
        st->always_kill = json_get_bool(resp, "always_kill", 0);
        st->log_level = json_get_int(resp, "log_level", 0);
    }
}

#define MAX_XG_RULES 256
typedef struct {
    char type[16], action[32], pattern[192];
} XgRule;

static int xisguard_list_rules(XgRule *rules, int max)
{
    char path[PATH_MAX];
    xisguard_ctl_path(path, sizeof(path));
    char resp[JSON_BUF_LEN];
    if (!json_line_send(path, "{\"cmd\":\"LIST_RULES\"}", resp, sizeof(resp)) || !json_ok(resp)) {
        return 0;
    }
    const char *arr = strstr(resp, "\"rules\"");
    if (!arr) {
        return 0;
    }
    arr = strchr(arr, '[');
    if (!arr) {
        return 0;
    }
    int n = 0;
    const char *p = arr + 1;
    while (n < max) {
        const char *obj_start = strchr(p, '{');
        const char *obj_end = obj_start ? strchr(obj_start, '}') : NULL;
        if (!obj_start || !obj_end) {
            break;
        }
        size_t len = (size_t)(obj_end - obj_start) + 1;
        char obj[512];
        if (len >= sizeof(obj)) {
            len = sizeof(obj) - 1;
        }
        memcpy(obj, obj_start, len);
        obj[len] = '\0';
        json_get_str(obj, "type", rules[n].type, sizeof(rules[n].type));
        json_get_str(obj, "action", rules[n].action, sizeof(rules[n].action));
        json_get_str(obj, "pattern", rules[n].pattern, sizeof(rules[n].pattern));
        n++;
        p = obj_end + 1;
        if (*p == ']' || !*p) {
            break;
        }
    }
    return n;
}

static int xisguard_send_rule_cmd(const char *cmd, const char *action, const char *pattern, const char *type)
{
    char eaction[64], epattern[256], etype[32];
    json_escape(action, eaction, sizeof(eaction));
    json_escape(pattern, epattern, sizeof(epattern));
    json_escape(type, etype, sizeof(etype));
    char req[512];
    snprintf(req, sizeof(req), "{\"cmd\":\"%s\",\"action\":\"%s\",\"pattern\":\"%s\",\"type\":\"%s\"}",
              cmd, eaction, epattern, etype);
    char path[PATH_MAX];
    xisguard_ctl_path(path, sizeof(path));
    char resp[JSON_BUF_LEN];
    return json_line_send(path, req, resp, sizeof(resp)) && json_ok(resp);
}

static int xisguard_reload(void)
{
    char path[PATH_MAX];
    xisguard_ctl_path(path, sizeof(path));
    char resp[JSON_BUF_LEN];
    return json_line_send(path, "{\"cmd\":\"RELOAD\"}", resp, sizeof(resp)) && json_ok(resp);
}

/* SET_STATUS accepts each field independently -- only the ones that
 * actually changed since the last GET_STATUS/apply are sent, same "diff
 * against baseline" rule as every other tab. */
static void apply_xg_status_diff(int no_pause, int quiet, int always_kill, int log_level)
{
    if (!g_xg_baseline.online) {
        return;
    }
    char req[256];
    int pos = snprintf(req, sizeof(req), "{\"cmd\":\"SET_STATUS\"");
    int changed = 0;
    if (no_pause != g_xg_baseline.no_pause) {
        pos += snprintf(req + pos, sizeof(req) - (size_t)pos, ",\"no_pause\":%d", no_pause);
        changed = 1;
    }
    if (quiet != g_xg_baseline.quiet) {
        pos += snprintf(req + pos, sizeof(req) - (size_t)pos, ",\"quiet\":%d", quiet);
        changed = 1;
    }
    if (always_kill != g_xg_baseline.always_kill) {
        pos += snprintf(req + pos, sizeof(req) - (size_t)pos, ",\"always_kill\":%d", always_kill);
        changed = 1;
    }
    if (log_level != g_xg_baseline.log_level) {
        pos += snprintf(req + pos, sizeof(req) - (size_t)pos, ",\"log_level\":%d", log_level);
        changed = 1;
    }
    snprintf(req + pos, sizeof(req) - (size_t)pos, "}");
    if (!changed) {
        return;
    }
    char path[PATH_MAX];
    xisguard_ctl_path(path, sizeof(path));
    char resp[JSON_BUF_LEN];
    json_line_send(path, req, resp, sizeof(resp));
    g_xg_baseline.no_pause = no_pause;
    g_xg_baseline.quiet = quiet;
    g_xg_baseline.always_kill = always_kill;
    g_xg_baseline.log_level = log_level;
}

static void refresh_xg_rules(void)
{
    gtk_list_store_clear(g_xg_rules_store);
    XgRule rules[MAX_XG_RULES];
    int n = g_xg_baseline.online ? xisguard_list_rules(rules, MAX_XG_RULES) : 0;
    for (int i = 0; i < n; i++) {
        GtkTreeIter it;
        gtk_list_store_append(g_xg_rules_store, &it);
        gtk_list_store_set(g_xg_rules_store, &it,
                            COL_XG_TYPE, rules[i].type, COL_XG_ACTION, rules[i].action,
                            COL_XG_PATTERN, rules[i].pattern, -1);
    }
}

static void apply_xg_status_cb(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    int no_pause = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_xg_no_pause_chk));
    int quiet = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_xg_quiet_chk));
    int always_kill = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_xg_always_kill_chk));
    int log_level = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_xg_log_level_spin));
    apply_xg_status_diff(no_pause, quiet, always_kill, log_level);
}

static void on_xg_add_rule(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    if (!g_xg_baseline.online) {
        return;
    }
    gchar *action_disp = gtk_combo_box_get_active_text(GTK_COMBO_BOX(g_xg_action_combo));
    gchar *type = gtk_combo_box_get_active_text(GTK_COMBO_BOX(g_xg_type_combo));
    const gchar *pattern = gtk_entry_get_text(GTK_ENTRY(g_xg_pattern_entry));
    if (!action_disp || !type || !pattern[0]) {
        g_free(action_disp);
        g_free(type);
        return;
    }
    /* action_disp is "NAME -- description"; only NAME goes over the wire */
    char action[32];
    char *sep = strstr(action_disp, " -- ");
    size_t len = sep ? (size_t)(sep - action_disp) : strlen(action_disp);
    if (len >= sizeof(action)) {
        len = sizeof(action) - 1;
    }
    memcpy(action, action_disp, len);
    action[len] = '\0';

    if (xisguard_send_rule_cmd("ADD_RULE", action, pattern, type)) {
        refresh_xg_rules();
        gtk_entry_set_text(GTK_ENTRY(g_xg_pattern_entry), "");
    }
    g_free(action_disp);
    g_free(type);
}

static void on_xg_remove_rule(GtkWidget *widget, gpointer data)
{
    (void)widget;
    GtkTreeView *view = GTK_TREE_VIEW(data);
    GtkTreeSelection *sel = gtk_tree_view_get_selection(view);
    GtkTreeIter it;
    if (!gtk_tree_selection_get_selected(sel, NULL, &it)) {
        return;
    }
    gchar *type, *action, *pattern;
    gtk_tree_model_get(GTK_TREE_MODEL(g_xg_rules_store), &it,
                        COL_XG_TYPE, &type, COL_XG_ACTION, &action, COL_XG_PATTERN, &pattern, -1);
    if (xisguard_send_rule_cmd("REMOVE_RULE", action, pattern, type)) {
        refresh_xg_rules();
    }
    g_free(type);
    g_free(action);
    g_free(pattern);
}

static void on_xg_reload(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    xisguard_reload();
    refresh_xg_rules();
}

GtkWidget *build_permissoes_tab(void)
{
    xisguard_get_status(&g_xg_baseline);

    GtkWidget *outer = gtk_vbox_new(FALSE, 8);
    gtk_container_set_border_width(GTK_CONTAINER(outer), 12);

    char status_text[128];
    if (g_xg_baseline.online) {
        snprintf(status_text, sizeof(status_text), "xisguard conectado.");
    } else {
        snprintf(status_text, sizeof(status_text),
                  "xisguard inacessivel (daemon parado, ou socket de controle ainda nao existe).");
    }
    g_xg_status_label = gtk_label_new(status_text);
    gtk_misc_set_alignment(GTK_MISC(g_xg_status_label), 0.0, 0.5);
    gtk_box_pack_start(GTK_BOX(outer), g_xg_status_label, FALSE, FALSE, 0);

    GtkWidget *status_table = gtk_table_new(4, 2, FALSE);
    g_xg_no_pause_chk = gtk_check_button_new_with_label("no_pause (nao pausar decisao)");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_xg_no_pause_chk), g_xg_baseline.no_pause);
    gtk_table_attach(GTK_TABLE(status_table), g_xg_no_pause_chk, 0, 2, 0, 1, GTK_FILL, GTK_FILL, 4, 2);
    g_xg_quiet_chk = gtk_check_button_new_with_label("quiet (sem notificacoes)");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_xg_quiet_chk), g_xg_baseline.quiet);
    gtk_table_attach(GTK_TABLE(status_table), g_xg_quiet_chk, 0, 2, 1, 2, GTK_FILL, GTK_FILL, 4, 2);
    g_xg_always_kill_chk = gtk_check_button_new_with_label("always_kill (sempre matar em DENY)");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_xg_always_kill_chk), g_xg_baseline.always_kill);
    gtk_table_attach(GTK_TABLE(status_table), g_xg_always_kill_chk, 0, 2, 2, 3, GTK_FILL, GTK_FILL, 4, 2);
    g_xg_log_level_spin = gtk_spin_button_new_with_range(0, 5, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_xg_log_level_spin), g_xg_baseline.log_level);
    labeled_row(status_table, 3, "log_level:", g_xg_log_level_spin);
    gtk_widget_set_sensitive(status_table, g_xg_baseline.online);
    gtk_box_pack_start(GTK_BOX(outer), frame_with("Modo de execucao", status_table), FALSE, FALSE, 0);

    GtkWidget *status_apply_btn = gtk_button_new_with_label("Aplicar modo");
    g_signal_connect(status_apply_btn, "clicked", G_CALLBACK(apply_xg_status_cb), NULL);
    gtk_widget_set_sensitive(status_apply_btn, g_xg_baseline.online);
    GtkWidget *status_btnbox = gtk_hbox_new(FALSE, 0);
    gtk_box_pack_end(GTK_BOX(status_btnbox), status_apply_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), status_btnbox, FALSE, FALSE, 0);

    g_xg_rules_store = gtk_list_store_new(N_XG_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    g_xg_rules_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(g_xg_rules_store));
    const char *xg_titles[N_XG_COLS] = {"Tipo", "Acao", "Padrao"};
    for (int col = 0; col < N_XG_COLS; col++) {
        GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
        GtkTreeViewColumn *tvcol = gtk_tree_view_column_new_with_attributes(xg_titles[col], renderer, "text", col, NULL);
        gtk_tree_view_column_set_expand(tvcol, TRUE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(g_xg_rules_view), tvcol);
    }
    GtkWidget *rules_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(rules_scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(rules_scroll, -1, 160);
    gtk_container_add(GTK_CONTAINER(rules_scroll), g_xg_rules_view);

    GtkWidget *rules_box = gtk_vbox_new(FALSE, 4);
    gtk_box_pack_start(GTK_BOX(rules_box), rules_scroll, TRUE, TRUE, 0);

    GtkWidget *add_row = gtk_hbox_new(FALSE, 4);
    g_xg_action_combo = gtk_combo_box_new_text();
    for (int i = 0; XNOTIFY_ACTIONS[i][0]; i++) {
        char label[96];
        snprintf(label, sizeof(label), "%s -- %s", XNOTIFY_ACTIONS[i][0], XNOTIFY_ACTIONS[i][1]);
        gtk_combo_box_append_text(GTK_COMBO_BOX(g_xg_action_combo), label);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(g_xg_action_combo), 0);
    gtk_box_pack_start(GTK_BOX(add_row), g_xg_action_combo, TRUE, TRUE, 0);
    g_xg_type_combo = gtk_combo_box_new_text();
    gtk_combo_box_append_text(GTK_COMBO_BOX(g_xg_type_combo), "ALLOW");
    gtk_combo_box_append_text(GTK_COMBO_BOX(g_xg_type_combo), "DENY");
    gtk_combo_box_set_active(GTK_COMBO_BOX(g_xg_type_combo), 0);
    gtk_box_pack_start(GTK_BOX(add_row), g_xg_type_combo, FALSE, FALSE, 0);
    g_xg_pattern_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(g_xg_pattern_entry), "*");
    gtk_box_pack_start(GTK_BOX(add_row), g_xg_pattern_entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(rules_box), add_row, FALSE, FALSE, 0);

    GtkWidget *rules_btnbox = gtk_hbox_new(FALSE, 6);
    GtkWidget *add_btn = gtk_button_new_with_label("Adicionar regra");
    GtkWidget *remove_btn = gtk_button_new_with_label("Remover selecionada");
    GtkWidget *reload_btn = gtk_button_new_with_label("Recarregar (RELOAD)");
    g_signal_connect(add_btn, "clicked", G_CALLBACK(on_xg_add_rule), NULL);
    g_signal_connect(remove_btn, "clicked", G_CALLBACK(on_xg_remove_rule), g_xg_rules_view);
    g_signal_connect(reload_btn, "clicked", G_CALLBACK(on_xg_reload), NULL);
    gtk_box_pack_start(GTK_BOX(rules_btnbox), add_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(rules_btnbox), remove_btn, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(rules_btnbox), reload_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(rules_box), rules_btnbox, FALSE, FALSE, 0);

    gtk_widget_set_sensitive(rules_box, g_xg_baseline.online);
    gtk_box_pack_start(GTK_BOX(outer), frame_with("Regras XNOTIFY", rules_box), TRUE, TRUE, 0);

    refresh_xg_rules();
    return outer;
}
