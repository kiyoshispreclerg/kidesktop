/* ks_log - see ks_log.h. */
#include "ks_log.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char events_dir[600];
static char pause_path[256];

static void mkdir_p(const char *path)
{
    char tmp[600];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0700);
            *p = '/';
        }
    }
    mkdir(tmp, 0700);
}

int ks_log_init(const char *data_dir, int display)
{
    snprintf(events_dir, sizeof(events_dir), "%s/events", data_dir);
    mkdir_p(events_dir);
    chmod(data_dir, 0700);
    const char *run = getenv("XDG_RUNTIME_DIR");
    snprintf(pause_path, sizeof(pause_path), "%s/kistory-paused.%d", run && *run ? run : "/tmp", display);
    return access(events_dir, W_OK) == 0;
}

int ks_log_paused(void)
{
    FILE *f = fopen(pause_path, "r");
    if (!f)
        return 0;
    long until = 0;
    if (fscanf(f, "%ld", &until) != 1)
        until = 0;
    fclose(f);
    if (until == 0 || until > (long)time(NULL))
        return 1;
    unlink(pause_path);   /* expired */
    return 0;
}

static void put_field(FILE *f, const char *s)
{
    for (; s && *s; s++) {
        if (*s == '\t')      fputs("\\t", f);
        else if (*s == '\n') fputs("\\n", f);
        else if (*s == '\r') continue;
        else if (*s == '\\') fputs("\\\\", f);
        else                 fputc(*s, f);
    }
}

void ks_log_event(time_t ts, const char *kind, const char *app, const char *exe, long desktop,
                  const char *output, const char *subject, const char *detail)
{
    if (ks_log_paused())
        return;
    struct tm tm;
    localtime_r(&ts, &tm);
    char path[700], when[32];
    snprintf(path, sizeof(path), "%s/%04d-%02d-%02d.tsv", events_dir, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    strftime(when, sizeof(when), "%Y-%m-%dT%H:%M:%S", &tm);

    FILE *f = fopen(path, "a");
    if (!f)
        return;
    fchmod(fileno(f), 0600);
    fputs(when, f);
    fputc('\t', f); put_field(f, kind);
    fputc('\t', f); put_field(f, app);
    fputc('\t', f); put_field(f, exe);
    fputc('\t', f);
    if (desktop == 0xFFFFFFFFL)
        fputc('*', f);        /* sticky: on every desktop */
    else if (desktop >= 0)
        fprintf(f, "%ld", desktop);
    fputc('\t', f); put_field(f, output);
    fputc('\t', f); put_field(f, subject);
    fputc('\t', f); put_field(f, detail);
    fputc('\n', f);
    fclose(f);
}

void ks_log_prune(int days)
{
    if (days <= 0)
        return;
    time_t cutoff = time(NULL) - (time_t)days * 86400;
    struct tm tm;
    localtime_r(&cutoff, &tm);
    char oldest[40];
    snprintf(oldest, sizeof(oldest), "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);

    DIR *d = opendir(events_dir);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d))) {
        /* YYYY-MM-DD.tsv[.gz]: names sort as dates. */
        if (strlen(e->d_name) < 14 || e->d_name[4] != '-' || strncmp(e->d_name, oldest, 10) >= 0)
            continue;
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", events_dir, e->d_name);
        unlink(path);
    }
    closedir(d);
}

void ks_log_range(char *out, unsigned long outsz, time_t start, time_t end)
{
    struct tm a, b;
    localtime_r(&start, &a);
    localtime_r(&end, &b);
    char sa[32], sb[32];
    int same_day = a.tm_year == b.tm_year && a.tm_yday == b.tm_yday;
    strftime(sa, sizeof(sa), same_day ? "%H:%M:%S" : "%Y-%m-%dT%H:%M:%S", &a);
    strftime(sb, sizeof(sb), same_day ? "%H:%M:%S" : "%Y-%m-%dT%H:%M:%S", &b);
    snprintf(out, outsz, "%s-%s", sa, sb);
}
