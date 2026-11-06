/*
 * audio_streams.c - which applications are playing audio right now, for the
 * tasklist's speaker badge (and, later, its per-app mute).
 *
 * The data is `pactl list sink-inputs`, read through asyncmd.c like every
 * other external command here. It is not polled: audio_events.c forwards the
 * sink-input new/change/remove events of its `pactl subscribe` stream to
 * audio_streams_invalidate(), which makes the snapshot stale so the next run
 * is spawned right then. Until a widget has asked for the data once
 * (audio_streams_gen()), events cost nothing.
 *
 * A stream belongs to a window when the window's _NET_WM_PID is the stream's
 * process or one of its ancestors: Chromium plays from an audio-service child
 * of the browser process, which is the one owning the window.
 */
#include "xispanel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STREAMS_CMD "LC_ALL=C pactl list sink-inputs 2>/dev/null"
#define STREAMS_MAX 64
#define ANCESTORS_MAX 12

typedef struct {
    int index;
    int corked;
    int muted;
    unsigned long pid;
    unsigned long chain[ANCESTORS_MAX]; /* pid and its ancestors, 0-terminated */
} Stream;

static Stream g_streams[STREAMS_MAX];
static int g_n_streams;
static uint64_t g_parsed_gen;
static uint64_t g_local_gen;
static int g_wanted;

static unsigned long parent_of(unsigned long pid)
{
    char path[48], buf[512] = { 0 };
    snprintf(path, sizeof(path), "/proc/%lu/stat", pid);
    FILE *f = fopen(path, "r");
    if (!f) {
        return 0;
    }
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    /* "pid (comm) S ppid ...": comm may hold spaces and parens, so go from the last ')'. */
    const char *rp = strrchr(buf, ')');
    unsigned long ppid = 0;
    if (!rp || sscanf(rp + 1, " %*c %lu", &ppid) != 1) {
        return 0;
    }
    return ppid;
}

static void fill_chain(Stream *s)
{
    int n = 0;
    unsigned long pid = s->pid;
    while (pid > 1 && n < ANCESTORS_MAX - 1) {
        s->chain[n++] = pid;
        pid = parent_of(pid);
    }
    s->chain[n] = 0;
}

static const char *prop_value(const char *line, const char *key)
{
    while (*line == ' ' || *line == '\t') {
        line++;
    }
    size_t kl = strlen(key);
    if (strncmp(line, key, kl) != 0 || strncmp(line + kl, " = \"", 4) != 0) {
        return NULL;
    }
    return line + kl + 4;
}

static void parse(const char *text)
{
    g_n_streams = 0;
    Stream *cur = NULL;
    char line[512];
    const char *p = text;
    while (asyncmd_next_line(&p, line, sizeof(line))) {
        if (!strncmp(line, "Sink Input #", 12)) {
            if (cur) {
                fill_chain(cur);
            }
            cur = g_n_streams < STREAMS_MAX ? &g_streams[g_n_streams++] : NULL;
            if (cur) {
                memset(cur, 0, sizeof(*cur));
                cur->index = atoi(line + 12);
            }
            continue;
        }
        if (!cur) {
            continue;
        }
        const char *v;
        if (!strncmp(line, "\tCorked: ", 9)) {
            cur->corked = line[9] == 'y';
        } else if (!strncmp(line, "\tMute: ", 7)) {
            cur->muted = line[7] == 'y';
        } else if ((v = prop_value(line, "application.process.id"))) {
            cur->pid = strtoul(v, NULL, 10);
        }
    }
    if (cur) {
        fill_chain(cur);
    }
}

/* Refreshes the parsed table from asyncmd's latest snapshot. Returns a
 * counter that changes whenever the table does, 0 when audio isn't available. */
uint64_t audio_streams_gen(void)
{
    if (!pulse_available()) {
        return 0;
    }
    g_wanted = 1;
    const char *text;
    uint64_t gen = asyncmd_get(STREAMS_CMD, 3600u * 1000u, &text);
    if (gen != g_parsed_gen) {
        g_parsed_gen = gen;
        parse(text);
        g_local_gen++;
    }
    return g_local_gen;
}

int audio_streams_any(void)
{
    return g_n_streams > 0;
}

/* 0 = nothing playing for this process, 1 = playing, 2 = playing but muted
 * (every one of its uncorked streams is muted). */
int audio_streams_state_for_pid(unsigned long pid)
{
    if (!pid) {
        return 0;
    }
    int playing = 0, audible = 0;
    for (int i = 0; i < g_n_streams; i++) {
        const Stream *s = &g_streams[i];
        if (s->corked) {
            continue;
        }
        for (int k = 0; s->chain[k]; k++) {
            if (s->chain[k] == pid) {
                playing = 1;
                audible |= !s->muted;
                break;
            }
        }
    }
    return playing ? (audible ? 1 : 2) : 0;
}

void audio_streams_invalidate(void)
{
    if (!g_wanted) {
        return;
    }
    asyncmd_invalidate(STREAMS_CMD);
    const char *text;
    asyncmd_get(STREAMS_CMD, 3600u * 1000u, &text); /* spawns now unless a run is in flight */
}

/* Mutes every stream of `pid` (and its descendants) when any is audible,
 * unmutes them otherwise -- what the speaker badge's state says a click
 * should flip. The change comes back as a sink-input event and refreshes
 * the badge, so nothing is updated here. Corked streams are included: a
 * paused player keeps the mute when it resumes. */
void audio_streams_toggle_mute_for_pid(unsigned long pid)
{
    if (!pid) {
        return;
    }
    int mute = audio_streams_state_for_pid(pid) == 1;
    char cmd[STREAMS_MAX * 48];
    size_t len = 0;
    cmd[0] = 0;
    for (int i = 0; i < g_n_streams && len + 48 < sizeof(cmd); i++) {
        for (int k = 0; g_streams[i].chain[k]; k++) {
            if (g_streams[i].chain[k] == pid) {
                len += (size_t)snprintf(cmd + len, sizeof(cmd) - len, "pactl set-sink-input-mute %d %d; ",
                                        g_streams[i].index, mute);
                break;
            }
        }
    }
    if (len) {
        run_detached(cmd);
    }
}
