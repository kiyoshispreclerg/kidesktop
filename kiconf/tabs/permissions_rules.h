/* kiconf - Permissoes tab: the X server's system rule files as a model that
 * remembers where every rule came from, so a later edit can write each rule
 * back to the same file at the same place. See permissions_rules.c. */
#ifndef KICONF_PERMISSIONS_RULES_H
#define KICONF_PERMISSIONS_RULES_H

typedef struct {
    char *path;    /* full path */
    char *name;    /* basename, e.g. "50-xisguard.conf" */
    int priority;  /* leading number of the name ("50-x.conf" -> 50), -1 if none */
    int is_main;   /* the single xnotify.conf, which the server reads after the drop-ins */
    int writable;  /* current user can write it (root-owned files normally are not: polkit) */
    char **lines;  /* every line of the file as read, without the newline: comments and
                    * blank lines included, so rewriting loses nothing */
    int n_lines;
    int n_rules;
} XgRuleFile;

typedef struct {
    char type[8];     /* ALLOW / DENY */
    char action[32];  /* action name, "ALL" for every action */
    char *pattern;
    int file;         /* index into XgRuleSet.files */
    int line;         /* index into that file's lines */
    int order;        /* 0-based position in the order the server applies the rules */
} XgFileRule;

typedef struct {
    XgRuleFile *files;
    int n_files;
    XgFileRule *rules;
    int n_rules;
} XgRuleSet;

/* Reads `dir`'s *.conf files in the order the server loads them, then `main_file`
 * (may be ""). Returns the number of rules. Replaces the set's contents. */
int xg_ruleset_load(XgRuleSet *set, const char *dir, const char *main_file);
void xg_ruleset_clear(XgRuleSet *set);

#endif
