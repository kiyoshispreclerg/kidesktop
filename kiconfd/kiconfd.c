/*
 * kiconfd - session settings daemon for KiDesktop.
 *
 * Reads a central config file and applies it to every toolkit KiDesktop
 * cares about, staying resident so SIGHUP can reload+reapply without a
 * restart. This is the daemon half of the xisconf remake (kiconf being the
 * GTK2 front-end).
 *
 * Also the one thing in the session that applies the night light
 * schedule (kiconf's Energia tab) on a timer -- see the "night light"
 * section below and apply_nightlight()'s own comment for why this file
 * ended up owning that instead of a separate daemon. Each time that
 * actually turns the tint on or off (never for a mid-window temperature-
 * only edit), it also tells xispanel to pop a toast about it --
 * notify_xispanel_osd(), a fire-and-forget JSON line to xispanel-ctl's
 * `OSD` command (see xispanel/PROTOCOL.md) that silently does nothing if
 * xispanel isn't running.
 *
 * Log: off by default (stdout/stderr behave normally, i.e. whatever
 * kisession/startx/the display manager's Xsession script already does
 * with an inherited child's fds). Pass --log, or set KICONFD_LOG=1 in
 * the environment kiconfd is launched with, to redirect them instead to
 * a fixed $XDG_CONFIG_HOME/kiconfd.log (fallback ~/.config/kiconfd.log)
 * -- see redirect_log_to_file() -- for debugging startup issues (a
 * saved screens layout not applying, etc.) without hunting for wherever
 * the launcher happened to route the inherited fds.
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
 *   export_to_other_desktops = 0|1   (see below, default 0)
 *
 * Any key missing the first time is filled with a sane default and saved
 * back, same as cursor_theme/cursor_size always did.
 *
 * export_to_other_desktops: off by default. ~/.gtkrc-2.0, ~/.config/
 * gtk-{3,4}.0/settings.ini and ~/.config/qt{5,6}ct/qt{5,6}ct.conf are not
 * KiDesktop-specific -- they're the *same* files any other desktop's GTK/Qt
 * apps (and, for GTK, the desktop's own theme-sync tool, e.g. Plasma's
 * kde-gtk-config) read regardless of which session wrote them last. With
 * this off, apply_all() leaves those files alone entirely and a KiDesktop
 * session still themes itself correctly through the two channels that are
 * genuinely scoped to the current X session -- XSETTINGS (live, covers
 * theme/icon/font/cursor for GTK2/3 and any Qt app following it) and
 * RESOURCE_MANAGER (Xcursor/Xft). The one casualty is the custom color
 * palette, which has no session-only channel (see the XSETTINGS comment
 * below) and so only ever reaches apps through those shared files -- with
 * export off, GTK/Qt apps keep the palette their own theme ships instead.
 * Turning it on is an explicit "make this account's GTK/Qt apps look like
 * this under every desktop" opt-in from kiconf's Aparencia tab; turning it
 * back off does not revert files a prior export already wrote.
 *
 * What gets touched per toolkit:
 *   XSETTINGS (_XSETTINGS_S<screen>) -- theme/icon theme/font/cursor. The
 *     only channel that reaches apps that are *already running*: every
 *     file backend below is read once at app startup, so without this,
 *     "Aplicar" would only affect programs launched afterwards. See
 *     apply_xsettings(). Colors are the exception -- XSETTINGS has no key
 *     for a palette, so those still need an app restart. Always applied,
 *     regardless of export_to_other_desktops -- scoped to this X session.
 *   Xresources (RESOURCE_MANAGER) -- Xcursor.theme/size, Xft.font. The
 *     one thing every X11 app can fall back to regardless of toolkit.
 *     Always applied -- also scoped to this X session.
 *   GTK2   -- ~/.gtkrc-2.0, inside a "# BEGIN/END KICONF" marked block
 *     (theme/icon-theme/font/cursor keys, plus a style override for the
 *     color palette) so anything else the user hand-edited there survives.
 *     Only written when export_to_other_desktops is on.
 *   GTK3/4 -- ~/.config/gtk-{3,4}.0/settings.ini (theme/icon-theme/font/
 *     cursor keys upserted under [Settings], other keys left alone) plus a
 *     fully kiconfd-owned gtk-{3,4}.0/kiconf-colors.css using @define-color,
 *     imported from gtk.css via one marked line. Only written when
 *     export_to_other_desktops is on.
 *   Screens (xrandr) -- $XDG_CONFIG_HOME/kiconfd-screens.conf, a separate
 *     file kiconf's Telas tab writes on Aplicar (see its save_screens_
 *     layout()) and kiconfd replays via one `xrandr` call at session
 *     start (see apply_screens_layout()) -- xrandr's own layout doesn't
 *     survive a logout/login on its own. Kept out of kiconfd.conf
 *     because that file gets fully rewritten by kiconf's Aparencia tab,
 *     which knows nothing about screens; a separate file is the same
 *     trick already used for the GTK/Qt color-scheme files below. Only
 *     applied once, at startup -- not on SIGHUP, since Telas already
 *     applies its changes live with its own direct xrandr calls.
 *   Input (kiconfd-input.conf) -- $XDG_CONFIG_HOME/kiconfd-input.conf, a
 *     separate file kiconf's Entrada tab writes on Aplicar (same
 *     out-of-kiconfd.conf reasoning as Screens above): NumLock-on-start
 *     (applied via XkbLockModifiers, no external tool) and the two XiS
 *     keyboard flags (ToggleModifiersOnPress/KickHotkeysOnRelease,
 *     applied via `xinput set-prop`, same calls Entrada's own Aplicar
 *     makes live) -- see apply_input_settings(). Unlike Screens, also
 *     reapplied on SIGHUP: NumLock has no live-apply of its own in
 *     kiconf (nothing to press to see it happen this session without
 *     this), so Entrada's Aplicar signals kiconfd the same way Energia's
 *     does for night light.
 *   Qt5/6  -- ~/.config/qt{5,6}ct/qt{5,6}ct.conf (style/icon_theme/fonts/
 *     color_scheme_path upserted under [Appearance]/[Fonts]) plus a fully
 *     kiconfd-owned qt{5,6}ct/colors/kiconf.conf QPalette color scheme.
 *     Only read when qt5ct/qt6ct is installed *and* QT_QPA_PLATFORMTHEME
 *     names it; setting that variable is kisession's job (see
 *     setup_qt_platformtheme() there), and it prefers the "gtk3" plugin
 *     when available, in which case Qt apps follow the GTK3 settings and
 *     the XSETTINGS broadcast above instead of these files. Only written
 *     when export_to_other_desktops is on.
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
#include <X11/XKBlib.h>
#include <X11/extensions/Xrandr.h>
#include <X11/keysym.h>

#include "../shared/xis_outputs.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define KICONFD_VERSION "0.2.11"
#define LINE_MAX_LEN 512
#define COLOR_LEN 16
#define NAME_LEN 128
#define FONT_LEN 128

static Display *g_dpy;
static Window g_root;

/* XSETTINGS manager state, see the block above apply_xsettings(). */
static Window g_xs_win = None;
static Atom g_xs_selection = None;
static Atom g_xs_prop = None;
static unsigned long g_xs_serial = 0;
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
/* -1 = not yet loaded from config (apply_and_persist_defaults() fills it
 * with 0). See the export_to_other_desktops doc comment above main(). */
