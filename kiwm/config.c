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
    wm.border_thickness = 0;
    wm.border_r = 0.0; wm.border_g = 0.0; wm.border_b = 0.0;
    wm.snap_threshold = 20;
    wm.magnet_threshold = 10;
    wm.focus_follows_mouse = false;
    snprintf(wm.theme_path, sizeof(wm.theme_path), "greenxp");

    static const DecoElemKind default_layout[] = {
        DECO_ICON, DECO_TITLE, DECO_SHADE, DECO_MINIMIZE, DECO_MAXIMIZE, DECO_CLOSE
    };
    memcpy(wm.deco_layout, default_layout, sizeof(default_layout));
    wm.deco_layout_count = sizeof(default_layout) / sizeof(default_layout[0]);
}

/* "icon,title,shade,minimize,maximize,close,keep_above,keep_all_desktops"
 * (any subset, any order, kiwm.conf's titlebar_layout=) -> wm.deco_layout.
 * Unknown tokens are skipped with a warning rather than rejecting the
 * whole line, same spirit as the rest of this parser -- one bad token
 * shouldn't cost the reordering of everything else. Leaves the built-in
 * default in place (already applied by apply_builtin_defaults()) if the
 * key is missing or every token turns out invalid. */
static void parse_titlebar_layout(const char *val)
{
    DecoElemKind parsed[MAX_DECO_ELEMS];
    int n = 0;
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", val);

    char *save = NULL;
    for (char *tok = strtok_r(buf, ",", &save); tok && n < MAX_DECO_ELEMS; tok = strtok_r(NULL, ",", &save)) {
        while (*tok == ' ' || *tok == '\t') tok++;
        size_t len = strlen(tok);
        while (len > 0 && (tok[len - 1] == ' ' || tok[len - 1] == '\t')) tok[--len] = '\0';

        if (strcmp(tok, "title") == 0 || strcmp(tok, "name") == 0) parsed[n++] = DECO_TITLE;
        else if (strcmp(tok, "icon") == 0) parsed[n++] = DECO_ICON;
        else if (strcmp(tok, "shade") == 0) parsed[n++] = DECO_SHADE;
        else if (strcmp(tok, "minimize") == 0) parsed[n++] = DECO_MINIMIZE;
        else if (strcmp(tok, "maximize") == 0) parsed[n++] = DECO_MAXIMIZE;
        else if (strcmp(tok, "close") == 0) parsed[n++] = DECO_CLOSE;
        else if (strcmp(tok, "keep_above") == 0) parsed[n++] = DECO_KEEP_ABOVE;
        else if (strcmp(tok, "keep_all_desktops") == 0) parsed[n++] = DECO_KEEP_ALL_DESKTOPS;
        else
            fprintf(stderr, "kiwm: config: skipping unknown titlebar_layout element '%s'\n", tok);
    }

    if (n == 0) {
        fprintf(stderr, "kiwm: config: titlebar_layout had no valid elements, keeping default\n");
        return;
    }
    memcpy(wm.deco_layout, parsed, sizeof(DecoElemKind) * (size_t)n);
    wm.deco_layout_count = n;
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
        "mod_control=meta\n"
        "\n"
        "# Left/right/bottom decoration border thickness in pixels (0 = no\n"
        "# border, just the titlebar). No theming yet, just a flat color.\n"
        "border_thickness=0\n"
        "border_color=#000000\n"
        "\n"
        "# How close (in pixels) the pointer must get to an output's usable-\n"
        "# area edge, while dragging a window by its titlebar or via\n"
        "# mod_control-drag, to snap it there (top = maximize, left/right =\n"
        "# half-width, like Windows 7/kwin). 0 disables snapping.\n"
        "snap_threshold=20\n"
        "\n"
        "# How close (in pixels) a dragged window's edge must get to another\n"
        "# window's edge (decoration included) or to the screen edge before it\n"
        "# snaps flush against it, gap-free -- just a position nudge, not a\n"
        "# tiling snap like snap_threshold above. 0 disables it.\n"
        "magnet_threshold=10\n"
        "\n"
        "# Raise+focus a window just by moving the pointer into it, instead\n"
        "# of requiring a click (0 = click-to-focus, the default; 1 =\n"
        "# focus-follows-mouse/\"sloppy focus\").\n"
        "focus_follows_mouse=0\n"
        "\n"
        "# Theme folder (bg.png/slice, btns.png/btns.slice, colors -- see\n"
        "# kiwm/README or the greenxp/ folder itself for the file formats).\n"
        "# Resolved the same way kiwm looks for its own binary-relative\n"
        "# files: tried as ../<theme>, ./<theme> and plain <theme>.\n"
        "theme=greenxp\n"
        "\n"
        "# Titlebar element order, left to right, comma-separated. Available:\n"
        "# icon, title, shade, minimize, maximize (also serves as \"restore\"\n"
        "# once a window is maximized, same slot), close, keep_above,\n"
        "# keep_all_desktops. \"title\" is the only flexible element -- it\n"
        "# takes whatever width the fixed-size ones (everything else, one\n"
        "# BUTTON_W each) don't use, wherever it falls in the order.\n"
        "titlebar_layout=icon,title,shade,minimize,maximize,close\n");
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
        } else if (strcmp(key, "border_thickness") == 0) {
            int n = atoi(val);
            wm.border_thickness = n < 0 ? 0 : n;
        } else if (strcmp(key, "border_color") == 0) {
            if (!parse_hex_color(val, &wm.border_r, &wm.border_g, &wm.border_b))
                fprintf(stderr, "kiwm: config: invalid border_color '%s' (expected #rrggbb)\n", val);
        } else if (strcmp(key, "snap_threshold") == 0) {
            int n = atoi(val);
            wm.snap_threshold = n < 0 ? 0 : n;
        } else if (strcmp(key, "magnet_threshold") == 0) {
            int n = atoi(val);
            wm.magnet_threshold = n < 0 ? 0 : n;
        } else if (strcmp(key, "focus_follows_mouse") == 0) {
            wm.focus_follows_mouse = atoi(val) != 0;
        } else if (strcmp(key, "theme") == 0) {
            snprintf(wm.theme_path, sizeof(wm.theme_path), "%s", val);
        } else if (strcmp(key, "titlebar_layout") == 0) {
            parse_titlebar_layout(val);
        } else {
            fprintf(stderr, "kiwm: config: skipping unknown key '%s'\n", key);
        }
    }
    fclose(f);
}
