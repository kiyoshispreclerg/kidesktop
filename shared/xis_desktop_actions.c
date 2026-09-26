#include "xis_desktop_actions.h"

#include <stdio.h>
#include <string.h>
#include <limits.h>

/* Reads a single top-level [Desktop Entry] key's raw value out of a
 * .desktop file -- shared by xis_desktop_load_actions() (Actions=/each
 * action group's own Name=+Exec=) and xis_desktop_build_exec_with_file()
 * (Exec=) below, both of which only need one or two fields rather than a
 * full multi-group parse. `group` is the exact "[Group Name]" line to
 * read from ("[Desktop Entry]" for the main group, "[Desktop Action
 * <token>]" for one jumplist action). */
static int xis_desktop_read_group_key(const char *path, const char *group, const char *key, char *out, size_t outsz)
{
    out[0] = 0;
    FILE *f = fopen(path, "r");
    if (!f) {
        return 0;
    }
    char line[2048];
    int in_group = 0, seen_group = 0, found = 0;
    while (fgets(line, sizeof(line), f)) {
        size_t l = strlen(line);
        while (l > 0 && (line[l - 1] == '\n' || line[l - 1] == '\r')) {
            line[--l] = 0;
        }
        if (line[0] == '[') {
            in_group = strcmp(line, group) == 0;
            if (in_group) {
                seen_group = 1;
            } else if (seen_group) {
                break; /* left the group after having read it */
            }
            continue;
        }
        if (!in_group) {
            continue;
        }
        char *eq = strchr(line, '=');
        if (!eq) {
            continue;
        }
        *eq = 0;
        if (strcmp(line, key) == 0) {
            snprintf(out, outsz, "%s", eq + 1);
            found = 1;
        }
    }
    fclose(f);
    return found;
}

/* Drops %f/%F/%u/%U/%i/%c/%k/etc field codes per the .desktop spec (a
 * jumplist action's own Exec= carries no file to substitute), keeps a
 * literal %% as a single %. */
static void xis_desktop_strip_field_codes(const char *in, char *out, size_t outsz)
{
    size_t o = 0;
    for (const char *p = in; *p && o + 1 < outsz; p++) {
        if (*p == '%' && p[1]) {
            if (p[1] == '%' && o + 1 < outsz) {
                out[o++] = '%';
            }
            p++;
            continue;
        }
        out[o++] = *p;
    }
    out[o] = 0;
}

/* Single-quotes `in` for safe use inside an `sh -c` command string
 * (embedded single quotes become '\''). */
static void xis_desktop_shell_quote(const char *in, char *out, size_t outsz)
{
    size_t o = 0;
    if (o + 1 < outsz) {
        out[o++] = '\'';
    }
    for (const char *p = in; *p && o + 1 < outsz; p++) {
        if (*p == '\'') {
            const char *rep = "'\\''";
            for (const char *r = rep; *r && o + 1 < outsz; r++) {
                out[o++] = *r;
            }
        } else {
            out[o++] = *p;
        }
    }
    if (o + 1 < outsz) {
        out[o++] = '\'';
    }
    out[o] = 0;
}

int xis_desktop_load_actions(const char *desktop_path, char out_names[][128], char out_execs[][512], int max)
{
    if (!desktop_path || !desktop_path[0] || max <= 0) {
        return 0;
    }

    char actions_raw[512];
    if (!xis_desktop_read_group_key(desktop_path, "[Desktop Entry]", "Actions", actions_raw, sizeof(actions_raw)) ||
        !actions_raw[0]) {
        return 0;
    }

    int n = 0;
    char *save = NULL;
    for (char *tok = strtok_r(actions_raw, ";", &save); tok && n < max; tok = strtok_r(NULL, ";", &save)) {
        char group[96];
        snprintf(group, sizeof(group), "[Desktop Action %s]", tok);

        char name[128], exec_raw[512];
        int have_name = xis_desktop_read_group_key(desktop_path, group, "Name", name, sizeof(name));
        int have_exec = xis_desktop_read_group_key(desktop_path, group, "Exec", exec_raw, sizeof(exec_raw));
        if (have_name && name[0] && have_exec && exec_raw[0]) {
            snprintf(out_names[n], 128, "%s", name);
            xis_desktop_strip_field_codes(exec_raw, out_execs[n], 512);
            n++;
        }
    }
    return n;
}

int xis_desktop_build_exec_with_file(const char *desktop_path, const char *file_path, char *out, size_t outsz)
{
    char exec_raw[1024];
    if (!xis_desktop_read_group_key(desktop_path, "[Desktop Entry]", "Exec", exec_raw, sizeof(exec_raw)) ||
        !exec_raw[0]) {
        return 0;
    }

    char quoted[PATH_MAX + 4];
    xis_desktop_shell_quote(file_path, quoted, sizeof(quoted));
    size_t ql = strlen(quoted);

    size_t o = 0;
    int inserted = 0;
    for (const char *p = exec_raw; *p && o + 1 < outsz; p++) {
        if (*p == '%' && p[1]) {
            char c = p[1];
            if (c == '%') {
                out[o++] = '%';
            } else if (!inserted && (c == 'f' || c == 'F' || c == 'u' || c == 'U') && o + ql < outsz) {
                memcpy(out + o, quoted, ql);
                o += ql;
                inserted = 1;
            }
            p++;
            continue;
        }
        out[o++] = *p;
    }
    out[o] = 0;
    if (!inserted && o + 1 + ql < outsz) {
        out[o++] = ' ';
        snprintf(out + o, outsz - o, "%s", quoted);
    }
    return 1;
}
