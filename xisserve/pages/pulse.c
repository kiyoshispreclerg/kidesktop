/*
 * pulse.c - see pulse.h for why this shells out to `pactl` instead of
 * linking libpulse/libpipewire.
 *
 * Parsing note: `pactl list <kind>` emits one block per object, each
 * introduced by an unindented "Sink #3" / "Sink Input #63" header, then
 * one-tab "Key: value" fields, then a "Properties:" section of two-tab
 * `key = "value"` lines. Indentation is what separates the two field
 * syntaxes, so the parser keys off it rather than trying to guess from
 * the content -- a property value can itself contain ": " (media.name
 * of a browser tab, say), which would otherwise parse as a field.
 */
#include "pulse.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checked = 0;
static int g_available = 0;

void pulse_invalidate_available(void)
{
    g_checked = 0;
}

int pulse_available(void)
{
    if (g_checked) {
        return g_available;
    }
    g_checked = 1;
    /* `pactl info` rather than `pactl --version`: the binary existing
     * isn't enough here, since the whole page is useless without a
     * server actually answering. `info` fails (non-zero, no output) when
     * nothing is listening, which is exactly the case that should render
     * the "no sound server" placeholder rather than an empty mixer. */
    FILE *f = popen("LC_ALL=C pactl info 2>/dev/null", "r");
    if (!f) {
        g_available = 0;
        return 0;
    }
    char buf[256];
    g_available = fgets(buf, sizeof(buf), f) != NULL;
    pclose(f);
    return g_available;
}

static FILE *pactl_run(const char *args)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "LC_ALL=C pactl %s 2>/dev/null", args);
    return popen(cmd, "r");
}

static void pactl_run_fire(const char *args)
{
    FILE *f = pactl_run(args);
    if (f) {
        pclose(f);
    }
}

static void rstrip(char *s)
{
    size_t l = strlen(s);
    while (l > 0 && (s[l - 1] == '\n' || s[l - 1] == '\r' || s[l - 1] == ' ' || s[l - 1] == '\t')) {
        s[--l] = 0;
    }
}

/* First "NN%" in a Volume: line (e.g. "front-left: 65536 / 100% / 0.00
 * dB, front-right: ..."). Channels can differ slightly; one
 * representative percentage is what a single slider shows anyway. */
static int parse_volume_pct(const char *value)
{
    const char *pct = strchr(value, '%');
    if (!pct) {
        return -1;
    }
    const char *p = pct;
    while (p > value && isdigit((unsigned char)p[-1])) {
        p--;
    }
    if (p == pct) {
        return -1;
    }
    return atoi(p);
}

/* Reads the default sink/source names out of `pactl info` so listed
 * devices can be flagged -- neither name appears in `pactl list` output
 * itself. */
static void read_defaults(char *sink, size_t sink_sz, char *source, size_t source_sz)
{
    sink[0] = 0;
    source[0] = 0;
    FILE *f = pactl_run("info");
    if (!f) {
        return;
    }
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        rstrip(line);
        if (strncmp(line, "Default Sink: ", 14) == 0) {
            snprintf(sink, sink_sz, "%s", line + 14);
        } else if (strncmp(line, "Default Source: ", 16) == 0) {
            snprintf(source, source_sz, "%s", line + 16);
        }
    }
    pclose(f);
}

/* One `pactl list <listing>` pass, appending every block it finds to
 * `out` as a PulseEntry of kind `kind`. */
