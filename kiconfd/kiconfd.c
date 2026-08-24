/*
 * kiconfd - session settings daemon for KiDesktop.
 *
 * Reads a central config file and applies it to every toolkit KiDesktop
 * cares about, staying resident so SIGHUP can reload+reapply without a
 * restart. This is the daemon half of the xisconf remake (kiconf being the
 * GTK2 front-end).
 *
 * Config: $XDG_CONFIG_HOME/kiconfd.conf (fallback ~/.config/kiconfd.conf),
 * simple "key = value" lines, '#' comments. Recognized keys:
 *
 *   cursor_theme      = <Xcursor theme name>
 *   cursor_size       = <pixel size>
 *   color_bg          = #rrggbb   (window background)
 *   color_fg          = #rrggbb   (text/foreground)
 *   color_base        = #rrggbb   (text entry/list background)
 *   color_accent      = #rrggbb   (buttons, links, highlight)
 *   color_selection_bg = #rrggbb
 *   color_selection_fg = #rrggbb
 *   font_general      = <Pango font description, e.g. "Sans 10">
 *   font_monospace    = <Pango font description, e.g. "Monospace 10">
 *   gtk2_theme        = <GTK2 theme name>
 *   gtk3_theme        = <GTK3 theme name>
 *   gtk4_theme        = <GTK4 theme name>
 *   icon_theme        = <icon theme name>
 *   qt_style          = <QStyle name, e.g. "Fusion">
 *
 * Any key missing the first time is filled with a sane default and saved
 * back, same as cursor_theme/cursor_size always did.
 *
 * What gets touched per toolkit:
 *   Xresources (RESOURCE_MANAGER) -- Xcursor.theme/size, Xft.font. The
 *     one thing every X11 app can fall back to regardless of toolkit.
 *   GTK2   -- ~/.gtkrc-2.0, inside a "# BEGIN/END KICONF" marked block
 *     (theme/icon-theme/font/cursor keys, plus a style override for the
 *     color palette) so anything else the user hand-edited there survives.
 *   GTK3/4 -- ~/.config/gtk-{3,4}.0/settings.ini (theme/icon-theme/font/
 *     cursor keys upserted under [Settings], other keys left alone) plus a
 *     fully kiconfd-owned gtk-{3,4}.0/kiconf-colors.css using @define-color,
 *     imported from gtk.css via one marked line.
 *   Qt5/6  -- ~/.config/qt{5,6}ct/qt{5,6}ct.conf (style/icon_theme/fonts/
 *     color_scheme_path upserted under [Appearance]/[Fonts]) plus a fully
 *     kiconfd-owned qt{5,6}ct/colors/kiconf.conf QPalette color scheme.
 *     Requires qt5ct/qt6ct installed and QT_QPA_PLATFORMTHEME set for Qt
 *     apps to actually read it -- not kiconfd's job to enforce that env
 *     var, that's a kisession/profile concern.
 *   wx (wxWidgets) -- no separate file: wxGTK (the default on Linux) is a
 *     GTK wrapper and already follows the GTK settings above. Nothing to
 *     do here.
 *
 * The Qt QPalette color-scheme mapping is the least certain part of this
 * file (see build_qt_colorscheme()): qt5ct's on-disk role ordering isn't
 * officially documented, this uses the ordering commonly seen in
 * hand-inspected qt5ct color-scheme files. Treat it as a starting point,
 * not gospel -- verify against an actual qt5ct install before trusting it.
 */

#include <X11/Xatom.h>
#include <X11/Xcursor/Xcursor.h>
#include <X11/Xlib.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#define KICONFD_VERSION "0.2.0"
#define LINE_MAX_LEN 512
#define COLOR_LEN 16
#define NAME_LEN 128
#define FONT_LEN 128

static Display *g_dpy;
static Window g_root;
static char g_configpath[PATH_MAX];
static volatile sig_atomic_t g_quit = 0;
static volatile sig_atomic_t g_reload = 0;

