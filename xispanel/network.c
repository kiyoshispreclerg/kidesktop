/*
 * network.c - connectivity summary for widgets/network.c, shelling out to
 * `nmcli` -- same "no libnm/dbus link, just the CLI" pattern pulse.c
 * already uses for pactl. Deliberately small: only what the widget's icon
 * and tooltip need (is there a connected device, what kind, its
 * connection name, wifi signal if applicable) -- the full device list and
 * wifi scan/connect UI lives in xisserve's own --network page
 * (xisserve/pages/network.c, a separate binary/codebase -- nothing here
 * is shared with it), which this widget's click opens.
 */
#include "xispanel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void rstrip(char *s)
{
    size_t l = strlen(s);
    while (l > 0 && (s[l - 1] == '\n' || s[l - 1] == '\r')) {
        s[--l] = 0;
    }
}

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

int network_get_summary(char *type, size_t type_sz, char *name, size_t name_sz, int *signal_pct)
{
    *signal_pct = -1;
    type[0] = 0;
    name[0] = 0;

    FILE *f = popen("LC_ALL=C nmcli -t -f DEVICE,TYPE,STATE,CONNECTION device status 2>/dev/null", "r");
    if (!f) {
        return 0;
    }
    char line[512];
    int found = 0;
    while (!found && fgets(line, sizeof(line), f)) {
        rstrip(line);
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
    pclose(f);

    if (found && strcmp(type, "wifi") == 0) {
        FILE *wf = popen("LC_ALL=C nmcli -t -f IN-USE,SIGNAL dev wifi list 2>/dev/null", "r");
        if (wf) {
            char wline[128];
            while (fgets(wline, sizeof(wline), wf)) {
                rstrip(wline);
                if (wline[0] != '*') {
                    continue;
                }
                char *colon = strchr(wline, ':');
                if (colon) {
                    *signal_pct = atoi(colon + 1);
                }
                break;
            }
            pclose(wf);
        }
    }

    return found;
}
