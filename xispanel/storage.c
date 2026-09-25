/*
 * storage.c - removable block device snapshot for widgets/storage.c's
 * tooltip and storage_events.c's toast diffing, shelling out to
 * `lsblk -P` -- same "no udev/udisks2 client library" pattern xisserve's
 * own --storage page (xisserve/pages/storage.c) uses, duplicated here
 * rather than shared since xispanel is a separate binary/codebase (same
 * "duplicated on purpose" call notifications.c's own doc comment makes
 * for xisserve/xispanel's two independent small JSON/kv parsers).
 */
#include "xispanel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* One `KEY="value"` pair starting at or after *pp, value unescaped
 * (lsblk backslash-escapes '"'/'\\' inside -P output). Advances *pp past
 * it. Returns 0 once the line is exhausted. */
static int next_kv(const char **pp, char *key, size_t keysz, char *val, size_t valsz)
{
    const char *p = *pp;
    while (*p == ' ') {
        p++;
    }
    if (!*p) {
        *pp = p;
        return 0;
    }
    size_t ko = 0;
    while (*p && *p != '=' && ko + 1 < keysz) {
        key[ko++] = *p++;
    }
    key[ko] = 0;
    if (*p != '=' || p[1] != '"') {
        *pp = p;
        return 0;
    }
    p += 2;
    size_t vo = 0;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) {
            p++;
        }
        if (vo + 1 < valsz) {
            val[vo++] = *p;
        }
        p++;
    }
    val[vo] = 0;
    if (*p == '"') {
        p++;
    }
    *pp = p;
    return 1;
}

static void apply_kv(StorageDevice *d, char removable[8], char hotplug[8], const char *key, const char *val)
{
    if (!strcmp(key, "NAME")) {
        snprintf(d->name, sizeof(d->name), "%s", val);
    } else if (!strcmp(key, "PKNAME")) {
        snprintf(d->pkname, sizeof(d->pkname), "%s", val);
    } else if (!strcmp(key, "TYPE")) {
        snprintf(d->type, sizeof(d->type), "%s", val);
    } else if (!strcmp(key, "SIZE")) {
        snprintf(d->size, sizeof(d->size), "%s", val);
    } else if (!strcmp(key, "FSTYPE")) {
        snprintf(d->fstype, sizeof(d->fstype), "%s", val);
    } else if (!strcmp(key, "LABEL")) {
        snprintf(d->label, sizeof(d->label), "%s", val);
    } else if (!strcmp(key, "MOUNTPOINT")) {
        snprintf(d->mountpoint, sizeof(d->mountpoint), "%s", val);
    } else if (!strcmp(key, "RM")) {
        snprintf(removable, 8, "%s", val);
    } else if (!strcmp(key, "HOTPLUG")) {
        snprintf(hotplug, 8, "%s", val);
    }
}

int storage_list(StorageDevice *out, int max, int *out_count)
{
    *out_count = 0;
    FILE *f = popen("LC_ALL=C lsblk -P -o NAME,PKNAME,TYPE,SIZE,FSTYPE,LABEL,MOUNTPOINT,RM,HOTPLUG 2>/dev/null", "r");
    if (!f) {
        return 0;
    }
    char line[1024];
    while (*out_count < max && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = 0;
        if (!line[0]) {
            continue;
        }
        StorageDevice d;
        memset(&d, 0, sizeof(d));
        char removable[8] = "0", hotplug[8] = "0";
        const char *p = line;
        char key[32], val[PATH_MAX];
        while (next_kv(&p, key, sizeof(key), val, sizeof(val))) {
            apply_kv(&d, removable, hotplug, key, val);
        }
        int is_removable = !strcmp(removable, "1") || !strcmp(hotplug, "1");
        if (!is_removable) {
            continue;
        }
        if (!strcmp(d.type, "disk") && !d.fstype[0]) {
            continue; /* partitioned disk: its own partitions speak for it */
        }
        if (strcmp(d.type, "part") != 0 && strcmp(d.type, "disk") != 0) {
            continue;
        }
        out[(*out_count)++] = d;
    }
    pclose(f);
    return 1;
}