static char g_cursor_theme[NAME_LEN] = "";
static int g_cursor_size = 0;
static char g_color_bg[COLOR_LEN] = "";
static char g_color_fg[COLOR_LEN] = "";
static char g_color_base[COLOR_LEN] = "";
static char g_color_accent[COLOR_LEN] = "";
static char g_color_selection_bg[COLOR_LEN] = "";
static char g_color_selection_fg[COLOR_LEN] = "";
static char g_font_general[FONT_LEN] = "";
static char g_font_monospace[FONT_LEN] = "";
static char g_gtk2_theme[NAME_LEN] = "";
static char g_gtk3_theme[NAME_LEN] = "";
static char g_gtk4_theme[NAME_LEN] = "";
static char g_icon_theme[NAME_LEN] = "";
static char g_qt_style[NAME_LEN] = "";

static void handle_signal(int sig)
{
    if (sig == SIGHUP) {
        g_reload = 1;
    } else {
        g_quit = 1;
    }
}

static void resolve_configpath(void)
{
    const char *xdg_config = getenv("XDG_CONFIG_HOME");
    if (xdg_config && *xdg_config) {
        mkdir(xdg_config, 0700);
        snprintf(g_configpath, sizeof(g_configpath), "%s/kiconfd.conf", xdg_config);
        return;
    }
    const char *home = getenv("HOME");
    if (!home || !*home) {
        home = "/tmp";
    }
    char configdir[PATH_MAX];
    snprintf(configdir, sizeof(configdir), "%s/.config", home);
    mkdir(configdir, 0700);
    snprintf(g_configpath, sizeof(g_configpath), "%s/kiconfd.conf", configdir);
}

/* Trims leading/trailing whitespace in place, returns the (possibly
 * shifted) start of the trimmed string. */
static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' || end[-1] == '\r')) {
        *--end = '\0';
    }
    return s;
}

/* ------------------------------------------------------------------ */
/* generic file helpers, shared by every toolkit backend below         */
/* ------------------------------------------------------------------ */

/* mkdir -p on the parent directory of `path`. */
static void ensure_parent_dir(const char *path)
{
    char buf[PATH_MAX];
    snprintf(buf, sizeof(buf), "%s", path);
    char *slash = strrchr(buf, '/');
    if (!slash || slash == buf) {
        return;
    }
    *slash = '\0';
    for (char *p = buf + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(buf, 0755);
            *p = '/';
        }
    }
    mkdir(buf, 0755);
}

/* Rewrites the region between `begin_marker`/`end_marker` lines inside
 * `path` with `body` (appending a fresh block at EOF if the markers
 * aren't found yet), leaving everything else in the file untouched --
 * this is what lets kiconfd share a file like ~/.gtkrc-2.0 with the
 * user's own hand edits instead of overwriting the whole thing. `body`
 * must end with '\n' and must not itself contain the marker lines. */
static void update_marked_block(const char *path, const char *begin_marker, const char *end_marker, const char *body)
{
    ensure_parent_dir(path);
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    FILE *in = fopen(path, "r");
    FILE *out = fopen(tmp, "w");
    if (!out) {
        fprintf(stderr, "kiconfd: could not write '%s': %s\n", tmp, strerror(errno));
        if (in) {
            fclose(in);
        }
        return;
    }

    int wrote_block = 0;
    int in_block = 0;
    char line[1024];
    if (in) {
        while (fgets(line, sizeof(line), in)) {
            if (!in_block && strncmp(line, begin_marker, strlen(begin_marker)) == 0) {
                in_block = 1;
                fprintf(out, "%s\n%s%s\n", begin_marker, body, end_marker);
                wrote_block = 1;
                continue;
            }
            if (in_block) {
                if (strncmp(line, end_marker, strlen(end_marker)) == 0) {
                    in_block = 0;
                }
                continue;
            }
            fputs(line, out);
        }
        fclose(in);
    }
    if (!wrote_block) {
        fprintf(out, "\n%s\n%s%s\n", begin_marker, body, end_marker);
    }
    fclose(out);
    if (rename(tmp, path) != 0) {
        fprintf(stderr, "kiconfd: could not save '%s': %s\n", path, strerror(errno));
    }
}

/* Fully (re)writes a file kiconfd exclusively owns (no user content to
 * preserve there), e.g. a generated color-scheme file. */
