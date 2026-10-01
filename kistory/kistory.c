/* kistory - search and manage kistoryd's activity log. Reads the day files
 * directly ($XDG_DATA_HOME/kistory/events/YYYY-MM-DD.tsv), so it works
 * whether kistoryd is running or not. */
#define _GNU_SOURCE   /* strcasestr */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define KISTORY_VERSION "0.1.1"
#define NF 8   /* ts kind app exe desktop output subject detail */

static char events_dir[600];
static char pause_path[300];

static void usage(void)
{
    printf("Usage: kistory COMMAND\n"
           "  search TERM... [--since D] [--until D] [--kind K] [--app A] [-n N]\n"
           "                   lines containing every TERM (case-insensitive), default last 30 days\n"
           "  day [D]          one day's log (default today)\n"
           "  at 'D HH:MM'     what was focused, playing or captured at that moment\n"
           "  stats [--since D]  disk use per day and lines per app\n"
           "  purge (--all | [--app A] [--kind K] [--since D] [--until D])\n"
           "  pause [MINUTES]  stop logging (default: until resume)\n"
           "  resume\n"
           "  status\n"
           "  --version\n"
           "D: YYYY-MM-DD, today, yesterday, or Nd (N days ago)\n");
}

/* ---- dates ---- */

static void day_string(time_t t, char out[11])
{
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(out, 11, "%Y-%m-%d", &tm);
}

/* D -> "YYYY-MM-DD"; 0 if unparseable. */
static int parse_day(const char *s, char out[11])
{
    time_t now = time(NULL);
    if (!strcmp(s, "today")) {
        day_string(now, out);
        return 1;
    }
    if (!strcmp(s, "yesterday")) {
        day_string(now - 86400, out);
        return 1;
    }
    size_t n = strlen(s);
    if (n > 1 && s[n - 1] == 'd' && strspn(s, "0123456789") == n - 1) {
        day_string(now - (time_t)atol(s) * 86400, out);
        return 1;
    }
    int y, m, d;
    if (sscanf(s, "%4d-%2d-%2d", &y, &m, &d) != 3 || y < 1 || m < 1 || m > 12 || d < 1 || d > 31)
        return 0;
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d", y, m, d);
    memcpy(out, buf, 11);
    return 1;
}

static time_t parse_iso(const char *s)
{
    struct tm tm = { 0 };
    if (!strptime(s, "%Y-%m-%dT%H:%M:%S", &tm))
        return 0;
    tm.tm_isdst = -1;
    return mktime(&tm);
}

/* ---- day files ---- */

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* Day file names within [since, until] (either may be ""), sorted. */
static char **list_days(const char *since, const char *until, int *count)
{
    *count = 0;
    DIR *d = opendir(events_dir);
    if (!d)
        return NULL;
    char **names = NULL;
    int cap = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strlen(e->d_name) != 14 || strcmp(e->d_name + 10, ".tsv"))
            continue;
        if ((since[0] && strncmp(e->d_name, since, 10) < 0) || (until[0] && strncmp(e->d_name, until, 10) > 0))
            continue;
        if (*count == cap) {
            cap = cap ? cap * 2 : 64;
            char **nn = realloc(names, sizeof(char *) * (size_t)cap);
            if (!nn)
                break;
            names = nn;
        }
        names[(*count)++] = strdup(e->d_name);
    }
    closedir(d);
    if (*count)
        qsort(names, (size_t)*count, sizeof(char *), cmp_str);
    return names;
}

static void free_days(char **names, int n)
{
    for (int i = 0; i < n; i++)
        free(names[i]);
    free(names);
}

/* Splits a raw line into NF fields in place (unescaping). */
static int split(char *line, char **f)
{
    int n = 0;
    char *out = line, *start = line;
    for (char *p = line; ; p++) {
        if (*p == '\\' && p[1]) {
            p++;
            *out++ = *p == 't' ? '\t' : *p == 'n' ? '\n' : *p;
            continue;
        }
        if (*p == '\t' || *p == '\n' || *p == '\0') {
            char c = *p;
            *out++ = '\0';
            if (n < NF)
                f[n++] = start;
            if (c != '\t')
                break;
            start = out;
            continue;
        }
        *out++ = *p;
    }
    for (int i = n; i < NF; i++)
        f[i] = "";
    return n;
}

