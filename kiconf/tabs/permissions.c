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
static GtkWidget *g_xg_secure_mode_chk, *g_xg_permissive_chk;
/* "Avisos": one check per action (XNOTIFY_ACTIONS[1..]) */
static GtkWidget *g_xg_notify_chk[32];
static int g_xg_n_notify_chk;
static GtkWidget *g_xg_rules_view;
static GtkListStore *g_xg_rules_store;
static GtkWidget *g_xg_action_combo, *g_xg_type_combo, *g_xg_pattern_entry;
/* System rules (SYSCONFDIR's xnotify.conf.d directory, *.conf files) --
 * read-only for now,
 * see xisguard_list_system_rules()'s own doc comment. */
static GtkWidget *g_xg_sys_rules_view;
static GtkListStore *g_xg_sys_rules_store;
static GtkWidget *g_xg_sys_rules_status_label;
/* ignore.conf: programs whose REPORTs are not shown (LIST/ADD/REMOVE_IGNORED) */
static GtkWidget *g_xg_ignored_view, *g_xg_ignored_entry;
static GtkListStore *g_xg_ignored_store;

typedef struct {
    int online;
    int no_pause, quiet, always_kill, log_level, secure_mode;
    int permissive;
    char notify[512]; /* comma list of the actions to tell the user about ("" = none) */
} XgStatus;
static XgStatus g_xg_baseline;

enum { COL_XG_TYPE = 0, COL_XG_ACTION, COL_XG_PATTERN, N_XG_COLS };
/* the system rules list has one more column: which file the rule is from */
enum { COL_XG_SOURCE = N_XG_COLS, N_XG_SYS_COLS };

/* XNOTIFY_ACTIONS[i][0] is the protocol token (goes into perms.conf
 * verbatim, never translated); [i][1] is only ever read into a label
 * below, so it's N_()-marked here for extraction and _()-looked-up at
 * the read site instead, same pattern as shortcuts.c's FixedShortcut. */
static const char *const XNOTIFY_ACTIONS[][2] = {
    {"ALL", N_("Todas as acoes")},
    {"ATTACH", N_("Usar memoria compartilhada")},
    {"SELECTION", N_("Acessar area de transferencia")},
    {"COMPOSITE", N_("Acessar outras janelas")},
    {"SCREEN", N_("Capturar e desenhar na tela")},
    {"RECORD", N_("Gravar eventos - como teclas")},
    {"CURSOR", N_("Acessar imagem/posicao do cursor")},
    {"INPUT_GRAB", N_("Capturar mouse ou teclado")},
    {"INPUT_INJECT", N_("Inserir eventos de teclado")},
    {"HOTKEY", N_("Registrar atalhos globais")},
    {"INPUT", N_("Capturar entrada mesmo sem foco")},
    {"MANAGE", N_("Listar/ler propriedades de outras janelas")},
    {"GRAB_OVERRIDE", N_("Permitir roubar um grab (telas de bloqueio)")},
    {"WARP", N_("Mover o cursor do mouse")},
    {"FOCUS", N_("Roubar o foco de entrada")},
    {"RANDR", N_("Mudar configuracao de tela")},
    {"OVERLAY", N_("Criar janela overlay (transparente)")},
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
        st->permissive = json_get_int(resp, "permissive", 0) != 0;
        json_get_str(resp, "notify_actions", st->notify, sizeof(st->notify));
    }
}

#define MAX_XG_RULES 256
typedef struct {
    char type[16], action[32], pattern[192], source[64];
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
 * xisguard.c, used to write secure-mode rules via polkit). The server reads
 * two things: the xnotify.conf.d directory (*.conf) and the single
 * xnotify.conf next to it. Returns 1 and fills `dir` (and `file`, which is
 * derived from `dir` if an older xisguard does not report it). */
static int xisguard_get_system_rules_paths(char *dir, size_t dirsz, char *file, size_t filesz)
{
    char path[PATH_MAX];
    xisguard_ctl_path(path, sizeof(path));
    char resp[JSON_BUF_LEN];
    if (!json_line_send(path, "{\"cmd\":\"GET_SYSTEM_RULES_PATH\"}", resp, sizeof(resp)) || !json_ok(resp)) {
        return 0;
    }
    json_get_str(resp, "dir", dir, dirsz);
    if (file) {
        json_get_str(resp, "file", file, filesz);
        size_t n = strlen(dir);
        if (!file[0] && n > 2 && !strcmp(dir + n - 2, ".d")) {
            snprintf(file, filesz, "%.*s", (int)(n - 2), dir);
        }
    }
    return dir[0] != '\0';
}

