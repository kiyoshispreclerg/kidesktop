/*
 * audio_events.c - toast feedback for volume/mute and default-device
 * changes, event-driven via `pactl subscribe` instead of polling.
 *
 * `pactl subscribe` runs as a long-lived child (popen(), same "shell out
 * to pactl" call pulse.c already makes -- nothing new to link) that
 * prints one line per PulseAudio/PipeWire-pulse event, e.g.:
 *
 *   Event 'change' on sink #0
 *   Event 'change' on server #0
 *
 * and keeps running until killed -- unlike every other pactl invocation
 * in this codebase, which is a one-shot popen()/pclose() pair. Its fd is
 * folded into xispanel.c's own select() loop (see audio_events_fd(),
 * called every iteration like sni_fd()) so reacting to an event costs
 * nothing between events: no timer, no re-querying pactl on a schedule.
 *
 * Deliberately narrow: only `change` events on `sink` (volume/mute) and
 * `server` (the default sink/source itself switching) are handled.
 * `new`/`remove` (hotplug) and source volume changes aren't -- this
 * covers the two things actually asked for (volume, and which device is
 * in use), not a general audio-event log.
 *
 * Never blocks the main loop: the child's stdout fd is set O_NONBLOCK
 * right after spawning, and audio_events_poll() only ever reads what's
 * already buffered, accumulating a partial line across calls rather than
 * assuming one read() lines up with one event -- a plain blocking
 * fgets() here would risk freezing the whole panel mid-line if pactl's
 * write happened to split a line across two TCP-like buffer chunks
 * (select() readiness only promises "at least one byte", not "one whole
 * line").
 */
#include "xispanel.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Respawn backoff after the subscribe child dies (EOF/error) or fails to
 * even start (pactl missing, no sound server yet at xispanel's own
 * startup) -- keeps a permanently-broken case from busy-looping popen()
 * every single main-loop iteration. */
#define AUDIO_EVENTS_RESPAWN_MS 5000
#define AUDIO_EVENTS_LINE_BUF 256

static FILE *g_sub = NULL;
static int g_sub_fd = -1;
static uint64_t g_next_spawn_ms = 0;
static char g_line_buf[AUDIO_EVENTS_LINE_BUF];
static size_t g_line_len = 0;

/* Last values *this file* has seen -- deliberately separate from the
 * `volume` widget's own VolumePriv state (widgets/volume.c): that struct
 * is private to whichever widget instances exist (zero, one, or several
 * across panels), while this needs exactly one shared "what did we last
 * tell the user" baseline regardless of how the panel is configured. */
static int g_last_pct = -1;
static int g_last_muted = -1;
static char g_last_sink_name[256] = "";
static char g_last_source_name[256] = "";

static void seed_baseline(void)
{
    pulse_get_sink_state("@DEFAULT_SINK@", &g_last_pct, &g_last_muted);
    pulse_get_default_sink_name(g_last_sink_name, sizeof(g_last_sink_name));
    pulse_get_default_source_name(g_last_source_name, sizeof(g_last_source_name));
}

static void stop_subscribe(void)
{
    if (g_sub) {
        pclose(g_sub);
        g_sub = NULL;
    }
    g_sub_fd = -1;
    g_line_len = 0;
    g_next_spawn_ms = now_ms() + AUDIO_EVENTS_RESPAWN_MS;
}

int audio_events_fd(void)
{
    if (g_sub) {
        return g_sub_fd;
    }
    if (!pulse_available()) {
        return -1;
    }
    uint64_t now = now_ms();
    if (now < g_next_spawn_ms) {
        return -1;
    }
    /* LC_ALL=C: same reasoning as every other pactl call in this
     * codebase (pulse.c's own doc comment) -- the "Event '...' on ..."
     * words this parses would otherwise come out translated under a
     * non-English locale. */
    g_sub = popen("LC_ALL=C pactl subscribe 2>/dev/null", "r");
    if (!g_sub) {
        g_next_spawn_ms = now + AUDIO_EVENTS_RESPAWN_MS;
        return -1;
    }
    g_sub_fd = fileno(g_sub);
    int flags = fcntl(g_sub_fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(g_sub_fd, F_SETFL, flags | O_NONBLOCK);
    }
    g_line_len = 0;
    /* Baseline right as the stream comes up, not lazily on the first
     * event -- otherwise the first genuine volume change after startup
     * would show a jump from "whatever pct happened to be sitting in a
     * -1-initialized variable" instead of the real prior value, and
     * (since pct never legitimately equals -1) would always look like a
     * change even when the event was for something else on the sink. */
    seed_baseline();
    return g_sub_fd;
}