static int g_export_other_desktops = -1;

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

/* Opt-in (see main()'s --log/KICONFD_LOG handling): redirects every
 * later fprintf(stderr, ...)/printf() in this file to a fixed file
 * instead of the inherited stdout/stderr. Whatever launches kiconfd --
 * kisession, startx with no .xinitrc, a display manager's Xsession
 * script -- has its own, often surprising ideas about where a child's
 * stdout/stderr end up (inherited fds get mixed with the X server's own
 * banner, silently swallowed, or routed to a log the user doesn't know
 * to look at); this sidesteps all of that for the times it actually
 * matters (debugging), without kiconfd normally touching the launcher's
 * own logging at all. Line-buffered so a later crash/kill doesn't lose
 * the tail of it. */
static void redirect_log_to_file(void)
{
    char logpath[PATH_MAX];
    const char *xdg_config = getenv("XDG_CONFIG_HOME");
    if (xdg_config && *xdg_config) {
        mkdir(xdg_config, 0700);
        snprintf(logpath, sizeof(logpath), "%s/kiconfd.log", xdg_config);
    } else {
        const char *home = getenv("HOME");
        if (!home || !*home) {
            home = "/tmp";
        }
        char configdir[PATH_MAX];
        snprintf(configdir, sizeof(configdir), "%s/.config", home);
        mkdir(configdir, 0700);
        snprintf(logpath, sizeof(logpath), "%s/kiconfd.log", configdir);
    }

    int fd = open(logpath, O_CREAT | O_WRONLY | O_APPEND, 0600);
    if (fd < 0) {
        fprintf(stderr, "kiconfd: could not open log file '%s': %s (logging to the inherited stderr instead)\n",
                 logpath, strerror(errno));
        return;
    }
    dup2(fd, STDOUT_FILENO);
    dup2(fd, STDERR_FILENO);
    if (fd > STDERR_FILENO) {
        close(fd);
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);

    time_t now = time(NULL);
    char timebuf[64];
    struct tm tmv;
    localtime_r(&now, &tmv);
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", &tmv);
    fprintf(stderr, "\n---- kiconfd %s starting, pid %d, %s ----\n", KICONFD_VERSION, (int)getpid(), timebuf);
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
/* generic subprocess helper (xrandr), mirroring kiconf/common.c's      */
/* run_fire() -- duplicated rather than shared since kiconfd and kiconf */
/* are separate binaries with their own Makefiles.                      */
/* ------------------------------------------------------------------ */

static int run_fire(char *const argv[])
{
    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        execvp(argv[0], argv);
        _exit(127);
    }
    int status;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* Same duplication rationale as run_fire() above, mirroring kiconf/
 * common.c's own run_capture(): runs argv and captures its stdout,
 * returning 1 on a clean exit (0 otherwise) -- used to read back xinput's
 * current property values so apply_input_settings() below only fires a
 * `set-prop` for whatever actually needs changing. */
static int run_capture(char *const argv[], char *out, size_t outsz)
{
    out[0] = '\0';
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        return 0;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return 0;
    }
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(pipefd[1]);
    size_t total = 0;
    ssize_t n;
    while (total + 1 < outsz && (n = read(pipefd[0], out + total, outsz - 1 - total)) > 0) {
        total += (size_t)n;
    }
    out[total] = '\0';
    close(pipefd[0]);
    int status;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
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

/* ------------------------------------------------------------------ */
/* XSETTINGS (_XSETTINGS_S<screen>)                                     */
/* ------------------------------------------------------------------ */

/*
 * The one channel that reaches apps that are *already running*. Every
 * other backend in this file writes a config file, which a toolkit reads
 * once at startup -- so without this, "Aplicar" in kiconf only affects
 * programs launched afterwards, and the user's open windows keep the old
 * theme until they restart them. GTK2 and GTK3 both watch the XSETTINGS
 * manager and restyle live; Qt does too when built with the platform
 * theme that reads it.
 *
 * The protocol: own the _XSETTINGS_S<screen> selection with a window,
 * publish the settings as one _XSETTINGS_SETTINGS property on it, and
 * announce the ownership with a MANAGER client message so clients that
 * were already up notice. Clients then watch that property for changes,
 * which is why the serial has to move on every apply.
 *
 * The wire format is a byte-order flag, a serial, a count, then per
 * setting: type, 16-bit name length, the name padded to 4 bytes, that
 * setting's own serial, and the value. Everything below is serialized
 * little-endian and the header declares LSB-first -- legal per spec (the
 * flag exists precisely so the manager may pick), and it avoids having to
 * detect the host's byte order.
 *
 * Not everything in kiconfd.conf can travel this way: XSETTINGS has no
 * key for a color palette, so the bg/fg/accent colors stay in the GTK CSS
 * and Qt palette files, and only take effect for newly started apps.
 */

#define XS_TYPE_INT 0
#define XS_TYPE_STRING 1

static unsigned char g_xs_buf[8192];
static size_t g_xs_len;
static size_t g_xs_count_off;
static unsigned long g_xs_count;

static void xs_u8(unsigned v)
{
    if (g_xs_len < sizeof(g_xs_buf)) {
        g_xs_buf[g_xs_len++] = (unsigned char)(v & 0xff);
    }
}

static void xs_u16(unsigned v)
{
    xs_u8(v);
    xs_u8(v >> 8);
}

static void xs_u32(unsigned long v)
{
    xs_u16((unsigned)(v & 0xffff));
    xs_u16((unsigned)((v >> 16) & 0xffff));
}

static void xs_bytes(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        xs_u8((unsigned char)s[i]);
    }
}

/* Every variable-length field is padded to a 4-byte boundary. */
static void xs_pad(void)
{
    while (g_xs_len & 3) {
        xs_u8(0);
    }
}

static void xs_begin(void)
{
    g_xs_len = 0;
    g_xs_count = 0;
    xs_u8(0); /* byte order: LSB first */
    xs_u8(0);
    xs_u8(0);
    xs_u8(0); /* padding */
    xs_u32(g_xs_serial);
    g_xs_count_off = g_xs_len;
    xs_u32(0); /* n_settings, patched by xs_end() */
}

