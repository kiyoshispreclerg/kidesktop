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
static GtkWidget *g_xg_secure_mode_chk;
static GtkWidget *g_xg_rules_view;
static GtkListStore *g_xg_rules_store;
static GtkWidget *g_xg_action_combo, *g_xg_type_combo, *g_xg_pattern_entry;
/* System rules (SYSCONFDIR's xnotify.conf.d directory, *.conf files) --
 * read-only for now,
 * see xisguard_list_system_rules()'s own doc comment. */
static GtkWidget *g_xg_sys_rules_view;
static GtkListStore *g_xg_sys_rules_store;
static GtkWidget *g_xg_sys_rules_status_label;

typedef struct {
    int online;
    int no_pause, quiet, always_kill, log_level, secure_mode;
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
    /* no_pause/quiet/always_kill/secure_mode come back as JSON integers
     * (0/1, see xisguard.c's GET_STATUS handler), not the literal
     * true/false json_get_bool() looks for -- using that here always
     * read back 0 regardless of the daemon's real state, which is why
     * every checkbox opened unchecked no matter what xisguard was
     * actually running with. */
    if (json_line_send(path, "{\"cmd\":\"GET_STATUS\"}", resp, sizeof(resp)) && json_ok(resp)) {
        st->online = 1;
        st->no_pause = json_get_int(resp, "no_pause", 0) != 0;
        st->quiet = json_get_int(resp, "quiet", 0) != 0;
        st->always_kill = json_get_int(resp, "always_kill", 0) != 0;
        st->log_level = json_get_int(resp, "log_level", 0);
        st->secure_mode = json_get_int(resp, "secure_mode", 0) != 0;
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

/* Asks xisguard where the X server's system-wide rules live (see
 * XISGUARD.md's GET_SYSTEM_RULES_PATH) -- SYSCONFDIR is a compile-time
 * constant of the server, not of kiconf, so this can't just be guessed;
 * xisguard itself learns it the same way (wait_for_secure_conf_dir() in
 * xisguard.c, used to write secure-mode rules via polkit). Returns 1 and
 * fills `out` on success. */
static int xisguard_get_system_rules_dir(char *out, size_t outsz)
{
    char path[PATH_MAX];
    xisguard_ctl_path(path, sizeof(path));
    char resp[JSON_BUF_LEN];
    if (!json_line_send(path, "{\"cmd\":\"GET_SYSTEM_RULES_PATH\"}", resp, sizeof(resp)) || !json_ok(resp)) {
        return 0;
    }
    json_get_str(resp, "dir", out, outsz);
    return out[0] != '\0';
}

static int rule_conf_name_filter(const struct dirent *e)
{
    size_t n = strlen(e->d_name);
    return n > 5 && !strcmp(e->d_name + n - 5, ".conf");
}

/* Reads every *.conf file in the system rules directory (in the same
 * numeric-prefix order the X server itself applies them, e.g.
 * "10-foo.conf" before "50-xisguard.conf" -- see secure_save_rule() in
 * xisguard.c for the one file kiconf's own polkit-gated rules end up in),
 * parsing each non-comment, non-empty line as "TYPE ACTION PATTERN" --
 * the exact same 3-whitespace-token grammar as perms.conf/LIST_RULES
 * (see load_user_config() in xisguard.c). Read-only for now: writing here
 * needs a polkit prompt per file, same as secure_save_rule() -- see
 * kiconf.c's own doc comment on this tab for what's deliberately not
 * ported yet. */
static int xisguard_list_system_rules(XgRule *rules, int max)
{
    char dir[512];
    if (!xisguard_get_system_rules_dir(dir, sizeof(dir))) {
        return 0;
    }
    struct dirent **names;
    int n_files = scandir(dir, &names, rule_conf_name_filter, alphasort);
    if (n_files < 0) {
        return 0;
    }
    int n = 0;
    for (int fi = 0; fi < n_files && n < max; fi++) {
        char filepath[PATH_MAX];
        snprintf(filepath, sizeof(filepath), "%s/%s", dir, names[fi]->d_name);
        FILE *f = fopen(filepath, "r");
        if (f) {
            char line[512];
            while (n < max && fgets(line, sizeof(line), f)) {
                line[strcspn(line, "\r\n")] = '\0';
                char *save = NULL;
                char *tok1 = strtok_r(line, " \t", &save);
                if (!tok1 || tok1[0] == '#') {
                    continue;
                }
                char *tok2 = strtok_r(NULL, " \t", &save);
                char *tok3 = strtok_r(NULL, " \t", &save);
                if (!tok2 || !tok3) {
                    continue;
                }
                snprintf(rules[n].type, sizeof(rules[n].type), "%s", tok1);
                snprintf(rules[n].action, sizeof(rules[n].action), "%s", tok2);
                snprintf(rules[n].pattern, sizeof(rules[n].pattern), "%s", tok3);
                n++;
            }
            fclose(f);
        }
        free(names[fi]);
    }
    free(names);
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
static void refresh_xg_rules(void);

static void apply_xg_status_diff(int no_pause, int quiet, int always_kill, int log_level, int secure_mode)
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
    if (secure_mode != g_xg_baseline.secure_mode) {
        pos += snprintf(req + pos, sizeof(req) - (size_t)pos, ",\"secure_mode\":%d", secure_mode);
        changed = 1;
    }
    snprintf(req + pos, sizeof(req) - (size_t)pos, "}");
    if (!changed) {
        return;
    }
    int secure_mode_changed = secure_mode != g_xg_baseline.secure_mode;
    char path[PATH_MAX];
    xisguard_ctl_path(path, sizeof(path));
    char resp[JSON_BUF_LEN];
    json_line_send(path, req, resp, sizeof(resp));
    g_xg_baseline.no_pause = no_pause;
    g_xg_baseline.quiet = quiet;
    g_xg_baseline.always_kill = always_kill;
    g_xg_baseline.log_level = log_level;
    g_xg_baseline.secure_mode = secure_mode;
    /* Secure mode toggling changes which set of user rules is actually
     * live (see xisguard.c's SET_STATUS handler) -- refresh so the user
     * rules list reflects that instead of showing stale entries. */
    if (secure_mode_changed) {
        refresh_xg_rules();
    }
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

static void refresh_xg_sys_rules(void)
{
    gtk_list_store_clear(g_xg_sys_rules_store);
    XgRule rules[MAX_XG_RULES];
    char dir[512] = "";
    int have_dir = g_xg_baseline.online && xisguard_get_system_rules_dir(dir, sizeof(dir));
    int n = have_dir ? xisguard_list_system_rules(rules, MAX_XG_RULES) : 0;
    for (int i = 0; i < n; i++) {
        GtkTreeIter it;
        gtk_list_store_append(g_xg_sys_rules_store, &it);
        gtk_list_store_set(g_xg_sys_rules_store, &it,
                            COL_XG_TYPE, rules[i].type, COL_XG_ACTION, rules[i].action,
                            COL_XG_PATTERN, rules[i].pattern, -1);
    }
    if (g_xg_sys_rules_status_label) {
        char status[600];
        if (!g_xg_baseline.online) {
            snprintf(status, sizeof(status), "xisguard inacessivel.");
        } else if (!have_dir) {
            snprintf(status, sizeof(status), "Nao foi possivel descobrir o diretorio de regras de sistema (timeout do X server).");
        } else {
            snprintf(status, sizeof(status), "%s -- %d regra(s). Somente leitura por enquanto.", dir, n);
        }
        gtk_label_set_text(GTK_LABEL(g_xg_sys_rules_status_label), status);
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
    int secure_mode = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_xg_secure_mode_chk));
    apply_xg_status_diff(no_pause, quiet, always_kill, log_level, secure_mode);
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
    refresh_xg_sys_rules();
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

    GtkWidget *status_table = gtk_table_new(5, 2, FALSE);
    g_xg_no_pause_chk = gtk_check_button_new_with_label("no_pause (nao pausar decisao)");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_xg_no_pause_chk), g_xg_baseline.no_pause);
    gtk_table_attach(GTK_TABLE(status_table), g_xg_no_pause_chk, 0, 2, 0, 1, GTK_FILL, GTK_FILL, 4, 2);
    g_xg_quiet_chk = gtk_check_button_new_with_label("quiet (sem notificacoes)");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_xg_quiet_chk), g_xg_baseline.quiet);
    gtk_table_attach(GTK_TABLE(status_table), g_xg_quiet_chk, 0, 2, 1, 2, GTK_FILL, GTK_FILL, 4, 2);
    g_xg_always_kill_chk = gtk_check_button_new_with_label("always_kill (sempre matar em DENY)");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_xg_always_kill_chk), g_xg_baseline.always_kill);
    gtk_table_attach(GTK_TABLE(status_table), g_xg_always_kill_chk, 0, 2, 2, 3, GTK_FILL, GTK_FILL, 4, 2);
    g_xg_secure_mode_chk = gtk_check_button_new_with_label(
        "secure_mode (regras de usuario desligadas; novas regras permanentes pedem senha root)");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_xg_secure_mode_chk), g_xg_baseline.secure_mode);
    gtk_table_attach(GTK_TABLE(status_table), g_xg_secure_mode_chk, 0, 2, 3, 4, GTK_FILL, GTK_FILL, 4, 2);
    g_xg_log_level_spin = gtk_spin_button_new_with_range(0, 5, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_xg_log_level_spin), g_xg_baseline.log_level);
    labeled_row(status_table, 4, "log_level:", g_xg_log_level_spin);
    gtk_widget_set_sensitive(status_table, g_xg_baseline.online);
    gtk_box_pack_start(GTK_BOX(outer), frame_with("Modo de execucao", status_table), FALSE, FALSE, 0);

    GtkWidget *status_apply_btn = gtk_button_new_with_label("Aplicar modo");
    g_signal_connect(status_apply_btn, "clicked", G_CALLBACK(apply_xg_status_cb), NULL);
    gtk_widget_set_sensitive(status_apply_btn, g_xg_baseline.online);
    GtkWidget *status_btnbox = gtk_hbox_new(FALSE, 0);
    gtk_box_pack_end(GTK_BOX(status_btnbox), status_apply_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), status_btnbox, FALSE, FALSE, 0);

    GtkWidget *rules_notebook = gtk_notebook_new();

    /* ---- "Regras de usuario" page: perms.conf, via LIST_RULES/ADD_RULE/
     * REMOVE_RULE -- everything the tab already did before the system
     * rules page below existed. */
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
    gtk_container_set_border_width(GTK_CONTAINER(rules_box), 6);
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
    gtk_notebook_append_page(GTK_NOTEBOOK(rules_notebook), rules_box, gtk_label_new("Regras de usuario"));

    /* ---- "Regras de sistema" page: SYSCONFDIR's xnotify.conf.d directory
     * (*.conf files), read directly by kiconf once xisguard tells it where that is (see
     * xisguard_get_system_rules_dir()) -- read-only for now, a future
     * pass can make double-click editable the same way the user rules
     * list could, gated behind a polkit prompt to save (same mechanism
     * secure_save_rule() in xisguard.c already uses). */
    g_xg_sys_rules_store = gtk_list_store_new(N_XG_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    g_xg_sys_rules_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(g_xg_sys_rules_store));
    for (int col = 0; col < N_XG_COLS; col++) {
        GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
        GtkTreeViewColumn *tvcol = gtk_tree_view_column_new_with_attributes(xg_titles[col], renderer, "text", col, NULL);
        gtk_tree_view_column_set_expand(tvcol, TRUE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(g_xg_sys_rules_view), tvcol);
    }
    GtkWidget *sys_rules_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sys_rules_scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(sys_rules_scroll, -1, 160);
    gtk_container_add(GTK_CONTAINER(sys_rules_scroll), g_xg_sys_rules_view);

    GtkWidget *sys_rules_box = gtk_vbox_new(FALSE, 4);
    gtk_container_set_border_width(GTK_CONTAINER(sys_rules_box), 6);
    gtk_box_pack_start(GTK_BOX(sys_rules_box), sys_rules_scroll, TRUE, TRUE, 0);

    g_xg_sys_rules_status_label = gtk_label_new("-");
    gtk_misc_set_alignment(GTK_MISC(g_xg_sys_rules_status_label), 0.0, 0.5);
    gtk_label_set_line_wrap(GTK_LABEL(g_xg_sys_rules_status_label), TRUE);
    gtk_box_pack_start(GTK_BOX(sys_rules_box), g_xg_sys_rules_status_label, FALSE, FALSE, 0);

    GtkWidget *sys_reload_btn = gtk_button_new_with_label("Recarregar");
    g_signal_connect(sys_reload_btn, "clicked", G_CALLBACK(on_xg_reload), NULL);
    GtkWidget *sys_rules_btnbox = gtk_hbox_new(FALSE, 0);
    gtk_box_pack_end(GTK_BOX(sys_rules_btnbox), sys_reload_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(sys_rules_box), sys_rules_btnbox, FALSE, FALSE, 0);

    gtk_widget_set_sensitive(sys_rules_box, g_xg_baseline.online);
    gtk_notebook_append_page(GTK_NOTEBOOK(rules_notebook), sys_rules_box, gtk_label_new("Regras de sistema"));

    gtk_box_pack_start(GTK_BOX(outer), frame_with("Regras XNOTIFY", rules_notebook), TRUE, TRUE, 0);

    refresh_xg_rules();
    refresh_xg_sys_rules();
    return outer;
}