static void write_owned_file(const char *path, const char *content)
{
    ensure_parent_dir(path);
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) {
        fprintf(stderr, "kiconfd: could not write '%s': %s\n", tmp, strerror(errno));
        return;
    }
    fputs(content, f);
    fclose(f);
    if (rename(tmp, path) != 0) {
        fprintf(stderr, "kiconfd: could not save '%s': %s\n", path, strerror(errno));
    }
}

/* Sets `key = value` inside `[section]` in an ini/GKeyFile-style file at
 * `path` (creating the file/section if missing), leaving every other
 * section/key already there alone. Reads the whole file into memory --
 * fine for the small config files this is used on, called rarely (on
 * Aplicar/reload, not a hot path). */
static void ini_upsert(const char *path, const char *section, const char *key, const char *value)
{
    ensure_parent_dir(path);

    char sec_hdr[160];
    snprintf(sec_hdr, sizeof(sec_hdr), "[%s]", section);

    FILE *in = fopen(path, "r");
    char **lines = NULL;
    int n_lines = 0;
    int cap = 0;
    if (in) {
        char buf[1024];
        while (fgets(buf, sizeof(buf), in)) {
            if (n_lines >= cap) {
                cap = cap ? cap * 2 : 64;
                lines = realloc(lines, (size_t)cap * sizeof(char *));
            }
            lines[n_lines++] = strdup(buf);
        }
        fclose(in);
    }

    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *out = fopen(tmp, "w");
    if (!out) {
        fprintf(stderr, "kiconfd: could not write '%s': %s\n", tmp, strerror(errno));
        for (int i = 0; i < n_lines; i++) {
            free(lines[i]);
        }
        free(lines);
        return;
    }

    int in_section = 0;
    int section_found = 0;
    int key_written = 0;
    size_t keylen = strlen(key);

    for (int i = 0; i < n_lines; i++) {
        char *l = lines[i];
        char trimmed[1024];
        snprintf(trimmed, sizeof(trimmed), "%s", l);
        char *t = trim(trimmed);

        if (t[0] == '[') {
            if (in_section && !key_written) {
                fprintf(out, "%s=%s\n", key, value);
                key_written = 1;
            }
            in_section = (strcmp(t, sec_hdr) == 0);
            if (in_section) {
                section_found = 1;
            }
            fputs(l, out);
            continue;
        }

        if (in_section && !key_written && strncmp(t, key, keylen) == 0 &&
            (t[keylen] == '=' || (t[keylen] == ' ' && strchr(t, '=')))) {
            fprintf(out, "%s=%s\n", key, value);
            key_written = 1;
            continue;
        }

        fputs(l, out);
    }

    if (in_section && !key_written) {
        fprintf(out, "%s=%s\n", key, value);
        key_written = 1;
    }
    if (!section_found) {
        fprintf(out, "%s[%s]\n%s=%s\n", n_lines > 0 ? "\n" : "", section, key, value);
    }

    fclose(out);
    for (int i = 0; i < n_lines; i++) {
        free(lines[i]);
    }
    free(lines);

    if (rename(tmp, path) != 0) {
        fprintf(stderr, "kiconfd: could not save '%s': %s\n", path, strerror(errno));
    }
}

/* Splits a Pango-style "Family Name NN" font description into family and
 * point size (defaulting to 10 if the trailing token isn't numeric). */
static void parse_font_spec(const char *spec, char *family, size_t famsz, int *size)
{
    char buf[FONT_LEN];
    snprintf(buf, sizeof(buf), "%s", spec);
    char *last_space = strrchr(buf, ' ');
    int sz = 10;
    if (last_space) {
        char *end;
        long v = strtol(last_space + 1, &end, 10);
        if (*end == '\0' && v > 0) {
            sz = (int)v;
            *last_space = '\0';
        }
    }
    snprintf(family, famsz, "%s", buf);
    *size = sz;
}

static const char *xdg_home(void)
{
    const char *home = getenv("HOME");
    return (home && *home) ? home : "/tmp";
}

static const char *xdg_config_home(void)
{
    const char *v = getenv("XDG_CONFIG_HOME");
    return (v && *v) ? v : NULL;
}

