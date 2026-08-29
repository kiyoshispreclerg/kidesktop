/*
 * pulse.h - audio backend for the --audio page, shelling out to `pactl`
 * rather than linking libpulse/libpipewire.
 *
 * Same reasoning (and the same fail-soft shape) as xispanel's own
 * pulse.c, which this extends rather than reinvents: `pactl` is bundled
 * with pulseaudio-utils *and* provided by pipewire-pulse, so one code
 * path covers both real PulseAudio and PipeWire-with-pulse-compat
 * systems, with no build-time dependency at all -- no headers, nothing
 * to link, no ifdef. libpulse's own client library isn't designed
 * around one-shot synchronous calls (it's a persistent async connection
 * plus a callback mainloop), so linking it would mean folding a chunk of
 * that mainloop into GTK's, for comparatively little benefit over
 * running `pactl`.
 *
 * If `pactl` isn't installed, or there's no sound server running for it
 * to talk to, pulse_available() reports 0 and the page renders an
 * explanatory placeholder instead of failing to open -- xisserve still
 * starts and every other view keeps working.
 *
 * Every invocation forces LC_ALL=C so the output is locale-independent
 * to parse (a pt_BR session otherwise prints "Mute: não" for "Mute:
 * no") -- the same trap xispanel's pulse.c documents having hit.
 */
#ifndef XISSERVE_PULSE_H
#define XISSERVE_PULSE_H

#include <glib.h>

typedef enum {
    PULSE_SINK,          /* output device */
    PULSE_SOURCE,        /* input device */
    PULSE_SINK_INPUT,    /* stream playing to a sink */
    PULSE_SOURCE_OUTPUT, /* stream recording from a source */
} PulseKind;

typedef struct {
    PulseKind kind;
    int index;          /* pactl object index -- how streams are addressed */
    char name[256];     /* sink/source name -- how devices are addressed (stable across restarts, unlike index) */
    char label[256];    /* human-readable: Description for devices, application.name for streams */
    char sublabel[256]; /* smaller second line: media.name for streams, "" for devices */
    char icon_name[128];/* application.icon_name, when the stream provides one */
    int volume_pct;
    gboolean muted;
    gboolean is_default;  /* devices only */
    gboolean suspended;   /* devices only: State: SUSPENDED */
    gboolean corked;      /* streams only: paused rather than actively playing */
} PulseEntry;

/* 1 if `pactl` exists and a sound server answered it. Probed once and
 * cached; pulse_invalidate_available() forces the next call to re-probe
 * (so starting PulseAudio/PipeWire after xisserve is already running
 * doesn't require restarting xisserve). */
int pulse_available(void);
void pulse_invalidate_available(void);

/* All four object kinds in one snapshot, in the order listed above.
 * Monitor sources (a sink's own loopback, name ending ".monitor") are
 * skipped -- they're neither a real capture device nor something a user
 * picks as a default input, and listing them roughly doubles the input
 * list with confusing near-duplicates of every output.
 * Returns a GPtrArray of owned PulseEntry* (free with
 * pulse_entries_free), never NULL -- an empty array if pactl is missing
 * or the server has nothing to report. */
GPtrArray *pulse_list(void);
void pulse_entries_free(GPtrArray *entries);

/* Absolute set, 0..150 -- clamped by pactl itself beyond the server's
 * own limits. Devices are addressed by name, streams by index. */
void pulse_set_volume(const PulseEntry *e, int pct);
void pulse_set_mute(const PulseEntry *e, gboolean muted);

/* Devices only; no-ops on streams. */
void pulse_set_default(const PulseEntry *e);
void pulse_set_suspended(const PulseEntry *e, gboolean suspended);

#endif