static int xisguard_get_system_rules_dir(char *out, size_t outsz)
{
    return xisguard_get_system_rules_paths(out, outsz, NULL, 0);
}

static int rule_conf_name_filter(const struct dirent *e)
{
    size_t n = strlen(e->d_name);
    return n > 5 && !strcmp(e->d_name + n - 5, ".conf");
}

/* One line of a server rule file, in the grammar the X server itself reads
 * (XnotifyLoadFile() in xnotify.c):
 *   ALLOW|DENY ACTION /path[|args]   rule for one program and action
 *   ALLOW|DENY /path[|args]          every action ("ALL")
 *   ALLOW|DENY ACTION                the action for everyone ("*")
 *   ALLOW_ACTION ACTION              same as "ALLOW ACTION"
 * Returns 1 and fills `r` (except source) for a rule, 0 for anything else. */
static int parse_system_rule_line(char *line, XgRule *r)
{
    line[strcspn(line, "\r\n")] = '\0';
    char *save = NULL;
    char *cmd = strtok_r(line, " \t", &save);
    if (!cmd || cmd[0] == '#') {
        return 0;
    }
    int allow_action = !strcasecmp(cmd, "ALLOW_ACTION");
    if (!allow_action && strcasecmp(cmd, "ALLOW") && strcasecmp(cmd, "DENY")) {
        return 0;
    }
    snprintf(r->type, sizeof(r->type), "%s", allow_action ? "ALLOW" : (toupper((unsigned char)cmd[0]) == 'A' ? "ALLOW" : "DENY"));
    char *tok = strtok_r(NULL, " \t", &save);
    if (!tok) {
        return 0;
    }
    if (tok[0] == '/') { /* no action: every one */
        snprintf(r->action, sizeof(r->action), "ALL");
        snprintf(r->pattern, sizeof(r->pattern), "%s", tok);
        return 1;
    }
    snprintf(r->action, sizeof(r->action), "%s", tok);
    /* the pattern runs to the end of the line and may contain spaces */
    char *rest = save ? save : (char *)"";
    while (*rest == ' ' || *rest == '\t') {
        rest++;
    }
    size_t rl = strlen(rest);
    while (rl > 0 && (rest[rl - 1] == ' ' || rest[rl - 1] == '\t')) {
        rest[--rl] = '\0';
    }
    snprintf(r->pattern, sizeof(r->pattern), "%s", rl ? rest : "*");
    return 1;
}

static int read_system_rule_file(const char *filepath, const char *source, XgRule *rules, int n, int max)
{
    FILE *f = fopen(filepath, "r");
    if (!f) {
        return n;
    }
    char line[512];
    while (n < max && fgets(line, sizeof(line), f)) {
        if (parse_system_rule_line(line, &rules[n])) {
            snprintf(rules[n].source, sizeof(rules[n].source), "%s", source);
            n++;
        }
    }
    fclose(f);
    return n;
}

/* Reads the system rules the way the X server loads them: every *.conf in
 * the xnotify.conf.d directory in name order ("10-foo.conf" before
 * "50-xisguard.conf" -- see secure_save_rule() in xisguard.c for the one
 * file kiconf's own polkit-gated rules end up in), then the xnotify.conf
 * file itself. Read-only for now: writing here needs a polkit prompt per
 * file, same as secure_save_rule(). */
static int xisguard_list_system_rules(XgRule *rules, int max)
{
    char dir[512], file[512] = "";
    if (!xisguard_get_system_rules_paths(dir, sizeof(dir), file, sizeof(file))) {
        return 0;
    }
    int n = 0;
    struct dirent **names;
    int n_files = scandir(dir, &names, rule_conf_name_filter, alphasort);
    for (int fi = 0; fi < n_files; fi++) {
        char filepath[PATH_MAX];
        snprintf(filepath, sizeof(filepath), "%s/%s", dir, names[fi]->d_name);
        n = read_system_rule_file(filepath, names[fi]->d_name, rules, n, max);
        free(names[fi]);
    }
    if (n_files >= 0) {
        free(names);
    }
    if (file[0]) {
        const char *base = strrchr(file, '/');
        n = read_system_rule_file(file, base ? base + 1 : file, rules, n, max);
    }
    return n;
}