static void xs_header(int type, const char *name)
{
    size_t n = strlen(name);
    xs_u8((unsigned)type);
    xs_u8(0); /* padding */
    xs_u16((unsigned)n);
    xs_bytes(name, n);
    xs_pad();
    /* Per-setting "last changed" serial. kiconfd rewrites everything on
     * every apply rather than tracking which individual keys moved, so
     * they all carry the current serial -- clients re-read the lot. */
    xs_u32(g_xs_serial);
    g_xs_count++;
}

static void xs_string(const char *name, const char *val)
{
    xs_header(XS_TYPE_STRING, name);
    size_t n = strlen(val);
    xs_u32(n);
    xs_bytes(val, n);
    xs_pad();
}

static void xs_int(const char *name, long val)
{
    xs_header(XS_TYPE_INT, name);
    xs_u32((unsigned long)val);
}

static void xs_end(void)
{
    g_xs_buf[g_xs_count_off + 0] = (unsigned char)(g_xs_count & 0xff);
    g_xs_buf[g_xs_count_off + 1] = (unsigned char)((g_xs_count >> 8) & 0xff);
    g_xs_buf[g_xs_count_off + 2] = (unsigned char)((g_xs_count >> 16) & 0xff);
    g_xs_buf[g_xs_count_off + 3] = (unsigned char)((g_xs_count >> 24) & 0xff);
}

/* Claims the manager selection and announces it. Returns 1 if kiconfd is
 * the XSETTINGS manager afterwards. */
static int init_xsettings(void)
{
    char selname[64];
    snprintf(selname, sizeof(selname), "_XSETTINGS_S%d", DefaultScreen(g_dpy));
    g_xs_selection = XInternAtom(g_dpy, selname, False);
    g_xs_prop = XInternAtom(g_dpy, "_XSETTINGS_SETTINGS", False);

    Window existing = XGetSelectionOwner(g_dpy, g_xs_selection);
    if (existing != None) {
        /* Another settings daemon (xfsettingsd, gsd-xsettings, ...) is
         * already the manager. Taking the selection from it would just
         * start a fight over every GTK app's theme, so leave it alone --
         * the file backends above still work, they just won't update
         * running apps. */
        fprintf(stderr, "kiconfd: another XSETTINGS manager already owns %s; "
                        "not claiming it (running apps won't update live)\n",
                selname);
        return 0;
    }

    g_xs_win = XCreateSimpleWindow(g_dpy, g_root, -100, -100, 1, 1, 0, 0, 0);
    XSetSelectionOwner(g_dpy, g_xs_selection, g_xs_win, CurrentTime);
    if (XGetSelectionOwner(g_dpy, g_xs_selection) != g_xs_win) {
        fprintf(stderr, "kiconfd: could not become the XSETTINGS manager\n");
        XDestroyWindow(g_dpy, g_xs_win);
        g_xs_win = None;
        return 0;
    }

    /* Clients started before us are watching the root for this, and it's
     * how they learn to go look for the settings property at all. */
    XClientMessageEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = ClientMessage;
    ev.window = g_root;
    ev.message_type = XInternAtom(g_dpy, "MANAGER", False);
    ev.format = 32;
    ev.data.l[0] = CurrentTime;
    ev.data.l[1] = (long)g_xs_selection;
    ev.data.l[2] = (long)g_xs_win;
    XSendEvent(g_dpy, g_root, False, StructureNotifyMask, (XEvent *)&ev);

    fprintf(stderr, "kiconfd: XSETTINGS manager for %s\n", selname);
    return 1;
}

static void apply_xsettings(void)
{
    if (g_xs_win == None) {
        return;
    }

    /* Clients compare serials to decide what to re-read, so this must
     * move on every apply or a reload would be silently ignored. */
    g_xs_serial++;

    xs_begin();
    /* XSETTINGS carries a single theme name, and GTK2 honours it over
     * ~/.gtkrc-2.0. So gtk3_theme is what goes on the wire and GTK2 apps
     * follow it too; gtk2_theme only still matters for apps that start
     * with no XSETTINGS manager around. */
    xs_string("Net/ThemeName", g_gtk3_theme);
    xs_string("Net/IconThemeName", g_icon_theme);
    xs_string("Gtk/FontName", g_font_general);
    xs_string("Gtk/CursorThemeName", g_cursor_theme);
    xs_int("Gtk/CursorThemeSize", g_cursor_size);
    xs_end();

    XChangeProperty(g_dpy, g_xs_win, g_xs_prop, g_xs_prop, 8, PropModeReplace, g_xs_buf, (int)g_xs_len);

    if (strcmp(g_gtk2_theme, g_gtk3_theme) != 0) {
        fprintf(stderr,
                "kiconfd: note: gtk2_theme ('%s') differs from gtk3_theme ('%s'), but XSETTINGS "
                "carries only one theme name -- running GTK2 apps will follow '%s'\n",
                g_gtk2_theme, g_gtk3_theme, g_gtk3_theme);
    }
}