static void list_kind(const char *listing, PulseKind kind, const char *header, GPtrArray *out, const char *default_sink,
                       const char *default_source)
{
    char args[64];
    snprintf(args, sizeof(args), "list %s", listing);
    FILE *f = pactl_run(args);
    if (!f) {
        return;
    }

    PulseEntry *e = NULL;
    size_t header_len = strlen(header);
    char line[2048];
    while (fgets(line, sizeof(line), f)) {
        rstrip(line);

        if (strncmp(line, header, header_len) == 0) {
            /* New block. `header` already ends in the '#' ("Sink #"),
             * so header_len lands directly on the first digit -- no
             * further offset, which would eat that digit ("Sink #3"
             * parsing as 0, "Sink Input #63" as 3). */
            e = g_new0(PulseEntry, 1);
            e->kind = kind;
            e->index = atoi(line + header_len);
            e->volume_pct = -1;
            g_ptr_array_add(out, e);
            continue;
        }
        if (!e) {
            continue;
        }

        if (line[0] == '\t' && line[1] == '\t') {
            /* Properties section: key = "value" */
            char *eq = strstr(line, " = ");
            if (!eq) {
                continue;
            }
            *eq = 0;
            const char *key = line + 2;
            char *val = eq + 3;
            size_t vl = strlen(val);
            if (vl >= 2 && val[0] == '"' && val[vl - 1] == '"') {
                val[vl - 1] = 0;
                val++;
            }
            if (strcmp(key, "application.name") == 0 && !e->label[0]) {
                snprintf(e->label, sizeof(e->label), "%s", val);
            } else if (strcmp(key, "media.name") == 0) {
                snprintf(e->sublabel, sizeof(e->sublabel), "%s", val);
            } else if (strcmp(key, "application.icon_name") == 0) {
                snprintf(e->icon_name, sizeof(e->icon_name), "%s", val);
            } else if (strcmp(key, "application.process.binary") == 0 && !e->icon_name[0]) {
                /* Not an icon name as such, but a binary name resolves
                 * against the icon theme often enough (firefox, mpv,
                 * vlc...) to be a decent fallback when the app didn't
                 * set application.icon_name itself. */
                snprintf(e->icon_name, sizeof(e->icon_name), "%s", val);
            }
            continue;
        }

        if (line[0] != '\t') {
            continue;
        }

        /* One-tab field: "Key: value". */
        char *colon = strchr(line, ':');
        if (!colon) {
            continue;
        }
        *colon = 0;
        const char *key = line + 1;
        const char *val = colon + 1;
        while (*val == ' ') {
            val++;
        }

        if (strcmp(key, "Name") == 0) {
            snprintf(e->name, sizeof(e->name), "%s", val);
        } else if (strcmp(key, "Description") == 0) {
            snprintf(e->label, sizeof(e->label), "%s", val);
        } else if (strcmp(key, "Mute") == 0) {
            e->muted = strcmp(val, "yes") == 0;
        } else if (strcmp(key, "Corked") == 0) {
            e->corked = strcmp(val, "yes") == 0;
        } else if (strcmp(key, "State") == 0) {
            e->suspended = strcmp(val, "SUSPENDED") == 0;
        } else if (strcmp(key, "Volume") == 0) {
            /* Exact match only -- "Base Volume:" carries a percentage
             * too, and would overwrite the real one with the device's
             * reference level if it were matched loosely. */
            int pct = parse_volume_pct(val);
            if (pct >= 0 && e->volume_pct < 0) {
                e->volume_pct = pct;
            }
        }
    }
    pclose(f);

    /* Post-pass: fill in whatever the block didn't provide. Done here
     * rather than inline because a block's fields arrive in no
     * guaranteed order -- application.name can follow media.name, and
     * the default flags need the whole name to have been read. */
    for (guint i = 0; i < out->len; i++) {
        PulseEntry *p = g_ptr_array_index(out, i);
        if (p->kind != kind) {
            continue;
        }
        if (p->volume_pct < 0) {
            p->volume_pct = 0;
        }
        if (!p->label[0]) {
            snprintf(p->label, sizeof(p->label), "%s", p->sublabel[0] ? p->sublabel : p->name);
        }
        if (!p->label[0]) {
            snprintf(p->label, sizeof(p->label), "#%d", p->index);
        }
        if (kind == PULSE_SINK) {
            p->is_default = default_sink[0] && strcmp(p->name, default_sink) == 0;
        } else if (kind == PULSE_SOURCE) {
            p->is_default = default_source[0] && strcmp(p->name, default_source) == 0;
        }
    }
}

GPtrArray *pulse_list(void)
{
    GPtrArray *out = g_ptr_array_new();
    if (!pulse_available()) {
        return out;
    }

    char default_sink[256], default_source[256];
    read_defaults(default_sink, sizeof(default_sink), default_source, sizeof(default_source));

    list_kind("sinks", PULSE_SINK, "Sink #", out, default_sink, default_source);
    list_kind("sources", PULSE_SOURCE, "Source #", out, default_sink, default_source);
    list_kind("sink-inputs", PULSE_SINK_INPUT, "Sink Input #", out, default_sink, default_source);
    list_kind("source-outputs", PULSE_SOURCE_OUTPUT, "Source Output #", out, default_sink, default_source);

    /* Drop monitor sources -- see pulse.h. Done after listing rather
     * than during, since list_kind() is kind-agnostic. */
    for (guint i = out->len; i > 0; i--) {
        PulseEntry *e = g_ptr_array_index(out, i - 1);
        if (e->kind != PULSE_SOURCE) {
            continue;
        }
        size_t nl = strlen(e->name);
        if (nl >= 8 && strcmp(e->name + nl - 8, ".monitor") == 0) {
            g_free(e);
            g_ptr_array_remove_index(out, i - 1);
        }
    }
    return out;
}

