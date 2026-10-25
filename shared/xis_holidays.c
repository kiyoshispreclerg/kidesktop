/* xis_holidays - see xis_holidays.h. */
#define _GNU_SOURCE
#include "xis_holidays.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef enum { RULE_FIXED, RULE_EASTER, RULE_NTH } RuleKind;

typedef struct {
    RuleKind kind;
    int month, day; /* FIXED; NTH uses month */
    int offset;     /* EASTER */
    int nth, dow;   /* NTH */
    char name[128];
} Rule;

#define MAX_RULES 128

/* The parsed rules of the last country asked for, reloaded when the country
 * or the file's mtime changes (so an updated list shows without a restart). */
static struct {
    char country[8];
    char path[PATH_MAX];
    time_t mtime;
    int n;
    Rule rules[MAX_RULES];
} g_cache;

static void tz_from_system(char *out, size_t sz)
{
    out[0] = '\0';
    const char *tz = getenv("TZ");
    if (tz && *tz) {
        snprintf(out, sz, "%s", tz[0] == ':' ? tz + 1 : tz);
        return;
    }
    FILE *f = fopen("/etc/timezone", "r");
    if (f) {
        if (fgets(out, (int)sz, f)) {
            out[strcspn(out, "\r\n")] = '\0';
        }
        fclose(f);
        if (out[0]) {
            return;
        }
    }
    char link[PATH_MAX];
    ssize_t n = readlink("/etc/localtime", link, sizeof(link) - 1);
    if (n > 0) {
        link[n] = '\0';
        const char *z = strstr(link, "zoneinfo/");
        if (z) {
            snprintf(out, sz, "%s", z + 9);
        }
    }
}

/* zone.tab: "CC <tab> coordinates <tab> TZ <tab> comment" */
static int country_of_tz(const char *tz, char *out, size_t sz)
{
    static const char *const tabs[] = {"/usr/share/zoneinfo/zone.tab", "/usr/share/zoneinfo/zone1970.tab", NULL};
    for (int t = 0; tabs[t]; t++) {
        FILE *f = fopen(tabs[t], "r");
        if (!f) {
            continue;
        }
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            if (line[0] == '#') {
                continue;
            }
            char *save = NULL;
            char *cc = strtok_r(line, "\t", &save);
            char *coord = strtok_r(NULL, "\t", &save);
            char *zone = strtok_r(NULL, "\t\r\n", &save);
            if (cc && coord && zone && !strcmp(zone, tz)) {
                /* zone1970.tab may list several countries: take the first */
                snprintf(out, sz, "%.2s", cc);
                fclose(f);
                return 1;
            }
        }
        fclose(f);
    }
    return 0;
}

void xis_holidays_country(char *out, size_t out_sz)
{
    out[0] = '\0';
    char tz[128];
    tz_from_system(tz, sizeof(tz));
    if (tz[0] && country_of_tz(tz, out, out_sz)) {
        return;
    }
    /* pt_BR.UTF-8 -> BR */
    const char *loc = getenv("LC_TIME");
    if (!loc || !*loc) loc = getenv("LC_ALL");
    if (!loc || !*loc) loc = getenv("LANG");
    const char *us = loc ? strchr(loc, '_') : NULL;
    if (us && isalpha((unsigned char)us[1]) && isalpha((unsigned char)us[2])) {
        snprintf(out, out_sz, "%c%c", toupper((unsigned char)us[1]), toupper((unsigned char)us[2]));
    }
}

