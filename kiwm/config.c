/* $XDG_CONFIG_HOME/kiwm.conf (fallback ~/.config/kiwm.conf) -- flat
 * key=value lines, '#' comments, same spirit as xisback/xispanel's config
 * files but simpler since kiwm has no nested panel/widget-style objects
 * to describe, just a handful of scalar settings. */
#include "config.h"
#include "wm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>

static void config_path(char *out, size_t outsz)
{
    const char *xdg_config = getenv("XDG_CONFIG_HOME");
    if (xdg_config && *xdg_config) {
        mkdir(xdg_config, 0700);
        snprintf(out, outsz, "%s/kiwm.conf", xdg_config);
        return;
    }

    const char *home = getenv("HOME");
    if (!home || !*home)
        home = "/tmp";
    char configdir[480];
    snprintf(configdir, sizeof(configdir), "%s/.config", home);
    mkdir(configdir, 0700);
    snprintf(out, outsz, "%s/kiwm.conf", configdir);
}

static void apply_builtin_defaults(void)
{
    wm.deco_bg_r = 0.0; wm.deco_bg_g = 0.0; wm.deco_bg_b = 0.0;
    wm.deco_fg_r = 1.0; wm.deco_fg_g = 1.0; wm.deco_fg_b = 1.0;
    wm.hide_deco_on_maximize = false;
    wm.num_desktops = DEFAULT_NUM_DESKTOPS;
    wm.mod_cycle = MOD_ALT;
    wm.mod_control = MOD_META;
}

/* "#rrggbb" (leading '#' optional) -> 0..1 doubles, Cairo's native range. */
static bool parse_hex_color(const char *s, double *r, double *g, double *b)
{
    if (s[0] == '#')
        s++;
    unsigned int ri, gi, bi;
    if (sscanf(s, "%2x%2x%2x", &ri, &gi, &bi) != 3)
        return false;
    *r = ri / 255.0;
    *g = gi / 255.0;
    *b = bi / 255.0;
    return true;
}

static uint16_t parse_mod(const char *s, uint16_t fallback)
{
    if (strcasecmp(s, "alt") == 0)
        return MOD_ALT;
    if (strcasecmp(s, "meta") == 0 || strcasecmp(s, "super") == 0)
        return MOD_META;
    fprintf(stderr, "kiwm: config: unknown modifier '%s' (expected alt or meta), keeping current value\n", s);
    return fallback;
}

static void write_default_config(const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "kiwm: could not write default config to %s: %s\n", path, strerror(errno));
        return;
    }
    fprintf(f,
        "# kiwm configuration.\n"
        "#\n"
        "# Lines are key=value; '#' starts a comment. Missing keys keep\n"
        "# kiwm's built-in defaults (the values already written below).\n"
        "# See kiwm/PROTOCOL.md for the per-output virtual desktop protocol\n"
        "# this feeds into, and kiwm-kicomp-projeto.md for the overall design.\n"
        "\n"
        "# Fallback decoration colors, used only when the theme PNG\n"
        "# (greenxp/bg.png by default, or $KIWM_DECO_BG) can't be loaded.\n"
        "deco_bg=#000000\n"
        "deco_fg=#ffffff\n"
        "\n"
        "# Keep window decoration visible while maximized (0 = hidden, 1 = visible).\n"
        "hide_deco_on_maximize=0\n"
        "\n"
        "# Virtual desktops per output.\n"
        "num_desktops=4\n"
        "\n"
        "# Modifier for Tab / Shift+Tab window switching: alt or meta.\n"
        "mod_cycle=alt\n"
        "\n"
        "# Modifier for window control -- move via mouse-down+drag, maximize\n"
        "# via +Up, per-output desktop switch via +Tab/+Shift+Tab: alt or meta.\n"
        "mod_control=meta\n");
    fclose(f);
    fprintf(stderr, "kiwm: no config found, wrote defaults to %s\n", path);
}

void config_load(void)
{
    apply_builtin_defaults();

    char path[512];
    config_path(path, sizeof(path));

    if (access(path, F_OK) != 0)
        write_default_config(path);

    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "kiwm: could not read config %s: %s (using built-in defaults)\n", path, strerror(errno));
        return;
    }

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = 0;

        char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '\0' || *p == '#')
            continue;

        char *eq = strchr(p, '=');
        if (!eq) {
            fprintf(stderr, "kiwm: config: skipping malformed line: '%s'\n", line);
            continue;
        }
        *eq = '\0';
        char *key = p;
        char *val = eq + 1;

        size_t klen = strlen(key);
        while (klen > 0 && (key[klen - 1] == ' ' || key[klen - 1] == '\t'))
            key[--klen] = '\0';
        while (*val == ' ' || *val == '\t')
            val++;

        if (strcmp(key, "deco_bg") == 0) {
            if (!parse_hex_color(val, &wm.deco_bg_r, &wm.deco_bg_g, &wm.deco_bg_b))
                fprintf(stderr, "kiwm: config: invalid deco_bg '%s' (expected #rrggbb)\n", val);
        } else if (strcmp(key, "deco_fg") == 0) {
            if (!parse_hex_color(val, &wm.deco_fg_r, &wm.deco_fg_g, &wm.deco_fg_b))
                fprintf(stderr, "kiwm: config: invalid deco_fg '%s' (expected #rrggbb)\n", val);
        } else if (strcmp(key, "hide_deco_on_maximize") == 0) {
            wm.hide_deco_on_maximize = atoi(val) != 0;
        } else if (strcmp(key, "num_desktops") == 0) {
            int n = atoi(val);
            if (n < 1) n = 1;
            if (n > MAX_DESKTOPS) n = MAX_DESKTOPS;
            wm.num_desktops = n;
        } else if (strcmp(key, "mod_cycle") == 0) {
            wm.mod_cycle = parse_mod(val, wm.mod_cycle);
        } else if (strcmp(key, "mod_control") == 0) {
            wm.mod_control = parse_mod(val, wm.mod_control);
        } else {
            fprintf(stderr, "kiwm: config: skipping unknown key '%s'\n", key);
        }
    }
    fclose(f);
}