static void apply_cursor_theme(void)
{
    /* XcursorLibraryLoadCursor()'s argument is a *cursor* name
     * ("left_ptr", "watch", "xterm"), never a theme name. Passing the
     * theme meant asking for a glyph called e.g. "Adwaita", which no
     * theme has: the load always failed and the root window was left
     * with no cursor at all. X renders that as an *invisible* pointer
     * over the root and over every window that inherits from it -- which
     * is xisback and xispanel, neither of which sets a cursor of its own
     * (deliberately: inheriting is what makes them follow this one live,
     * including on a later reload). The theme is selected with
     * XcursorSetTheme() instead, and only then is a real cursor loaded
     * out of it. */
    XcursorSetTheme(g_dpy, g_cursor_theme);
    if (g_cursor_size > 0) {
        XcursorSetDefaultSize(g_dpy, g_cursor_size);
    }

    /* "left_ptr" is the X11 name for the plain arrow and "default" the
     * freedesktop one; themes normally ship both as aliases of each
     * other, but not all of them do. */
    static const char *const arrow_names[] = {"left_ptr", "default", "arrow", NULL};
    Cursor cur = None;
    for (int i = 0; arrow_names[i] && cur == None; i++) {
        cur = XcursorLibraryLoadCursor(g_dpy, arrow_names[i]);
    }
    if (cur == None) {
        fprintf(stderr, "kiconfd: no arrow cursor in theme '%s', leaving the root cursor as-is\n", g_cursor_theme);
        return;
    }
    XDefineCursor(g_dpy, g_root, cur);
    XFreeCursor(g_dpy, cur);
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
/* Screens (xrandr) -- $XDG_CONFIG_HOME/kiconfd-screens.conf            */
/* ------------------------------------------------------------------ */

#define MAX_SCREENS 16

typedef struct {
    char name[NAME_LEN];
    int enabled, primary;
    char mode[16], rate[16];
    int x, y;
    char rotation[16];
    double scale_x, scale_y;
    int dpi;
    char mirror_of[NAME_LEN];
} ScreenLayout;

static char g_screenspath[PATH_MAX];

static void resolve_screenspath(void)
{
    const char *xdg_config = getenv("XDG_CONFIG_HOME");
    if (xdg_config && *xdg_config) {
        snprintf(g_screenspath, sizeof(g_screenspath), "%s/kiconfd-screens.conf", xdg_config);
        return;
    }
    const char *home = getenv("HOME");
    if (!home || !*home) {
        home = "/tmp";
    }
    snprintf(g_screenspath, sizeof(g_screenspath), "%s/.config/kiconfd-screens.conf", home);
}

/* Parses kiconfd-screens.conf -- one line per connected output, fields
 * in the fixed order kiconf's save_screens_layout() writes them in:
 * NAME ENABLED MODE RATE X Y ROTATION PRIMARY SCALE_X SCALE_Y DPI MIRROR
 * ('-' standing in for an absent MODE/RATE/MIRROR). Returns the number
 * of outputs parsed. */
static int load_screens_layout(ScreenLayout *outs, int max)
{
    FILE *f = fopen(g_screenspath, "r");
    if (!f) {
        return 0;
    }
    char line[512];
    int n = 0;
    while (n < max && fgets(line, sizeof(line), f)) {
        char *l = trim(line);
        if (!*l || *l == '#') {
            continue;
        }
        ScreenLayout *o = &outs[n];
        memset(o, 0, sizeof(*o));
        char mirror[NAME_LEN];
        int got = sscanf(l, "%127s %d %15s %15s %d %d %15s %d %lf %lf %d %127s",
                           o->name, &o->enabled, o->mode, o->rate, &o->x, &o->y,
                           o->rotation, &o->primary, &o->scale_x, &o->scale_y, &o->dpi, mirror);
        if (got != 12) {
            fprintf(stderr, "kiconfd: screens: skipping malformed line: '%s'\n", l);
            continue;
        }
        if (!strcmp(o->mode, "-")) {
            o->mode[0] = '\0';
        }
        if (!strcmp(o->rate, "-")) {
            o->rate[0] = '\0';
        }
        if (strcmp(mirror, "-") != 0) {
            snprintf(o->mirror_of, sizeof(o->mirror_of), "%s", mirror);
        }
        n++;
    }
    fclose(f);
    return n;
}

/* Replays a saved layout via one `xrandr` call covering every saved
 * output that's still connected (an --output block for a name xrandr
 * doesn't currently recognize fails the whole call, so those are
 * skipped rather than attempted) -- same one-shot-covering-everything
 * shape as xisconf.py's build_command(). Silently does nothing if no
 * layout was ever saved (kiconf's Telas tab never applied one, or
 * XDG_CONFIG_HOME has no kiconfd-screens.conf yet).
 *
 * Each saved ScreenLayout::name can be an "edid:..." stable-monitor id or
 * a literal connector name (see shared/xis_outputs.h) -- resolved to
 * whatever connector that monitor currently really is via
 * xis_resolve_output() (edid: ids) or xis_build_output_rename_map()'s
 * positional self-heal (plain names only, same as before this existed),
 * exactly like xisback/xispanel resolve the same kind of saved id.
 *
 * Returns the number of saved outputs that did NOT resolve to anything
 * currently connected (0 meaning every saved output was applied, or
 * there was no saved layout at all) -- the caller (main(), see its own
 * retry loop) uses this to retry a few times a moment later rather than
 * give up for the rest of the session: unlike xisback/xispanel, which
 * react to every RRScreenChangeNotify for as long as they run, kiconfd
 * has no event loop at all (just pause() waiting for signals -- see
 * main()) and only ever calls this once, right at startup, before
 * kiwm/kicomp even start. A monitor whose EDID the X server hasn't
 * finished reading yet at that exact moment (a real race -- see
 * xisback.c's own reconcile_layer_outputs() for the same problem on its
 * side) would otherwise silently keep the session on whatever default
 * layout the driver picked, for good, since there is nothing later to
 * ever retry it. */
static int apply_screens_layout(void)
{
    ScreenLayout screens[MAX_SCREENS];
    int n = load_screens_layout(screens, MAX_SCREENS);
    if (n == 0) {
        fprintf(stderr, "kiconfd: screens: no saved layout at '%s' (or it was empty/unparsable), nothing to apply\n",
                 g_screenspath);
        return 0;
    }

    const char *saved_ptrs[MAX_SCREENS];
    for (int i = 0; i < n; i++) {
        saved_ptrs[i] = screens[i].name;
    }
    XisOutputRename rename_map[MAX_SCREENS];
    int n_rename = xis_build_output_rename_map(g_dpy, saved_ptrs, n, rename_map, MAX_SCREENS);

    char *argv[8 + MAX_SCREENS * 14];
    int ac = 0;
    argv[ac++] = "xrandr";
    char bufs[MAX_SCREENS][6][32];
    char resolved_name[MAX_SCREENS][XIS_OUTPUT_STR_LEN];
    char resolved_mirror[MAX_SCREENS][XIS_OUTPUT_STR_LEN];
    int n_applied = 0;

    for (int i = 0; i < n; i++) {
        ScreenLayout *o = &screens[i];
        const char *want = strncmp(o->name, "edid:", 5) == 0 ? o->name : xis_apply_output_rename(rename_map, n_rename, o->name);
        /* forced=1: this whole function runs at most a handful of times,
         * right at session startup (see main()'s own retry loop around
         * the call to this function) -- not a hot path, and exactly the
         * "X server's RandR cache might still be missing this monitor's
         * EDID" moment forcing a poll matters most for. See
         * xis_list_outputs()'s own doc comment on `forced`. */
        if (!xis_resolve_output(g_dpy, want, resolved_name[i], sizeof(resolved_name[i]), 1)) {
            continue;
        }
        n_applied++;
        argv[ac++] = "--output";
        argv[ac++] = resolved_name[i];
        if (!o->enabled) {
            argv[ac++] = "--off";
            continue;
        }
        if (o->mirror_of[0]) {
            const char *mirror_want = strncmp(o->mirror_of, "edid:", 5) == 0 ? o->mirror_of : xis_apply_output_rename(rename_map, n_rename, o->mirror_of);
            if (xis_resolve_output(g_dpy, mirror_want, resolved_mirror[i], sizeof(resolved_mirror[i]), 1)) {
                argv[ac++] = "--same-as";
                argv[ac++] = resolved_mirror[i];
            }
        } else {
            if (o->mode[0]) {
                argv[ac++] = "--mode";
                argv[ac++] = o->mode;
            }
            if (o->rate[0]) {
                argv[ac++] = "--rate";
                argv[ac++] = o->rate;
            }
            snprintf(bufs[i][0], sizeof(bufs[i][0]), "%dx%d", o->x, o->y);
            argv[ac++] = "--pos";
            argv[ac++] = bufs[i][0];
        }
        argv[ac++] = "--rotate";
        argv[ac++] = o->rotation[0] ? o->rotation : "normal";
        argv[ac++] = o->primary ? "--primary" : "--noprimary";
        double sx = fabs(o->scale_x) > 1e-6 ? o->scale_x : 1.0;
        double sy = fabs(o->scale_y) > 1e-6 ? o->scale_y : 1.0;
        snprintf(bufs[i][1], sizeof(bufs[i][1]), "%.4fx%.4f", sx, sy);
        argv[ac++] = "--scale";
        argv[ac++] = bufs[i][1];
        if (o->dpi) {
            argv[ac++] = "--set";
            argv[ac++] = "DPI";
            snprintf(bufs[i][2], sizeof(bufs[i][2]), "%d", o->dpi);
            argv[ac++] = bufs[i][2];
        }
    }
    argv[ac] = NULL;

    if (n_applied == 0) {
        fprintf(stderr, "kiconfd: screens: none of the %d saved output(s) match a currently connected "
                        "output, applying nothing\n", n);
        return n;
    }
    if (run_fire(argv) != 0) {
        fprintf(stderr, "kiconfd: screens: xrandr call failed applying saved layout\n");
    } else {
        fprintf(stderr, "kiconfd: applied saved screen layout (%d/%d output(s))\n", n_applied, n);
    }
    return n - n_applied;
}

/* ------------------------------------------------------------------ */
/* Input (kiconfd-input.conf)                                          */
/* ------------------------------------------------------------------ */

/* Set by kiconf's Entrada tab, applied here once at the start of each
 * session -- see entrada.c's own doc comment on why this needs a
 * separate file from kiconfd.conf (that one is rewritten whole by the
 * Aparencia tab's own Aplicar, which would silently drop these keys). */
typedef struct {
    int numlock_on_start;
    int toggle_mods_on_press;
    int kick_hotkeys_on_release;
} InputSessionConfig;

static char g_inputpath[PATH_MAX];

static void resolve_inputpath(void)
{
    const char *xdg_config = getenv("XDG_CONFIG_HOME");
    if (xdg_config && *xdg_config) {
        snprintf(g_inputpath, sizeof(g_inputpath), "%s/kiconfd-input.conf", xdg_config);
        return;
    }
    const char *home = getenv("HOME");
    if (!home || !*home) {
        home = "/tmp";
    }
    snprintf(g_inputpath, sizeof(g_inputpath), "%s/.config/kiconfd-input.conf", home);
}

/* Returns 1 if kiconfd-input.conf exists (whether or not it set
 * anything to non-default) -- apply_input_settings() below uses this the
 * same way apply_nightlight() uses load_nightlight_schedule()'s return:
 * "nobody ever touched this feature" and "explicitly configured to the
 * default" need to be told apart before deciding whether to touch
 * anything. */
static int load_input_config(InputSessionConfig *c)
{
    c->numlock_on_start = 0;
    c->toggle_mods_on_press = 0;
    c->kick_hotkeys_on_release = 0;

    FILE *f = fopen(g_inputpath, "r");
    if (!f) {
        return 0;
    }
    char line[LINE_MAX_LEN];
    while (fgets(line, sizeof(line), f)) {
        char *l = trim(line);
        if (!*l || *l == '#') {
            continue;
        }
        char *eq = strchr(l, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        char *key = trim(l);
        char *val = trim(eq + 1);
        if (!strcmp(key, "numlock_on_start")) {
            c->numlock_on_start = atoi(val) != 0;
        } else if (!strcmp(key, "toggle_mods_on_press")) {
            c->toggle_mods_on_press = atoi(val) != 0;
        } else if (!strcmp(key, "kick_hotkeys_on_release")) {
            c->kick_hotkeys_on_release = atoi(val) != 0;
        }
    }
    fclose(f);
    return 1;
}

/* Same tree-drawing-glyph/id= stripping kiconf/entrada.c's own
 * clean_xinput_name()/master_keyboard_name() do -- duplicated rather
 * than shared, see this file's own run_fire() doc comment. */
static void clean_xinput_name(char *s)
{
    char *t = trim(s);
    while (*t && !isalnum((unsigned char)*t)) {
        t++;
    }
    if (t != s) {
        memmove(s, t, strlen(t) + 1);
    }
}

static void master_keyboard_name(char *out, size_t outsz)
{
    snprintf(out, outsz, "Virtual core keyboard");
    char *argv[] = {"xinput", "list", NULL};
    char buf[8192];
    if (!run_capture(argv, buf, sizeof(buf))) {
        return;
    }
    char *save = NULL;
    char *line = strtok_r(buf, "\n", &save);
    while (line) {
        if (strstr(line, "master keyboard")) {
            char *idpos = strstr(line, "id=");
            if (idpos) {
                char name[NAME_LEN];
                size_t len = (size_t)(idpos - line);
                if (len >= sizeof(name)) {
                    len = sizeof(name) - 1;
                }
                memcpy(name, line, len);
                name[len] = '\0';
                clean_xinput_name(name);
                if (name[0]) {
                    snprintf(out, outsz, "%s", name);
                    return;
                }
            }
        }
        line = strtok_r(NULL, "\n", &save);
    }
}

/* Same "<Prop Name> (id):\t<value>" line scan as kiconf/entrada.c's own
 * xinput_get_prop_line(). */
static int xinput_get_prop_line(const char *output, const char *propname, char *out, size_t outsz)
{
    out[0] = '\0';
    size_t plen = strlen(propname);
    const char *p = strstr(output, propname);
    if (!p) {
        return 0;
    }
    const char *line_end = strchr(p, '\n');
    if (!line_end) {
        line_end = p + strlen(p);
    }
    const char *colon = NULL;
    for (const char *q = p + plen; q < line_end; q++) {
        if (*q == ':') {
            colon = q;
        }
    }
    if (!colon) {
        return 0;
    }
    const char *v = colon + 1;
    while (*v == ' ' || *v == '\t') {
        v++;
    }
    size_t len = (size_t)(line_end - v);
    if (len >= outsz) {
        len = outsz - 1;
    }
    memcpy(out, v, len);
    out[len] = '\0';
    while (len > 0 && (out[len - 1] == ' ' || out[len - 1] == '\t' || out[len - 1] == '\r')) {
        out[--len] = '\0';
    }
    return 1;
}

/* NumLock isn't always Mod2Mask -- same numlock_mask() technique used by
 * xispanel/hotkey.c and xiskeys.c (see either's own comment): whatever
 * modifier slot the server's current map puts Num_Lock's keycode in. */
static unsigned int numlock_mask(void)
{
    KeyCode numlock_kc = XKeysymToKeycode(g_dpy, XK_Num_Lock);
    if (!numlock_kc) {
        return 0;
    }
    XModifierKeymap *map = XGetModifierMapping(g_dpy);
    if (!map) {
        return 0;
    }
    unsigned int mask = 0;
    for (int mod = 0; mod < 8; mod++) {
        for (int k = 0; k < map->max_keypermod; k++) {
            if (map->modifiermap[mod * map->max_keypermod + k] == numlock_kc) {
                mask = 1u << mod;
            }
        }
    }
    XFreeModifiermap(map);
    return mask;
}

/* Applies kiconfd-input.conf: NumLock's lock state via XkbLockModifiers
 * (no external tool needed, and the only way to set a modifier *lock*
 * rather than send a key event), the two XiS keyboard flags via `xinput
 * set-prop` on the master keyboard -- same calls kiconf/entrada.c's own
 * Aplicar makes live, just replayed here for a session kiconf wasn't
 * open for. Each one is read back first and only touched if it doesn't
 * already match, same "diff before firing" shape as entrada.c's
 * apply_kbd_diff() -- called both once at startup and on every SIGHUP
 * (see reload_config()), so re-running it mid-session never re-fires a
 * `set-prop` that already took. */
static void apply_input_settings(void)
{
    InputSessionConfig c;
    if (!load_input_config(&c)) {
        fprintf(stderr, "kiconfd: input: no '%s', nothing to apply\n", g_inputpath);
        return;
    }
    fprintf(stderr, "kiconfd: input: loaded '%s' (numlock_on_start=%d toggle_mods_on_press=%d "
                    "kick_hotkeys_on_release=%d)\n",
             g_inputpath, c.numlock_on_start, c.toggle_mods_on_press, c.kick_hotkeys_on_release);

    unsigned int nlmask = numlock_mask();
    if (!nlmask) {
        fprintf(stderr, "kiconfd: input: could not find NumLock's modifier bit "
                        "(no Num_Lock keycode, or it's not bound to any modifier) -- skipping\n");
    } else {
        XkbStateRec state;
        Bool got_state = XkbGetState(g_dpy, XkbUseCoreKbd, &state);
        int locked = got_state && (state.locked_mods & nlmask) != 0;
        fprintf(stderr, "kiconfd: input: numlock mask=0x%x got_state=%d locked_mods=0x%x "
                        "currently_locked=%d want=%d\n",
                 nlmask, got_state, got_state ? state.locked_mods : 0, locked, c.numlock_on_start);
        if (locked != c.numlock_on_start) {
            Bool ok = XkbLockModifiers(g_dpy, XkbUseCoreKbd, nlmask, c.numlock_on_start ? nlmask : 0);
            XFlush(g_dpy);
            fprintf(stderr, "kiconfd: input: XkbLockModifiers(%s) -> %d\n",
                     c.numlock_on_start ? "on" : "off", ok);
        }
    }

    char kbd[NAME_LEN];
    master_keyboard_name(kbd, sizeof(kbd));
    char *argv[] = {"xinput", "list-props", kbd, NULL};
    char out[8192];
    if (!run_capture(argv, out, sizeof(out))) {
        fprintf(stderr, "kiconfd: input: 'xinput list-props \"%s\"' failed, skipping XiS kbd flags\n", kbd);
        return;
    }
    char val[64];
    if (xinput_get_prop_line(out, "Toggle Lock Modifiers On Press", val, sizeof(val))) {
        int cur = atoi(val) != 0;
        if (cur != c.toggle_mods_on_press) {
            char *set[] = {"xinput", "set-prop", kbd, "Toggle Lock Modifiers On Press",
                             c.toggle_mods_on_press ? "1" : "0", NULL};
            run_fire(set);
        }
    }
    if (xinput_get_prop_line(out, "Kick Hotkeys On Release", val, sizeof(val))) {
        int cur = atoi(val) != 0;
        if (cur != c.kick_hotkeys_on_release) {
            char *set[] = {"xinput", "set-prop", kbd, "Kick Hotkeys On Release",
                             c.kick_hotkeys_on_release ? "1" : "0", NULL};
            run_fire(set);
        }
    }
}

/* ------------------------------------------------------------------ */
/* night light (kiconfd-nightlight.conf)                                */
/* ------------------------------------------------------------------ */

/* Schedule set by kiconf's Energia tab, applied here via `xsct`
 * (https://github.com/faf0/sct) -- see xisserve/PROTOCOL.md's `--energy`
 * entry for the other writer of this same file: its checkbox+slider is
 * the "right now, manual" side of the same feature, toggling `enabled`
 * and, while off, setting `temp` directly. Neither of those other two
 * processes ever calls xsct on a timer themselves -- only kiconfd does,
 * since it's the one thing in this session that's already resident and
 * already has a loop to hang a periodic check off of.
 *
 * Checked once at startup and then every NIGHTLIGHT_POLL_SEC while
 * main()'s loop is otherwise just waiting on a signal (see main()'s own
 * comment) -- a plain wall-clock poll rather than computing the exact
 * next transition and sleeping until then, since the loop already has to
 * wake up periodically for this and the extra precision buys nothing a
 * user would notice for a screen tint. SIGHUP (kiconf's Aplicar, or
 * xisserve's checkbox) short-circuits the wait so a change made by hand
 * still takes effect immediately rather than up to NIGHTLIGHT_POLL_SEC
 * late. */
#define NIGHTLIGHT_POLL_SEC 60

typedef struct {
    int enabled;
    int start_min; /* minutes since midnight */
    int end_min;
    int temp;
} NightlightSchedule;

static char g_nightlightpath[PATH_MAX];

static void resolve_nightlightpath(void)
{
    const char *xdg_config = getenv("XDG_CONFIG_HOME");
    if (xdg_config && *xdg_config) {
        snprintf(g_nightlightpath, sizeof(g_nightlightpath), "%s/kiconfd-nightlight.conf", xdg_config);
        return;
    }
    const char *home = getenv("HOME");
    if (!home || !*home) {
        home = "/tmp";
    }
    snprintf(g_nightlightpath, sizeof(g_nightlightpath), "%s/.config/kiconfd-nightlight.conf", home);
}

/* "HH:MM" -> minutes since midnight, clamped to a valid time of day on
 * anything malformed rather than propagating garbage into the window
 * check below. */
static int parse_hhmm(const char *s)
{
    int h = 0, m = 0;
    sscanf(s, "%d:%d", &h, &m);
    if (h < 0 || h > 23) {
        h = 0;
    }
    if (m < 0 || m > 59) {
        m = 0;
    }
    return h * 60 + m;
}

/* Returns 1 if kiconfd-nightlight.conf exists -- apply_nightlight()'s
 * manual-mode branch needs this to tell "nobody has ever touched the
 * night light feature" (stay at the display's native temperature) apart
 * from "the struct's in-memory defaults" (which happen to be the same
 * zeroed/4000K values either way), see its own comment. */
static int load_nightlight_schedule(NightlightSchedule *c)
{
    c->enabled = 0;
    c->start_min = 20 * 60;
    c->end_min = 6 * 60;
    c->temp = 4000;

    FILE *f = fopen(g_nightlightpath, "r");
    if (!f) {
        return 0;
    }
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        char *l = trim(line);
        if (!*l || *l == '#') {
            continue;
        }
        char *eq = strchr(l, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        char *key = trim(l);
        char *val = trim(eq + 1);
        if (!strcmp(key, "enabled")) {
            c->enabled = atoi(val) ? 1 : 0;
        } else if (!strcmp(key, "start")) {
            c->start_min = parse_hhmm(val);
        } else if (!strcmp(key, "end")) {
            c->end_min = parse_hhmm(val);
        } else if (!strcmp(key, "temp")) {
            c->temp = atoi(val);
        }
    }
    fclose(f);
    return 1;
}

/* `now` within [start, end), wrapping past midnight when end <= start
 * (the ordinary case for a night light: e.g. start=20:00, end=06:00) --
 * start == end is treated as "never on" rather than "always on", since
 * that's almost certainly an unset/zeroed field, not a deliberate
 * 24-hour request. */
static int nightlight_time_in_window(int now_min, int start_min, int end_min)
{
    if (start_min == end_min) {
        return 0;
    }
    if (start_min < end_min) {
        return now_min >= start_min && now_min < end_min;
    }
    return now_min >= start_min || now_min < end_min;
}

static int have_cmd(const char *name)
{
    char *argv[] = {"sh", "-c", NULL, NULL};
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "command -v %s >/dev/null 2>&1", name);
    argv[2] = cmd;
    return run_fire(argv) == 0;
}

/* -1 = not yet applied this run, 0 = day/off last applied, 1 = night
 * temperature last applied -- so a call that finds nothing changed since
 * the last one doesn't re-run xsct every NIGHTLIGHT_POLL_SEC for no
 * reason. Tracks the temperature too: editing just the number in kiconf
 * while already inside the window (no on/off transition) still needs a
 * fresh xsct call to pick it up. */
static int g_nightlight_applied = -1;
static int g_nightlight_applied_temp = -1;

/* Same JSON-line-over-Unix-socket send kiconf/common.c's own
 * xispanel_reload() does (json_line_send() there) -- duplicated rather
 * than shared, same as every other cross-binary bit of this codebase
 * (nothing here links against kiconf's object files). Fire-and-forget:
 * doesn't wait for or read the response, and silently does nothing if
 * xispanel isn't running or its socket doesn't exist (connect() just
 * fails) -- a night light toggle showing no popup because nothing was
 * there to show it is fine; kiconfd stalling or erroring over a missing
 * panel process is not. `summary`/`icon` are always one of this file's
 * own fixed strings (never user input), so no JSON escaping is needed. */
static void notify_xispanel_osd(const char *summary, const char *icon)
{
    const char *rundir = getenv("XDG_RUNTIME_DIR");
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/xispanel-ctl.sock", (rundir && *rundir) ? rundir : "/tmp");

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return;
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
        char line[256];
        snprintf(line, sizeof(line), "{\"cmd\":\"OSD\",\"summary\":\"%s\",\"icon\":\"%s\"}\n", summary, icon);
        ssize_t unused = write(fd, line, strlen(line));
        (void)unused; /* best-effort, see the function's own doc comment */
        /* xispanel always writes a response line back -- draining it
         * (rather than just closing) avoids an EPIPE on *its* side that
         * would otherwise land in its own log every time this fires. The
         * response body itself isn't useful here (nothing to react to),
         * so it's read and discarded, not parsed. */
        shutdown(fd, SHUT_WR);
        char resp[64];
        while (read(fd, resp, sizeof(resp)) > 0) {
        }
    }
    close(fd);
}