/* ---- ignore.conf (programs whose REPORTs are hidden) ---- */

static int xisguard_list_ignored(char (*out)[192], int max)
{
    char path[PATH_MAX];
    xisguard_ctl_path(path, sizeof(path));
    char resp[JSON_BUF_LEN];
    if (!json_line_send(path, "{\"cmd\":\"LIST_IGNORED\"}", resp, sizeof(resp)) || !json_ok(resp)) {
        return 0;
    }
    const char *p = strstr(resp, "\"ignored\"");
    p = p ? strchr(p, '[') : NULL;
    int n = 0;
    while (p && n < max) {
        p = strchr(p, '"');
        if (!p) {
            break;
        }
        p++;
        size_t i = 0;
        while (*p && *p != '"' && i + 1 < sizeof(out[0])) {
            if (*p == '\\' && p[1]) {
                p++;
            }
            out[n][i++] = *p++;
        }
        out[n][i] = '\0';
        while (*p && *p != '"') { /* skip the rest of an over-long entry */
            if (*p == '\\' && p[1]) {
                p++;
            }
            p++;
        }
        if (*p == '"') {
            p++;
        }
        n++;
    }
    return n;
}

/* The KiDesktop programs xisguard always ignores (LIST_IGNORED "builtin"). */
static void xisguard_builtin_ignored(char *out, size_t outsz)
{
    out[0] = '\0';
    char path[PATH_MAX];
    xisguard_ctl_path(path, sizeof(path));
    char resp[JSON_BUF_LEN];
    if (!json_line_send(path, "{\"cmd\":\"LIST_IGNORED\"}", resp, sizeof(resp)) || !json_ok(resp)) {
        return;
    }
    const char *p = strstr(resp, "\"builtin\"");
    p = p ? strchr(p, '[') : NULL;
    size_t o = 0;
    while (p && (p = strchr(p, '"')) && o + 1 < outsz) {
        p++;
        if (o) {
            o += (size_t)snprintf(out + o, outsz - o, ", ");
        }
        while (*p && *p != '"' && o + 1 < outsz) {
            out[o++] = *p++;
        }
        out[o] = '\0';
        if (*p == '"') {
            p++;
        }
    }
}

static int xisguard_send_ignored_cmd(const char *cmd, const char *pattern)
{
    char path[PATH_MAX];
    xisguard_ctl_path(path, sizeof(path));
    char esc[512];
    json_escape(pattern, esc, sizeof(esc));
    char req[640], resp[JSON_BUF_LEN];
    snprintf(req, sizeof(req), "{\"cmd\":\"%s\",\"pattern\":\"%s\"}", cmd, esc);
    return json_line_send(path, req, resp, sizeof(resp)) && json_ok(resp);
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

static void apply_xg_status_diff(int no_pause, int quiet, int always_kill, int log_level, int secure_mode,
                                 const char *notify)
{
    if (!g_xg_baseline.online) {
        return;
    }
    char req[768];
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
    if (strcmp(notify, g_xg_baseline.notify) != 0) {
        pos += snprintf(req + pos, sizeof(req) - (size_t)pos, ",\"notify_actions\":\"%s\"", notify[0] ? notify : "NONE");
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
    snprintf(g_xg_baseline.notify, sizeof(g_xg_baseline.notify), "%s", notify);
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
                            COL_XG_PATTERN, rules[i].pattern, COL_XG_SOURCE, rules[i].source, -1);
    }
    if (g_xg_sys_rules_status_label) {
        char status[600];
        if (!g_xg_baseline.online) {
            snprintf(status, sizeof(status), _("xisguard inacessivel."));
        } else if (!have_dir) {
            snprintf(status, sizeof(status), _("Nao foi possivel descobrir o diretorio de regras de sistema (timeout do X server)."));
        } else {
            snprintf(status, sizeof(status), _("%s (e xnotify.conf) -- %d regra(s). Somente leitura por enquanto."), dir, n);
        }
        gtk_label_set_text(GTK_LABEL(g_xg_sys_rules_status_label), status);
    }
}