static void print_fields(char **f)
{
    /* 2026-09-30T14:02:11 -> 2026-09-30 14:02:11 */
    char ts[24];
    snprintf(ts, sizeof(ts), "%s", f[0]);
    if (ts[10] == 'T')
        ts[10] = ' ';
    printf("%s  %-10s %-16s", ts, f[1], f[2][0] ? f[2] : "-");
    /* Where: "[d2 HDMI-1]", "[d2]", "[HDMI-1]" or nothing. */
    if (f[4][0] && f[5][0])
        printf(" [d%s %s]", f[4], f[5]);
    else if (f[4][0] || f[5][0])
        printf(f[4][0] ? " [d%s]" : " [%s]", f[4][0] ? f[4] : f[5]);
    if (f[6][0])
        printf("  %s", f[6]);
    if (f[7][0])
        printf("  %s", f[7]);
    putchar('\n');
}

/* ---- commands ---- */

static int cmd_search(int argc, char **argv)
{
    char since[11] = "", until[11] = "", tmp[11];
    const char *kind = NULL, *app = NULL;
    const char *terms[32];
    int nterms = 0;
    long limit = 200;
    day_string(time(NULL) - 30 * 86400, since);
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--since") && i + 1 < argc && parse_day(argv[++i], tmp))
            memcpy(since, tmp, 11);
        else if (!strcmp(argv[i], "--until") && i + 1 < argc && parse_day(argv[++i], tmp))
            memcpy(until, tmp, 11);
        else if (!strcmp(argv[i], "--kind") && i + 1 < argc)
            kind = argv[++i];
        else if (!strcmp(argv[i], "--app") && i + 1 < argc)
            app = argv[++i];
        else if (!strcmp(argv[i], "-n") && i + 1 < argc)
            limit = atol(argv[++i]);
        else if (nterms < 32)
            terms[nterms++] = argv[i];
    }

    int ndays;
    char **days = list_days(since, until, &ndays);
    /* Keep the last `limit` matches: a ring of lines. */
    char **ring = calloc((size_t)(limit > 0 ? limit : 1), sizeof(char *));
    long nring = 0;
    char *line = NULL;
    size_t cap = 0;
    for (int d = 0; d < ndays && ring; d++) {
        char path[700];
        snprintf(path, sizeof(path), "%s/%s", events_dir, days[d]);
        FILE *fp = fopen(path, "r");
        if (!fp)
            continue;
        while (getline(&line, &cap, fp) > 0) {
            int ok = 1;
            for (int t = 0; t < nterms && ok; t++)
                ok = strcasestr(line, terms[t]) != NULL;
            if (!ok)
                continue;
            char *copy = strdup(line), *f[NF];
            split(copy, f);
            if ((kind && strcasecmp(f[1], kind)) || (app && !strcasestr(f[2], app))) {
                free(copy);
                continue;
            }
            free(copy);
            long slot = limit > 0 ? nring % limit : 0;
            free(ring[slot]);
            ring[slot] = strdup(line);
            nring++;
        }
        fclose(fp);
    }
    long shown = limit > 0 && nring > limit ? limit : nring;
    for (long i = nring - shown; i < nring; i++) {
        char *f[NF];
        char *l = ring[limit > 0 ? i % limit : 0];
        split(l, f);
        print_fields(f);
    }
    if (limit > 0 && nring > limit)
        fprintf(stderr, "(%ld older matches not shown; -n to change)\n", nring - limit);
    for (long i = 0; ring && i < (limit > 0 ? limit : 1); i++)
        free(ring[i]);
    free(ring);
    free(line);
    free_days(days, ndays);
    return nring ? 0 : 1;
}