static void apply_nightlight(void)
{
    NightlightSchedule c;
    int have_config = load_nightlight_schedule(&c);

    /* Whether this call is transitioning out of a real prior state
     * (0 or 1) rather than kiconfd's own startup (-1, "not yet applied
     * this run") -- only a real transition is worth a toast. Without
     * this, every session start would pop "luz noturna desativada" the
     * instant kiconfd applies its very first day-default reset, even
     * though nothing the user would recognize as a *change* happened. */
    int had_prior_state = g_nightlight_applied != -1;

    if (!c.enabled) {
        /* Manual mode (xisserve's --energy checkbox unchecked, or the
         * schedule simply never turned on): apply whatever temperature
         * was last set by hand instead of resetting to day -- xisserve's
         * slider saves `temp` here on every drag precisely so a fresh X
         * session (gamma always starts back at native, xsct's own state
         * doesn't survive a logout/login any more than xrandr's does)
         * picks the same tint back up instead of coming up plain until
         * the user revisits the slider. Gated on have_config: nobody
         * ever having touched kiconf's Energia tab or xisserve's
         * --energy page at all must still mean "leave the display
         * alone", not "apply the struct's bare 4000K default out of
         * nowhere". */
        if (!have_config) {
            if (g_nightlight_applied != 0) {
                run_fire((char *const[]){"xsct", NULL});
                if (had_prior_state) {
                    notify_xispanel_osd("Luz noturna desativada", "night-light-symbolic");
                }
                g_nightlight_applied = 0;
                g_nightlight_applied_temp = -1;
            }
            return;
        }
        if (g_nightlight_applied != 1 || g_nightlight_applied_temp != c.temp) {
            char tempstr[16];
            snprintf(tempstr, sizeof(tempstr), "%d", c.temp);
            run_fire((char *const[]){"xsct", tempstr, NULL});
            g_nightlight_applied = 1;
            g_nightlight_applied_temp = c.temp;
        }
        return;
    }

    if (!have_cmd("xsct")) {
        /* Nothing to do, and nothing to warn about on every poll --
         * kiconf's Energia tab already tells the user xsct is missing
         * when they open it. */
        return;
    }

    time_t t = time(NULL);
    struct tm lt;
    localtime_r(&t, &lt);
    int now_min = lt.tm_hour * 60 + lt.tm_min;

    if (nightlight_time_in_window(now_min, c.start_min, c.end_min)) {
        if (g_nightlight_applied != 1 || g_nightlight_applied_temp != c.temp) {
            char tempstr[16];
            snprintf(tempstr, sizeof(tempstr), "%d", c.temp);
            run_fire((char *const[]){"xsct", tempstr, NULL});
            /* Only the on/off edge gets a toast, not a mid-window
             * temperature-only change (kiconf's Energia tab Aplicar
             * while already inside the window) -- that one only needed
             * a fresh xsct call, not an announcement. */
            if (had_prior_state && g_nightlight_applied != 1) {
                notify_xispanel_osd("Luz noturna ativada", "night-light-symbolic");
            }
            g_nightlight_applied = 1;
            g_nightlight_applied_temp = c.temp;
        }
    } else {
        if (g_nightlight_applied != 0) {
            run_fire((char *const[]){"xsct", NULL});
            if (had_prior_state) {
                notify_xispanel_osd("Luz noturna desativada", "night-light-symbolic");
            }
            g_nightlight_applied = 0;
            g_nightlight_applied_temp = -1;
        }
    }
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
    fprintf(f, "export_to_other_desktops = %d\n", g_export_other_desktops > 0 ? 1 : 0);
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
        } else if (!strcmp(key, "export_to_other_desktops")) {
            g_export_other_desktops = atoi(val) ? 1 : 0;
        } else {
            fprintf(stderr, "kiconfd: config: unknown key '%s', ignoring\n", key);
        }
    }
    fclose(f);
}