static void refresh_xg_ignored(void)
{
    gtk_list_store_clear(g_xg_ignored_store);
    static char items[MAX_XG_RULES][192];
    int n = g_xg_baseline.online ? xisguard_list_ignored(items, MAX_XG_RULES) : 0;
    for (int i = 0; i < n; i++) {
        GtkTreeIter it;
        gtk_list_store_append(g_xg_ignored_store, &it);
        gtk_list_store_set(g_xg_ignored_store, &it, 0, items[i], -1);
    }
}

static void on_xg_add_ignored(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    const gchar *pattern = gtk_entry_get_text(GTK_ENTRY(g_xg_ignored_entry));
    if (!g_xg_baseline.online || !pattern[0]) {
        return;
    }
    if (xisguard_send_ignored_cmd("ADD_IGNORED", pattern)) {
        gtk_entry_set_text(GTK_ENTRY(g_xg_ignored_entry), "");
        refresh_xg_ignored();
    }
}

static void on_xg_remove_ignored(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    GtkTreeIter it;
    if (!gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(g_xg_ignored_view)), NULL, &it)) {
        return;
    }
    gchar *pattern;
    gtk_tree_model_get(GTK_TREE_MODEL(g_xg_ignored_store), &it, 0, &pattern, -1);
    if (xisguard_send_ignored_cmd("REMOVE_IGNORED", pattern)) {
        refresh_xg_ignored();
    }
    g_free(pattern);
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
    char notify[512] = "";
    for (int i = 0; i < g_xg_n_notify_chk; i++) {
        if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_xg_notify_chk[i]))) {
            size_t o = strlen(notify);
            snprintf(notify + o, sizeof(notify) - o, "%s%s", o ? "," : "", XNOTIFY_ACTIONS[i + 1][0]);
        }
    }
    apply_xg_status_diff(no_pause, quiet, always_kill, log_level, secure_mode, notify);

    int permissive = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_xg_permissive_chk));
    if (g_xg_baseline.online && permissive != g_xg_baseline.permissive) {
        char path[PATH_MAX];
        xisguard_ctl_path(path, sizeof(path));
        char resp[JSON_BUF_LEN];
        json_line_send(path, permissive ? "{\"cmd\":\"SET_PERMISSIVE\",\"enabled\":1,\"persist\":1}"
                                        : "{\"cmd\":\"SET_PERMISSIVE\",\"enabled\":0,\"persist\":1}",
                       resp, sizeof(resp));
        g_xg_baseline.permissive = permissive;
    }
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
    refresh_xg_ignored();
}