static int cmd_day(const char *arg)
{
    char day[11];
    if (!parse_day(arg ? arg : "today", day)) {
        fprintf(stderr, "kistory: bad date %s\n", arg);
        return 2;
    }
    char path[700];
    snprintf(path, sizeof(path), "%s/%s.tsv", events_dir, day);
    FILE *fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "kistory: nothing logged on %s\n", day);
        return 1;
    }
    char *line = NULL, *f[NF];
    size_t cap = 0;
    while (getline(&line, &cap, fp) > 0) {
        split(line, f);
        print_fields(f);
    }
    free(line);
    fclose(fp);
    return 0;
}

/* End of a period line (focus/audio/guard), from its "start-end" subject. */
static time_t period_end(char **f, time_t start)
{
    const char *dash = strrchr(f[6], '-');
    if (!dash || !strchr(f[6], ':'))
        return 0;
    if (strchr(dash, 'T'))
        return parse_iso(dash + 1);
    /* Same-day "HH:MM:SS-HH:MM:SS": the end's date is the start's. */
    char iso[32];
    snprintf(iso, sizeof(iso), "%.10sT%s", f[0], dash + 1);
    time_t end = parse_iso(iso);
    return end >= start ? end : 0;
}

static int cmd_at(int argc, char **argv)
{
    if (argc < 1) {
        fprintf(stderr, "kistory: at needs a time ('YYYY-MM-DD HH:MM' or 'HH:MM' for today)\n");
        return 2;
    }
    char when[64];
    if (argc >= 2)
        snprintf(when, sizeof(when), "%s %s", argv[0], argv[1]);
    else
        snprintf(when, sizeof(when), "%s", argv[0]);
    char day[11], hm[16] = "";
    if (!strchr(when, ' ')) {   /* just a time: today */
        day_string(time(NULL), day);
        snprintf(hm, sizeof(hm), "%.15s", when);
    } else {
        char d[32];
        sscanf(when, "%31s %15s", d, hm);
        if (!parse_day(d, day)) {
            fprintf(stderr, "kistory: bad date %s\n", d);
            return 2;
        }
    }
    char iso[40];
    snprintf(iso, sizeof(iso), "%sT%s%s", day, hm, strlen(hm) <= 5 ? ":00" : "");
    time_t t = parse_iso(iso);
    if (!t) {
        fprintf(stderr, "kistory: bad time %s\n", hm);
        return 2;
    }

    /* The day before too: a period may have started before midnight. */
    char prev[11];
    day_string(t - 86400, prev);
    int ndays;
    char **days = list_days(prev, day, &ndays);
    char *line = NULL;
    size_t cap = 0;
    int found = 0;
    for (int d = 0; d < ndays; d++) {
        char path[700];
        snprintf(path, sizeof(path), "%s/%s", events_dir, days[d]);
        FILE *fp = fopen(path, "r");
        if (!fp)
            continue;
        while (getline(&line, &cap, fp) > 0) {
            char *copy = strdup(line), *f[NF];
            split(copy, f);
            time_t start = parse_iso(f[0]);
            time_t end = period_end(f, start);
            int hit = end ? (start <= t && t <= end)
                          : (start >= t - 120 && start <= t + 120);   /* instants: +-2 min */
            if (hit) {
                print_fields(f);
                found++;
            }
            free(copy);
        }
        fclose(fp);
    }
    free(line);
    free_days(days, ndays);
    if (!found)
        fprintf(stderr, "kistory: nothing logged around %s\n", iso);
    return found ? 0 : 1;
}

typedef struct {
    char app[64];
    long lines;
    long bytes;
} AppStat;