static void apply_all(void)
{
    /* Resources first: Xcursor.theme/size is what every *other* client
     * reads when it loads its own cursors, and kiwm reads it once at its
     * own startup. Publishing it before the visible change keeps the two
     * consistent for anything looking during the apply. */
    apply_resource_manager();
    apply_cursor_theme();
    apply_xsettings();
    if (g_export_other_desktops > 0) {
        apply_gtk2();
        apply_gtk_modern("3.0", g_gtk3_theme);
        apply_gtk_modern("4.0", g_gtk4_theme);
        apply_qt("qt5ct");
        apply_qt("qt6ct");
    }
    XFlush(g_dpy);
    fprintf(stderr, "kiconfd: applied settings (cursor='%s' gtk2='%s' gtk3='%s' gtk4='%s' qt_style='%s' export_other_desktops=%d)\n",
             g_cursor_theme, g_gtk2_theme, g_gtk3_theme, g_gtk4_theme, g_qt_style, g_export_other_desktops > 0);
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

    if (g_export_other_desktops < 0) {
        g_export_other_desktops = 0;
        changed = 1;
    }

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
    g_export_other_desktops = -1;
    load_config();
    apply_and_persist_defaults();
    apply_input_settings();
}

int main(int argc, char **argv)
{
    /* Checked before anything else (no display/lock/config needed) so
     * `kiconfd --version` works to check whether the installed build is
     * current without starting the daemon at all -- same as kiconf's
     * own `--version`/`-V`. */
    if (argc > 1 && (!strcmp(argv[1], "--version") || !strcmp(argv[1], "-V"))) {
        printf("kiconfd %s\n", KICONFD_VERSION);
        return 0;
    }

    if (argc > 1 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
        printf("kiconfd %s - session settings daemon for KiDesktop\n", KICONFD_VERSION);
        printf("Usage: kiconfd [--log|--version|-V]\n");
        printf("Config: $XDG_CONFIG_HOME/kiconfd.conf (fallback ~/.config/kiconfd.conf)\n");
        printf("Screens: $XDG_CONFIG_HOME/kiconfd-screens.conf, applied via xrandr at startup only\n");
        printf("Input: $XDG_CONFIG_HOME/kiconfd-input.conf (NumLock-on-start + XiS keyboard flags), "
                "applied at startup and on SIGHUP\n");
        printf("Night light: $XDG_CONFIG_HOME/kiconfd-nightlight.conf, applied via xsct every %ds "
                "(if installed)\n", NIGHTLIGHT_POLL_SEC);
        printf("Log: off by default (inherits stdout/stderr as usual). --log, or KICONFD_LOG=1 in "
                "the environment, redirects them to $XDG_CONFIG_HOME/kiconfd.log instead.\n");
        printf("SIGHUP reloads the config and reapplies settings.\n");
        return 0;
    }

    int want_log = (argc > 1 && !strcmp(argv[1], "--log"));
    const char *envlog = getenv("KICONFD_LOG");
    if (envlog && *envlog && strcmp(envlog, "0") != 0) {
        want_log = 1;
    }
    if (want_log) {
        redirect_log_to_file();
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
    resolve_screenspath();
    resolve_nightlightpath();
    resolve_inputpath();

    g_dpy = XOpenDisplay(NULL);
    if (!g_dpy) {
        fprintf(stderr, "kiconfd: could not open X display\n");
        return 1;
    }
    g_root = DefaultRootWindow(g_dpy);

    /* Xkb's per-Display bookkeeping (what XkbGetState()/XkbLockModifiers()
     * in apply_input_settings() need) isn't set up just by having a
     * Display* -- the client has to negotiate the extension first, same
     * as any other X extension. Skipping this makes those calls silent
     * no-ops rather than errors, which is exactly what made NumLock-on-
     * start look like it did nothing. */
    {
        int xkb_opcode, xkb_event, xkb_error, xkb_major = XkbMajorVersion, xkb_minor = XkbMinorVersion;
        if (!XkbQueryExtension(g_dpy, &xkb_opcode, &xkb_event, &xkb_error, &xkb_major, &xkb_minor)) {
            fprintf(stderr, "kiconfd: XKEYBOARD extension not available -- NumLock-on-start won't work\n");
        }
    }

    /* Before everything else: kiwm/kicomp start right after kiconfd (see
     * kisession's service order) and read the output layout at their own
     * startup, so the saved arrangement needs to be live before they do.
     *
     * Retried a few times, briefly, if some saved output didn't resolve
     * the first try -- this is normally exactly the startup race
     * apply_screens_layout()'s own doc comment describes (a monitor's
     * EDID not fully readable yet at the very first attempt, this early
     * in the session), and it typically clears within the first attempt
     * or two. Capped at ~1.5s total so a genuinely-disconnected monitor
     * (nothing more will ever resolve it) doesn't stall the rest of
     * session startup behind it for long. */
    for (int attempt = 0; attempt < 8; attempt++) {
        int unresolved = apply_screens_layout();
        if (unresolved <= 0) {
            break;
        }
        usleep(200000);
    }

    /* Claimed before the first apply_all() below, so the very first
     * publish already goes out over XSETTINGS too. */
    init_xsettings();

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGHUP, handle_signal);
    signal(SIGPIPE, SIG_IGN);

    load_config();
    apply_and_persist_defaults();
    apply_input_settings();
    apply_nightlight();

    /* sleep() rather than pause(): the loop now also has to wake up on
     * its own, without any signal, for the night light schedule (see
     * apply_nightlight()'s own comment) -- a plain signal wait has
     * nothing to wake it for that. Any of the three signals below still
     * cuts the sleep short (EINTR), so SIGHUP still reloads/reapplies
     * immediately instead of waiting up to NIGHTLIGHT_POLL_SEC. */
    while (!g_quit) {
        if (g_reload) {
            g_reload = 0;
            reload_config();
        }
        apply_nightlight();
        sleep(NIGHTLIGHT_POLL_SEC);
    }

    XCloseDisplay(g_dpy);
    fprintf(stderr, "kiconfd: shutting down\n");
    return 0;
}