static void path_in_config(char *out, size_t outsz, const char *rel)
{
    const char *xc = xdg_config_home();
    if (xc) {
        snprintf(out, outsz, "%s/%s", xc, rel);
    } else {
        snprintf(out, outsz, "%s/.config/%s", xdg_home(), rel);
    }
}

/* ------------------------------------------------------------------ */
/* Xresources (RESOURCE_MANAGER)                                       */
/* ------------------------------------------------------------------ */

/* Rewrites the RESOURCE_MANAGER property (same format xrdb uses), dropping
 * any pre-existing kiconfd-owned lines and appending the current ones --
 * so kiconfd only ever touches its own keys and leaves whatever else
 * xrdb/the WM put there alone. */
static void apply_resource_manager(void)
{
    static const char *owned[] = {"Xcursor.theme:", "Xcursor.size:", "Xft.font:"};

    Atom resman = XInternAtom(g_dpy, "RESOURCE_MANAGER", False);

    Atom type;
    int format;
    unsigned long nitems, bytes_after;
    unsigned char *data = NULL;
    XGetWindowProperty(g_dpy, g_root, resman, 0, 65536, False, XA_STRING,
                        &type, &format, &nitems, &bytes_after, &data);

    char kept[65536] = "";
    size_t kept_len = 0;
    if (data && type == XA_STRING) {
        char *copy = malloc(nitems + 1);
        if (copy) {
            memcpy(copy, data, nitems);
            copy[nitems] = '\0';
            char *save = NULL;
            char *ln = strtok_r(copy, "\n", &save);
            while (ln) {
                int is_owned = 0;
                for (size_t i = 0; i < sizeof(owned) / sizeof(owned[0]); i++) {
                    if (strncmp(ln, owned[i], strlen(owned[i])) == 0) {
                        is_owned = 1;
                        break;
                    }
                }
                if (!is_owned) {
                    size_t l = strlen(ln);
                    if (kept_len + l + 1 < sizeof(kept)) {
                        memcpy(kept + kept_len, ln, l);
                        kept_len += l;
                        kept[kept_len++] = '\n';
                    }
                }
                ln = strtok_r(NULL, "\n", &save);
            }
            free(copy);
        }
    }
    if (data) {
        XFree(data);
    }

    char out[65536 + 512];
    snprintf(out, sizeof(out), "%sXcursor.theme:\t%s\nXcursor.size:\t%d\nXft.font:\t%s\n",
              kept, g_cursor_theme, g_cursor_size, g_font_general);

    XChangeProperty(g_dpy, g_root, resman, XA_STRING, 8, PropModeReplace,
                     (unsigned char *)out, (int)strlen(out));
}

static void apply_cursor_theme(void)
{
    Cursor cur = XcursorLibraryLoadCursor(g_dpy, g_cursor_theme);
    if (cur == None) {
        fprintf(stderr, "kiconfd: could not load cursor theme '%s', leaving root cursor as-is\n", g_cursor_theme);
    } else {
        XDefineCursor(g_dpy, g_root, cur);
        XFreeCursor(g_dpy, cur);
    }
}

/* ------------------------------------------------------------------ */
/* GTK2 -- ~/.gtkrc-2.0                                                 */
/* ------------------------------------------------------------------ */

#define GTKRC2_BEGIN "# BEGIN KICONF"
#define GTKRC2_END "# END KICONF"

static void apply_gtk2(void)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/.gtkrc-2.0", xdg_home());

    char body[2048];
    snprintf(body, sizeof(body),
              "gtk-theme-name = \"%s\"\n"
              "gtk-icon-theme-name = \"%s\"\n"
              "gtk-font-name = \"%s\"\n"
              "gtk-cursor-theme-name = \"%s\"\n"
              "gtk-cursor-theme-size = %d\n"
              "\n"
              "style \"kiconf-colors\"\n"
              "{\n"
              "    bg[NORMAL] = \"%s\"\n"
              "    fg[NORMAL] = \"%s\"\n"
              "    base[NORMAL] = \"%s\"\n"
              "    text[NORMAL] = \"%s\"\n"
              "    bg[SELECTED] = \"%s\"\n"
              "    fg[SELECTED] = \"%s\"\n"
              "    base[SELECTED] = \"%s\"\n"
              "    text[SELECTED] = \"%s\"\n"
              "}\n"
              "widget \"*\" style \"kiconf-colors\"\n",
              g_gtk2_theme, g_icon_theme, g_font_general, g_cursor_theme, g_cursor_size,
              g_color_bg, g_color_fg, g_color_base, g_color_fg,
              g_color_selection_bg, g_color_selection_fg, g_color_selection_bg, g_color_selection_fg);

    update_marked_block(path, GTKRC2_BEGIN, GTKRC2_END, body);
}