static int cmd_stats(int argc, char **argv)
{
    char since[11] = "", tmp[11];
    for (int i = 0; i < argc; i++)
        if (!strcmp(argv[i], "--since") && i + 1 < argc && parse_day(argv[++i], tmp))
            memcpy(since, tmp, 11);
    int ndays;
    char **days = list_days(since, "", &ndays);
    AppStat *apps = NULL;
    int napps = 0;
    long total = 0, total_lines = 0;
    char *line = NULL;
    size_t cap = 0;
    for (int d = 0; d < ndays; d++) {
        char path[700];
        snprintf(path, sizeof(path), "%s/%s", events_dir, days[d]);
        FILE *fp = fopen(path, "r");
        if (!fp)
            continue;
        long lines = 0, bytes = 0;
        ssize_t n;
        while ((n = getline(&line, &cap, fp)) > 0) {
            lines++;
            bytes += n;
            char *f[NF];
            split(line, f);
            const char *app = f[2][0] ? f[2] : "-";
            int k = 0;
            while (k < napps && strcmp(apps[k].app, app))
                k++;
            if (k == napps) {
                AppStat *na = realloc(apps, sizeof(AppStat) * (size_t)(napps + 1));
                if (!na)
                    continue;
                apps = na;
                memset(&apps[napps], 0, sizeof(AppStat));
                snprintf(apps[napps++].app, sizeof(apps[0].app), "%s", app);
            }
            apps[k].lines++;
            apps[k].bytes += n;
        }
        fclose(fp);
        printf("%.10s  %6ld lines  %8.1f KiB\n", days[d], lines, bytes / 1024.0);
        total += bytes;
        total_lines += lines;
    }
    printf("total       %6ld lines  %8.1f KiB in %d days\n\nby app:\n", total_lines, total / 1024.0, ndays);
    for (int i = 0; i < napps; i++)   /* biggest first: simple selection */
        for (int j = i + 1; j < napps; j++)
            if (apps[j].bytes > apps[i].bytes) {
                AppStat t = apps[i];
                apps[i] = apps[j];
                apps[j] = t;
            }
    for (int i = 0; i < napps; i++)
        printf("  %-24s %6ld lines  %8.1f KiB\n", apps[i].app, apps[i].lines, apps[i].bytes / 1024.0);
    free(apps);
    free(line);
    free_days(days, ndays);
    return 0;
}

static int cmd_purge(int argc, char **argv)
{
    char since[11] = "", until[11] = "", tmp[11];
    const char *kind = NULL, *app = NULL;
    int all = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--all"))
            all = 1;
        else if (!strcmp(argv[i], "--since") && i + 1 < argc && parse_day(argv[++i], tmp))
            memcpy(since, tmp, 11);
        else if (!strcmp(argv[i], "--until") && i + 1 < argc && parse_day(argv[++i], tmp))
            memcpy(until, tmp, 11);
        else if (!strcmp(argv[i], "--kind") && i + 1 < argc)
            kind = argv[++i];
        else if (!strcmp(argv[i], "--app") && i + 1 < argc)
            app = argv[++i];
        else {
            fprintf(stderr, "kistory: purge: unknown option %s\n", argv[i]);
            return 2;
        }
    }
    if (!all && !kind && !app && !since[0] && !until[0]) {
        fprintf(stderr, "kistory: purge needs --all or a filter (--app, --kind, --since, --until)\n");
        return 2;
    }

    int ndays;
    char **days = list_days(since, until, &ndays);
    long removed = 0;
    char *line = NULL;
    size_t cap = 0;
    for (int d = 0; d < ndays; d++) {
        char path[700], tmpp[720];
        snprintf(path, sizeof(path), "%s/%s", events_dir, days[d]);
        if (!kind && !app) {   /* whole days */
            FILE *fp = fopen(path, "r");
            while (fp && getline(&line, &cap, fp) > 0)
                removed++;
            if (fp)
                fclose(fp);
            unlink(path);
            continue;
        }
        FILE *in = fopen(path, "r");
        if (!in)
            continue;
        flock(fileno(in), LOCK_EX);   /* kistoryd appends under the same lock */
        snprintf(tmpp, sizeof(tmpp), "%s.tmp", path);
        FILE *out = fopen(tmpp, "w");
        if (!out) {
            fclose(in);
            continue;
        }
        fchmod(fileno(out), 0600);
        while (getline(&line, &cap, in) > 0) {
            char *copy = strdup(line), *f[NF];
            split(copy, f);
            int match = (!kind || !strcasecmp(f[1], kind)) && (!app || !strcasecmp(f[2], app));
            free(copy);
            if (match)
                removed++;
            else
                fputs(line, out);
        }
        if (fclose(out) == 0)
            rename(tmpp, path);
        else
            unlink(tmpp);
        fclose(in);   /* releases the lock after the rename */
    }
    free(line);
    free_days(days, ndays);
    printf("removed %ld lines\n", removed);
    return 0;
}