void pulse_entries_free(GPtrArray *entries)
{
    if (!entries) {
        return;
    }
    for (guint i = 0; i < entries->len; i++) {
        g_free(g_ptr_array_index(entries, i));
    }
    g_ptr_array_free(entries, TRUE);
}

/* pactl's own setter name for each kind, plus how that kind is
 * addressed: devices by name (stable), streams by index (all they
 * have). */
static const char *setter_object(PulseKind kind)
{
    switch (kind) {
    case PULSE_SINK: return "sink";
    case PULSE_SOURCE: return "source";
    case PULSE_SINK_INPUT: return "sink-input";
    default: return "source-output";
    }
}

static void setter_target(const PulseEntry *e, char *out, size_t outsz)
{
    if (e->kind == PULSE_SINK || e->kind == PULSE_SOURCE) {
        snprintf(out, outsz, "%s", e->name);
    } else {
        snprintf(out, outsz, "%d", e->index);
    }
}

void pulse_set_volume(const PulseEntry *e, int pct)
{
    if (!pulse_available()) {
        return;
    }
    if (pct < 0) {
        pct = 0;
    }
    char target[256];
    setter_target(e, target, sizeof(target));
    char args[512];
    snprintf(args, sizeof(args), "set-%s-volume %s %d%%", setter_object(e->kind), target, pct);
    pactl_run_fire(args);
}

void pulse_set_mute(const PulseEntry *e, gboolean muted)
{
    if (!pulse_available()) {
        return;
    }
    char target[256];
    setter_target(e, target, sizeof(target));
    char args[512];
    snprintf(args, sizeof(args), "set-%s-mute %s %d", setter_object(e->kind), target, muted ? 1 : 0);
    pactl_run_fire(args);
}

/* Moves every existing stream of the matching kind onto `target`.
 *
 * `pactl list short <listing>` is one line per stream, tab-separated,
 * with the index first -- all this needs. (The verbose listing would do
 * too, but the short form is exactly one field deep.) */
static void move_all_streams(const char *listing, const char *mover, const char *target)
{
    char args[64];
    snprintf(args, sizeof(args), "list short %s", listing);
    FILE *f = pactl_run(args);
    if (!f) {
        return;
    }
    /* Indices are collected before any move, not moved as they're read:
     * moving a stream can change what a still-open listing reports, and
     * the pipe would be read while pactl is being run again underneath
     * it. */
    int indices[256];
    int n = 0;
    char line[512];
    while (n < (int)(sizeof(indices) / sizeof(indices[0])) && fgets(line, sizeof(line), f)) {
        char *end = NULL;
        long idx = strtol(line, &end, 10);
        if (end != line && idx >= 0) {
            indices[n++] = (int)idx;
        }
    }
    pclose(f);

    for (int i = 0; i < n; i++) {
        char move[512];
        snprintf(move, sizeof(move), "%s %d %s", mover, indices[i], target);
        pactl_run_fire(move);
    }
}

/* Setting the default device only decides where *future* streams land --
 * PulseAudio/PipeWire deliberately leave already-playing streams on
 * whatever device they were routed to (verified: after
 * set-default-sink, an existing sink-input keeps its old sink). On its
 * own that makes "make this the default output" look like it did
 * nothing, since the audio you can actually hear keeps coming out of
 * the old device.
 *
 * So this does what the desktop mixers do (and what the user means by
 * picking a default): set the default *and* move everything currently
 * playing/recording over to it. The move also updates
 * module-stream-restore's per-application memory, so those apps keep
 * using the new device next time rather than snapping back. */
void pulse_set_default(const PulseEntry *e)
{
    if (!pulse_available() || (e->kind != PULSE_SINK && e->kind != PULSE_SOURCE)) {
        return;
    }
    char args[512];
    snprintf(args, sizeof(args), "set-default-%s %s", setter_object(e->kind), e->name);
    pactl_run_fire(args);

    if (e->kind == PULSE_SINK) {
        move_all_streams("sink-inputs", "move-sink-input", e->name);
    } else {
        move_all_streams("source-outputs", "move-source-output", e->name);
    }
}

/* "Disable" for a device means suspending it, not tearing down its card
 * profile: suspend is per-device, instantly reversible, and doesn't
 * disturb the other devices sharing the same card the way switching
 * that card to profile "off" would. */
void pulse_set_suspended(const PulseEntry *e, gboolean suspended)
{
    if (!pulse_available() || (e->kind != PULSE_SINK && e->kind != PULSE_SOURCE)) {
        return;
    }
    char args[512];
    snprintf(args, sizeof(args), "suspend-%s %s %d", setter_object(e->kind), e->name, suspended ? 1 : 0);
    pactl_run_fire(args);
}