/* ------------------------------------------------------------------ */
/* GTK3 / GTK4 -- settings.ini + kiconf-colors.css                     */
/* ------------------------------------------------------------------ */

#define GTKCSS_BEGIN "/* BEGIN KICONF */"
#define GTKCSS_END "/* END KICONF */"

static void apply_gtk_modern(const char *ver, const char *theme)
{
    char settings_path[PATH_MAX];
    char rel[64];
    snprintf(rel, sizeof(rel), "gtk-%s/settings.ini", ver);
    path_in_config(settings_path, sizeof(settings_path), rel);

    ini_upsert(settings_path, "Settings", "gtk-theme-name", theme);
    ini_upsert(settings_path, "Settings", "gtk-icon-theme-name", g_icon_theme);
    ini_upsert(settings_path, "Settings", "gtk-font-name", g_font_general);
    ini_upsert(settings_path, "Settings", "gtk-cursor-theme-name", g_cursor_theme);
    {
        char size_str[16];
        snprintf(size_str, sizeof(size_str), "%d", g_cursor_size);
        ini_upsert(settings_path, "Settings", "gtk-cursor-theme-size", size_str);
    }

    /* Colors: settings.ini has no bg/fg/accent keys of its own (those
     * follow whatever the theme draws), so kiconfd expresses the palette
     * as GTK3/4-native @define-color overrides instead, in a file it
     * fully owns, imported (once) from gtk.css. */
    char css_path[PATH_MAX];
    snprintf(rel, sizeof(rel), "gtk-%s/kiconf-colors.css", ver);
    path_in_config(css_path, sizeof(css_path), rel);

    char css[1024];
    snprintf(css, sizeof(css),
              "/* generated by kiconfd -- do not edit, use kiconf instead */\n"
              "@define-color theme_bg_color %s;\n"
              "@define-color theme_fg_color %s;\n"
              "@define-color theme_base_color %s;\n"
              "@define-color theme_text_color %s;\n"
              "@define-color theme_selected_bg_color %s;\n"
              "@define-color theme_selected_fg_color %s;\n"
              "@define-color accent_color %s;\n"
              "@define-color accent_bg_color %s;\n",
              g_color_bg, g_color_fg, g_color_base, g_color_fg,
              g_color_selection_bg, g_color_selection_fg, g_color_accent, g_color_accent);
    write_owned_file(css_path, css);

    char gtkcss_path[PATH_MAX];
    snprintf(rel, sizeof(rel), "gtk-%s/gtk.css", ver);
    path_in_config(gtkcss_path, sizeof(gtkcss_path), rel);
    char import_line[128];
    snprintf(import_line, sizeof(import_line), "@import url(\"kiconf-colors.css\");\n");
    update_marked_block(gtkcss_path, GTKCSS_BEGIN, GTKCSS_END, import_line);
}

/* ------------------------------------------------------------------ */
/* Qt5/Qt6 via qt5ct/qt6ct                                              */
/* ------------------------------------------------------------------ */

/* Best-effort QPalette color scheme for qt5ct/qt6ct's "custom_palette".
 * Role ordering below (WindowText, Button, Light, Midlight, Dark, Mid,
 * Text, BrightText, ButtonText, Base, Window, Shadow, Highlight,
 * HighlightedText, Link, LinkVisited, AlternateBase, NoRole, ToolTipBase,
 * ToolTipText) matches QPalette::ColorRole and the ordering seen in
 * hand-inspected qt5ct color-scheme files -- it is NOT from official qt5ct
 * documentation (there isn't any) and hasn't been verified against every
 * Qt/qt5ct version. The "bevel" roles (Light/Midlight/Dark/Mid/Shadow)
 * aren't part of kiconf's user-facing palette, so they get fixed neutral
 * values rather than derived ones. Revisit this against a real qt5ct
 * install before trusting it further.
 */
