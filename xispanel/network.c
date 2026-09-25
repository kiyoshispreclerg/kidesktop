/*
 * network.c - connectivity summary for widgets/network.c, shelling out to
 * `nmcli` -- same "no libnm/dbus link, just the CLI" pattern pulse.c
 * already uses for pactl. Deliberately small: only what the widget's icon
 * and tooltip need (is there a connected device, what kind, its
 * connection name, wifi signal if applicable) -- the full device list and
 * wifi scan/connect UI lives in xisserve's own --network page
 * (xisserve/pages/network.c, a separate binary/codebase -- nothing here
 * is shared with it), which this widget's click opens.
 *
 * The `device status` call goes through asyncmd.c rather than popen():
 * this is the file that made that mechanism necessary in the first
 * place (see asyncmd.c's own doc comment) -- nmcli itself is fast
 * (~10ms), but nothing shelling out from a widget's on_tick may ever
 * block the main loop, no matter how fast it usually is.
 *
 * The signal strength, however, no longer shells out to `nmcli dev wifi
 * list` at all. That command asks NetworkManager to re-scan the radio
 * whenever its cached scan is older than ~30s, and the scan itself
 * measured 6.3 seconds on a USB wifi dongle -- the original stall this
 * whole mechanism exists to fix. Even rate-limited through asyncmd it
 * still meant forcing a real radio scan every 30s just to read a number,
 * which measurably hurt throughput on that same dongle. The kernel
 * already publishes per-interface signal quality at all times, no scan
 * needed, in /proc/net/wireless -- world-readable (mode 0444) like the
 * rest of /proc/net, and a plain synchronous read of it costs nowhere
 * near enough to need asyncmd.c's treatment (it's not shelling out to
 * anything, just parsing a kernel-maintained /proc file).
 */
#include "xispanel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NETWORK_STATUS_CMD "LC_ALL=C nmcli -t -f DEVICE,TYPE,STATE,CONNECTION device status"


/* Splits `line` in place on ':' into at most 4 fields (DEVICE, TYPE,
 * STATE, CONNECTION), the last one taking whatever's left -- so a
 * connection name containing ':' (unusual, but a user can name a profile
 * anything) still comes through whole instead of getting cut at the
 * first colon inside it. DEVICE/TYPE/STATE themselves never legitimately
 * contain ':', so this needs no escape handling the way xisserve's own
 * parser does for wifi SSIDs. */
static int split4(char *line, char *out[4])
{
    for (int i = 0; i < 3; i++) {
        out[i] = line;
        char *colon = strchr(line, ':');
        if (!colon) {
            return 0;
        }
        *colon = 0;
        line = colon + 1;
    }
    out[3] = line;
    return 1;
}

/* /proc/net/wireless's "link" column for `dev`, as a 0-100 percentage, or
 * -1 if `dev` has no entry there (not a wifi interface after all, or a
 * driver that never registered with the wireless-extensions compat layer
 * -- vanishingly rare among wifi drivers still in use, but every caller
 * here already treats -1 as "signal unknown" rather than an error, so
 * this degrades exactly as gracefully as the old nmcli-based lookup did
 * on any failure).
 *
 * Format, header included (see `man 5 proc`, or wireless.h in an old
 * wireless-tools source tree):
 *
 *   Inter-| sta-|   Quality        |   Discarded packets               | ...
 *    face | tus | link level noise |  nwid  crypt   frag  retry   misc | ...
 *   wlan0: 0000   61.  -60.  -256.       0      0      0      0      0   ...
 *
 * `link`'s trailing '.' is wireless-tools' historical stand-in for a
 * value flagged "updated" -- present on every normal reading, harmless to
 * atoi() either way since it just stops the number there. The scale
 * itself is fixed at 0-100 by the kernel's cfg80211 wext-compat layer for
 * every driver that goes through it (essentially all of them today:
 * mac80211-based drivers, which covers Intel/Realtek/Atheros/Broadcom/
 * MediaTek in-tree and out-of-tree alike) -- unlike raw RSSI or a
 * driver-private "quality" unit, so no further normalization is needed. */
static int wifi_link_quality(const char *dev)
{
    FILE *f = fopen("/proc/net/wireless", "r");
    if (!f) {
        return -1;
    }
    char line[256];
    int pct = -1;
    /* First two lines are the header shown above; every line after that
     * is one interface. A file with fewer than two lines (truncated?)
     * just falls through to the loop below finding nothing to match. */
    if (!fgets(line, sizeof(line), f) || !fgets(line, sizeof(line), f)) {
        fclose(f);
        return -1;
    }
    size_t dev_len = strlen(dev);
    while (fgets(line, sizeof(line), f)) {
        char *colon = strchr(line, ':');
        if (!colon) {
            continue;
        }
        size_t name_len = (size_t)(colon - line);
        /* Leading whitespace before the name is padding, not part of it
         * -- interface names never legitimately contain spaces. */
        char *name_start = line;
        while (name_len > 0 && *name_start == ' ') {
            name_start++;
            name_len--;
        }
        if (name_len != dev_len || strncmp(name_start, dev, dev_len) != 0) {
            continue;
        }
        int status;
        double link;
        if (sscanf(colon + 1, "%x %lf", &status, &link) == 2) {
            pct = (int)link;
            if (pct < 0) {
                pct = 0;
            } else if (pct > 100) {
                pct = 100;
            }
        }
        break;
    }
    fclose(f);
    return pct;
}

uint64_t network_get_summary(unsigned refresh_ms, char *type, size_t type_sz, char *name, size_t name_sz,
                              int *signal_pct, int *out_connected)
{
    *signal_pct = -1;
    *out_connected = 0;
    type[0] = 0;
    name[0] = 0;

    const char *text = "";
    uint64_t gen = asyncmd_get(NETWORK_STATUS_CMD, refresh_ms, &text);
    if (gen == 0) {
        return 0; /* first run hasn't finished yet -- nothing to report */
    }

    char line[512];
    int found = 0;
    char dev_name[64] = "";
    while (!found && asyncmd_next_line(&text, line, sizeof(line))) {
        if (!line[0]) {
            continue;
        }
        char *fields[4];
        if (!split4(line, fields)) {
            continue;
        }
        const char *dev = fields[0], *typ = fields[1], *state = fields[2], *conn = fields[3];
        if (strcmp(typ, "wifi") != 0 && strcmp(typ, "ethernet") != 0) {
            continue;
        }
        if (strncmp(dev, "p2p-dev-", 8) == 0) {
            continue;
        }
        if (strncmp(state, "connected", 9) != 0) {
            continue; /* "disconnected"/"unavailable"/"connecting..." -- not up */
        }
        snprintf(type, type_sz, "%s", typ);
        snprintf(name, name_sz, "%s", conn[0] ? conn : dev);
        snprintf(dev_name, sizeof(dev_name), "%s", dev);
        found = 1;
    }

    /* Ethernet has no signal to report (stays -1); wifi reads the
     * kernel's own live quality figure -- see wifi_link_quality()'s own
     * doc comment on why this no longer shells out to nmcli at all. */
    if (found && strcmp(type, "wifi") == 0) {
        *signal_pct = wifi_link_quality(dev_name);
    }

    *out_connected = found;
    return gen;
}