static int cmd_pause(const char *minutes)
{
    FILE *f = fopen(pause_path, "w");
    if (!f) {
        perror("kistory");
        return 1;
    }
    long until = minutes ? (long)time(NULL) + atol(minutes) * 60 : 0;
    fprintf(f, "%ld\n", until);
    fclose(f);
    if (until)
        printf("kistory paused for %s minutes\n", minutes);
    else
        printf("kistory paused until 'kistory resume'\n");
    return 0;
}

static int cmd_status(void)
{
    FILE *f = fopen(pause_path, "r");
    long until = -1;
    if (f) {
        if (fscanf(f, "%ld", &until) != 1)
            until = 0;
        fclose(f);
    }
    if (until == 0)
        printf("paused until 'kistory resume'\n");
    else if (until > (long)time(NULL))
        printf("paused for %ld more minutes\n", (until - (long)time(NULL) + 59) / 60);
    else
        printf("logging\n");
    int ndays;
    char **days = list_days("", "", &ndays);
    long total = 0;
    for (int d = 0; d < ndays; d++) {
        char path[700];
        struct stat st;
        snprintf(path, sizeof(path), "%s/%s", events_dir, days[d]);
        if (stat(path, &st) == 0)
            total += st.st_size;
    }
    printf("%d days, %.1f KiB in %s", ndays, total / 1024.0, events_dir);
    if (ndays)
        printf(" (%.10s .. %.10s)", days[0], days[ndays - 1]);
    putchar('\n');
    free_days(days, ndays);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2 || !strcmp(argv[1], "--help") || !strcmp(argv[1], "-h")) {
        usage();
        return argc < 2;
    }
    if (!strcmp(argv[1], "--version") || !strcmp(argv[1], "-V")) {
        printf("kistory %s\n", KISTORY_VERSION);
        return 0;
    }

    const char *xdg = getenv("XDG_DATA_HOME");
    const char *home = getenv("HOME");
    if (xdg && *xdg)
        snprintf(events_dir, sizeof(events_dir), "%.500s/kistory/events", xdg);
    else
        snprintf(events_dir, sizeof(events_dir), "%.500s/.local/share/kistory/events", home ? home : ".");
    const char *run = getenv("XDG_RUNTIME_DIR");
    const char *dsp = getenv("DISPLAY");
    const char *colon = dsp ? strrchr(dsp, ':') : NULL;
    snprintf(pause_path, sizeof(pause_path), "%.200s/kistory-paused.%d", run && *run ? run : "/tmp",
             colon ? atoi(colon + 1) : 0);

    const char *cmd = argv[1];
    if (!strcmp(cmd, "search"))
        return cmd_search(argc - 2, argv + 2);
    if (!strcmp(cmd, "day"))
        return cmd_day(argc > 2 ? argv[2] : NULL);
    if (!strcmp(cmd, "at"))
        return cmd_at(argc - 2, argv + 2);
    if (!strcmp(cmd, "stats"))
        return cmd_stats(argc - 2, argv + 2);
    if (!strcmp(cmd, "purge"))
        return cmd_purge(argc - 2, argv + 2);
    if (!strcmp(cmd, "pause"))
        return cmd_pause(argc > 2 ? argv[2] : NULL);
    if (!strcmp(cmd, "resume")) {
        unlink(pause_path);
        printf("kistory logging\n");
        return 0;
    }
    if (!strcmp(cmd, "status"))
        return cmd_status();
    fprintf(stderr, "kistory: unknown command %s\n", cmd);
    usage();
    return 2;
}
