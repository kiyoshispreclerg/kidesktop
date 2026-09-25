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
 * Both nmcli calls go through asyncmd.c rather than popen(): this is the
 * file that made that mechanism necessary. `nmcli dev wifi list` asks
 * NetworkManager to re-scan the radio whenever its cached scan is older
 * than ~30s, and the scan itself measured 6.3 seconds on a USB wifi
 * dongle -- straight popen()/fgets() here froze the entire panel for
 * those 6.3 seconds, roughly every 36. Nothing is parsed until a run has
 * actually completed, so every function below reads a snapshot string and
 * returns immediately.
 */
#include "xispanel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NETWORK_STATUS_CMD "LC_ALL=C nmcli -t -f DEVICE,TYPE,STATE,CONNECTION device status"
#define NETWORK_WIFI_CMD "LC_ALL=C nmcli -t -f IN-USE,SIGNAL dev wifi list"

/* The wifi signal refreshes far more slowly than the connectivity summary
 * it decorates, because of that rescan: it is no longer able to stall the
 * panel, but a rescan every few seconds still keeps the radio busy and
 * measurably hurts throughput on the dongle. Signal strength changing a
 * bar or two late is not worth that; which network is connected (the
 * cheap `device status` call, ~10ms) keeps the caller's own interval. */
#define NETWORK_SIGNAL_REFRESH_MS 30000

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
        found = 1;
    }

    /* Only ask for the signal once something wifi is actually up: on
     * ethernet or with nothing connected the rescan would cost the same
     * and answer a question nobody is asking. Its own snapshot may well
     * still be empty (gen 0) on the first passes -- *signal_pct then
     * stays -1, which the widget already draws as "connected, strength
     * unknown" rather than as a missing connection. */
    if (found && strcmp(type, "wifi") == 0) {
        const char *wtext = "";
        if (asyncmd_get(NETWORK_WIFI_CMD, NETWORK_SIGNAL_REFRESH_MS, &wtext) != 0) {
            char wline[128];
            while (asyncmd_next_line(&wtext, wline, sizeof(wline))) {
                if (wline[0] != '*') {
                    continue;
                }
                char *colon = strchr(wline, ':');
                if (colon) {
                    *signal_pct = atoi(colon + 1);
                }
                break;
            }
        }
    }

    *out_connected = found;
    return gen;
}
