/* $XDG_CONFIG_HOME/kiwm.conf (fallback ~/.config/kiwm.conf) -- flat
 * key=value lines, '#' comments, same spirit as xisback/xispanel's config
 * files but simpler since kiwm has no nested panel/widget-style objects
 * to describe, just a handful of scalar settings. */
#include "config.h"
#include "wm.h"
#include "keybind.h"

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
    wm.deco_bg_r = 0.0; wm.deco_bg_g = 0.0; wm.deco_bg_b = 0.0; wm.deco_bg_a = 1.0;
    wm.deco_fg_r = 1.0; wm.deco_fg_g = 1.0; wm.deco_fg_b = 1.0; wm.deco_fg_a = 1.0;
    wm.hide_deco_on_maximize = false;
    wm.num_desktops = DEFAULT_NUM_DESKTOPS;
    wm.mod_cycle = MOD_ALT;
    wm.mod_control = MOD_META;
    wm.border_thickness = 0;
    wm.border_r = 0.0; wm.border_g = 0.0; wm.border_b = 0.0; wm.border_a = 1.0;
    wm.snap_threshold = 20;
    wm.live_snap_resize = false;
    wm.outline_width = 16;
    wm.resize_grip = 12;
    wm.live_resize = true;
    wm.magnet_threshold = 10;
    wm.link_resize_neighbors = false;
    wm.focus_follows_mouse = false;
    wm.osd_enabled = true;
    wm.osd_live_preview_windows = false;
    wm.osd_live_preview_desktops = false;
    wm.osd_output_follows_pointer = false;
    wm.osd_mru_order = false;
    wm.osd_desktop_windows = true;
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

/* "#rrggbb" or "#rrggbbaa" (leading '#' optional) -> 0..1 doubles,
 * Cairo's native range. */
