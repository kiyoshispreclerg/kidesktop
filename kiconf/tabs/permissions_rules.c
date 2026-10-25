/* kiconf - Permissoes tab: system rule files model. See permissions_rules.h. */
#include "permissions_rules.h"

#include <ctype.h>
#include <dirent.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

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
        qsort(names, (size_t)n, sizeof(char *), cmp_names);
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
