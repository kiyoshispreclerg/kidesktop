/* ks_config - kistoryd's settings ($XDG_CONFIG_HOME/kistory.conf,
 * key=value, SIGHUP reloads) and the small matching helpers every source
 * uses to apply them. */
#ifndef KS_CONFIG_H
#define KS_CONFIG_H

#include <regex.h>
#include <string.h>
#include <strings.h>

typedef struct {
    int min_dwell_s;              /* focus periods shorter than this aren't logged */
    int retention_days;           /* 0: keep forever */
    char exclude[512];            /* comma list of WM_CLASS / exe basenames: nothing logged */
    char title_only_exclude[512]; /* logged, but without titles / file names / URLs */
    char private_title_regex[256];/* titles matching it are hidden (empty: off) */
    int  fd_sampling;             /* look at the focused app's open files */
    char guard_actions[256];      /* xisguard actions to subscribe to */
    int have_private_re;
    regex_t private_re;
} KsConfig;

extern KsConfig ks_conf;

static inline int ks_in_list(const char *list, const char *name)
{
    if (!name || !*name)
        return 0;
    size_t n = strlen(name);
    for (const char *p = list; *p; ) {
        while (*p == ',' || *p == ' ')
            p++;
        const char *e = p;
        while (*e && *e != ',')
            e++;
        const char *te = e;
        while (te > p && te[-1] == ' ')
            te--;
        if ((size_t)(te - p) == n && strncasecmp(p, name, n) == 0)
            return 1;
        p = e;
    }
    return 0;
}

static inline const char *ks_exe_base(const char *exe)
{
    const char *b = strrchr(exe, '/');
    return b ? b + 1 : exe;
}

/* 1: nothing about this app is logged. */
static inline int ks_excluded(const char *wm_class, const char *wm_instance, const char *exe)
{
    return ks_in_list(ks_conf.exclude, wm_class) || ks_in_list(ks_conf.exclude, wm_instance) ||
           ks_in_list(ks_conf.exclude, ks_exe_base(exe ? exe : ""));
}

/* 1: log this app's activity without titles/paths/URLs. */
static inline int ks_titles_hidden(const char *wm_class, const char *wm_instance, const char *exe,
                                   const char *title)
{
    if (ks_in_list(ks_conf.title_only_exclude, wm_class) || ks_in_list(ks_conf.title_only_exclude, wm_instance) ||
        ks_in_list(ks_conf.title_only_exclude, ks_exe_base(exe ? exe : "")))
        return 1;
    return ks_conf.have_private_re && title && *title && regexec(&ks_conf.private_re, title, 0, NULL, 0) == 0;
}

#endif /* KS_CONFIG_H */
