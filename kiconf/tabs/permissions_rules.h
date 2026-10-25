/* kiconf - Permissoes tab: the X server's system rule files as a model that
 * remembers where every rule came from, so a later edit can write each rule
 * back to the same file at the same place. See permissions_rules.c. */
#ifndef KICONF_PERMISSIONS_RULES_H
#define KICONF_PERMISSIONS_RULES_H

#include <stddef.h>

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
    int dirty;     /* changed in memory, not saved yet */
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

/* In-memory edits: only the touched line changes, everything else in the file
 * stays as read. Each returns 1 on success, else 0 with a message in `err`.
 * Nothing reaches the disk until xg_ruleset_save(). */
int xg_ruleset_edit(XgRuleSet *set, int rule, const char *type, const char *action,
                    const char *pattern, char *err, size_t err_sz);
int xg_ruleset_remove(XgRuleSet *set, int rule);
/* Appends the rule to the drop-in `dir`/"NN-name.conf" (priority 0..99 as a
 * two-digit prefix), creating the file when it does not exist yet. */
int xg_ruleset_add(XgRuleSet *set, const char *dir, int priority, const char *name, const char *type,
                   const char *action, const char *pattern, char *err, size_t err_sz);

/* Writes every changed file back to the path it was read from. A file the user
 * cannot write (root's xnotify.conf.d) goes through ONE pkexec prompt for all of
 * them. Returns 1 when everything was saved; on failure 0 with `err`, and the
 * caller should reload the set from disk. */
int xg_ruleset_save(XgRuleSet *set, char *err, size_t err_sz);

#endif
