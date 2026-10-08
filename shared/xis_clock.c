/* xis_clock - see xis_clock.h. */
#include "xis_clock.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static void config_dir(char *out, size_t outsz)
{
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) {
        snprintf(out, outsz, "%s", xdg);
        return;
    }
    const char *home = getenv("HOME");
    snprintf(out, outsz, "%s/.config", home ? home : "/tmp");
}

void xis_clock_path(char *out, size_t outsz)
{
    char dir[PATH_MAX - 32];
    config_dir(dir, sizeof(dir));
    snprintf(out, outsz, "%s/ki-clock.conf", dir);
}

long long xis_clock_now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* Splits line in place on tabs; the last field keeps any further tabs
 * (there are none after a save -- labels are sanitized). */
static int split_tabs(char *line, char **f, int max)
{
    int n = 0;
    f[n++] = line;
    while (n < max) {
        char *t = strchr(f[n - 1], '\t');
        if (!t) break;
        *t = 0;
        f[n++] = t + 1;
    }
    return n;
}

static unsigned parse_days(const char *s)
{
    unsigned days = 0;
    for (; *s; s++) {
        if (*s >= '0' && *s <= '6') days |= 1u << (*s - '0');
    }
    return days;
}

int xis_clock_load(XisClock *c)
{
    memset(c, 0, sizeof(*c));
    char path[PATH_MAX];
    xis_clock_path(path, sizeof(path));
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;
    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        size_t l = strlen(line);
        while (l > 0 && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
        if (!line[0] || line[0] == '#') continue;
        char *f[8];
        int n = split_tabs(line, f, 8);
        if (!strcmp(f[0], "ALARM") && n == 8 && c->nalarms < XIS_CLOCK_MAX_ALARMS) {
            XisAlarm *a = &c->alarms[c->nalarms];
            if (sscanf(f[2], "%d:%d", &a->hour, &a->minute) != 2) continue;
            if (a->hour < 0 || a->hour > 23 || a->minute < 0 || a->minute > 59) continue;
            a->id = atoi(f[1]);
            a->days = parse_days(f[3]);
            a->once_at = atoll(f[4]);
            a->enabled = atoi(f[5]) != 0;
            a->snooze_until = atoll(f[6]);
            snprintf(a->label, sizeof(a->label), "%s", f[7]);
            c->nalarms++;
        } else if (!strcmp(f[0], "TIMER") && n == 6 && c->ntimers < XIS_CLOCK_MAX_TIMERS) {
            XisTimer *t = &c->timers[c->ntimers];
            t->id = atoi(f[1]);
            t->duration_ms = atoll(f[2]);
            t->end_ms = atoll(f[3]);
            t->remaining_ms = atoll(f[4]);
            if (t->duration_ms <= 0) continue;
            snprintf(t->label, sizeof(t->label), "%s", f[5]);
            c->ntimers++;
        } else if (!strcmp(f[0], "STOPWATCH") && n == 3) {
            c->sw.start_ms = atoll(f[1]);
            c->sw.accum_ms = atoll(f[2]);
        } else if (!strcmp(f[0], "LAP") && n == 2 && c->sw.nlaps < XIS_CLOCK_MAX_LAPS) {
            c->sw.laps[c->sw.nlaps++] = atoll(f[1]);
        }
    }
    fclose(fp);
    return 1;
}

/* Tabs/newlines would break the line format. */
static void put_label(FILE *fp, const char *s)
{
    for (; *s; s++) fputc((*s == '\t' || *s == '\n' || *s == '\r') ? ' ' : *s, fp);
}