GtkWidget *build_permissions_tab(void)
{
    xisguard_get_status(&g_xg_baseline);

    GtkWidget *outer = gtk_vbox_new(FALSE, 4);
    gtk_container_set_border_width(GTK_CONTAINER(outer), 8);

    char status_text[128];
    if (g_xg_baseline.online) {
        snprintf(status_text, sizeof(status_text), _("xisguard conectado."));
    } else {
        snprintf(status_text, sizeof(status_text),
                  _("xisguard inacessivel (daemon parado, ou socket de controle ainda nao existe)."));
    }
    g_xg_status_label = gtk_label_new(status_text);
    gtk_misc_set_alignment(GTK_MISC(g_xg_status_label), 0.0, 0.5);
    gtk_label_set_line_wrap(GTK_LABEL(g_xg_status_label), TRUE);
    gtk_widget_set_size_request(g_xg_status_label, 660, -1);
    gtk_box_pack_start(GTK_BOX(outer), g_xg_status_label, FALSE, FALSE, 0);

    GtkWidget *status_table = gtk_table_new(6, 2, FALSE);
    g_xg_no_pause_chk = gtk_check_button_new_with_label(_("no_pause (nao pausar decisao)"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_xg_no_pause_chk), g_xg_baseline.no_pause);
    gtk_table_attach(GTK_TABLE(status_table), g_xg_no_pause_chk, 0, 2, 0, 1, GTK_FILL, GTK_FILL, 4, 1);
    g_xg_quiet_chk = gtk_check_button_new_with_label(_("quiet (sem notificacoes)"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_xg_quiet_chk), g_xg_baseline.quiet);
    gtk_table_attach(GTK_TABLE(status_table), g_xg_quiet_chk, 0, 2, 1, 2, GTK_FILL, GTK_FILL, 4, 1);
    g_xg_always_kill_chk = gtk_check_button_new_with_label(_("always_kill (sempre matar em DENY)"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_xg_always_kill_chk), g_xg_baseline.always_kill);
    gtk_table_attach(GTK_TABLE(status_table), g_xg_always_kill_chk, 0, 2, 2, 3, GTK_FILL, GTK_FILL, 4, 1);
    g_xg_secure_mode_chk = gtk_check_button_new_with_label(_("secure_mode"));
    gtk_widget_set_tooltip_text(g_xg_secure_mode_chk, _("Regras de usuario desligadas; novas regras permanentes pedem senha root"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_xg_secure_mode_chk), g_xg_baseline.secure_mode);
    gtk_table_attach(GTK_TABLE(status_table), g_xg_secure_mode_chk, 0, 2, 3, 4, GTK_FILL, GTK_FILL, 4, 1);
    g_xg_log_level_spin = gtk_spin_button_new_with_range(0, 5, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_xg_log_level_spin), g_xg_baseline.log_level);
    labeled_row(status_table, 4, "log_level:", g_xg_log_level_spin);
    g_xg_permissive_chk = gtk_check_button_new_with_label(_("Modo permissivo (libera tudo, so registra)"));
    gtk_widget_set_tooltip_text(g_xg_permissive_chk,
        _("O XisGuard libera todas as acoes mas os programas continuam sendo registrados. "
          "Fica salvo e vale tambem nas proximas sessoes (padrao em instalacoes novas). "
          "Programas que ja ganharam acesso so o perdem ao reconectar."));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_xg_permissive_chk), g_xg_baseline.permissive);
    gtk_table_attach(GTK_TABLE(status_table), g_xg_permissive_chk, 0, 2, 5, 6, GTK_FILL, GTK_FILL, 4, 1);
    gtk_widget_set_sensitive(status_table, g_xg_baseline.online);

    /* "Avisos": which actions are worth a toast / a line on the xisserve
     * page. Default (set by the guard): screen and window capture. */
    GtkWidget *notify_table = gtk_table_new(8, 2, FALSE);
    g_xg_n_notify_chk = 0;
    for (int i = 1; XNOTIFY_ACTIONS[i][0] && g_xg_n_notify_chk < 32; i++) {
        char *want = g_strdup_printf(",%s,", XNOTIFY_ACTIONS[i][0]);
        char *have = g_strdup_printf(",%s,", g_xg_baseline.notify);
        GtkWidget *chk = gtk_check_button_new_with_label(_(XNOTIFY_ACTIONS[i][1]));
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(chk), strstr(have, want) != NULL);
        gtk_widget_set_tooltip_text(chk, XNOTIFY_ACTIONS[i][0]);
        int idx = g_xg_n_notify_chk++;
        g_xg_notify_chk[idx] = chk;
        gtk_table_attach(GTK_TABLE(notify_table), chk, idx / 8, idx / 8 + 1, idx % 8, idx % 8 + 1, GTK_FILL, GTK_FILL, 4, 1);
        g_free(want);
        g_free(have);
    }
    gtk_widget_set_sensitive(notify_table, g_xg_baseline.online);
    GtkWidget *modes_row = gtk_hbox_new(FALSE, 6);
    gtk_box_pack_start(GTK_BOX(modes_row), frame_with("Modo de execucao", status_table), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(modes_row), frame_with("Avisos", notify_table), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(outer), modes_row, FALSE, FALSE, 0);

    GtkWidget *status_apply_btn = gtk_button_new_with_label(_("Aplicar"));
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
    gtk_widget_set_size_request(rules_scroll, -1, 50);
    gtk_container_add(GTK_CONTAINER(rules_scroll), g_xg_rules_view);

    GtkWidget *rules_box = gtk_vbox_new(FALSE, 2);
    gtk_container_set_border_width(GTK_CONTAINER(rules_box), 2);
    gtk_box_pack_start(GTK_BOX(rules_box), rules_scroll, TRUE, TRUE, 0);

    GtkWidget *add_row = gtk_hbox_new(FALSE, 4);
    g_xg_action_combo = gtk_combo_box_new_text();
    for (int i = 0; XNOTIFY_ACTIONS[i][0]; i++) {
        char label[96];
        snprintf(label, sizeof(label), "%s -- %s", XNOTIFY_ACTIONS[i][0], _(XNOTIFY_ACTIONS[i][1]));
        gtk_combo_box_append_text(GTK_COMBO_BOX(g_xg_action_combo), label);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(g_xg_action_combo), 0);
    /* Capped instead of sizing to its longest entry ("CURSOR -- Acessar
     * imagem/posicao do cursor" and friends) -- the full text still shows
     * once the dropdown is open, this only bounds the closed button. */
    gtk_widget_set_size_request(g_xg_action_combo, 170, -1);
    gtk_box_pack_start(GTK_BOX(add_row), g_xg_action_combo, TRUE, TRUE, 0);
    g_xg_type_combo = gtk_combo_box_new_text();
    gtk_combo_box_append_text(GTK_COMBO_BOX(g_xg_type_combo), "ALLOW");
    gtk_combo_box_append_text(GTK_COMBO_BOX(g_xg_type_combo), "DENY");
    gtk_combo_box_set_active(GTK_COMBO_BOX(g_xg_type_combo), 0);
    gtk_box_pack_start(GTK_BOX(add_row), g_xg_type_combo, FALSE, FALSE, 0);
    g_xg_pattern_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(g_xg_pattern_entry), "*");
    a11y_name(g_xg_pattern_entry, _("Padrao"));
    gtk_box_pack_start(GTK_BOX(add_row), g_xg_pattern_entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(rules_box), add_row, FALSE, FALSE, 0);

    GtkWidget *rules_btnbox = gtk_hbox_new(FALSE, 6);
    GtkWidget *add_btn = gtk_button_new_with_label(_("Adicionar regra"));
    GtkWidget *remove_btn = gtk_button_new_with_label(_("Remover selecionada"));
    GtkWidget *reload_btn = gtk_button_new_with_label(_("Recarregar (RELOAD)"));
    g_signal_connect(add_btn, "clicked", G_CALLBACK(on_xg_add_rule), NULL);
    g_signal_connect(remove_btn, "clicked", G_CALLBACK(on_xg_remove_rule), g_xg_rules_view);
    g_signal_connect(reload_btn, "clicked", G_CALLBACK(on_xg_reload), NULL);
    gtk_box_pack_start(GTK_BOX(rules_btnbox), add_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(rules_btnbox), remove_btn, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(rules_btnbox), reload_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(rules_box), rules_btnbox, FALSE, FALSE, 0);

    gtk_widget_set_sensitive(rules_box, g_xg_baseline.online);
    gtk_notebook_append_page(GTK_NOTEBOOK(rules_notebook), rules_box, gtk_label_new(_("Regras de usuario")));

    /* ---- "Regras de sistema" page: SYSCONFDIR's xnotify.conf.d directory
     * (*.conf files) plus the xnotify.conf next to it, read directly by kiconf once xisguard tells it where that is (see
     * xisguard_get_system_rules_paths()) -- read-only for now, a future
     * pass can make double-click editable the same way the user rules
     * list could, gated behind a polkit prompt to save (same mechanism
     * secure_save_rule() in xisguard.c already uses). */
    g_xg_sys_rules_store = gtk_list_store_new(N_XG_SYS_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    g_xg_sys_rules_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(g_xg_sys_rules_store));
    const char *xg_sys_titles[N_XG_SYS_COLS] = {"Tipo", "Acao", "Padrao", "Arquivo"};
    for (int col = 0; col < N_XG_SYS_COLS; col++) {
        GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
        GtkTreeViewColumn *tvcol = gtk_tree_view_column_new_with_attributes(xg_sys_titles[col], renderer, "text", col, NULL);
        gtk_tree_view_column_set_expand(tvcol, TRUE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(g_xg_sys_rules_view), tvcol);
    }
    GtkWidget *sys_rules_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sys_rules_scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(sys_rules_scroll, -1, 50);
    gtk_container_add(GTK_CONTAINER(sys_rules_scroll), g_xg_sys_rules_view);

    GtkWidget *sys_rules_box = gtk_vbox_new(FALSE, 2);
    gtk_container_set_border_width(GTK_CONTAINER(sys_rules_box), 2);
    gtk_box_pack_start(GTK_BOX(sys_rules_box), sys_rules_scroll, TRUE, TRUE, 0);

    g_xg_sys_rules_status_label = gtk_label_new(_("-"));
    gtk_misc_set_alignment(GTK_MISC(g_xg_sys_rules_status_label), 0.0, 0.5);
    gtk_label_set_line_wrap(GTK_LABEL(g_xg_sys_rules_status_label), TRUE);
    gtk_box_pack_start(GTK_BOX(sys_rules_box), g_xg_sys_rules_status_label, FALSE, FALSE, 0);

    GtkWidget *sys_reload_btn = gtk_button_new_with_label(_("Recarregar"));
    g_signal_connect(sys_reload_btn, "clicked", G_CALLBACK(on_xg_reload), NULL);
    GtkWidget *sys_rules_btnbox = gtk_hbox_new(FALSE, 0);
    gtk_box_pack_end(GTK_BOX(sys_rules_btnbox), sys_reload_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(sys_rules_box), sys_rules_btnbox, FALSE, FALSE, 0);

    gtk_widget_set_sensitive(sys_rules_box, g_xg_baseline.online);
    gtk_notebook_append_page(GTK_NOTEBOOK(rules_notebook), sys_rules_box, gtk_label_new(_("Regras de sistema")));

    /* ---- "Ignorados" page: ignore.conf, programs whose REPORTs are not
     * shown (by the guard's own log, nor by the panel's xisguard widget). */
    g_xg_ignored_store = gtk_list_store_new(1, G_TYPE_STRING);
    g_xg_ignored_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(g_xg_ignored_store));
    gtk_tree_view_append_column(GTK_TREE_VIEW(g_xg_ignored_view),
                                gtk_tree_view_column_new_with_attributes(_("Programa (caminho ou padrao)"),
                                                                         gtk_cell_renderer_text_new(), "text", 0, NULL));
    GtkWidget *ign_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(ign_scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(ign_scroll, -1, 50);
    gtk_container_add(GTK_CONTAINER(ign_scroll), g_xg_ignored_view);
    GtkWidget *ign_box = gtk_vbox_new(FALSE, 2);
    gtk_container_set_border_width(GTK_CONTAINER(ign_box), 2);
    GtkWidget *ign_hint = gtk_label_new(_("REPORTs destes programas nao aparecem (programas sempre necessarios na "
                                          "sessao, como gerenciadores de janela, area de transferencia e teclado)."));
    gtk_misc_set_alignment(GTK_MISC(ign_hint), 0.0, 0.5);
    gtk_label_set_line_wrap(GTK_LABEL(ign_hint), TRUE);
    gtk_box_pack_start(GTK_BOX(ign_box), ign_hint, FALSE, FALSE, 0);
    char builtin[512];
    xisguard_builtin_ignored(builtin, sizeof(builtin));
    if (builtin[0]) {
        char *txt = g_strdup_printf(_("Sempre ignorados (KiDesktop): %s"), builtin);
        GtkWidget *bl = gtk_label_new(txt);
        g_free(txt);
        gtk_misc_set_alignment(GTK_MISC(bl), 0.0, 0.5);
        gtk_label_set_line_wrap(GTK_LABEL(bl), TRUE);
        gtk_widget_set_sensitive(bl, FALSE);
        gtk_box_pack_start(GTK_BOX(ign_box), bl, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(ign_box), ign_scroll, TRUE, TRUE, 0);
    GtkWidget *ign_row = gtk_hbox_new(FALSE, 4);
    g_xg_ignored_entry = gtk_entry_new();
    a11y_name(g_xg_ignored_entry, _("Programa ignorado"));
    g_signal_connect(g_xg_ignored_entry, "activate", G_CALLBACK(on_xg_add_ignored), NULL);
    GtkWidget *ign_add_btn = gtk_button_new_with_label(_("Adicionar"));
    GtkWidget *ign_remove_btn = gtk_button_new_with_label(_("Remover selecionado"));
    g_signal_connect(ign_add_btn, "clicked", G_CALLBACK(on_xg_add_ignored), NULL);
    g_signal_connect(ign_remove_btn, "clicked", G_CALLBACK(on_xg_remove_ignored), NULL);
    gtk_box_pack_start(GTK_BOX(ign_row), g_xg_ignored_entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(ign_row), ign_add_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ign_row), ign_remove_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ign_box), ign_row, FALSE, FALSE, 0);
    gtk_widget_set_sensitive(ign_box, g_xg_baseline.online);
    gtk_notebook_append_page(GTK_NOTEBOOK(rules_notebook), ign_box, gtk_label_new(_("Ignorados")));

    gtk_box_pack_start(GTK_BOX(outer), frame_with("Regras XNOTIFY", rules_notebook), TRUE, TRUE, 0);

    refresh_xg_rules();
    refresh_xg_sys_rules();
    refresh_xg_ignored();
    return outer;
}
