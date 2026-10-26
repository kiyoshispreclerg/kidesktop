/* kiconf - Permissoes tab: system rule files model. See permissions_rules.h. */
#include "permissions_rules.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../common.h"

/* One line of a server rule file, in the grammar the X server itself reads
 * (XnotifyLoadFile() in xnotify.c):
 *   ALLOW|DENY ACTION /path[|args]   rule for one program and action
 *   ALLOW|DENY /path[|args]          every action ("ALL")
 *   ALLOW|DENY ACTION                the action for everyone ("*")
 *   ALLOW_ACTION ACTION              same as "ALLOW ACTION"
 * `line` is modified. Returns 1 and fills type/action/pattern for a rule. */
static int parse_rule_line(char *line, char *type, size_t type_sz, char *action, size_t action_sz, char **pattern)
{
    char *save = NULL;
    char *cmd = strtok_r(line, " \t", &save);
    if (!cmd || cmd[0] == '#') {
        return 0;
    }
    int allow_action = !strcasecmp(cmd, "ALLOW_ACTION");
    if (!allow_action && strcasecmp(cmd, "ALLOW") && strcasecmp(cmd, "DENY")) {
        return 0;
    }
    snprintf(type, type_sz, "%s", allow_action ? "ALLOW" : (toupper((unsigned char)cmd[0]) == 'A' ? "ALLOW" : "DENY"));
    char *tok = strtok_r(NULL, " \t", &save);
    if (!tok) {
        return 0;
    }
    if (tok[0] == '/') { /* no action: every one */
        snprintf(action, action_sz, "ALL");
        *pattern = g_strdup(tok);
        return 1;
    }
    snprintf(action, action_sz, "%s", tok);
    /* the pattern runs to the end of the line and may contain spaces */
    char *rest = save ? save : (char *)"";
    while (*rest == ' ' || *rest == '\t') {
        rest++;
    }
    size_t rl = strlen(rest);
    while (rl > 0 && (rest[rl - 1] == ' ' || rest[rl - 1] == '\t')) {
        rest[--rl] = '\0';
    }
    *pattern = g_strdup(rl ? rest : "*");
    return 1;
}

static void add_file(XgRuleSet *set, const char *path, int is_main)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        return;
    }
    int fi = set->n_files++;
    set->files = g_renew(XgRuleFile, set->files, set->n_files);
    XgRuleFile *rf = &set->files[fi];
    memset(rf, 0, sizeof(*rf));
    rf->path = g_strdup(path);
    const char *slash = strrchr(path, '/');
    rf->name = g_strdup(slash ? slash + 1 : path);
    rf->priority = isdigit((unsigned char)rf->name[0]) ? atoi(rf->name) : -1;
    rf->is_main = is_main;
    rf->writable = access(path, W_OK) == 0;

    char *buf = NULL;
    size_t cap = 0;
    ssize_t len;
    while ((len = getline(&buf, &cap, f)) >= 0) {
        while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
            buf[--len] = '\0';
        }
        int li = rf->n_lines++;
        rf->lines = g_renew(char *, rf->lines, rf->n_lines);
        rf->lines[li] = g_strdup(buf);

        char *copy = g_strdup(buf);
        XgFileRule r;
        memset(&r, 0, sizeof(r));
        if (parse_rule_line(copy, r.type, sizeof(r.type), r.action, sizeof(r.action), &r.pattern)) {
            r.file = fi;
            r.line = li;
            r.order = set->n_rules;
            set->rules = g_renew(XgFileRule, set->rules, set->n_rules + 1);
            set->rules[set->n_rules++] = r;
            rf->n_rules++;
        }
        g_free(copy);
    }
    free(buf);
    fclose(f);
}

void xg_ruleset_clear(XgRuleSet *set)
{
    for (int i = 0; i < set->n_files; i++) {
        g_free(set->files[i].path);
        g_free(set->files[i].name);
        for (int l = 0; l < set->files[i].n_lines; l++) {
            g_free(set->files[i].lines[l]);
        }
        g_free(set->files[i].lines);
    }
    for (int i = 0; i < set->n_rules; i++) {
        g_free(set->rules[i].pattern);
    }
    g_free(set->files);
    g_free(set->rules);
    memset(set, 0, sizeof(*set));
}

/* The server (XnotifyLoadConfigDir()) takes every regular "*.conf"
 * (suffix case-insensitive) and sorts the full paths with plain strcmp(),
 * not the locale's collation -- so "10-a.conf" < "5-b.conf" < "50-x.conf". */
