/* ks_audio - see ks_audio.h. */
#include "ks_audio.h"
#include "ks_config.h"
#include "ks_log.h"
#include "../shared/xis_pactl_subscribe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MIN_PERIOD_S 2
#define MAX_STREAMS 64

typedef struct {
    int rec;              /* 0 sink input (playing), 1 source output (recording) */
    int idx;
    int corked;
    time_t start;         /* 0: not running */
    char app[64], binary[64], media[160];
    int pid;
} Stream;

static Stream streams[MAX_STREAMS];
static int nstreams;
static XisPactlSubscribe *sub;

static void period_end(Stream *s, time_t now)
{
    if (!s->start)
        return;
    time_t start = s->start;
    s->start = 0;
    if (now - start < MIN_PERIOD_S || ks_excluded(s->app, s->app, s->binary))
        return;
    char range[64], detail[224];
    ks_log_range(range, sizeof(range), start, now);
    int hidden = ks_titles_hidden(s->app, s->app, s->binary, NULL);
    snprintf(detail, sizeof(detail), "%s%spid=%d", hidden ? "" : s->media, hidden || !s->media[0] ? "" : " ",
             s->pid);
    ks_log_event(start, s->rec ? "audio_rec" : "audio_play", s->app, s->binary, -1, "", range, detail);
}

static Stream *stream_find(int rec, int idx)
{
    for (int i = 0; i < nstreams; i++)
        if (streams[i].rec == rec && streams[i].idx == idx)
            return &streams[i];
    return NULL;
}

static void stream_remove(int rec, int idx)
{
    Stream *s = stream_find(rec, idx);
    if (!s)
        return;
    period_end(s, time(NULL));
    *s = streams[--nstreams];
}

static void prop_value(const char *line, const char *key, char *out, size_t outsz)
{
    const char *p = strstr(line, key);
    if (!p)
        return;
    p = strchr(p, '"');
    if (!p)
        return;
    p++;
    size_t n = strcspn(p, "\"");
    snprintf(out, outsz, "%.*s", (int)(n < outsz ? n : outsz - 1), p);
}

/* Re-reads every stream of one kind (only_idx >= 0: just that one) and
 * applies corked/running transitions. */
static void refresh(int rec, int only_idx)
{
    FILE *f = popen(rec ? "LC_ALL=C pactl list source-outputs 2>/dev/null"
                        : "LC_ALL=C pactl list sink-inputs 2>/dev/null", "r");
    if (!f)
        return;
    const char *header = rec ? "Source Output #" : "Sink Input #";
    char line[1024];
    Stream cur = { 0 };
    int have = 0;
    time_t now = time(NULL);

    for (;;) {
        char *got = fgets(line, sizeof(line), f);
        if (!got || !strncmp(line, header, strlen(header))) {
            /* Close the block just read. */
            if (have && (only_idx < 0 || cur.idx == only_idx) && strcmp(cur.media, "Peak detect") != 0) {
                Stream *s = stream_find(rec, cur.idx);
                if (!s && nstreams < MAX_STREAMS) {
                    s = &streams[nstreams++];
                    *s = cur;
                    s->start = 0;
                }
                if (s) {
                    snprintf(s->app, sizeof(s->app), "%s", cur.app);
                    snprintf(s->binary, sizeof(s->binary), "%s", cur.binary);
                    snprintf(s->media, sizeof(s->media), "%s", cur.media);
                    s->pid = cur.pid;
                    s->corked = cur.corked;
                    if (cur.corked)
                        period_end(s, now);
                    else if (!s->start)
                        s->start = now;
                }
            }
            if (!got)
                break;
            memset(&cur, 0, sizeof(cur));
            cur.rec = rec;
            cur.idx = atoi(line + strlen(header));
            have = 1;
            continue;
        }
        if (!have)
            continue;
        char *t = line;
        while (*t == ' ' || *t == '\t')
            t++;
        if (!strncmp(t, "Corked:", 7)) {
            cur.corked = strstr(t, "yes") != NULL;
        } else if (!strncmp(t, "application.name ", 17)) {
            prop_value(t, "application.name", cur.app, sizeof(cur.app));
        } else if (!strncmp(t, "application.process.binary ", 27)) {
            prop_value(t, "application.process.binary", cur.binary, sizeof(cur.binary));
        } else if (!strncmp(t, "application.process.id ", 23)) {
            char pid[16] = "";
            prop_value(t, "application.process.id", pid, sizeof(pid));
            cur.pid = atoi(pid);
        } else if (!strncmp(t, "media.name ", 11)) {
            prop_value(t, "media.name", cur.media, sizeof(cur.media));
        }
    }
    pclose(f);
}

static void on_event(const char *type, const char *facility, int index, void *user_data)
{
    (void)user_data;
    int rec;
    if (!strcmp(facility, "sink-input"))
        rec = 0;
    else if (!strcmp(facility, "source-output"))
        rec = 1;
    else
        return;
    if (!strcmp(type, "remove"))
        stream_remove(rec, index);
    else if (index >= 0)
        refresh(rec, index);
}

int ks_audio_init(void)
{
    sub = xis_pactl_subscribe_new(on_event, NULL);
    refresh(0, -1);
    refresh(1, -1);
    return sub != NULL;
}

int ks_audio_fd(void)
{
    return sub ? xis_pactl_subscribe_fd(sub) : -1;
}

void ks_audio_handle(void)
{
    if (sub)
        xis_pactl_subscribe_poll(sub);
}

void ks_audio_flush(void)
{
    time_t now = time(NULL);
    for (int i = 0; i < nstreams; i++)
        period_end(&streams[i], now);
}
