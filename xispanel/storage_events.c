/*
 * storage_events.c - toast feedback for removable-device hotplug/mount
 * state, polling `lsblk` (via storage.c's storage_list()) instead of
 * event-driven `udisksctl monitor`: unlike `pactl subscribe`'s one-line-
 * per-event stream (audio_events.c), `udisksctl monitor`'s output is
 * multi-line D-Bus PropertiesChanged dumps with no single line carrying
 * "device X unmounted" -- reliably parsing it needs tracking which
 * object path a block of indented property lines belongs to, for a
 * class of event (someone plugging in a USB stick) that isn't remotely
 * latency-sensitive the way volume/mute is. A snapshot diff every few
 * seconds is far simpler and just as good here.
 *
 * xispanel.c's main loop calls storage_events_poll(now) every iteration,
 * same call shape as notifd_poll() -- no fd to wire in (see
 * STORAGE_EVENTS_POLL_MS below, checked internally, same pattern
 * widgets' own on_tick uses via next_tick_ms, just not tied to any one
 * widget instance so the toast fires once regardless of how many
 * `storage` widgets exist across panels -- see audio_events.c's own
 * doc comment for why that separation matters).
 */
#include "xispanel.h"

#include <string.h>

#define STORAGE_EVENTS_POLL_MS 2000
#define STORAGE_EVENTS_MAX_DEVICES 32

static uint64_t g_next_poll_ms = 0;
static StorageDevice g_last[STORAGE_EVENTS_MAX_DEVICES];
static int g_last_count = 0;
static int g_seeded = 0;
static uint64_t g_last_gen = 0;

static const StorageDevice *find_by_name(const StorageDevice *list, int count, const char *name)
{
    for (int i = 0; i < count; i++) {
        if (!strcmp(list[i].name, name)) {
            return &list[i];
        }
    }
    return NULL;
}

/* Whether any sibling of `dev` (same PKNAME, or `dev` itself if it has no
 * PKNAME -- i.e. is its own whole-disk filesystem) is still mounted in
 * `list` -- the "safe to physically remove" check for the toast below. */
static int any_sibling_mounted(const StorageDevice *list, int count, const StorageDevice *dev)
{
    const char *disk = dev->pkname[0] ? dev->pkname : dev->name;
    for (int i = 0; i < count; i++) {
        const char *other_disk = list[i].pkname[0] ? list[i].pkname : list[i].name;
        if (!strcmp(other_disk, disk) && list[i].mountpoint[0]) {
            return 1;
        }
    }
    return 0;
}

void storage_events_poll(uint64_t now)
{
    if (now < g_next_poll_ms) {
        return;
    }
    g_next_poll_ms = now + STORAGE_EVENTS_POLL_MS;

    StorageDevice cur[STORAGE_EVENTS_MAX_DEVICES];
    int cur_count = 0;
    uint64_t gen = storage_list(STORAGE_EVENTS_POLL_MS, cur, STORAGE_EVENTS_MAX_DEVICES, &cur_count);
    if (gen == 0) {
        /* First `lsblk` run still in flight. Critically NOT the same as
         * "no devices attached": seeding the baseline from an empty
         * snapshot here would make every already-plugged device look
         * newly arrived the moment the real one lands, toasting them all
         * at startup. */
        return;
    }
    if (gen == g_last_gen) {
        return; /* same snapshot as last time -- nothing can have changed */
    }
    g_last_gen = gen;

    if (!g_seeded) {
        /* First poll after startup: seed the baseline silently -- every
         * device already plugged in at xispanel's launch is not "new",
         * and none of them just got unmounted just now either. Same
         * reasoning as audio_events.c's seed_baseline(). */
        memcpy(g_last, cur, sizeof(StorageDevice) * (size_t)cur_count);
        g_last_count = cur_count;
        g_seeded = 1;
        return;
    }

    /* New devices: in `cur` but not in the old snapshot at all. */
    for (int i = 0; i < cur_count; i++) {
        if (find_by_name(g_last, g_last_count, cur[i].name)) {
            continue;
        }
        const char *label = cur[i].label[0] ? cur[i].label : cur[i].name;
        char summary[300];
        snprintf(summary, sizeof(summary), "Novo dispositivo: %s (%s)", label, cur[i].size);
        toast_show_osd(xispanel_first_panel_icon("drive-removable-media", 40), summary, NULL, -1,
                        TOAST_URGENCY_NORMAL, 0, "storage-new");
    }

    /* Unmounted (and, once nothing else on the same disk is still
     * mounted, safe to physically remove): mountpoint was non-empty in
     * the old snapshot, empty now. */
    for (int i = 0; i < cur_count; i++) {
        if (cur[i].mountpoint[0]) {
            continue; /* still mounted */
        }
        const StorageDevice *prev = find_by_name(g_last, g_last_count, cur[i].name);
        if (!prev || !prev->mountpoint[0]) {
            continue; /* wasn't mounted before either -- nothing changed */
        }
        if (any_sibling_mounted(cur, cur_count, &cur[i])) {
            continue; /* another partition of the same disk is still up -- not safe yet */
        }
        const char *label = cur[i].label[0] ? cur[i].label : cur[i].name;
        char summary[300];
        snprintf(summary, sizeof(summary), "%s desmontado \xe2\x80\x94 pode remover com seguran\xc3\xa7""a", label);
        toast_show_osd(xispanel_first_panel_icon("drive-removable-media", 40), summary, NULL, -1,
                        TOAST_URGENCY_NORMAL, 0, "storage-safe-remove");
    }

    memcpy(g_last, cur, sizeof(StorageDevice) * (size_t)cur_count);
    g_last_count = cur_count;
}