static bool parse_hex_color(const char *s, double *r, double *g, double *b, double *a)
{
    if (s[0] == '#')
        s++;

    /* #rrggbb or #rrggbbaa -- the alpha is optional and defaults to fully
     * opaque, so every color written before it existed keeps meaning
     * exactly what it did. */
    unsigned int ri, gi, bi, ai = 255;
    int n = sscanf(s, "%2x%2x%2x%2x", &ri, &gi, &bi, &ai);
    if (n < 3)
        return false;
    if (n == 3)
        ai = 255;

    *r = ri / 255.0;
    *g = gi / 255.0;
    *b = bi / 255.0;
    if (a)
        *a = ai / 255.0;
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
        "# Every color here takes #rrggbb or #rrggbbaa; the alpha is real on\n"
        "# an ARGB window, i.e. once a compositor is running.\n"
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
        "# Whether an edge snap resizes the window while you drag it (1), or\n"
        "# just outlines where it will land and applies that size when you\n"
        "# release the button (0, the default).\n"
        "live_snap_resize=0\n"
        "\n"
        "# Thickness, in pixels, of the outline drawn around a window kiwm is\n"
        "# pointing at without moving it yet -- the snap preview above, and\n"
        "# the switcher with osd_live_preview_windows=0. Straddles the edge,\n"
        "# half outside and half in.\n"
        "outline_width=16\n"
        "\n"
        "# Width, in pixels, of the invisible resize grip along a window's\n"
        "# edges: a plain click within this far of an edge resizes instead of\n"
        "# going to the application -- from the corner when two edges are in\n"
        "# range, along one axis otherwise. Works with or without a visible\n"
        "# border. 0 disables it (resizing then needs the modifier drag).\n"
        "resize_grip=12\n"
        "\n"
        "# Whether resizing changes the window as you drag (1, the default),\n"
        "# or just outlines the size it is heading for and resizes for real\n"
        "# when you release the button (0). Applies to every resize: the grip\n"
        "# above, a modifier-drag, or the application asking for one.\n"
        "live_resize=1\n"
        "\n"
        "# How close (in pixels) a dragged window's edge must get to another\n"
        "# window's edge (decoration included) or to the screen edge before it\n"
        "# snaps flush against it, gap-free -- just a position nudge, not a\n"
        "# tiling snap like snap_threshold above. 0 disables it.\n"
        "magnet_threshold=10\n"
        "\n"
        "# While resizing a window, also resize whatever's touching the edge\n"
        "# being dragged (within 1px), oppositely, so both stay touching --\n"
        "# shrinking one grows its neighbor and vice versa. Same-output only.\n"
        "# Off by default: a resize behaves exactly like before unless you\n"
        "# turn this on.\n"
        "link_resize_neighbors=0\n"
        "\n"
        "# Raise+focus a window just by moving the pointer into it, instead\n"
        "# of requiring a click (0 = click-to-focus, the default; 1 =\n"
        "# focus-follows-mouse/\"sloppy focus\").\n"
        "focus_follows_mouse=0\n"
        "\n"
        "# Show a themed on-screen overlay while holding mod_cycle+Tab (window\n"
        "# list) or mod_control+Tab (per-output desktop grid), only switching\n"
        "# once the modifier is released -- like a real Alt+Tab -- instead of\n"
        "# switching immediately on every Tab press. Escape cancels without\n"
        "# switching. 1 = on (default), 0 = the original immediate-switch\n"
        "# behavior, no overlay at all.\n"
        "osd_enabled=1\n"
        "\n"
        "# Whether an overlay applies each step live (raise+focus the\n"
        "# highlighted window, or switch to the highlighted desktop) instead\n"
        "# of only once on release. 0 (default) leaves everything untouched\n"
        "# until you decide, and Escape reverts to nothing having changed;\n"
        "# 1 previews live, and Escape goes back to whatever was active when\n"
        "# the hold started. Separate for the two switchers -- previewing a\n"
        "# window raises and focuses it, which is a lot more disruptive than\n"
        "# previewing a desktop. Ignored when osd_enabled=0.\n"
        "osd_live_preview_windows=0\n"
        "osd_live_preview_desktops=0\n"
        "\n"
        "# Which output an overlay opens on (and lists/cycles the windows or\n"
        "# desktops of) -- 0 (default) uses the currently focused window's\n"
        "# output (falling back to the pointer's output only if nothing is\n"
        "# focused, same as before this existed); 1 always uses whichever\n"
        "# output the pointer is on right when the hold starts. Not the same\n"
        "# as focus_follows_mouse= -- this only picks which screen Alt+Tab/\n"
        "# Meta+Tab themselves act on, never what receives keyboard input.\n"
        "osd_output_follows_pointer=0\n"
        "\n"
        "# Order the window switcher lists windows in: 'list' (the default,\n"
        "# kiwm's own client order) or 'mru' (most recently used first, so a\n"
        "# single Tab flips to the previous window).\n"
        "osd_order=list\n"
        "\n"
        "# Draw the windows of each desktop inside its square in the desktop\n"
        "# switcher, as little rectangles at their real (scaled down)\n"
        "# geometry -- the same picture xispanel's pager widget draws with\n"
        "# show_windows=yes. 1 = on (default), 0 = plain numbered squares.\n"
        "osd_desktop_windows=1\n"
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
        "titlebar_layout=icon,title,shade,minimize,maximize,close\n"
        "\n");
    /* Generated from keybind.c's own table, so the shipped file always
     * lists exactly the shortcuts this build actually has. */
    keybind_write_default_config(f);
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
            if (!parse_hex_color(val, &wm.deco_bg_r, &wm.deco_bg_g, &wm.deco_bg_b, &wm.deco_bg_a))
                fprintf(stderr, "kiwm: config: invalid deco_bg '%s' (expected #rrggbb or #rrggbbaa)\n", val);
        } else if (strcmp(key, "deco_fg") == 0) {
            if (!parse_hex_color(val, &wm.deco_fg_r, &wm.deco_fg_g, &wm.deco_fg_b, &wm.deco_fg_a))
                fprintf(stderr, "kiwm: config: invalid deco_fg '%s' (expected #rrggbb or #rrggbbaa)\n", val);
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
            if (!parse_hex_color(val, &wm.border_r, &wm.border_g, &wm.border_b, &wm.border_a))
                fprintf(stderr, "kiwm: config: invalid border_color '%s' (expected #rrggbb or #rrggbbaa)\n", val);
        } else if (strcmp(key, "snap_threshold") == 0) {
            int n = atoi(val);
            wm.snap_threshold = n < 0 ? 0 : n;
        } else if (strcmp(key, "live_snap_resize") == 0) {
            wm.live_snap_resize = atoi(val) != 0;
        } else if (strcmp(key, "outline_width") == 0) {
            int n = atoi(val);
            if (n < 1) n = 1;
            if (n > MAX_OUTLINE_WIDTH) n = MAX_OUTLINE_WIDTH;
            wm.outline_width = n;
        } else if (strcmp(key, "resize_grip") == 0) {
            int n = atoi(val);
            if (n < 0) n = 0;
            if (n > MAX_RESIZE_GRIP) n = MAX_RESIZE_GRIP;
            wm.resize_grip = n;
        } else if (strcmp(key, "live_resize") == 0) {
            wm.live_resize = atoi(val) != 0;
        } else if (strcmp(key, "magnet_threshold") == 0) {
            int n = atoi(val);
            wm.magnet_threshold = n < 0 ? 0 : n;
        } else if (strcmp(key, "link_resize_neighbors") == 0) {
            wm.link_resize_neighbors = atoi(val) != 0;
        } else if (strcmp(key, "focus_follows_mouse") == 0) {
            wm.focus_follows_mouse = atoi(val) != 0;
        } else if (strcmp(key, "osd_enabled") == 0) {
            wm.osd_enabled = atoi(val) != 0;
        } else if (strcmp(key, "osd_live_preview") == 0) {
            /* Pre-split spelling: sets both. */
            wm.osd_live_preview_windows = wm.osd_live_preview_desktops = atoi(val) != 0;
        } else if (strcmp(key, "osd_live_preview_windows") == 0) {
            wm.osd_live_preview_windows = atoi(val) != 0;
        } else if (strcmp(key, "osd_live_preview_desktops") == 0) {
            wm.osd_live_preview_desktops = atoi(val) != 0;
        } else if (strcmp(key, "osd_order") == 0) {
            if (strcasecmp(val, "mru") == 0)
                wm.osd_mru_order = true;
            else if (strcasecmp(val, "list") == 0)
                wm.osd_mru_order = false;
            else
                fprintf(stderr, "kiwm: config: unknown osd_order '%s' (expected list or mru)\n", val);
        } else if (strcmp(key, "osd_desktop_windows") == 0) {
            wm.osd_desktop_windows = atoi(val) != 0;
        } else if (strcmp(key, "osd_output_follows_pointer") == 0) {
            wm.osd_output_follows_pointer = atoi(val) != 0;
        } else if (strcmp(key, "theme") == 0) {
            snprintf(wm.theme_path, sizeof(wm.theme_path), "%s", val);
        } else if (strcmp(key, "titlebar_layout") == 0) {
            parse_titlebar_layout(val);
        } else if (keybind_config_set(key, val)) {
            /* key_* shortcut: recorded by keybind.c, resolved to an actual
             * keycode later in keybind_init() (needs the X connection,
             * which doesn't exist yet at config_load() time). */
        } else {
            fprintf(stderr, "kiwm: config: skipping unknown key '%s'\n", key);
        }
    }
    fclose(f);
}