static void toast_volume_change(void)
{
    int pct, muted;
    if (!pulse_get_sink_state("@DEFAULT_SINK@", &pct, &muted)) {
        return;
    }
    if (pct == g_last_pct && muted == g_last_muted) {
        return; /* this sink event was about something else (port, latency, ...) */
    }
    g_last_pct = pct;
    g_last_muted = muted;

    const char *icon_name = muted ? "volume-muted" : (pct < 34 ? "volume-low" : (pct < 67 ? "volume-medium" : "volume-high"));
    char summary[64];
    snprintf(summary, sizeof(summary), muted ? "Volume: mudo" : "Volume: %d%%", pct);
    toast_show_osd(xispanel_first_panel_icon(icon_name, 40), summary, NULL, muted ? 0 : pct, TOAST_URGENCY_NORMAL, 0,
                    "volume");
}

static void toast_device_change(void)
{
    char name[256];
    if (pulse_get_default_sink_name(name, sizeof(name)) && strcmp(name, g_last_sink_name) != 0) {
        snprintf(g_last_sink_name, sizeof(g_last_sink_name), "%s", name);
        char summary[300];
        snprintf(summary, sizeof(summary), "Saida de audio: %s", name);
        toast_show_osd(xispanel_first_panel_icon("audio-card", 40), summary, NULL, -1, TOAST_URGENCY_NORMAL, 0,
                        "audio-sink");
        /* The switch is also a real volume baseline change (a different
         * device very likely has a different level/mute) -- re-seeding
         * here avoids the *next* sink "change" event being compared
         * against the old device's now-meaningless pct/muted. */
        pulse_get_sink_state("@DEFAULT_SINK@", &g_last_pct, &g_last_muted);
    }
    if (pulse_get_default_source_name(name, sizeof(name)) && strcmp(name, g_last_source_name) != 0) {
        snprintf(g_last_source_name, sizeof(g_last_source_name), "%s", name);
        char summary[300];
        snprintf(summary, sizeof(summary), "Entrada de audio: %s", name);
        toast_show_osd(xispanel_first_panel_icon("audio-input-microphone", 40), summary, NULL, -1,
                        TOAST_URGENCY_NORMAL, 0, "audio-source");
    }
}

/* "Event '<type>' on <facility> #<index>" -> reacts to the two
 * (type, facility) pairs this file cares about; everything else
 * (new/remove, sink-input/source-output/module/client/card facilities)
 * is silently ignored -- see the file's own doc comment on scope. */
static void handle_event_line(const char *line)
{
    char type[16], facility[24];
    if (sscanf(line, "Event '%15[^']' on %23[^ #]", type, facility) != 2) {
        return;
    }
    if (strcmp(type, "change") != 0) {
        return;
    }
    if (!strcmp(facility, "sink")) {
        toast_volume_change();
    } else if (!strcmp(facility, "server")) {
        toast_device_change();
    }
}

void audio_events_poll(void)
{
    if (!g_sub) {
        return;
    }
    for (;;) {
        char chunk[128];
        ssize_t n = read(g_sub_fd, chunk, sizeof(chunk));
        if (n > 0) {
            for (ssize_t i = 0; i < n; i++) {
                if (chunk[i] == '\n') {
                    g_line_buf[g_line_len] = 0;
                    handle_event_line(g_line_buf);
                    g_line_len = 0;
                } else if (g_line_len + 1 < sizeof(g_line_buf)) {
                    g_line_buf[g_line_len++] = chunk[i];
                }
                /* A line too long for g_line_buf (never legitimately
                 * happens for this protocol) just gets silently
                 * truncated rather than desyncing the parser -- the
                 * overflow bytes are dropped until the next '\n'. */
            }
            if (n == (ssize_t)sizeof(chunk)) {
                continue; /* more may already be queued */
            }
            return;
        }
        if (n == 0) {
            stop_subscribe(); /* child exited: sound server restarted, pactl killed, etc. */
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return; /* fully drained for now */
        }
        if (errno == EINTR) {
            continue;
        }
        stop_subscribe();
        return;
    }
}
