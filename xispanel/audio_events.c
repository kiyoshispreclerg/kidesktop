/*
 * audio_events.c - toast feedback for volume/mute and default-device
 * changes, event-driven via `pactl subscribe` instead of polling.
 *
 * The subscribe child itself (spawn/respawn-with-backoff, line buffering,
 * "Event 'TYPE' on FACILITY #N" parsing) lives in
 * shared/xis_pactl_subscribe.c now, alongside xisserve's own use of it --
 * this file just supplies the callback and folds the fd into xispanel.c's
 * own select() loop (see audio_events_fd(), called every iteration like
 * sni_fd()) so reacting to an event costs nothing between events: no
 * timer, no re-querying pactl on a schedule.
 *
 * Deliberately narrow: only `change` events on `sink` (volume/mute) and
 * `server` (the default sink/source itself switching) are handled.
 * `new`/`remove` (hotplug) and source volume changes aren't -- this
 * covers the two things actually asked for (volume, and which device is
 * in use), not a general audio-event log.
 */
#include "xispanel.h"
#include "../shared/xis_pactl_subscribe.h"

#include <stdio.h>
#include <string.h>

static XisPactlSubscribe *g_sub = NULL;
static int g_was_running = 0; /* whether xis_pactl_subscribe_fd() reported a live child last call */

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

static void toast_volume_change(void);
static void toast_device_change(void);

/* Only reacts to the two (type, facility) pairs this file cares about;
 * everything else (new/remove, sink-input/source-output/module/client/card
 * facilities) is silently ignored -- see the file's own doc comment on
 * scope. */
static void on_pactl_event(const char *type, const char *facility, int index, void *user_data)
{
    (void)index;
    (void)user_data;
    if (strcmp(type, "change") != 0) {
        return;
    }
    if (!strcmp(facility, "sink")) {
        toast_volume_change();
    } else if (!strcmp(facility, "server")) {
        toast_device_change();
    }
}

int audio_events_fd(void)
{
    if (!pulse_available()) {
        return -1;
    }
    if (!g_sub) {
        g_sub = xis_pactl_subscribe_new(on_pactl_event, NULL);
    }
    int fd = xis_pactl_subscribe_fd(g_sub);
    /* Baseline right as the stream comes up, not lazily on the first
     * event -- otherwise the first genuine volume change after startup
     * would show a jump from "whatever pct happened to be sitting in a
     * -1-initialized variable" instead of the real prior value, and
     * (since pct never legitimately equals -1) would always look like a
     * change even when the event was for something else on the sink. */
    if (fd >= 0 && !g_was_running) {
        seed_baseline();
    }
    g_was_running = fd >= 0;
    return fd;
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

void audio_events_poll(void)
{
    xis_pactl_subscribe_poll(g_sub);
}