int xis_clock_save(const XisClock *c)
{
    char dir[PATH_MAX - 32];
    config_dir(dir, sizeof(dir));
    mkdir(dir, 0700);
    char path[PATH_MAX], tmp[PATH_MAX + 8];
    xis_clock_path(path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *fp = fopen(tmp, "w");
    if (!fp) return -1;
    for (int i = 0; i < c->nalarms; i++) {
        const XisAlarm *a = &c->alarms[i];
        char days[8];
        int nd = 0;
        for (int d = 0; d < 7; d++) {
            if (a->days & (1u << d)) days[nd++] = (char)('0' + d);
        }
        if (!nd) days[nd++] = '-';
        days[nd] = 0;
        fprintf(fp, "ALARM\t%d\t%02d:%02d\t%s\t%lld\t%d\t%lld\t", a->id, a->hour, a->minute, days, a->once_at,
                a->enabled ? 1 : 0, a->snooze_until);
        put_label(fp, a->label);
        fputc('\n', fp);
    }
    for (int i = 0; i < c->ntimers; i++) {
        const XisTimer *t = &c->timers[i];
        fprintf(fp, "TIMER\t%d\t%lld\t%lld\t%lld\t", t->id, t->duration_ms, t->end_ms, t->remaining_ms);
        put_label(fp, t->label);
        fputc('\n', fp);
    }
    fprintf(fp, "STOPWATCH\t%lld\t%lld\n", c->sw.start_ms, c->sw.accum_ms);
    for (int i = 0; i < c->sw.nlaps; i++) fprintf(fp, "LAP\t%lld\n", c->sw.laps[i]);
    int bad = ferror(fp);
    if (fclose(fp) != 0 || bad) {
        unlink(tmp);
        return -1;
    }
    return rename(tmp, path) == 0 ? 0 : -1;
}

long long xis_alarm_next_occurrence(int hour, int minute, unsigned days, long long after)
{
    time_t base = (time_t)after;
    struct tm day;
    localtime_r(&base, &day);
    /* 8 days: today's time may already be past, and the only matching
     * weekday may be today's, a week from now. */
    for (int off = 0; off <= 7; off++) {
        struct tm t = day;
        t.tm_mday += off;
        t.tm_hour = hour;
        t.tm_min = minute;
        t.tm_sec = 0;
        t.tm_isdst = -1;
        time_t when = mktime(&t);
        if (when == (time_t)-1 || (long long)when <= after) continue;
        if (days && !(days & (1u << t.tm_wday))) continue;
        return (long long)when;
    }
    return 0;
}

long long xis_alarm_next(const XisAlarm *a, long long after)
{
    if (!a->enabled) return 0;
    long long next = a->days ? xis_alarm_next_occurrence(a->hour, a->minute, a->days, after)
                             : (a->once_at > after ? a->once_at : 0);
    if (a->snooze_until > after && (!next || a->snooze_until < next)) next = a->snooze_until;
    return next;
}

long long xis_stopwatch_elapsed(const XisStopwatch *sw, long long now_ms)
{
    long long e = sw->accum_ms;
    if (sw->start_ms > 0 && now_ms > sw->start_ms) e += now_ms - sw->start_ms;
    return e;
}

long long xis_timer_remaining(const XisTimer *t, long long now_ms)
{
    long long r = t->end_ms > 0 ? t->end_ms - now_ms : t->remaining_ms;
    return r > 0 ? r : 0;
}

XisAlarm *xis_clock_find_alarm(XisClock *c, int id)
{
    for (int i = 0; i < c->nalarms; i++) {
        if (c->alarms[i].id == id) return &c->alarms[i];
    }
    return NULL;
}

XisTimer *xis_clock_find_timer(XisClock *c, int id)
{
    for (int i = 0; i < c->ntimers; i++) {
        if (c->timers[i].id == id) return &c->timers[i];
    }
    return NULL;
}

XisAlarm *xis_clock_add_alarm(XisClock *c)
{
    if (c->nalarms >= XIS_CLOCK_MAX_ALARMS) return NULL;
    int id = 1;
    for (int i = 0; i < c->nalarms; i++) {
        if (c->alarms[i].id >= id) id = c->alarms[i].id + 1;
    }
    XisAlarm *a = &c->alarms[c->nalarms++];
    memset(a, 0, sizeof(*a));
    a->id = id;
    return a;
}

XisTimer *xis_clock_add_timer(XisClock *c)
{
    if (c->ntimers >= XIS_CLOCK_MAX_TIMERS) return NULL;
    int id = 1;
    for (int i = 0; i < c->ntimers; i++) {
        if (c->timers[i].id >= id) id = c->timers[i].id + 1;
    }
    XisTimer *t = &c->timers[c->ntimers++];
    memset(t, 0, sizeof(*t));
    t->id = id;
    return t;
}

void xis_clock_remove_alarm(XisClock *c, int id)
{
    for (int i = 0; i < c->nalarms; i++) {
        if (c->alarms[i].id == id) {
            memmove(&c->alarms[i], &c->alarms[i + 1], (size_t)(c->nalarms - i - 1) * sizeof(c->alarms[0]));
            c->nalarms--;
            return;
        }
    }
}

void xis_clock_remove_timer(XisClock *c, int id)
{
    for (int i = 0; i < c->ntimers; i++) {
        if (c->timers[i].id == id) {
            memmove(&c->timers[i], &c->timers[i + 1], (size_t)(c->ntimers - i - 1) * sizeof(c->timers[0]));
            c->ntimers--;
            return;
        }
    }
}