static int parse_dow(const char *s)
{
    static const char *const names[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
    for (int i = 0; i < 7; i++) {
        if (!strncasecmp(s, names[i], 3)) {
            return i;
        }
    }
    return -1;
}

static int find_file(const char *country, char *path, size_t sz)
{
    const char *xdg = getenv("XDG_DATA_HOME");
    const char *home = getenv("HOME");
    char dir[PATH_MAX] = "";
    if (xdg && *xdg) {
        snprintf(dir, sizeof(dir), "%s/xisserve/holidays", xdg);
    } else if (home && *home) {
        snprintf(dir, sizeof(dir), "%s/.local/share/xisserve/holidays", home);
    }
    if (dir[0]) {
        snprintf(path, sz, "%s/%s.conf", dir, country);
        if (access(path, R_OK) == 0) {
            return 1;
        }
    }
    snprintf(path, sz, "%s/%s.conf", XIS_HOLIDAYS_DIR, country);
    return access(path, R_OK) == 0;
}

static void load_country(const char *country)
{
    char path[PATH_MAX];
    int have = find_file(country, path, sizeof(path));
    struct stat st;
    time_t mt = have && stat(path, &st) == 0 ? st.st_mtime : 0;
    if (!strcmp(g_cache.country, country) && !strcmp(g_cache.path, have ? path : "") && g_cache.mtime == mt) {
        return;
    }
    snprintf(g_cache.country, sizeof(g_cache.country), "%s", country);
    snprintf(g_cache.path, sizeof(g_cache.path), "%s", have ? path : "");
    g_cache.mtime = mt;
    g_cache.n = 0;
    FILE *f = have ? fopen(path, "r") : NULL;
    if (!f) {
        return;
    }
    char line[512];
    while (fgets(line, sizeof(line), f) && g_cache.n < MAX_RULES) {
        line[strcspn(line, "\r\n")] = '\0';
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#') {
            continue;
        }
        Rule r;
        memset(&r, 0, sizeof(r));
        char kw[16], name[128] = "";
        int off = 0;
        if (sscanf(p, "%15s%n", kw, &off) < 1) {
            continue;
        }
        const char *rest = p + off;
        int a = 0, b = 0, c = 0, used = 0;
        char dowtxt[8];
        if (!strcasecmp(kw, "FIXED") && sscanf(rest, " %d-%d%n", &a, &b, &used) == 2) {
            r.kind = RULE_FIXED;
            r.month = a;
            r.day = b;
        } else if (!strcasecmp(kw, "EASTER") && sscanf(rest, " %d%n", &a, &used) == 1) {
            r.kind = RULE_EASTER;
            r.offset = a;
        } else if (!strcasecmp(kw, "NTH") && sscanf(rest, " %d %d %7s%n", &a, &b, dowtxt, &used) == 3 &&
                   (c = parse_dow(dowtxt)) >= 0) {
            r.kind = RULE_NTH;
            r.month = a;
            r.nth = b;
            r.dow = c;
        } else {
            continue;
        }
        const char *n = rest + used;
        while (*n == ' ' || *n == '\t') n++;
        snprintf(name, sizeof(name), "%s", n);
        if (!name[0] || r.month < 0 || r.month > 12) {
            continue;
        }
        snprintf(r.name, sizeof(r.name), "%s", name);
        g_cache.rules[g_cache.n++] = r;
    }
    fclose(f);
}

/* Gauss' algorithm, Gregorian calendar. */
static void easter(int year, int *month, int *day)
{
    int a = year % 19, b = year / 100, c = year % 100;
    int d = b / 4, e = b % 4, f = (b + 8) / 25, g = (b - f + 1) / 3;
    int h = (19 * a + b - d - g + 15) % 30;
    int i = c / 4, k = c % 4;
    int l = (32 + 2 * e + 2 * i - h - k) % 7;
    int m = (a + 11 * h + 22 * l) / 451;
    *month = (h + l - 7 * m + 114) / 31;
    *day = ((h + l - 7 * m + 114) % 31) + 1;
}

/* (year, month, day + offset) normalised, via timegm so no DST is involved. */
static void add_days(int year, int month, int day, int offset, int *om, int *od)
{
    struct tm tmv;
    memset(&tmv, 0, sizeof(tmv));
    tmv.tm_year = year - 1900;
    tmv.tm_mon = month - 1;
    tmv.tm_mday = day + offset;
    tmv.tm_hour = 12;
    time_t t = timegm(&tmv);
    struct tm out;
    gmtime_r(&t, &out);
    *om = out.tm_mon + 1;
    *od = out.tm_mday;
}

static int nth_weekday(int year, int month, int nth, int dow)
{
    struct tm first;
    memset(&first, 0, sizeof(first));
    first.tm_year = year - 1900;
    first.tm_mon = month - 1;
    first.tm_mday = 1;
    first.tm_hour = 12;
    time_t t = timegm(&first);
    gmtime_r(&t, &first);
    int day = 1 + ((dow - first.tm_wday + 7) % 7);
    if (nth > 0) {
        return day + 7 * (nth - 1);
    }
    int dim = 31;
    int om, od;
    add_days(year, month, 31, 0, &om, &od);
    if (om != month) {
        dim = 31 - od; /* month has fewer than 31 days: 31 rolled over */
    }
    while (day + 7 <= dim) {
        day += 7;
    }
    return day;
}

static int cmp_holiday(const void *a, const void *b)
{
    const XisHoliday *x = a, *y = b;
    return (x->month * 100 + x->day) - (y->month * 100 + y->day);
}

int xis_holidays_for_year(const char *country, int year, XisHoliday *out, int max)
{
    if (!country || !*country) {
        return 0;
    }
    load_country(country);
    int n = 0, em, ed;
    easter(year, &em, &ed);
    for (int i = 0; i < g_cache.n && n < max; i++) {
        const Rule *r = &g_cache.rules[i];
        XisHoliday h;
        memset(&h, 0, sizeof(h));
        switch (r->kind) {
        case RULE_FIXED:
            h.month = r->month;
            h.day = r->day;
            break;
        case RULE_EASTER:
            add_days(year, em, ed, r->offset, &h.month, &h.day);
            break;
        case RULE_NTH:
            h.month = r->month;
            h.day = nth_weekday(year, r->month, r->nth, r->dow);
            break;
        }
        if (h.month < 1 || h.day < 1) {
            continue;
        }
        snprintf(h.name, sizeof(h.name), "%s", r->name);
        out[n++] = h;
    }
    qsort(out, (size_t)n, sizeof(out[0]), cmp_holiday);
    return n;
}