static void build_qt_colorscheme(char *out, size_t outsz)
{
    char row[512];
    snprintf(row, sizeof(row),
              "%s, %s, #ffffff, #d4d4d4, #4d4d4d, #a0a0a0, %s, #ffffff, %s, %s, %s, #000000, %s, %s, %s, %s, %s, #000000, #ffffdc, %s",
              g_color_fg, g_color_bg, g_color_fg, g_color_fg, g_color_base, g_color_bg,
              g_color_accent, g_color_selection_fg, g_color_accent, g_color_accent, g_color_base, g_color_fg);

    snprintf(out, outsz,
              "[ColorScheme]\n"
              "active_colors=%s\n"
              "inactive_colors=%s\n"
              "disabled_colors=%s\n",
              row, row, row);
}

static void apply_qt(const char *ctdir)
{
    char conf_path[PATH_MAX];
    char rel[64];
    snprintf(rel, sizeof(rel), "%s/%s.conf", ctdir, ctdir);
    path_in_config(conf_path, sizeof(conf_path), rel);

    ini_upsert(conf_path, "Appearance", "style", g_qt_style);
    ini_upsert(conf_path, "Appearance", "icon_theme", g_icon_theme);
    ini_upsert(conf_path, "Appearance", "custom_palette", "true");

    char scheme_rel[64];
    snprintf(scheme_rel, sizeof(scheme_rel), "%s/colors/kiconf.conf", ctdir);
    char scheme_path[PATH_MAX];
    path_in_config(scheme_path, sizeof(scheme_path), scheme_rel);
    ini_upsert(conf_path, "Appearance", "color_scheme_path", scheme_path);

    char scheme[2048];
    build_qt_colorscheme(scheme, sizeof(scheme));
    write_owned_file(scheme_path, scheme);

    char fam[FONT_LEN];
    int sz;
    parse_font_spec(g_font_general, fam, sizeof(fam), &sz);
    char general_qfont[192];
    snprintf(general_qfont, sizeof(general_qfont), "\"%s,%d,-1,5,50,0,0,0,0,0\"", fam, sz);
    ini_upsert(conf_path, "Fonts", "general", general_qfont);

    parse_font_spec(g_font_monospace, fam, sizeof(fam), &sz);
    char fixed_qfont[192];
    snprintf(fixed_qfont, sizeof(fixed_qfont), "\"%s,%d,-1,5,50,0,0,0,0,0\"", fam, sz);
    ini_upsert(conf_path, "Fonts", "fixed", fixed_qfont);
}

/* ------------------------------------------------------------------ */
/* config persistence                                                   */
/* ------------------------------------------------------------------ */

static void save_config(void)
{
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s.tmp", g_configpath);
    FILE *f = fopen(tmp, "w");
    if (!f) {
        fprintf(stderr, "kiconfd: could not write '%s': %s\n", tmp, strerror(errno));
        return;
    }
    fprintf(f, "# kiconfd config\n");
    fprintf(f, "cursor_theme = %s\n", g_cursor_theme);
    fprintf(f, "cursor_size = %d\n", g_cursor_size);
    fprintf(f, "color_bg = %s\n", g_color_bg);
    fprintf(f, "color_fg = %s\n", g_color_fg);
    fprintf(f, "color_base = %s\n", g_color_base);
    fprintf(f, "color_accent = %s\n", g_color_accent);
    fprintf(f, "color_selection_bg = %s\n", g_color_selection_bg);
    fprintf(f, "color_selection_fg = %s\n", g_color_selection_fg);
    fprintf(f, "font_general = %s\n", g_font_general);
    fprintf(f, "font_monospace = %s\n", g_font_monospace);
    fprintf(f, "gtk2_theme = %s\n", g_gtk2_theme);
    fprintf(f, "gtk3_theme = %s\n", g_gtk3_theme);
    fprintf(f, "gtk4_theme = %s\n", g_gtk4_theme);
    fprintf(f, "icon_theme = %s\n", g_icon_theme);
    fprintf(f, "qt_style = %s\n", g_qt_style);
    fclose(f);
    if (rename(tmp, g_configpath) != 0) {
        fprintf(stderr, "kiconfd: could not save '%s': %s\n", g_configpath, strerror(errno));
    }
}