static int cmp_names(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

int xg_ruleset_load(XgRuleSet *set, const char *dir, const char *main_file)
{
    xg_ruleset_clear(set);
    DIR *d = dir && dir[0] ? opendir(dir) : NULL;
    if (d) {
        char **names = NULL;
        int n = 0;
        struct dirent *e;
        while ((e = readdir(d))) {
            size_t len = strlen(e->d_name);
            if ((e->d_type != DT_REG && e->d_type != DT_UNKNOWN && e->d_type != DT_LNK) ||
                len <= 5 || strcasecmp(e->d_name + len - 5, ".conf")) {
                continue;
            }
            names = g_renew(char *, names, n + 1);
            names[n++] = g_strdup_printf("%s/%s", dir, e->d_name);
        }
        closedir(d);
        if (n > 1) {
            qsort(names, (size_t)n, sizeof(char *), cmp_names);
        }
        for (int i = 0; i < n; i++) {
            add_file(set, names[i], 0);
            g_free(names[i]);
        }
        g_free(names);
    }
    if (main_file && main_file[0]) {
        add_file(set, main_file, 1);
    }
    return set->n_rules;
}

/* The rule as one file line. A pattern may not hold a newline (it would
 * become a second line). */
static char *format_rule(const char *type, const char *action, const char *pattern, char *err, size_t err_sz)
{
    if (strcmp(type, "ALLOW") && strcmp(type, "DENY")) {
        snprintf(err, err_sz, "tipo invalido");
        return NULL;
    }
    if (!action[0] || strpbrk(action, " \t\r\n")) {
        snprintf(err, err_sz, "acao invalida");
        return NULL;
    }
    if (!pattern[0] || strpbrk(pattern, "\r\n")) {
        snprintf(err, err_sz, "padrao invalido");
        return NULL;
    }
    return g_strdup_printf("%s %s %s", type, action, pattern);
}

int xg_ruleset_edit(XgRuleSet *set, int rule, const char *type, const char *action,
                    const char *pattern, char *err, size_t err_sz)
{
    if (rule < 0 || rule >= set->n_rules) {
        snprintf(err, err_sz, "regra inexistente");
        return 0;
    }
    char *text = format_rule(type, action, pattern, err, err_sz);
    if (!text) {
        return 0;
    }
    XgFileRule *r = &set->rules[rule];
    XgRuleFile *rf = &set->files[r->file];
    g_free(rf->lines[r->line]);
    rf->lines[r->line] = text;
    snprintf(r->type, sizeof(r->type), "%s", type);
    snprintf(r->action, sizeof(r->action), "%s", action);
    g_free(r->pattern);
    r->pattern = g_strdup(pattern);
    rf->dirty = 1;
    return 1;
}

int xg_ruleset_remove(XgRuleSet *set, int rule)
{
    if (rule < 0 || rule >= set->n_rules) {
        return 0;
    }
    XgFileRule gone = set->rules[rule];
    XgRuleFile *rf = &set->files[gone.file];
    g_free(rf->lines[gone.line]);
    memmove(&rf->lines[gone.line], &rf->lines[gone.line + 1], sizeof(char *) * (size_t)(rf->n_lines - gone.line - 1));
    rf->n_lines--;
    rf->n_rules--;
    rf->dirty = 1;
    g_free(gone.pattern);
    memmove(&set->rules[rule], &set->rules[rule + 1], sizeof(XgFileRule) * (size_t)(set->n_rules - rule - 1));
    set->n_rules--;
    for (int i = 0; i < set->n_rules; i++) {
        if (i >= rule) {
            set->rules[i].order--;
        }
        if (set->rules[i].file == gone.file && set->rules[i].line > gone.line) {
            set->rules[i].line--;
        }
    }
    return 1;
}

int xg_ruleset_add(XgRuleSet *set, const char *dir, int priority, const char *name, const char *type,
                   const char *action, const char *pattern, char *err, size_t err_sz)
{
    if (priority < 0 || priority > 99) {
        snprintf(err, err_sz, "prioridade deve ser de 0 a 99");
        return 0;
    }
    if (!name[0]) {
        snprintf(err, err_sz, "nome do arquivo vazio");
        return 0;
    }
    for (const char *c = name; *c; c++) {
        if (!isalnum((unsigned char)*c) && *c != '-' && *c != '_') {
            snprintf(err, err_sz, "nome do arquivo so pode ter letras, numeros, - e _");
            return 0;
        }
    }
    char *text = format_rule(type, action, pattern, err, err_sz);
    if (!text) {
        return 0;
    }
    char *path = g_strdup_printf("%s/%02d-%s.conf", dir, priority, name);
    int fi = -1;
    for (int i = 0; i < set->n_files; i++) {
        if (!strcmp(set->files[i].path, path)) {
            fi = i;
        }
    }
    if (fi < 0) {
        fi = set->n_files++;
        set->files = g_renew(XgRuleFile, set->files, set->n_files);
        memset(&set->files[fi], 0, sizeof(XgRuleFile));
        set->files[fi].path = path;
        set->files[fi].name = g_path_get_basename(path);
        set->files[fi].priority = priority;
    } else {
        g_free(path);
    }
    XgRuleFile *rf = &set->files[fi];
    rf->lines = g_renew(char *, rf->lines, rf->n_lines + 1);
    rf->lines[rf->n_lines++] = text;
    rf->dirty = 1;
    return 1;
}

/* Runs as root: $1/$2 pairs of (temp file, destination). Each is copied next
 * to its destination under a name the server ignores (no ".conf" suffix) and
 * renamed over it, so a reader never sees a half-written file. */
static const char *const INSTALL_SCRIPT =
    "set -e\n"
    "while [ $# -ge 2 ]; do\n"
    "  mkdir -p \"$(dirname \"$2\")\"\n"
    "  install -m 644 \"$1\" \"$2.new\"\n"
    "  mv -f \"$2.new\" \"$2\"\n"
    "  shift 2\n"
    "done\n";

int xg_ruleset_save(XgRuleSet *set, char *err, size_t err_sz)
{
    char tmpdir[] = "/tmp/kiconf-rules-XXXXXX";
    if (!mkdtemp(tmpdir)) {
        snprintf(err, err_sz, "mkdtemp: %s", strerror(errno));
        return 0;
    }
    char *args[64];
    int n_args = 0, ok = 1, need_root = 0;
    char *tmps[32];
    int n_tmps = 0;
    for (int i = 0; i < set->n_files && ok; i++) {
        XgRuleFile *rf = &set->files[i];
        if (!rf->dirty) {
            continue;
        }
        if (n_tmps >= 30) {
            snprintf(err, err_sz, "arquivos demais para salvar de uma vez");
            ok = 0;
            break;
        }
        char *tmp = g_strdup_printf("%s/%d", tmpdir, i);
        tmps[n_tmps++] = tmp;
        FILE *f = fopen(tmp, "w");
        if (!f) {
            snprintf(err, err_sz, "%s: %s", tmp, strerror(errno));
            ok = 0;
            break;
        }
        for (int l = 0; l < rf->n_lines; l++) {
            fprintf(f, "%s\n", rf->lines[l]);
        }
        if (fclose(f) != 0) {
            snprintf(err, err_sz, "%s: %s", tmp, strerror(errno));
            ok = 0;
            break;
        }
        args[n_args++] = tmp;
        args[n_args++] = rf->path;
        /* Written by replacing the file (rename), so the directory must be
         * writable too. */
        char *dir = g_path_get_dirname(rf->path);
        int can = access(dir, W_OK) == 0 && (access(rf->path, F_OK) != 0 || access(rf->path, W_OK) == 0);
        g_free(dir);
        if (!can) {
            need_root = 1;
        }
    }
    args[n_args] = NULL;
    if (ok && n_args) {
        if (need_root) {
            int rc = run_privileged_script(INSTALL_SCRIPT, args, NULL);
            if (rc != 0) {
                snprintf(err, err_sz, rc == 126 || rc == 127 ? "autorizacao (polkit) negada ou cancelada"
                                                             : "falha ao gravar os arquivos (codigo %d)", rc);
                ok = 0;
            }
        } else {
            for (int i = 0; i < n_args && ok; i += 2) {
                gchar *content = NULL;
                gsize len = 0;
                GError *gerr = NULL;
                if (!g_file_get_contents(args[i], &content, &len, &gerr) ||
                    !g_file_set_contents(args[i + 1], content, (gssize)len, &gerr)) {
                    snprintf(err, err_sz, "%s", gerr ? gerr->message : "erro");
                    g_clear_error(&gerr);
                    ok = 0;
                }
                g_free(content);
            }
        }
    }
    for (int i = 0; i < n_tmps; i++) {
        unlink(tmps[i]);
        g_free(tmps[i]);
    }
    rmdir(tmpdir);
    if (ok) {
        for (int i = 0; i < set->n_files; i++) {
            set->files[i].dirty = 0;
        }
    }
    return ok;
}