/* Loads known keys from the config file into the in-memory settings.
 * Missing keys are left at whatever the caller pre-seeded (defaults). */
static void load_config(void)
{
    FILE *f = fopen(g_configpath, "r");
    if (!f) {
        return;
    }
    char line[LINE_MAX_LEN];
    while (fgets(line, sizeof(line), f)) {
        char *l = trim(line);
        if (!*l || *l == '#') {
            continue;
        }
        char *eq = strchr(l, '=');
        if (!eq) {
            fprintf(stderr, "kiconfd: config: skipping malformed line: '%s'\n", l);
            continue;
        }
        *eq = '\0';
        char *key = trim(l);
        char *val = trim(eq + 1);
        if (!strcmp(key, "cursor_theme")) {
            snprintf(g_cursor_theme, sizeof(g_cursor_theme), "%s", val);
        } else if (!strcmp(key, "cursor_size")) {
            g_cursor_size = atoi(val);
        } else if (!strcmp(key, "color_bg")) {
            snprintf(g_color_bg, sizeof(g_color_bg), "%s", val);
        } else if (!strcmp(key, "color_fg")) {
            snprintf(g_color_fg, sizeof(g_color_fg), "%s", val);
        } else if (!strcmp(key, "color_base")) {
            snprintf(g_color_base, sizeof(g_color_base), "%s", val);
        } else if (!strcmp(key, "color_accent")) {
            snprintf(g_color_accent, sizeof(g_color_accent), "%s", val);
        } else if (!strcmp(key, "color_selection_bg")) {
            snprintf(g_color_selection_bg, sizeof(g_color_selection_bg), "%s", val);
        } else if (!strcmp(key, "color_selection_fg")) {
            snprintf(g_color_selection_fg, sizeof(g_color_selection_fg), "%s", val);
        } else if (!strcmp(key, "font_general")) {
            snprintf(g_font_general, sizeof(g_font_general), "%s", val);
        } else if (!strcmp(key, "font_monospace")) {
            snprintf(g_font_monospace, sizeof(g_font_monospace), "%s", val);
        } else if (!strcmp(key, "gtk2_theme")) {
            snprintf(g_gtk2_theme, sizeof(g_gtk2_theme), "%s", val);
        } else if (!strcmp(key, "gtk3_theme")) {
            snprintf(g_gtk3_theme, sizeof(g_gtk3_theme), "%s", val);
        } else if (!strcmp(key, "gtk4_theme")) {
            snprintf(g_gtk4_theme, sizeof(g_gtk4_theme), "%s", val);
        } else if (!strcmp(key, "icon_theme")) {
            snprintf(g_icon_theme, sizeof(g_icon_theme), "%s", val);
        } else if (!strcmp(key, "qt_style")) {
            snprintf(g_qt_style, sizeof(g_qt_style), "%s", val);
        } else {
            fprintf(stderr, "kiconfd: config: unknown key '%s', ignoring\n", key);
        }
    }
    fclose(f);
}

static void apply_all(void)
{
    apply_cursor_theme();
    apply_resource_manager();
    apply_gtk2();
    apply_gtk_modern("3.0", g_gtk3_theme);
    apply_gtk_modern("4.0", g_gtk4_theme);
    apply_qt("qt5ct");
    apply_qt("qt6ct");
    XFlush(g_dpy);
    fprintf(stderr, "kiconfd: applied settings (cursor='%s' gtk2='%s' gtk3='%s' gtk4='%s' qt_style='%s')\n",
             g_cursor_theme, g_gtk2_theme, g_gtk3_theme, g_gtk4_theme, g_qt_style);
}

static void apply_and_persist_defaults(void)
{
    int changed = 0;

#define DEFAULT_STR(field, envvar, fallback) \
    if (!field[0]) { \
        const char *e = (envvar)[0] ? getenv(envvar) : NULL; \
        snprintf(field, sizeof(field), "%s", (e && *e) ? e : (fallback)); \
        changed = 1; \
    }

    DEFAULT_STR(g_cursor_theme, "XCURSOR_THEME", "default")
    if (g_cursor_size <= 0) {
        const char *env_size = getenv("XCURSOR_SIZE");
        g_cursor_size = (env_size && *env_size) ? atoi(env_size) : 24;
        if (g_cursor_size <= 0) {
            g_cursor_size = 24;
        }
        changed = 1;
    }
    DEFAULT_STR(g_color_bg, "", "#e0e0e0")
    DEFAULT_STR(g_color_fg, "", "#202020")
    DEFAULT_STR(g_color_base, "", "#ffffff")
    DEFAULT_STR(g_color_accent, "", "#3584e4")
    DEFAULT_STR(g_color_selection_bg, "", "#3584e4")
    DEFAULT_STR(g_color_selection_fg, "", "#ffffff")
    DEFAULT_STR(g_font_general, "", "Sans 10")
    DEFAULT_STR(g_font_monospace, "", "Monospace 10")
    DEFAULT_STR(g_gtk2_theme, "", "Default")
    DEFAULT_STR(g_gtk3_theme, "", "Adwaita")
    DEFAULT_STR(g_gtk4_theme, "", "Adwaita")
    DEFAULT_STR(g_icon_theme, "", "Adwaita")
    DEFAULT_STR(g_qt_style, "", "Fusion")

#undef DEFAULT_STR

    if (changed) {
        fprintf(stderr, "kiconfd: config missing some settings, filled in defaults and saved them\n");
        save_config();
    }
    apply_all();
}

static void reload_config(void)
{
    fprintf(stderr, "kiconfd: reloading config\n");
    g_cursor_theme[0] = '\0';
    g_cursor_size = 0;
    g_color_bg[0] = g_color_fg[0] = g_color_base[0] = '\0';
    g_color_accent[0] = g_color_selection_bg[0] = g_color_selection_fg[0] = '\0';
    g_font_general[0] = g_font_monospace[0] = '\0';
    g_gtk2_theme[0] = g_gtk3_theme[0] = g_gtk4_theme[0] = '\0';
    g_icon_theme[0] = g_qt_style[0] = '\0';
    load_config();
    apply_and_persist_defaults();
}

int main(int argc, char **argv)
{
    if (argc > 1 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
        printf("kiconfd %s - session settings daemon for KiDesktop\n", KICONFD_VERSION);
        printf("Usage: kiconfd\n");
        printf("Config: $XDG_CONFIG_HOME/kiconfd.conf (fallback ~/.config/kiconfd.conf)\n");
        printf("SIGHUP reloads the config and reapplies settings.\n");
        return 0;
    }

    const char *rundir = getenv("XDG_RUNTIME_DIR");
    if (!rundir || !*rundir) {
        rundir = "/tmp";
    }
    char lockpath[PATH_MAX];
    snprintf(lockpath, sizeof(lockpath), "%s/kiconfd.lock", rundir);
    int lockfd = open(lockpath, O_CREAT | O_RDWR, 0600);
    if (lockfd >= 0 && flock(lockfd, LOCK_EX | LOCK_NB) != 0) {
        fprintf(stderr, "kiconfd: already running (lock held on '%s')\n", lockpath);
        return 1;
    }

    resolve_configpath();

    g_dpy = XOpenDisplay(NULL);
    if (!g_dpy) {
        fprintf(stderr, "kiconfd: could not open X display\n");
        return 1;
    }
    g_root = DefaultRootWindow(g_dpy);

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGHUP, handle_signal);
    signal(SIGPIPE, SIG_IGN);

    load_config();
    apply_and_persist_defaults();

    while (!g_quit) {
        if (g_reload) {
            g_reload = 0;
            reload_config();
        }
        pause();
    }

    XCloseDisplay(g_dpy);
    fprintf(stderr, "kiconfd: shutting down\n");
    return 0;
}
