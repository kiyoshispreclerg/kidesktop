/*
 * kiconf - GTK2 configurator for KiDesktop, remake of xisconf (Python/Qt).
 *
 * Prototype scope: two working tabs plus placeholders for the rest of
 * xisconf's feature set (Screens, Pointer/Keyboard, Permissions), which
 * already has a known implementation path -- xrandr/xinput/xset
 * subprocess calls and the xisguard control socket, same as xisconf.py --
 * just not ported to GTK2/C yet. What's new here and actually implemented:
 *
 *   Aparencia -- edits kiconfd's cursor theme/size, color palette, GTK2/3/4
 *     + icon theme names, Qt style, and general/monospace fonts, all
 *     directly in $XDG_CONFIG_HOME/kiconfd.conf (fallback
 *     ~/.config/kiconfd.conf), then signals the running kiconfd with
 *     SIGHUP to reload+reapply. Theme/icon/cursor pickers are comboboxes
 *     populated by actually scanning what's installed (/usr/share/themes,
 *     /usr/share/icons, ~/.themes, ~/.icons) rather than free text, so the
 *     user can only pick something that exists; the Qt style list is a
 *     static fallback (Fusion/Windows/gtk2) since enumerating installed
 *     QStyle plugins would need linking against Qt itself. Since
 *     kiconfd.conf only reflects what kiconfd itself last applied, an
 *     "Importar da sessao atual" button (import_appearance_cb()) also
 *     scans the live places kiconfd writes to -- Xcursor.theme/size from
 *     `xrdb -query`, GTK3/4's settings.ini, ~/.gtkrc-2.0, qt5ct/qt6ct.conf
 *     -- and fills the widgets from that instead, so the tab isn't stuck
 *     showing stale/default values the first time kiconfd hasn't run yet
 *     in a session. Only fills widgets; still needs "Aplicar" to persist.
 *     The color palette has no such live source (no toolkit exposes "the
 *     current accent color" generically) so Importar leaves it alone.
 *   Atalhos -- edits xiskeys' BIND lines directly in
 *     $XDG_CONFIG_HOME/xiskeys.conf (fallback ~/.config/xiskeys.conf),
 *     then signals the running xiskeys with SIGHUP to reload.
 *   Entrada -- pointer/touchpad (xinput+libinput props), key repeat/bell
 *     (xset), and the two XiS keyboard flags (xinput props on the master
 *     keyboard), same subprocess-driven detect/diff/apply as xisconf.py's
 *     Pointer/Keyboard tab: only fields that actually changed since the
 *     last detect get a command sent, on Aplicar.
 *   Outras -- the 3rd XiS flag (DisablePrimarySelection, via xprop),
 *     DPMS + screensaver (xset), and virtual desktop count (wmctrl/EWMH),
 *     same detect/diff/apply pattern.
 *   Paineis -- plain editor for xispanel.conf (PANEL/WIDGET/THEME
 *     records, see xispanel/PROTOCOL.md), mirroring xisconf.py's Panels
 *     tab in spirit but not in UI depth: instead of a per-widget-type
 *     schema form (xisconf.py's WIDGET_TYPE_SCHEMAS, ~15 widget types
 *     worth of fields), widget/theme key=value options are edited as one
 *     raw text field per row -- still the exact on-disk format, just less
 *     hand-holding about which keys a given widget type accepts. Saving
 *     rewrites the whole file (this tab has no "running daemon state" to
 *     diff against, same as xisconf.py's) and sends RELOAD over
 *     xispanel's control socket.
 *   Permissoes -- xisguard's control socket (runtime mode via
 *     GET_STATUS/SET_STATUS, rules via LIST_RULES/ADD_RULE/REMOVE_RULE/
 *     RELOAD), same JSON-line-over-Unix-socket protocol as xispanel's.
 *
 * xisback/xisguard/xispanel don't have control sockets exposing every
 * field kiconfd would need generically, and xiskeys/kiconfd don't have
 * one at all yet (see XISDESKTOP_PLAN.md) -- so each tab here talks
 * whatever protocol that specific daemon already has (xinput/xset/xprop/
 * wmctrl subprocesses, xisguard-ctl/xispanel-ctl JSON sockets, or a
 * daemon's own config file directly), same as xisconf.py did. No JSON
 * library is linked: responses are small, flat, and known-shape, so a
 * few dozen lines of substring scanning (json_get_str/int/bool below)
 * covers it without adding a dependency for it.
 *
 *   Wallpaper -- plain xisback socket client (SET/CLEAR/CLEARALL/NEXT/
 *     LIST/ACTIONS/SETACTIONS, tab-separated lines, see
 *     xisback/PROTOCOL.md). No visual per-output canvas (that needs the
 *     Telas tab's RandR geometry, which this doesn't share state with) --
 *     just a table of active layers, click a row to load it into the
 *     set/replace form, same interaction the canvas gave in xisconf.py,
 *     minus the drawing.
 *   Telas -- xrandr layout, ported with an actual draggable canvas:
 *     GtkDrawingArea + Cairo (gdk_cairo_create() in an "expose-event"
 *     handler) is the GTK2 equivalent of the QGraphicsScene xisconf.py
 *     uses, and "button-press-event"/"motion-notify-event"/
 *     "button-release-event" on the same widget cover dragging, snapping
 *     to other outputs' edges within a few canvas pixels (screens_snap()).
 *     "Saida selecionada" also covers Mirror/DPI/Scale like xisconf.py's
 *     panel does -- but since plain `xrandr` (no --verbose) never reports
 *     them, they can't be diffed against real hardware state like every
 *     other field here: screens_redetect_preserving_extras() carries them
 *     across Detectar-novamente/Aplicar by output name instead of losing
 *     them to each fresh detect_outputs() call, so the "baseline" for
 *     just these three is "what kiconf last set them to". Deliberately
 *     NOT ported: xisconf.py's generic "advanced driver properties"
 *     system (TearFree, underscan, PRIME Sync, etc., parsed from `xrandr
 *     --verbose`'s per-output "supported:"/"range:" sub-lines) -- would
 *     need a second, much richer xrandr call and a dynamic per-property
 *     widget system. Untested against a real multi-output setup as of
 *     writing -- next session's job.
 */
#include <gtk/gtk.h>

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#define NAME_LEN 128
#define MAX_THEMES 128
#define JSON_BUF_LEN 65536

enum { COL_ACTION = 0, COL_SPEC, COL_COMMAND, N_SHORTCUT_COLS };

static GtkListStore *g_shortcuts_store;

/* Aparencia tab widgets */
static GtkWidget *g_cursor_combo;
static GtkWidget *g_cursor_size_spin;
static GtkWidget *g_gtk2_combo;
static GtkWidget *g_gtk3_combo;
static GtkWidget *g_gtk4_combo;
static GtkWidget *g_icon_combo;
static GtkWidget *g_qt_style_combo;
static GtkWidget *g_color_bg_btn;
static GtkWidget *g_color_fg_btn;
static GtkWidget *g_color_base_btn;
static GtkWidget *g_color_accent_btn;
static GtkWidget *g_color_selbg_btn;
static GtkWidget *g_color_selfg_btn;
static GtkWidget *g_font_general_btn;
static GtkWidget *g_font_mono_btn;

/* Entrada (pointer/keyboard) tab widgets + baselines */
static GtkWidget *g_pointer_combo;
static GtkWidget *g_pointer_accel_spin;
static GtkWidget *g_pointer_natural_chk, *g_pointer_lefth_chk, *g_pointer_tap_chk;
static GtkWidget *g_kbd_repeat_chk, *g_kbd_delay_spin, *g_kbd_rate_spin;
static GtkWidget *g_bell_percent_spin, *g_bell_pitch_spin, *g_bell_dur_spin;
static GtkWidget *g_toggle_mods_chk, *g_kick_hotkeys_chk;

typedef struct {
    double accel_speed;
    int has_accel;
    int natural_scroll, has_natural;
    int left_handed, has_lefth;
    int tapping, has_tap;
} PointerProps;
static PointerProps g_pointer_baseline;
static char g_pointer_baseline_device[NAME_LEN];
static char g_master_kbd[NAME_LEN];

typedef struct {
    int repeat_enabled, repeat_delay, repeat_rate;
    int bell_percent, bell_pitch, bell_duration;
} KbdState;
static KbdState g_kbd_baseline;
static int g_toggle_mods_baseline, g_kick_hotkeys_baseline;

/* Outras tab widgets + baselines */
static GtkWidget *g_disable_primsel_chk;
static GtkWidget *g_dpms_enabled_chk, *g_dpms_standby_spin, *g_dpms_suspend_spin, *g_dpms_off_spin;
static GtkWidget *g_saver_timeout_spin, *g_saver_cycle_spin, *g_prefer_blank_chk;
static GtkWidget *g_desktop_count_spin;

typedef struct {
    int dpms_enabled, dpms_standby, dpms_suspend, dpms_off;
    int saver_timeout, saver_cycle, prefer_blanking;
} PowerState;
static PowerState g_power_baseline;
static int g_disable_primsel_baseline;
static int g_desktop_count_baseline;

/* Permissoes tab widgets + baseline */
static GtkWidget *g_xg_status_label;
static GtkWidget *g_xg_no_pause_chk, *g_xg_quiet_chk, *g_xg_always_kill_chk, *g_xg_log_level_spin;
static GtkWidget *g_xg_rules_view;
static GtkListStore *g_xg_rules_store;
static GtkWidget *g_xg_action_combo, *g_xg_type_combo, *g_xg_pattern_entry;

typedef struct {
    int online;
    int no_pause, quiet, always_kill, log_level;
} XgStatus;
static XgStatus g_xg_baseline;

enum { COL_XG_TYPE = 0, COL_XG_ACTION, COL_XG_PATTERN, N_XG_COLS };

static const char *const XNOTIFY_ACTIONS[][2] = {
    {"ALL", "Todas as acoes"},
    {"ATTACH", "Usar memoria compartilhada"},
    {"SELECTION", "Acessar area de transferencia"},
    {"COMPOSITE", "Acessar outras janelas"},
    {"SCREEN", "Capturar e desenhar na tela"},
    {"RECORD", "Gravar eventos - como teclas"},
    {"CURSOR", "Acessar imagem/posicao do cursor"},
    {"INPUT_GRAB", "Capturar mouse ou teclado"},
    {"INPUT_INJECT", "Inserir eventos de teclado"},
    {"HOTKEY", "Registrar atalhos globais"},
    {"INPUT", "Capturar entrada mesmo sem foco"},
    {"MANAGE", "Listar/ler propriedades de outras janelas"},
    {"GRAB_OVERRIDE", "Permitir roubar um grab (telas de bloqueio)"},
    {"WARP", "Mover o cursor do mouse"},
    {"FOCUS", "Roubar o foco de entrada"},
    {"RANDR", "Mudar configuracao de tela"},
    {"OVERLAY", "Criar janela overlay (transparente)"},
    {NULL, NULL},
};

/* Paineis tab (xispanel.conf) widgets + state */
static GtkListStore *g_panels_store;
static GtkListStore *g_widgets_store;
static GtkWidget *g_theme_options_entry;
static char g_selected_panel[NAME_LEN] = "";

enum { COL_PANEL_NAME = 0, COL_PANEL_OUTPUT, COL_PANEL_OPTIONS, N_PANEL_COLS };
enum { COL_WIDGET_PANEL = 0, COL_WIDGET_TYPE, COL_WIDGET_OPTIONS, N_WIDGET_COLS };

/* Wallpaper tab (xisback) widgets + state */
#define MAX_WP_LAYERS 64
typedef struct {
    char output[NAME_LEN], desktop[16], mode[16];
    int interval, shuffle, fade_ms;
    char path[PATH_MAX];
} WpLayer;
static WpLayer g_wp_layers[MAX_WP_LAYERS];
static int g_n_wp_layers = 0;
static GtkListStore *g_wp_layers_store;
static GtkWidget *g_wp_layers_view;
static GtkWidget *g_wp_output_entry, *g_wp_desktop_entry, *g_wp_mode_combo;
static GtkWidget *g_wp_interval_spin, *g_wp_shuffle_chk, *g_wp_fade_spin, *g_wp_path_entry;
static GtkWidget *g_wp_action_left, *g_wp_action_right, *g_wp_action_middle, *g_wp_action_double;
static GtkWidget *g_wp_status_label;

enum { COL_WP_OUTPUT = 0, COL_WP_DESKTOP, COL_WP_MODE, COL_WP_INTERVAL, COL_WP_SHUFFLE, COL_WP_FADE, COL_WP_PATH, N_WP_COLS };

/* Telas tab (xrandr) widgets + state -- see build_telas_tab()'s doc
 * comment for what's deliberately NOT ported (scale/DPI/generic driver
 * properties). */
#define MAX_OUTPUTS 16
#define MAX_MODES 32
#define MAX_RATES 16
typedef struct {
    char rate[16];
    int is_current;
} ModeRate;
typedef struct {
    char name[16];
    ModeRate rates[MAX_RATES];
    int n_rates;
} OutMode;
typedef struct {
    char name[NAME_LEN];
    int connected, enabled, primary;
    int x, y, width, height;
    char rotation[16];
    char current_mode[16], current_rate[16];
    OutMode modes[MAX_MODES];
    int n_modes;
    /* Not detectable from plain `xrandr` (only --verbose shows them), so
     * these three start blank/neutral on every detect_outputs() call and
     * are restored from the previous in-memory state by
     * screens_restore_extras() right after -- meaning the "baseline" for
     * them is really "what kiconf last set them to", not "what the
     * hardware reports", unlike every other field here. */
    char mirror_of[NAME_LEN];
    int dpi;
    double scale_x, scale_y;
} ScreenOutput;
static ScreenOutput g_outputs[MAX_OUTPUTS];
static ScreenOutput g_outputs_baseline[MAX_OUTPUTS];
static int g_n_outputs = 0;
static int g_screens_selected = -1;
static int g_screens_dragging = 0;
static double g_screens_drag_dx, g_screens_drag_dy;
static int g_screens_syncing = 0;
static GtkWidget *g_screens_canvas;
static GtkWidget *g_screens_res_combo, *g_screens_rate_combo, *g_screens_rot_combo;
static GtkWidget *g_screens_enabled_chk, *g_screens_primary_chk;
static GtkWidget *g_screens_mirror_combo;
static GtkWidget *g_screens_dpi_spin, *g_screens_scale_spin;
static GtkWidget *g_screens_status_label;

/* ---- installed-theme scanning ----------------------------------------- */

typedef struct {
    char names[MAX_THEMES][NAME_LEN];
    int n;
} ThemeList;

static int theme_list_has(ThemeList *tl, const char *name)
{
    for (int i = 0; i < tl->n; i++) {
        if (!strcmp(tl->names[i], name)) {
            return 1;
        }
    }
    return 0;
}

static void theme_list_add(ThemeList *tl, const char *name)
{
    if (tl->n >= MAX_THEMES || theme_list_has(tl, name)) {
        return;
    }
    snprintf(tl->names[tl->n], NAME_LEN, "%s", name);
    tl->n++;
}

static int cmp_names(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

static void theme_list_sort(ThemeList *tl)
{
    qsort(tl->names, (size_t)tl->n, NAME_LEN, cmp_names);
}

/* Scans `bases` for subdirectories that themselves contain `marker`
 * (a subdirectory when `marker_is_dir` else a plain file) -- this is how
 * GTK theme dirs (marker="gtk-3.0"), icon themes (marker="index.theme",
 * file) and cursor themes (marker="cursors", dir) are all detected: a
 * theme is "installed" iff that marker exists under it. */
static void scan_marker_dirs(ThemeList *tl, const char *const *bases, const char *marker, int marker_is_dir)
{
    for (int b = 0; bases[b]; b++) {
        DIR *d = opendir(bases[b]);
        if (!d) {
            continue;
        }
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.') {
                continue;
            }
            char check[PATH_MAX];
            snprintf(check, sizeof(check), "%s/%s/%s", bases[b], e->d_name, marker);
            struct stat st;
            if (stat(check, &st) == 0 && (!marker_is_dir || S_ISDIR(st.st_mode))) {
                theme_list_add(tl, e->d_name);
            }
        }
        closedir(d);
    }
}

static void themes_dirs(char *home_themes, size_t sz, const char **bases)
{
    const char *home = getenv("HOME");
    snprintf(home_themes, sz, "%s/.themes", home ? home : "");
    bases[0] = "/usr/share/themes";
    bases[1] = home_themes;
    bases[2] = NULL;
}

static void icons_dirs(char *home_icons, size_t sz, const char **bases)
{
    const char *home = getenv("HOME");
    snprintf(home_icons, sz, "%s/.icons", home ? home : "");
    bases[0] = "/usr/share/icons";
    bases[1] = home_icons;
    bases[2] = NULL;
}

/* GTK2/3/4 theme dirs live under the *same* $theme/ directory, each
 * version's assets in its own gtk-X.0 subdirectory -- so a theme that
 * only ships a gtk-2.0 folder simply won't show up in the GTK3/4
 * comboboxes, which is the correct/expected behavior. */
static ThemeList scan_gtk_themes(const char *gtk_subdir)
{
    ThemeList tl = {.n = 0};
    char home_themes[PATH_MAX];
    const char *bases[3];
    themes_dirs(home_themes, sizeof(home_themes), bases);
    scan_marker_dirs(&tl, bases, gtk_subdir, 1);
    return tl;
}

static ThemeList scan_icon_themes(void)
{
    ThemeList tl = {.n = 0};
    char home_icons[PATH_MAX];
    const char *bases[3];
    icons_dirs(home_icons, sizeof(home_icons), bases);
    scan_marker_dirs(&tl, bases, "index.theme", 0);
    return tl;
}

static ThemeList scan_cursor_themes(void)
{
    ThemeList tl = {.n = 0};
    char home_icons[PATH_MAX];
    const char *bases[3];
    icons_dirs(home_icons, sizeof(home_icons), bases);
    scan_marker_dirs(&tl, bases, "cursors", 1);
    return tl;
}

/* Builds a GTK2 text combobox from `tl`, pre-selecting `current` -- if
 * `current` isn't among the scanned entries (a config value that predates
 * a theme being uninstalled, or just untested) it's appended too, so the
 * combo never silently discards what's actually configured. */
static GtkWidget *make_theme_combo(ThemeList *tl, const char *current)
{
    theme_list_sort(tl);
    GtkWidget *combo = gtk_combo_box_new_text();
    int idx = -1;
    for (int i = 0; i < tl->n; i++) {
        gtk_combo_box_append_text(GTK_COMBO_BOX(combo), tl->names[i]);
        if (!strcmp(tl->names[i], current)) {
            idx = i;
        }
    }
    if (idx < 0 && current && current[0]) {
        gtk_combo_box_append_text(GTK_COMBO_BOX(combo), current);
        idx = tl->n;
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), idx >= 0 ? idx : (tl->n > 0 ? 0 : -1));
    return combo;
}

static gchar *combo_active_text_or(GtkWidget *combo, const char *fallback)
{
    gchar *t = gtk_combo_box_get_active_text(GTK_COMBO_BOX(combo));
    if (t && *t) {
        return t;
    }
    g_free(t);
    return g_strdup(fallback);
}

/* ---- config path resolution (mirrors kiconfd.c/xiskeys.c exactly) ---- */

static void resolve_path(const char *filename, char *out, size_t outsz)
{
    const char *xdg_config = getenv("XDG_CONFIG_HOME");
    if (xdg_config && *xdg_config) {
        snprintf(out, outsz, "%s/%s", xdg_config, filename);
        return;
    }
    const char *home = getenv("HOME");
    if (!home || !*home) {
        home = "/tmp";
    }
    snprintf(out, outsz, "%s/.config/%s", home, filename);
}

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

/* Sends SIGHUP to every process named `procname` (exact match, not -f --
 * safe against accidentally hitting unrelated processes). Fire-and-forget,
 * same spirit as run_action() elsewhere in this repo's daemons. */
static void signal_daemon(const char *procname)
{
    pid_t pid = fork();
    if (pid < 0) {
        return;
    }
    if (pid == 0) {
        execlp("pkill", "pkill", "-HUP", "-x", procname, (char *)NULL);
        _exit(127);
    }
    int status;
    waitpid(pid, &status, 0);
}

/* ---- generic subprocess helpers (xrandr/xinput/xset/xprop/wmctrl) ---- */

/* Runs argv (NULL-terminated), waits for it, and returns its exit code
 * (-1 if it couldn't even be started). Used for "apply" commands where
 * kiconf doesn't need the output, just whether it worked. */
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

/* Runs argv and captures its stdout into `out` (truncated to outsz-1).
 * Returns 1 on success (process ran and exited 0), 0 otherwise -- `out`
 * is always NUL-terminated either way (possibly empty). */
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

/* ---- generic JSON-line-over-Unix-socket client (xisguard-ctl/xispanel-ctl) */

static int json_line_send(const char *sockpath, const char *req, char *resp, size_t respsz)
{
    resp[0] = '\0';
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return 0;
    }
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sockpath);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return 0;
    }
    char line[1024];
    snprintf(line, sizeof(line), "%s\n", req);
    if (write(fd, line, strlen(line)) < 0) {
        close(fd);
        return 0;
    }
    shutdown(fd, SHUT_WR);
    size_t total = 0;
    ssize_t n;
    while (total + 1 < respsz && (n = read(fd, resp + total, respsz - 1 - total)) > 0) {
        total += (size_t)n;
    }
    resp[total] = '\0';
    close(fd);
    return total > 0;
}

/* Ad hoc scanning for the small flat JSON objects these two control
 * sockets speak -- no library linked for it, see the file doc comment. */
static int json_get_bool(const char *json, const char *key, int deflt)
{
    char pat[80];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) {
        return deflt;
    }
    p += strlen(pat);
    while (*p == ' ' || *p == ':') {
        p++;
    }
    return strncmp(p, "true", 4) == 0;
}

static int json_get_int(const char *json, const char *key, int deflt)
{
    char pat[80];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) {
        return deflt;
    }
    p += strlen(pat);
    while (*p == ' ' || *p == ':') {
        p++;
    }
    return atoi(p);
}

static void json_get_str(const char *json, const char *key, char *out, size_t outsz)
{
    char pat[80];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    out[0] = '\0';
    const char *p = strstr(json, pat);
    if (!p) {
        return;
    }
    p += strlen(pat);
    while (*p == ' ' || *p == ':') {
        p++;
    }
    if (*p != '"') {
        return;
    }
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < outsz) {
        out[i++] = *p++;
    }
    out[i] = '\0';
}

static int json_ok(const char *json)
{
    return json_get_bool(json, "ok", 0);
}

/* Escapes '"' and '\\' for embedding `s` as a JSON string value -- the
 * only characters that appear in practice here (exe patterns, action
 * names) that would otherwise break the wire format. */
static void json_escape(const char *s, char *out, size_t outsz)
{
    size_t i = 0;
    for (; *s && i + 2 < outsz; s++) {
        if (*s == '"' || *s == '\\') {
            out[i++] = '\\';
        }
        out[i++] = *s;
    }
    out[i] = '\0';
}

/* ---- Aparencia tab: kiconfd.conf (theme/color/font settings) --------- */

typedef struct {
    char cursor_theme[NAME_LEN];
    int cursor_size;
    char color_bg[16], color_fg[16], color_base[16];
    char color_accent[16], color_selection_bg[16], color_selection_fg[16];
    char font_general[NAME_LEN], font_monospace[NAME_LEN];
    char gtk2_theme[NAME_LEN], gtk3_theme[NAME_LEN], gtk4_theme[NAME_LEN];
    char icon_theme[NAME_LEN], qt_style[NAME_LEN];
} Appearance;

/* Defaults mirror kiconfd's own apply_and_persist_defaults() -- if
 * kiconfd.conf doesn't exist yet (kiconfd never ran), the UI should still
 * show something sane instead of blank widgets. */
static void appearance_defaults(Appearance *a)
{
    snprintf(a->cursor_theme, sizeof(a->cursor_theme), "default");
    a->cursor_size = 24;
    snprintf(a->color_bg, sizeof(a->color_bg), "#e0e0e0");
    snprintf(a->color_fg, sizeof(a->color_fg), "#202020");
    snprintf(a->color_base, sizeof(a->color_base), "#ffffff");
    snprintf(a->color_accent, sizeof(a->color_accent), "#3584e4");
    snprintf(a->color_selection_bg, sizeof(a->color_selection_bg), "#3584e4");
    snprintf(a->color_selection_fg, sizeof(a->color_selection_fg), "#ffffff");
    snprintf(a->font_general, sizeof(a->font_general), "Sans 10");
    snprintf(a->font_monospace, sizeof(a->font_monospace), "Monospace 10");
    snprintf(a->gtk2_theme, sizeof(a->gtk2_theme), "Default");
    snprintf(a->gtk3_theme, sizeof(a->gtk3_theme), "Adwaita");
    snprintf(a->gtk4_theme, sizeof(a->gtk4_theme), "Adwaita");
    snprintf(a->icon_theme, sizeof(a->icon_theme), "Adwaita");
    snprintf(a->qt_style, sizeof(a->qt_style), "Fusion");
}

static void load_appearance(Appearance *a)
{
    appearance_defaults(a);

    char path[PATH_MAX];
    resolve_path("kiconfd.conf", path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) {
        return;
    }
    char line[512];
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
#define SET(k, field) if (!strcmp(key, k)) snprintf(a->field, sizeof(a->field), "%s", val)
        SET("cursor_theme", cursor_theme);
        else if (!strcmp(key, "cursor_size")) a->cursor_size = atoi(val);
        else SET("color_bg", color_bg);
        else SET("color_fg", color_fg);
        else SET("color_base", color_base);
        else SET("color_accent", color_accent);
        else SET("color_selection_bg", color_selection_bg);
        else SET("color_selection_fg", color_selection_fg);
        else SET("font_general", font_general);
        else SET("font_monospace", font_monospace);
        else SET("gtk2_theme", gtk2_theme);
        else SET("gtk3_theme", gtk3_theme);
        else SET("gtk4_theme", gtk4_theme);
        else SET("icon_theme", icon_theme);
        else SET("qt_style", qt_style);
#undef SET
    }
    fclose(f);
}

static GtkWidget *make_color_button(const char *hex)
{
    GdkColor c;
    if (!gdk_color_parse(hex, &c)) {
        gdk_color_parse("#000000", &c);
    }
    return gtk_color_button_new_with_color(&c);
}

static void color_button_hex(GtkWidget *btn, char *out, size_t outsz)
{
    GdkColor c;
    gtk_color_button_get_color(GTK_COLOR_BUTTON(btn), &c);
    snprintf(out, outsz, "#%02x%02x%02x", c.red >> 8, c.green >> 8, c.blue >> 8);
}

static void save_appearance_cb(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;

    Appearance a;
    gchar *cursor = combo_active_text_or(g_cursor_combo, "default");
    snprintf(a.cursor_theme, sizeof(a.cursor_theme), "%s", cursor);
    g_free(cursor);
    a.cursor_size = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_cursor_size_spin));

    color_button_hex(g_color_bg_btn, a.color_bg, sizeof(a.color_bg));
    color_button_hex(g_color_fg_btn, a.color_fg, sizeof(a.color_fg));
    color_button_hex(g_color_base_btn, a.color_base, sizeof(a.color_base));
    color_button_hex(g_color_accent_btn, a.color_accent, sizeof(a.color_accent));
    color_button_hex(g_color_selbg_btn, a.color_selection_bg, sizeof(a.color_selection_bg));
    color_button_hex(g_color_selfg_btn, a.color_selection_fg, sizeof(a.color_selection_fg));

    const gchar *fg = gtk_font_button_get_font_name(GTK_FONT_BUTTON(g_font_general_btn));
    snprintf(a.font_general, sizeof(a.font_general), "%s", fg ? fg : "Sans 10");
    const gchar *fm = gtk_font_button_get_font_name(GTK_FONT_BUTTON(g_font_mono_btn));
    snprintf(a.font_monospace, sizeof(a.font_monospace), "%s", fm ? fm : "Monospace 10");

    gchar *gtk2 = combo_active_text_or(g_gtk2_combo, "Default");
    snprintf(a.gtk2_theme, sizeof(a.gtk2_theme), "%s", gtk2);
    g_free(gtk2);
    gchar *gtk3 = combo_active_text_or(g_gtk3_combo, "Adwaita");
    snprintf(a.gtk3_theme, sizeof(a.gtk3_theme), "%s", gtk3);
    g_free(gtk3);
    gchar *gtk4 = combo_active_text_or(g_gtk4_combo, "Adwaita");
    snprintf(a.gtk4_theme, sizeof(a.gtk4_theme), "%s", gtk4);
    g_free(gtk4);
    gchar *icon = combo_active_text_or(g_icon_combo, "Adwaita");
    snprintf(a.icon_theme, sizeof(a.icon_theme), "%s", icon);
    g_free(icon);
    gchar *qtstyle = combo_active_text_or(g_qt_style_combo, "Fusion");
    snprintf(a.qt_style, sizeof(a.qt_style), "%s", qtstyle);
    g_free(qtstyle);

    char path[PATH_MAX];
    resolve_path("kiconfd.conf", path, sizeof(path));
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) {
        g_warning("kiconf: could not write '%s': %s", tmp, strerror(errno));
        return;
    }
    fprintf(f, "# kiconfd config\n");
    fprintf(f, "cursor_theme = %s\n", a.cursor_theme);
    fprintf(f, "cursor_size = %d\n", a.cursor_size);
    fprintf(f, "color_bg = %s\n", a.color_bg);
    fprintf(f, "color_fg = %s\n", a.color_fg);
    fprintf(f, "color_base = %s\n", a.color_base);
    fprintf(f, "color_accent = %s\n", a.color_accent);
    fprintf(f, "color_selection_bg = %s\n", a.color_selection_bg);
    fprintf(f, "color_selection_fg = %s\n", a.color_selection_fg);
    fprintf(f, "font_general = %s\n", a.font_general);
    fprintf(f, "font_monospace = %s\n", a.font_monospace);
    fprintf(f, "gtk2_theme = %s\n", a.gtk2_theme);
    fprintf(f, "gtk3_theme = %s\n", a.gtk3_theme);
    fprintf(f, "gtk4_theme = %s\n", a.gtk4_theme);
    fprintf(f, "icon_theme = %s\n", a.icon_theme);
    fprintf(f, "qt_style = %s\n", a.qt_style);
    fclose(f);
    rename(tmp, path);

    signal_daemon("kiconfd");
}

/* ---- Aparencia tab: "Importar da sessao atual" ------------------------
 * kiconfd.conf (read by load_appearance() above) only reflects what
 * kiconfd itself last applied -- if kiconfd never ran yet in this session,
 * or the user set a cursor/theme by hand (lxappearance, qt5ct directly,
 * a distro default), the tab shows stale/default values instead of
 * what's actually active. This scans the same live places kiconfd itself
 * writes to when it *does* run (so "Importar" then "Aplicar" is a no-op),
 * plus the X resource database as a cursor fallback, and only fills the
 * widgets -- nothing touches disk until "Aplicar" is clicked. The color
 * palette has no independent live source (it's a kiconfd-only construct,
 * no toolkit exposes "the current accent color" generically), so it's
 * left untouched by this. Monospace font is left alone too: unlike the
 * general font (GTK3's settings.ini gtk-font-name), there's no single
 * cross-toolkit place a "monospace font" is recorded system-wide. */

static int read_ini_value(const char *path, const char *section, const char *key, char *out, size_t outsz)
{
    out[0] = '\0';
    GKeyFile *kf = g_key_file_new();
    if (!g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
        g_key_file_free(kf);
        return 0;
    }
    gchar *v = g_key_file_get_string(kf, section, key, NULL);
    int ok = 0;
    if (v && *v) {
        snprintf(out, outsz, "%s", v);
        ok = 1;
    }
    g_free(v);
    g_key_file_free(kf);
    return ok;
}

/* ~/.gtkrc-2.0 isn't ini-format (no [section]s), just "key = value" lines
 * that GTK2 itself parses top-to-bottom with later lines winning -- scans
 * the whole file and keeps the last match, so kiconf's own "# BEGIN/END
 * KICONF" block (appended at the end) correctly takes priority when
 * present, same as it does for GTK2 itself. */
static int read_gtkrc2_value(const char *key, char *out, size_t outsz)
{
    out[0] = '\0';
    const char *home = getenv("HOME");
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/.gtkrc-2.0", home ? home : "");
    FILE *f = fopen(path, "r");
    if (!f) {
        return 0;
    }
    char line[512];
    int found = 0;
    size_t klen = strlen(key);
    while (fgets(line, sizeof(line), f)) {
        char *l = trim(line);
        if (strncmp(l, key, klen) != 0) {
            continue;
        }
        char *rest = trim(l + klen);
        if (*rest != '=') {
            continue;
        }
        rest = trim(rest + 1);
        size_t len = strlen(rest);
        if (len >= 2 && rest[0] == '"' && rest[len - 1] == '"') {
            rest[len - 1] = '\0';
            rest++;
        }
        snprintf(out, outsz, "%s", rest);
        found = 1;
    }
    fclose(f);
    return found;
}

/* Grabs the value following "propname:" up to end-of-line out of `xrdb
 * -query`'s output -- same tab-after-colon format Xresources always use. */
static int read_xrdb_prop(const char *xrdb_out, const char *propname, char *out, size_t outsz)
{
    out[0] = '\0';
    const char *p = strstr(xrdb_out, propname);
    if (!p) {
        return 0;
    }
    p += strlen(propname);
    const char *nl = strchr(p, '\n');
    size_t n = nl ? (size_t)(nl - p) : strlen(p);
    char tmp[256];
    if (n >= sizeof(tmp)) {
        n = sizeof(tmp) - 1;
    }
    memcpy(tmp, p, n);
    tmp[n] = '\0';
    char *t = trim(tmp);
    if (!*t) {
        return 0;
    }
    snprintf(out, outsz, "%s", t);
    return 1;
}

/* Selects `name` in a text combobox built by make_theme_combo(), appending
 * it (like make_theme_combo() itself does for the on-disk current value)
 * if the live value isn't one of the scanned/installed choices. */
static void combo_select_or_append(GtkWidget *combo, const char *name)
{
    if (!name || !*name) {
        return;
    }
    GtkTreeModel *model = gtk_combo_box_get_model(GTK_COMBO_BOX(combo));
    GtkTreeIter it;
    int idx = 0, found = -1;
    if (gtk_tree_model_get_iter_first(model, &it)) {
        do {
            gchar *t = NULL;
            gtk_tree_model_get(model, &it, 0, &t, -1);
            if (t && !strcmp(t, name)) {
                found = idx;
            }
            g_free(t);
            idx++;
        } while (found < 0 && gtk_tree_model_iter_next(model, &it));
    }
    if (found < 0) {
        gtk_combo_box_append_text(GTK_COMBO_BOX(combo), name);
        found = idx;
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), found);
}

static void import_appearance_cb(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;

    char val[NAME_LEN];
    char path[PATH_MAX];

    /* Cursor theme/size: the X resource database is what X clients that
     * don't read GTK/Qt settings actually use, so it's the most "session
     * truth" source available; GTK3's settings.ini is the fallback. */
    char xrdb_out[8192];
    char *xrdb_argv[] = {"xrdb", "-query", NULL};
    int have_xrdb = run_capture(xrdb_argv, xrdb_out, sizeof(xrdb_out));
    int got_cursor_theme = 0, got_cursor_size = 0;
    if (have_xrdb && read_xrdb_prop(xrdb_out, "Xcursor.theme:", val, sizeof(val))) {
        combo_select_or_append(g_cursor_combo, val);
        got_cursor_theme = 1;
    }
    if (have_xrdb && read_xrdb_prop(xrdb_out, "Xcursor.size:", val, sizeof(val)) && atoi(val) > 0) {
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_cursor_size_spin), atoi(val));
        got_cursor_size = 1;
    }

    resolve_path("gtk-3.0/settings.ini", path, sizeof(path));
    if (read_ini_value(path, "Settings", "gtk-theme-name", val, sizeof(val))) {
        combo_select_or_append(g_gtk3_combo, val);
    }
    if (read_ini_value(path, "Settings", "gtk-icon-theme-name", val, sizeof(val))) {
        combo_select_or_append(g_icon_combo, val);
    }
    if (read_ini_value(path, "Settings", "gtk-font-name", val, sizeof(val))) {
        gtk_font_button_set_font_name(GTK_FONT_BUTTON(g_font_general_btn), val);
    }
    if (!got_cursor_theme && read_ini_value(path, "Settings", "gtk-cursor-theme-name", val, sizeof(val))) {
        combo_select_or_append(g_cursor_combo, val);
    }
    if (!got_cursor_size && read_ini_value(path, "Settings", "gtk-cursor-theme-size", val, sizeof(val)) && atoi(val) > 0) {
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_cursor_size_spin), atoi(val));
    }

    resolve_path("gtk-4.0/settings.ini", path, sizeof(path));
    if (read_ini_value(path, "Settings", "gtk-theme-name", val, sizeof(val))) {
        combo_select_or_append(g_gtk4_combo, val);
    }

    if (read_gtkrc2_value("gtk-theme-name", val, sizeof(val))) {
        combo_select_or_append(g_gtk2_combo, val);
    }
    if (read_gtkrc2_value("gtk-icon-theme-name", val, sizeof(val))) {
        combo_select_or_append(g_icon_combo, val);
    }

    resolve_path("qt5ct/qt5ct.conf", path, sizeof(path));
    if (!read_ini_value(path, "Appearance", "style", val, sizeof(val))) {
        resolve_path("qt6ct/qt6ct.conf", path, sizeof(path));
        read_ini_value(path, "Appearance", "style", val, sizeof(val));
    }
    if (val[0]) {
        combo_select_or_append(g_qt_style_combo, val);
    }
}

static GtkWidget *labeled_row(GtkWidget *table, int row, const char *label_text, GtkWidget *widget)
{
    GtkWidget *label = gtk_label_new(label_text);
    gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
    gtk_table_attach(GTK_TABLE(table), label, 0, 1, row, row + 1, GTK_FILL, GTK_FILL, 4, 3);
    gtk_table_attach(GTK_TABLE(table), widget, 1, 2, row, row + 1, GTK_EXPAND | GTK_FILL, GTK_FILL, 4, 3);
    return widget;
}

static GtkWidget *frame_with(const char *title, GtkWidget *child)
{
    GtkWidget *frame = gtk_frame_new(title);
    gtk_container_set_border_width(GTK_CONTAINER(child), 8);
    gtk_container_add(GTK_CONTAINER(frame), child);
    return frame;
}

static GtkWidget *build_appearance_tab(void)
{
    Appearance a;
    load_appearance(&a);

    GtkWidget *outer = gtk_vbox_new(FALSE, 8);
    gtk_container_set_border_width(GTK_CONTAINER(outer), 12);

    /* Cursor */
    GtkWidget *cursor_table = gtk_table_new(2, 2, FALSE);
    ThemeList cursors = scan_cursor_themes();
    g_cursor_combo = make_theme_combo(&cursors, a.cursor_theme);
    labeled_row(cursor_table, 0, "Tema de cursor:", g_cursor_combo);
    g_cursor_size_spin = gtk_spin_button_new_with_range(8, 128, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_cursor_size_spin), a.cursor_size > 0 ? a.cursor_size : 24);
    labeled_row(cursor_table, 1, "Tamanho:", g_cursor_size_spin);
    gtk_box_pack_start(GTK_BOX(outer), frame_with("Cursor", cursor_table), FALSE, FALSE, 0);

    /* Toolkit themes */
    GtkWidget *themes_table = gtk_table_new(5, 2, FALSE);
    ThemeList gtk2t = scan_gtk_themes("gtk-2.0");
    g_gtk2_combo = make_theme_combo(&gtk2t, a.gtk2_theme);
    labeled_row(themes_table, 0, "Tema GTK2:", g_gtk2_combo);
    ThemeList gtk3t = scan_gtk_themes("gtk-3.0");
    g_gtk3_combo = make_theme_combo(&gtk3t, a.gtk3_theme);
    labeled_row(themes_table, 1, "Tema GTK3:", g_gtk3_combo);
    ThemeList gtk4t = scan_gtk_themes("gtk-4.0");
    g_gtk4_combo = make_theme_combo(&gtk4t, a.gtk4_theme);
    labeled_row(themes_table, 2, "Tema GTK4:", g_gtk4_combo);
    ThemeList icons = scan_icon_themes();
    g_icon_combo = make_theme_combo(&icons, a.icon_theme);
    labeled_row(themes_table, 3, "Tema de icones:", g_icon_combo);
    g_qt_style_combo = gtk_combo_box_new_text();
    {
        static const char *qt_styles[] = {"Fusion", "Windows", "gtk2", NULL};
        int idx = -1;
        for (int i = 0; qt_styles[i]; i++) {
            gtk_combo_box_append_text(GTK_COMBO_BOX(g_qt_style_combo), qt_styles[i]);
            if (!strcmp(qt_styles[i], a.qt_style)) {
                idx = i;
            }
        }
        if (idx < 0) {
            gtk_combo_box_append_text(GTK_COMBO_BOX(g_qt_style_combo), a.qt_style);
            idx = 3;
        }
        gtk_combo_box_set_active(GTK_COMBO_BOX(g_qt_style_combo), idx);
    }
    labeled_row(themes_table, 4, "Estilo Qt (aprox.):", g_qt_style_combo);
    gtk_box_pack_start(GTK_BOX(outer), frame_with("Temas dos toolkits", themes_table), FALSE, FALSE, 0);

    /* Color palette */
    GtkWidget *colors_table = gtk_table_new(6, 2, FALSE);
    g_color_bg_btn = labeled_row(colors_table, 0, "Fundo:", make_color_button(a.color_bg));
    g_color_fg_btn = labeled_row(colors_table, 1, "Texto:", make_color_button(a.color_fg));
    g_color_base_btn = labeled_row(colors_table, 2, "Fundo de campos:", make_color_button(a.color_base));
    g_color_accent_btn = labeled_row(colors_table, 3, "Destaque (accent):", make_color_button(a.color_accent));
    g_color_selbg_btn = labeled_row(colors_table, 4, "Selecao (fundo):", make_color_button(a.color_selection_bg));
    g_color_selfg_btn = labeled_row(colors_table, 5, "Selecao (texto):", make_color_button(a.color_selection_fg));
    gtk_box_pack_start(GTK_BOX(outer), frame_with("Paleta de cores", colors_table), FALSE, FALSE, 0);

    /* Fonts */
    GtkWidget *fonts_table = gtk_table_new(2, 2, FALSE);
    g_font_general_btn = gtk_font_button_new_with_font(a.font_general);
    labeled_row(fonts_table, 0, "Fonte geral:", g_font_general_btn);
    g_font_mono_btn = gtk_font_button_new_with_font(a.font_monospace);
    labeled_row(fonts_table, 1, "Fonte monoespacada:", g_font_mono_btn);
    gtk_box_pack_start(GTK_BOX(outer), frame_with("Fontes", fonts_table), FALSE, FALSE, 0);

    GtkWidget *import_btn = gtk_button_new_with_label("Importar da sessao atual");
    g_signal_connect(import_btn, "clicked", G_CALLBACK(import_appearance_cb), NULL);
    GtkWidget *apply_btn = gtk_button_new_with_label("Aplicar");
    g_signal_connect(apply_btn, "clicked", G_CALLBACK(save_appearance_cb), NULL);
    GtkWidget *btnbox = gtk_hbox_new(FALSE, 6);
    gtk_box_pack_start(GTK_BOX(btnbox), import_btn, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(btnbox), apply_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), btnbox, FALSE, FALSE, 0);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_add_with_viewport(GTK_SCROLLED_WINDOW(scroll), outer);
    return scroll;
}

/* ---- Atalhos tab: xiskeys.conf (BIND action spec command) ------------ */

static void load_shortcuts(void)
{
    gtk_list_store_clear(g_shortcuts_store);

    char path[PATH_MAX];
    resolve_path("xiskeys.conf", path, sizeof(path));

    FILE *f = fopen(path, "r");
    if (!f) {
        return;
    }
    char line[768];
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        if (!line[0] || line[0] == '#') {
            continue;
        }
        char *save = NULL;
        char *tag = strtok_r(line, "\t", &save);
        char *action = tag ? strtok_r(NULL, "\t", &save) : NULL;
        char *spec = action ? strtok_r(NULL, "\t", &save) : NULL;
        char *cmd = spec ? strtok_r(NULL, "", &save) : NULL;
        if (!tag || strcmp(tag, "BIND") != 0 || !action || !spec || !cmd) {
            continue;
        }
        GtkTreeIter it;
        gtk_list_store_append(g_shortcuts_store, &it);
        gtk_list_store_set(g_shortcuts_store, &it,
                            COL_ACTION, action, COL_SPEC, spec, COL_COMMAND, cmd, -1);
    }
    fclose(f);
}

static void save_shortcuts_cb(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;

    char path[PATH_MAX];
    resolve_path("xiskeys.conf", path, sizeof(path));

    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) {
        g_warning("kiconf: could not write '%s': %s", tmp, strerror(errno));
        return;
    }
    fprintf(f, "# xiskeys config -- one binding per line:\n");
    fprintf(f, "#   BIND\\t<action-name>\\t<Mod>+<Mod>+<Key>\\t<shell command>\n");

    GtkTreeIter it;
    gboolean valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(g_shortcuts_store), &it);
    while (valid) {
        gchar *action, *spec, *cmd;
        gtk_tree_model_get(GTK_TREE_MODEL(g_shortcuts_store), &it,
                            COL_ACTION, &action, COL_SPEC, &spec, COL_COMMAND, &cmd, -1);
        if (action && spec && cmd && *action && *spec && *cmd) {
            fprintf(f, "BIND\t%s\t%s\t%s\n", action, spec, cmd);
        }
        g_free(action);
        g_free(spec);
        g_free(cmd);
        valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(g_shortcuts_store), &it);
    }
    fclose(f);
    rename(tmp, path);

    signal_daemon("xiskeys");
}

static void add_shortcut_cb(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    GtkTreeIter it;
    gtk_list_store_append(g_shortcuts_store, &it);
    gtk_list_store_set(g_shortcuts_store, &it,
                        COL_ACTION, "novo-atalho", COL_SPEC, "Ctrl+F1", COL_COMMAND, "true", -1);
}

static void remove_shortcut_cb(GtkWidget *widget, gpointer data)
{
    GtkTreeView *view = GTK_TREE_VIEW(data);
    (void)widget;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(view);
    GtkTreeIter it;
    if (gtk_tree_selection_get_selected(sel, NULL, &it)) {
        gtk_list_store_remove(g_shortcuts_store, &it);
    }
}

static void shortcut_cell_edited(GtkCellRendererText *cell, gchar *path_str, gchar *new_text, gpointer data)
{
    (void)cell;
    gint col = GPOINTER_TO_INT(data);
    GtkTreePath *path = gtk_tree_path_new_from_string(path_str);
    GtkTreeIter it;
    if (gtk_tree_model_get_iter(GTK_TREE_MODEL(g_shortcuts_store), &it, path)) {
        gtk_list_store_set(g_shortcuts_store, &it, col, new_text, -1);
    }
    gtk_tree_path_free(path);
}

static GtkWidget *build_shortcuts_tab(void)
{
    GtkWidget *vbox = gtk_vbox_new(FALSE, 6);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 12);

    g_shortcuts_store = gtk_list_store_new(N_SHORTCUT_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    GtkWidget *view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(g_shortcuts_store));

    const char *titles[N_SHORTCUT_COLS] = {"Acao", "Atalho", "Comando"};
    for (int col = 0; col < N_SHORTCUT_COLS; col++) {
        GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
        g_object_set(renderer, "editable", TRUE, NULL);
        g_signal_connect(renderer, "edited", G_CALLBACK(shortcut_cell_edited), GINT_TO_POINTER(col));
        GtkTreeViewColumn *tvcol = gtk_tree_view_column_new_with_attributes(titles[col], renderer, "text", col, NULL);
        gtk_tree_view_column_set_expand(tvcol, TRUE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(view), tvcol);
    }

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), view);
    gtk_box_pack_start(GTK_BOX(vbox), scroll, TRUE, TRUE, 0);

    GtkWidget *btnbox = gtk_hbox_new(FALSE, 6);
    GtkWidget *add_btn = gtk_button_new_with_label("Adicionar");
    GtkWidget *remove_btn = gtk_button_new_with_label("Remover");
    GtkWidget *save_btn = gtk_button_new_with_label("Salvar e recarregar xiskeys");
    g_signal_connect(add_btn, "clicked", G_CALLBACK(add_shortcut_cb), NULL);
    g_signal_connect(remove_btn, "clicked", G_CALLBACK(remove_shortcut_cb), view);
    g_signal_connect(save_btn, "clicked", G_CALLBACK(save_shortcuts_cb), NULL);
    gtk_box_pack_start(GTK_BOX(btnbox), add_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(btnbox), remove_btn, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(btnbox), save_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), btnbox, FALSE, FALSE, 0);

    load_shortcuts();
    return vbox;
}

/* ---- Entrada tab: pointer/touchpad + key repeat/bell + XiS kbd flags -- */

/* Scans `xinput list-props` output for a line containing `propname` and
 * returns whatever follows the last ':' on that line, trimmed -- matches
 * xinput's "<Prop Name> (id):\t<value>" format without needing to know
 * the numeric prop id. */
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

/* Strips xinput's tree-drawing glyphs (multi-byte UTF-8 box/arrow chars
 * before the name, e.g. "⎣ "/"↳ ") and the trailing "id=N	[...]" tail
 * (already cut off by the caller), leaving just the device name. Must NOT
 * stop at the first non-ASCII byte -- that's exactly the glyph bytes that
 * need skipping, not a reason to keep them. */
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

static int list_pointer_devices(char names[][NAME_LEN], int max)
{
    char *argv[] = {"xinput", "list", "--short", NULL};
    char out[8192];
    if (!run_capture(argv, out, sizeof(out))) {
        return 0;
    }
    int n = 0;
    char *save = NULL;
    char *line = strtok_r(out, "\n", &save);
    while (line && n < max) {
        if (strstr(line, "slave") && strstr(line, "pointer") &&
            !strstr(line, "XTEST") && !strstr(line, "Virtual core")) {
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
                    snprintf(names[n], NAME_LEN, "%s", name);
                    n++;
                }
            }
        }
        line = strtok_r(NULL, "\n", &save);
    }
    return n;
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

static void detect_pointer_props(const char *device, PointerProps *pp)
{
    memset(pp, 0, sizeof(*pp));
    char *argv[] = {"xinput", "list-props", (char *)device, NULL};
    char out[8192];
    if (!run_capture(argv, out, sizeof(out))) {
        return;
    }
    char val[64];
    if (xinput_get_prop_line(out, "libinput Accel Speed", val, sizeof(val))) {
        pp->accel_speed = atof(val);
        pp->has_accel = 1;
    }
    if (xinput_get_prop_line(out, "libinput Natural Scrolling Enabled", val, sizeof(val))) {
        pp->natural_scroll = atoi(val) != 0;
        pp->has_natural = 1;
    }
    if (xinput_get_prop_line(out, "libinput Left Handed Enabled", val, sizeof(val))) {
        pp->left_handed = atoi(val) != 0;
        pp->has_lefth = 1;
    }
    if (xinput_get_prop_line(out, "libinput Tapping Enabled", val, sizeof(val))) {
        pp->tapping = atoi(val) != 0;
        pp->has_tap = 1;
    }
}

static void detect_special_kbd(const char *kbd, int *toggle_mods, int *kick_hotkeys)
{
    *toggle_mods = 0;
    *kick_hotkeys = 0;
    char *argv[] = {"xinput", "list-props", (char *)kbd, NULL};
    char out[8192];
    if (!run_capture(argv, out, sizeof(out))) {
        return;
    }
    char val[64];
    if (xinput_get_prop_line(out, "Toggle Lock Modifiers On Press", val, sizeof(val))) {
        *toggle_mods = atoi(val) != 0;
    }
    if (xinput_get_prop_line(out, "Kick Hotkeys On Release", val, sizeof(val))) {
        *kick_hotkeys = atoi(val) != 0;
    }
}

static void detect_kbd_xset(KbdState *k)
{
    k->repeat_enabled = 1;
    k->repeat_delay = 660;
    k->repeat_rate = 25;
    k->bell_percent = 50;
    k->bell_pitch = 400;
    k->bell_duration = 100;
    char *argv[] = {"xset", "q", NULL};
    char out[8192];
    if (!run_capture(argv, out, sizeof(out))) {
        return;
    }
    char *p;
    if ((p = strstr(out, "auto repeat:"))) {
        p += strlen("auto repeat:");
        while (*p == ' ') {
            p++;
        }
        k->repeat_enabled = strncmp(p, "on", 2) == 0;
    }
    if ((p = strstr(out, "auto repeat delay:"))) {
        int d = 0, r = 0;
        if (sscanf(p, "auto repeat delay:%d repeat rate:%d", &d, &r) == 2) {
            k->repeat_delay = d;
            k->repeat_rate = r;
        }
    }
    if ((p = strstr(out, "bell percent:"))) {
        int pc = 0, pi = 0, du = 0;
        if (sscanf(p, "bell percent:%d bell pitch:%d bell duration:%d", &pc, &pi, &du) == 3) {
            k->bell_percent = pc;
            k->bell_pitch = pi;
            k->bell_duration = du;
        }
    }
}

static void apply_kbd_diff(const KbdState *cur, const KbdState *base)
{
    if (cur->repeat_enabled != base->repeat_enabled) {
        char *argv[] = {"xset", "r", cur->repeat_enabled ? "on" : "off", NULL};
        run_fire(argv);
    }
    if (cur->repeat_delay != base->repeat_delay || cur->repeat_rate != base->repeat_rate) {
        char delaybuf[16], ratebuf[16];
        snprintf(delaybuf, sizeof(delaybuf), "%d", cur->repeat_delay);
        snprintf(ratebuf, sizeof(ratebuf), "%d", cur->repeat_rate);
        char *argv[] = {"xset", "r", "rate", delaybuf, ratebuf, NULL};
        run_fire(argv);
    }
    if (cur->bell_percent != base->bell_percent || cur->bell_pitch != base->bell_pitch ||
        cur->bell_duration != base->bell_duration) {
        char pbuf[16], pitbuf[16], dbuf[16];
        snprintf(pbuf, sizeof(pbuf), "%d", cur->bell_percent);
        snprintf(pitbuf, sizeof(pitbuf), "%d", cur->bell_pitch);
        snprintf(dbuf, sizeof(dbuf), "%d", cur->bell_duration);
        char *argv[] = {"xset", "b", pbuf, pitbuf, dbuf, NULL};
        run_fire(argv);
    }
}

static void on_pointer_device_changed(GtkComboBox *combo, gpointer data)
{
    (void)data;
    gchar *dev = gtk_combo_box_get_active_text(combo);
    if (!dev) {
        return;
    }
    snprintf(g_pointer_baseline_device, sizeof(g_pointer_baseline_device), "%s", dev);
    detect_pointer_props(dev, &g_pointer_baseline);
    g_free(dev);

    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_pointer_accel_spin), g_pointer_baseline.has_accel ? g_pointer_baseline.accel_speed : 0.0);
    gtk_widget_set_sensitive(g_pointer_accel_spin, g_pointer_baseline.has_accel);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_pointer_natural_chk), g_pointer_baseline.natural_scroll);
    gtk_widget_set_sensitive(g_pointer_natural_chk, g_pointer_baseline.has_natural);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_pointer_lefth_chk), g_pointer_baseline.left_handed);
    gtk_widget_set_sensitive(g_pointer_lefth_chk, g_pointer_baseline.has_lefth);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_pointer_tap_chk), g_pointer_baseline.tapping);
    gtk_widget_set_sensitive(g_pointer_tap_chk, g_pointer_baseline.has_tap);
}

static void apply_entrada_cb(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;

    if (g_pointer_baseline_device[0]) {
        double accel = gtk_spin_button_get_value(GTK_SPIN_BUTTON(g_pointer_accel_spin));
        int natural = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_pointer_natural_chk));
        int lefth = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_pointer_lefth_chk));
        int tap = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_pointer_tap_chk));

        if (g_pointer_baseline.has_accel && accel != g_pointer_baseline.accel_speed) {
            char val[32];
            snprintf(val, sizeof(val), "%.6f", accel);
            char *argv[] = {"xinput", "set-prop", g_pointer_baseline_device, "libinput Accel Speed", val, NULL};
            run_fire(argv);
        }
        if (g_pointer_baseline.has_natural && natural != g_pointer_baseline.natural_scroll) {
            char *argv[] = {"xinput", "set-prop", g_pointer_baseline_device,
                             "libinput Natural Scrolling Enabled", natural ? "1" : "0", NULL};
            run_fire(argv);
        }
        if (g_pointer_baseline.has_lefth && lefth != g_pointer_baseline.left_handed) {
            char *argv[] = {"xinput", "set-prop", g_pointer_baseline_device,
                             "libinput Left Handed Enabled", lefth ? "1" : "0", NULL};
            run_fire(argv);
        }
        if (g_pointer_baseline.has_tap && tap != g_pointer_baseline.tapping) {
            char *argv[] = {"xinput", "set-prop", g_pointer_baseline_device,
                             "libinput Tapping Enabled", tap ? "1" : "0", NULL};
            run_fire(argv);
        }
        detect_pointer_props(g_pointer_baseline_device, &g_pointer_baseline);
    }

    KbdState kcur;
    kcur.repeat_enabled = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_kbd_repeat_chk));
    kcur.repeat_delay = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_kbd_delay_spin));
    kcur.repeat_rate = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_kbd_rate_spin));
    kcur.bell_percent = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_bell_percent_spin));
    kcur.bell_pitch = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_bell_pitch_spin));
    kcur.bell_duration = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_bell_dur_spin));
    apply_kbd_diff(&kcur, &g_kbd_baseline);
    g_kbd_baseline = kcur;

    int toggle_mods = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_toggle_mods_chk));
    int kick_hotkeys = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_kick_hotkeys_chk));
    if (g_master_kbd[0]) {
        if (toggle_mods != g_toggle_mods_baseline) {
            char *argv[] = {"xinput", "set-prop", g_master_kbd, "Toggle Lock Modifiers On Press",
                             toggle_mods ? "1" : "0", NULL};
            run_fire(argv);
        }
        if (kick_hotkeys != g_kick_hotkeys_baseline) {
            char *argv[] = {"xinput", "set-prop", g_master_kbd, "Kick Hotkeys On Release",
                             kick_hotkeys ? "1" : "0", NULL};
            run_fire(argv);
        }
    }
    g_toggle_mods_baseline = toggle_mods;
    g_kick_hotkeys_baseline = kick_hotkeys;
}

static GtkWidget *build_entrada_tab(void)
{
    snprintf(g_pointer_baseline_device, sizeof(g_pointer_baseline_device), "%s", "");
    master_keyboard_name(g_master_kbd, sizeof(g_master_kbd));
    detect_special_kbd(g_master_kbd, &g_toggle_mods_baseline, &g_kick_hotkeys_baseline);
    detect_kbd_xset(&g_kbd_baseline);

    GtkWidget *outer = gtk_vbox_new(FALSE, 8);
    gtk_container_set_border_width(GTK_CONTAINER(outer), 12);

    GtkWidget *ptr_table = gtk_table_new(5, 2, FALSE);
    g_pointer_combo = gtk_combo_box_new_text();
    char devnames[32][NAME_LEN];
    int ndev = list_pointer_devices(devnames, 32);
    for (int i = 0; i < ndev; i++) {
        gtk_combo_box_append_text(GTK_COMBO_BOX(g_pointer_combo), devnames[i]);
    }
    labeled_row(ptr_table, 0, "Dispositivo:", g_pointer_combo);
    g_pointer_accel_spin = gtk_spin_button_new_with_range(-1.0, 1.0, 0.1);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(g_pointer_accel_spin), 2);
    labeled_row(ptr_table, 1, "Velocidade (Accel Speed):", g_pointer_accel_spin);
    g_pointer_natural_chk = gtk_check_button_new_with_label("Rolagem natural");
    gtk_table_attach(GTK_TABLE(ptr_table), g_pointer_natural_chk, 0, 2, 2, 3, GTK_FILL, GTK_FILL, 4, 2);
    g_pointer_lefth_chk = gtk_check_button_new_with_label("Canhoto (inverter botoes)");
    gtk_table_attach(GTK_TABLE(ptr_table), g_pointer_lefth_chk, 0, 2, 3, 4, GTK_FILL, GTK_FILL, 4, 2);
    g_pointer_tap_chk = gtk_check_button_new_with_label("Tocar para clicar (touchpad)");
    gtk_table_attach(GTK_TABLE(ptr_table), g_pointer_tap_chk, 0, 2, 4, 5, GTK_FILL, GTK_FILL, 4, 2);
    gtk_box_pack_start(GTK_BOX(outer), frame_with("Ponteiro/touchpad", ptr_table), FALSE, FALSE, 0);
    g_signal_connect(g_pointer_combo, "changed", G_CALLBACK(on_pointer_device_changed), NULL);
    if (ndev > 0) {
        gtk_combo_box_set_active(GTK_COMBO_BOX(g_pointer_combo), 0);
    } else {
        gtk_widget_set_sensitive(g_pointer_accel_spin, FALSE);
        gtk_widget_set_sensitive(g_pointer_natural_chk, FALSE);
        gtk_widget_set_sensitive(g_pointer_lefth_chk, FALSE);
        gtk_widget_set_sensitive(g_pointer_tap_chk, FALSE);
    }

    GtkWidget *kbd_table = gtk_table_new(3, 2, FALSE);
    g_kbd_repeat_chk = gtk_check_button_new_with_label("Repeticao automatica");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_kbd_repeat_chk), g_kbd_baseline.repeat_enabled);
    gtk_table_attach(GTK_TABLE(kbd_table), g_kbd_repeat_chk, 0, 2, 0, 1, GTK_FILL, GTK_FILL, 4, 2);
    g_kbd_delay_spin = gtk_spin_button_new_with_range(100, 3000, 10);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_kbd_delay_spin), g_kbd_baseline.repeat_delay);
    labeled_row(kbd_table, 1, "Atraso inicial (ms):", g_kbd_delay_spin);
    g_kbd_rate_spin = gtk_spin_button_new_with_range(1, 100, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_kbd_rate_spin), g_kbd_baseline.repeat_rate);
    labeled_row(kbd_table, 2, "Taxa (rep/s):", g_kbd_rate_spin);
    gtk_box_pack_start(GTK_BOX(outer), frame_with("Repeticao de tecla", kbd_table), FALSE, FALSE, 0);

    GtkWidget *bell_table = gtk_table_new(3, 2, FALSE);
    g_bell_percent_spin = gtk_spin_button_new_with_range(0, 100, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_bell_percent_spin), g_kbd_baseline.bell_percent);
    labeled_row(bell_table, 0, "Volume (%):", g_bell_percent_spin);
    g_bell_pitch_spin = gtk_spin_button_new_with_range(1, 5000, 10);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_bell_pitch_spin), g_kbd_baseline.bell_pitch);
    labeled_row(bell_table, 1, "Tom (Hz):", g_bell_pitch_spin);
    g_bell_dur_spin = gtk_spin_button_new_with_range(0, 5000, 10);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_bell_dur_spin), g_kbd_baseline.bell_duration);
    labeled_row(bell_table, 2, "Duracao (ms):", g_bell_dur_spin);
    gtk_box_pack_start(GTK_BOX(outer), frame_with("Campainha", bell_table), FALSE, FALSE, 0);

    GtkWidget *special_box = gtk_vbox_new(FALSE, 2);
    g_toggle_mods_chk = gtk_check_button_new_with_label(
        "ToggleModifiersOnPress -- alterna trava de modificador ao pressionar");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_toggle_mods_chk), g_toggle_mods_baseline);
    gtk_box_pack_start(GTK_BOX(special_box), g_toggle_mods_chk, FALSE, FALSE, 0);
    g_kick_hotkeys_chk = gtk_check_button_new_with_label(
        "KickHotkeysOnRelease -- troca layout de teclado ao soltar o atalho");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_kick_hotkeys_chk), g_kick_hotkeys_baseline);
    gtk_box_pack_start(GTK_BOX(special_box), g_kick_hotkeys_chk, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), frame_with("Opcoes especiais de teclado (XiS)", special_box), FALSE, FALSE, 0);

    GtkWidget *apply_btn = gtk_button_new_with_label("Aplicar");
    g_signal_connect(apply_btn, "clicked", G_CALLBACK(apply_entrada_cb), NULL);
    GtkWidget *btnbox = gtk_hbox_new(FALSE, 0);
    gtk_box_pack_end(GTK_BOX(btnbox), apply_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), btnbox, FALSE, FALSE, 0);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_add_with_viewport(GTK_SCROLLED_WINDOW(scroll), outer);
    return scroll;
}

/* ---- Outras tab: 3rd XiS flag + DPMS/screensaver + virtual desktops --- */

static int detect_disable_primsel(void)
{
    char *argv[] = {"xprop", "-root", "_DisablePrimarySelection", NULL};
    char out[256];
    if (!run_capture(argv, out, sizeof(out))) {
        return 0;
    }
    char *eq = strchr(out, '=');
    return eq ? atoi(eq + 1) != 0 : 0;
}

static void set_disable_primsel(int enable)
{
    char *argv[] = {"xprop", "-root", "-f", "_DisablePrimarySelection", "8i",
                     "-set", "_DisablePrimarySelection", enable ? "1" : "0", NULL};
    run_fire(argv);
}

static void detect_power_xset(PowerState *p)
{
    p->dpms_enabled = 1;
    p->dpms_standby = 0;
    p->dpms_suspend = 0;
    p->dpms_off = 0;
    p->saver_timeout = 0;
    p->saver_cycle = 600;
    p->prefer_blanking = 1;
    char *argv[] = {"xset", "q", NULL};
    char out[8192];
    if (!run_capture(argv, out, sizeof(out))) {
        return;
    }
    char *p2;
    if ((p2 = strstr(out, "Standby:"))) {
        sscanf(p2, "Standby:%d Suspend:%d Off:%d", &p->dpms_standby, &p->dpms_suspend, &p->dpms_off);
    }
    p->dpms_enabled = strstr(out, "DPMS is Enabled") != NULL;
    if ((p2 = strstr(out, "timeout:"))) {
        sscanf(p2, "timeout:%d cycle:%d", &p->saver_timeout, &p->saver_cycle);
    }
    if ((p2 = strstr(out, "prefer blanking:"))) {
        p2 += strlen("prefer blanking:");
        while (*p2 == ' ') {
            p2++;
        }
        p->prefer_blanking = strncmp(p2, "yes", 3) == 0;
    }
}

static void apply_power_diff(const PowerState *cur, const PowerState *base)
{
    if (cur->dpms_enabled != base->dpms_enabled) {
        char *argv[] = {"xset", cur->dpms_enabled ? "+dpms" : "-dpms", NULL};
        run_fire(argv);
    }
    if (cur->dpms_standby != base->dpms_standby || cur->dpms_suspend != base->dpms_suspend ||
        cur->dpms_off != base->dpms_off) {
        char a[16], b[16], c[16];
        snprintf(a, sizeof(a), "%d", cur->dpms_standby);
        snprintf(b, sizeof(b), "%d", cur->dpms_suspend);
        snprintf(c, sizeof(c), "%d", cur->dpms_off);
        char *argv[] = {"xset", "dpms", a, b, c, NULL};
        run_fire(argv);
    }
    if (cur->saver_timeout != base->saver_timeout || cur->saver_cycle != base->saver_cycle) {
        char a[16], b[16];
        snprintf(a, sizeof(a), "%d", cur->saver_timeout);
        snprintf(b, sizeof(b), "%d", cur->saver_cycle);
        char *argv[] = {"xset", "s", a, b, NULL};
        run_fire(argv);
    }
    if (cur->prefer_blanking != base->prefer_blanking) {
        char *argv[] = {"xset", "s", cur->prefer_blanking ? "blank" : "noblank", NULL};
        run_fire(argv);
    }
}

static int detect_desktop_count(void)
{
    char *argv[] = {"xprop", "-root", "_NET_NUMBER_OF_DESKTOPS", NULL};
    char out[256];
    if (!run_capture(argv, out, sizeof(out))) {
        return 1;
    }
    char *eq = strchr(out, '=');
    if (!eq) {
        return 1;
    }
    int v = atoi(eq + 1);
    return v > 0 ? v : 1;
}

static void set_desktop_count(int n)
{
    char nbuf[16];
    snprintf(nbuf, sizeof(nbuf), "%d", n);
    char *argv[] = {"wmctrl", "-n", nbuf, NULL};
    run_fire(argv);
}

static void apply_outras_cb(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;

    int primsel = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_disable_primsel_chk));
    if (primsel != g_disable_primsel_baseline) {
        set_disable_primsel(primsel);
        g_disable_primsel_baseline = primsel;
    }

    PowerState pcur;
    pcur.dpms_enabled = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_dpms_enabled_chk));
    pcur.dpms_standby = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_dpms_standby_spin));
    pcur.dpms_suspend = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_dpms_suspend_spin));
    pcur.dpms_off = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_dpms_off_spin));
    pcur.saver_timeout = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_saver_timeout_spin));
    pcur.saver_cycle = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_saver_cycle_spin));
    pcur.prefer_blanking = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_prefer_blank_chk));
    apply_power_diff(&pcur, &g_power_baseline);
    g_power_baseline = pcur;

    int dcount = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_desktop_count_spin));
    if (dcount != g_desktop_count_baseline) {
        set_desktop_count(dcount);
        g_desktop_count_baseline = dcount;
    }
}

static GtkWidget *build_outras_tab(void)
{
    g_disable_primsel_baseline = detect_disable_primsel();
    detect_power_xset(&g_power_baseline);
    g_desktop_count_baseline = detect_desktop_count();

    GtkWidget *outer = gtk_vbox_new(FALSE, 8);
    gtk_container_set_border_width(GTK_CONTAINER(outer), 12);

    GtkWidget *primsel_box = gtk_vbox_new(FALSE, 2);
    g_disable_primsel_chk = gtk_check_button_new_with_label(
        "DisablePrimarySelection -- desativa colar com o botao do meio");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_disable_primsel_chk), g_disable_primsel_baseline);
    gtk_box_pack_start(GTK_BOX(primsel_box), g_disable_primsel_chk, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), frame_with("Selecao primaria (XiS)", primsel_box), FALSE, FALSE, 0);

    GtkWidget *dpms_table = gtk_table_new(4, 2, FALSE);
    g_dpms_enabled_chk = gtk_check_button_new_with_label("DPMS habilitado");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_dpms_enabled_chk), g_power_baseline.dpms_enabled);
    gtk_table_attach(GTK_TABLE(dpms_table), g_dpms_enabled_chk, 0, 2, 0, 1, GTK_FILL, GTK_FILL, 4, 2);
    g_dpms_standby_spin = gtk_spin_button_new_with_range(0, 36000, 10);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_dpms_standby_spin), g_power_baseline.dpms_standby);
    labeled_row(dpms_table, 1, "Standby, s (0=off):", g_dpms_standby_spin);
    g_dpms_suspend_spin = gtk_spin_button_new_with_range(0, 36000, 10);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_dpms_suspend_spin), g_power_baseline.dpms_suspend);
    labeled_row(dpms_table, 2, "Suspend, s (0=off):", g_dpms_suspend_spin);
    g_dpms_off_spin = gtk_spin_button_new_with_range(0, 36000, 10);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_dpms_off_spin), g_power_baseline.dpms_off);
    labeled_row(dpms_table, 3, "Off, s (0=off):", g_dpms_off_spin);
    gtk_box_pack_start(GTK_BOX(outer), frame_with("DPMS", dpms_table), FALSE, FALSE, 0);

    GtkWidget *saver_table = gtk_table_new(3, 2, FALSE);
    g_saver_timeout_spin = gtk_spin_button_new_with_range(0, 36000, 10);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_saver_timeout_spin), g_power_baseline.saver_timeout);
    labeled_row(saver_table, 0, "Timeout, s (0=off):", g_saver_timeout_spin);
    g_saver_cycle_spin = gtk_spin_button_new_with_range(0, 36000, 10);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_saver_cycle_spin), g_power_baseline.saver_cycle);
    labeled_row(saver_table, 1, "Ciclo, s:", g_saver_cycle_spin);
    g_prefer_blank_chk = gtk_check_button_new_with_label("Preferir apagar a tela (blank)");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_prefer_blank_chk), g_power_baseline.prefer_blanking);
    gtk_table_attach(GTK_TABLE(saver_table), g_prefer_blank_chk, 0, 2, 2, 3, GTK_FILL, GTK_FILL, 4, 2);
    gtk_box_pack_start(GTK_BOX(outer), frame_with("Protetor de tela", saver_table), FALSE, FALSE, 0);

    GtkWidget *desk_table = gtk_table_new(1, 2, FALSE);
    g_desktop_count_spin = gtk_spin_button_new_with_range(1, 64, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_desktop_count_spin), g_desktop_count_baseline);
    labeled_row(desk_table, 0, "Numero de areas de trabalho:", g_desktop_count_spin);
    gtk_box_pack_start(GTK_BOX(outer), frame_with("Areas de trabalho virtuais", desk_table), FALSE, FALSE, 0);

    GtkWidget *apply_btn = gtk_button_new_with_label("Aplicar");
    g_signal_connect(apply_btn, "clicked", G_CALLBACK(apply_outras_cb), NULL);
    GtkWidget *btnbox = gtk_hbox_new(FALSE, 0);
    gtk_box_pack_end(GTK_BOX(btnbox), apply_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), btnbox, FALSE, FALSE, 0);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_add_with_viewport(GTK_SCROLLED_WINDOW(scroll), outer);
    return scroll;
}

/* ---- Permissoes tab: xisguard control socket -------------------------- */

static void xisguard_ctl_path(char *out, size_t outsz)
{
    const char *rundir = getenv("XDG_RUNTIME_DIR");
    if (!rundir || !*rundir) {
        rundir = "/tmp";
    }
    const char *disp = getenv("DISPLAY");
    int dispnum = 0;
    if (disp && disp[0] == ':') {
        dispnum = atoi(disp + 1);
    }
    snprintf(out, outsz, "%s/xisguard-ctl.%d.sock", rundir, dispnum);
}

static void xisguard_get_status(XgStatus *st)
{
    memset(st, 0, sizeof(*st));
    char path[PATH_MAX];
    xisguard_ctl_path(path, sizeof(path));
    char resp[JSON_BUF_LEN];
    if (json_line_send(path, "{\"cmd\":\"GET_STATUS\"}", resp, sizeof(resp)) && json_ok(resp)) {
        st->online = 1;
        st->no_pause = json_get_bool(resp, "no_pause", 0);
        st->quiet = json_get_bool(resp, "quiet", 0);
        st->always_kill = json_get_bool(resp, "always_kill", 0);
        st->log_level = json_get_int(resp, "log_level", 0);
    }
}

#define MAX_XG_RULES 256
typedef struct {
    char type[16], action[32], pattern[192];
} XgRule;

static int xisguard_list_rules(XgRule *rules, int max)
{
    char path[PATH_MAX];
    xisguard_ctl_path(path, sizeof(path));
    char resp[JSON_BUF_LEN];
    if (!json_line_send(path, "{\"cmd\":\"LIST_RULES\"}", resp, sizeof(resp)) || !json_ok(resp)) {
        return 0;
    }
    const char *arr = strstr(resp, "\"rules\"");
    if (!arr) {
        return 0;
    }
    arr = strchr(arr, '[');
    if (!arr) {
        return 0;
    }
    int n = 0;
    const char *p = arr + 1;
    while (n < max) {
        const char *obj_start = strchr(p, '{');
        const char *obj_end = obj_start ? strchr(obj_start, '}') : NULL;
        if (!obj_start || !obj_end) {
            break;
        }
        size_t len = (size_t)(obj_end - obj_start) + 1;
        char obj[512];
        if (len >= sizeof(obj)) {
            len = sizeof(obj) - 1;
        }
        memcpy(obj, obj_start, len);
        obj[len] = '\0';
        json_get_str(obj, "type", rules[n].type, sizeof(rules[n].type));
        json_get_str(obj, "action", rules[n].action, sizeof(rules[n].action));
        json_get_str(obj, "pattern", rules[n].pattern, sizeof(rules[n].pattern));
        n++;
        p = obj_end + 1;
        if (*p == ']' || !*p) {
            break;
        }
    }
    return n;
}

static int xisguard_send_rule_cmd(const char *cmd, const char *action, const char *pattern, const char *type)
{
    char eaction[64], epattern[256], etype[32];
    json_escape(action, eaction, sizeof(eaction));
    json_escape(pattern, epattern, sizeof(epattern));
    json_escape(type, etype, sizeof(etype));
    char req[512];
    snprintf(req, sizeof(req), "{\"cmd\":\"%s\",\"action\":\"%s\",\"pattern\":\"%s\",\"type\":\"%s\"}",
              cmd, eaction, epattern, etype);
    char path[PATH_MAX];
    xisguard_ctl_path(path, sizeof(path));
    char resp[JSON_BUF_LEN];
    return json_line_send(path, req, resp, sizeof(resp)) && json_ok(resp);
}

static int xisguard_reload(void)
{
    char path[PATH_MAX];
    xisguard_ctl_path(path, sizeof(path));
    char resp[JSON_BUF_LEN];
    return json_line_send(path, "{\"cmd\":\"RELOAD\"}", resp, sizeof(resp)) && json_ok(resp);
}

/* SET_STATUS accepts each field independently -- only the ones that
 * actually changed since the last GET_STATUS/apply are sent, same "diff
 * against baseline" rule as every other tab. */
static void apply_xg_status_diff(int no_pause, int quiet, int always_kill, int log_level)
{
    if (!g_xg_baseline.online) {
        return;
    }
    char req[256];
    int pos = snprintf(req, sizeof(req), "{\"cmd\":\"SET_STATUS\"");
    int changed = 0;
    if (no_pause != g_xg_baseline.no_pause) {
        pos += snprintf(req + pos, sizeof(req) - (size_t)pos, ",\"no_pause\":%d", no_pause);
        changed = 1;
    }
    if (quiet != g_xg_baseline.quiet) {
        pos += snprintf(req + pos, sizeof(req) - (size_t)pos, ",\"quiet\":%d", quiet);
        changed = 1;
    }
    if (always_kill != g_xg_baseline.always_kill) {
        pos += snprintf(req + pos, sizeof(req) - (size_t)pos, ",\"always_kill\":%d", always_kill);
        changed = 1;
    }
    if (log_level != g_xg_baseline.log_level) {
        pos += snprintf(req + pos, sizeof(req) - (size_t)pos, ",\"log_level\":%d", log_level);
        changed = 1;
    }
    snprintf(req + pos, sizeof(req) - (size_t)pos, "}");
    if (!changed) {
        return;
    }
    char path[PATH_MAX];
    xisguard_ctl_path(path, sizeof(path));
    char resp[JSON_BUF_LEN];
    json_line_send(path, req, resp, sizeof(resp));
    g_xg_baseline.no_pause = no_pause;
    g_xg_baseline.quiet = quiet;
    g_xg_baseline.always_kill = always_kill;
    g_xg_baseline.log_level = log_level;
}

static void refresh_xg_rules(void)
{
    gtk_list_store_clear(g_xg_rules_store);
    XgRule rules[MAX_XG_RULES];
    int n = g_xg_baseline.online ? xisguard_list_rules(rules, MAX_XG_RULES) : 0;
    for (int i = 0; i < n; i++) {
        GtkTreeIter it;
        gtk_list_store_append(g_xg_rules_store, &it);
        gtk_list_store_set(g_xg_rules_store, &it,
                            COL_XG_TYPE, rules[i].type, COL_XG_ACTION, rules[i].action,
                            COL_XG_PATTERN, rules[i].pattern, -1);
    }
}

static void apply_xg_status_cb(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    int no_pause = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_xg_no_pause_chk));
    int quiet = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_xg_quiet_chk));
    int always_kill = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_xg_always_kill_chk));
    int log_level = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_xg_log_level_spin));
    apply_xg_status_diff(no_pause, quiet, always_kill, log_level);
}

static void on_xg_add_rule(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    if (!g_xg_baseline.online) {
        return;
    }
    gchar *action_disp = gtk_combo_box_get_active_text(GTK_COMBO_BOX(g_xg_action_combo));
    gchar *type = gtk_combo_box_get_active_text(GTK_COMBO_BOX(g_xg_type_combo));
    const gchar *pattern = gtk_entry_get_text(GTK_ENTRY(g_xg_pattern_entry));
    if (!action_disp || !type || !pattern[0]) {
        g_free(action_disp);
        g_free(type);
        return;
    }
    /* action_disp is "NAME -- description"; only NAME goes over the wire */
    char action[32];
    char *sep = strstr(action_disp, " -- ");
    size_t len = sep ? (size_t)(sep - action_disp) : strlen(action_disp);
    if (len >= sizeof(action)) {
        len = sizeof(action) - 1;
    }
    memcpy(action, action_disp, len);
    action[len] = '\0';

    if (xisguard_send_rule_cmd("ADD_RULE", action, pattern, type)) {
        refresh_xg_rules();
        gtk_entry_set_text(GTK_ENTRY(g_xg_pattern_entry), "");
    }
    g_free(action_disp);
    g_free(type);
}

static void on_xg_remove_rule(GtkWidget *widget, gpointer data)
{
    (void)widget;
    GtkTreeView *view = GTK_TREE_VIEW(data);
    GtkTreeSelection *sel = gtk_tree_view_get_selection(view);
    GtkTreeIter it;
    if (!gtk_tree_selection_get_selected(sel, NULL, &it)) {
        return;
    }
    gchar *type, *action, *pattern;
    gtk_tree_model_get(GTK_TREE_MODEL(g_xg_rules_store), &it,
                        COL_XG_TYPE, &type, COL_XG_ACTION, &action, COL_XG_PATTERN, &pattern, -1);
    if (xisguard_send_rule_cmd("REMOVE_RULE", action, pattern, type)) {
        refresh_xg_rules();
    }
    g_free(type);
    g_free(action);
    g_free(pattern);
}

static void on_xg_reload(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    xisguard_reload();
    refresh_xg_rules();
}

static GtkWidget *build_permissoes_tab(void)
{
    xisguard_get_status(&g_xg_baseline);

    GtkWidget *outer = gtk_vbox_new(FALSE, 8);
    gtk_container_set_border_width(GTK_CONTAINER(outer), 12);

    char status_text[128];
    if (g_xg_baseline.online) {
        snprintf(status_text, sizeof(status_text), "xisguard conectado.");
    } else {
        snprintf(status_text, sizeof(status_text),
                  "xisguard inacessivel (daemon parado, ou socket de controle ainda nao existe).");
    }
    g_xg_status_label = gtk_label_new(status_text);
    gtk_misc_set_alignment(GTK_MISC(g_xg_status_label), 0.0, 0.5);
    gtk_box_pack_start(GTK_BOX(outer), g_xg_status_label, FALSE, FALSE, 0);

    GtkWidget *status_table = gtk_table_new(4, 2, FALSE);
    g_xg_no_pause_chk = gtk_check_button_new_with_label("no_pause (nao pausar decisao)");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_xg_no_pause_chk), g_xg_baseline.no_pause);
    gtk_table_attach(GTK_TABLE(status_table), g_xg_no_pause_chk, 0, 2, 0, 1, GTK_FILL, GTK_FILL, 4, 2);
    g_xg_quiet_chk = gtk_check_button_new_with_label("quiet (sem notificacoes)");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_xg_quiet_chk), g_xg_baseline.quiet);
    gtk_table_attach(GTK_TABLE(status_table), g_xg_quiet_chk, 0, 2, 1, 2, GTK_FILL, GTK_FILL, 4, 2);
    g_xg_always_kill_chk = gtk_check_button_new_with_label("always_kill (sempre matar em DENY)");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_xg_always_kill_chk), g_xg_baseline.always_kill);
    gtk_table_attach(GTK_TABLE(status_table), g_xg_always_kill_chk, 0, 2, 2, 3, GTK_FILL, GTK_FILL, 4, 2);
    g_xg_log_level_spin = gtk_spin_button_new_with_range(0, 5, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_xg_log_level_spin), g_xg_baseline.log_level);
    labeled_row(status_table, 3, "log_level:", g_xg_log_level_spin);
    gtk_widget_set_sensitive(status_table, g_xg_baseline.online);
    gtk_box_pack_start(GTK_BOX(outer), frame_with("Modo de execucao", status_table), FALSE, FALSE, 0);

    GtkWidget *status_apply_btn = gtk_button_new_with_label("Aplicar modo");
    g_signal_connect(status_apply_btn, "clicked", G_CALLBACK(apply_xg_status_cb), NULL);
    gtk_widget_set_sensitive(status_apply_btn, g_xg_baseline.online);
    GtkWidget *status_btnbox = gtk_hbox_new(FALSE, 0);
    gtk_box_pack_end(GTK_BOX(status_btnbox), status_apply_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), status_btnbox, FALSE, FALSE, 0);

    g_xg_rules_store = gtk_list_store_new(N_XG_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    g_xg_rules_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(g_xg_rules_store));
    const char *xg_titles[N_XG_COLS] = {"Tipo", "Acao", "Padrao"};
    for (int col = 0; col < N_XG_COLS; col++) {
        GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
        GtkTreeViewColumn *tvcol = gtk_tree_view_column_new_with_attributes(xg_titles[col], renderer, "text", col, NULL);
        gtk_tree_view_column_set_expand(tvcol, TRUE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(g_xg_rules_view), tvcol);
    }
    GtkWidget *rules_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(rules_scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(rules_scroll, -1, 160);
    gtk_container_add(GTK_CONTAINER(rules_scroll), g_xg_rules_view);

    GtkWidget *rules_box = gtk_vbox_new(FALSE, 4);
    gtk_box_pack_start(GTK_BOX(rules_box), rules_scroll, TRUE, TRUE, 0);

    GtkWidget *add_row = gtk_hbox_new(FALSE, 4);
    g_xg_action_combo = gtk_combo_box_new_text();
    for (int i = 0; XNOTIFY_ACTIONS[i][0]; i++) {
        char label[96];
        snprintf(label, sizeof(label), "%s -- %s", XNOTIFY_ACTIONS[i][0], XNOTIFY_ACTIONS[i][1]);
        gtk_combo_box_append_text(GTK_COMBO_BOX(g_xg_action_combo), label);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(g_xg_action_combo), 0);
    gtk_box_pack_start(GTK_BOX(add_row), g_xg_action_combo, TRUE, TRUE, 0);
    g_xg_type_combo = gtk_combo_box_new_text();
    gtk_combo_box_append_text(GTK_COMBO_BOX(g_xg_type_combo), "ALLOW");
    gtk_combo_box_append_text(GTK_COMBO_BOX(g_xg_type_combo), "DENY");
    gtk_combo_box_set_active(GTK_COMBO_BOX(g_xg_type_combo), 0);
    gtk_box_pack_start(GTK_BOX(add_row), g_xg_type_combo, FALSE, FALSE, 0);
    g_xg_pattern_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(g_xg_pattern_entry), "*");
    gtk_box_pack_start(GTK_BOX(add_row), g_xg_pattern_entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(rules_box), add_row, FALSE, FALSE, 0);

    GtkWidget *rules_btnbox = gtk_hbox_new(FALSE, 6);
    GtkWidget *add_btn = gtk_button_new_with_label("Adicionar regra");
    GtkWidget *remove_btn = gtk_button_new_with_label("Remover selecionada");
    GtkWidget *reload_btn = gtk_button_new_with_label("Recarregar (RELOAD)");
    g_signal_connect(add_btn, "clicked", G_CALLBACK(on_xg_add_rule), NULL);
    g_signal_connect(remove_btn, "clicked", G_CALLBACK(on_xg_remove_rule), g_xg_rules_view);
    g_signal_connect(reload_btn, "clicked", G_CALLBACK(on_xg_reload), NULL);
    gtk_box_pack_start(GTK_BOX(rules_btnbox), add_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(rules_btnbox), remove_btn, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(rules_btnbox), reload_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(rules_box), rules_btnbox, FALSE, FALSE, 0);

    gtk_widget_set_sensitive(rules_box, g_xg_baseline.online);
    gtk_box_pack_start(GTK_BOX(outer), frame_with("Regras XNOTIFY", rules_box), TRUE, TRUE, 0);

    refresh_xg_rules();
    return outer;
}

/* ---- Paineis tab: xispanel.conf editor + xispanel-ctl RELOAD ---------- */

#define MAX_PANELS 32
#define MAX_WIDGETS 256
typedef struct {
    char name[NAME_LEN], output[NAME_LEN], options[512];
} PanelRec;
typedef struct {
    char panel[NAME_LEN], type[64], options[512];
} WidgetRec;
typedef struct {
    char panel[NAME_LEN], options[512];
} ThemeRec;

static char *skip_ws(char *p)
{
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    return p;
}

/* Splits off the next whitespace-run-separated field from *cursor,
 * NUL-terminating it in place and advancing *cursor past it -- xispanel's
 * config format ("fields separated by any run of spaces and/or tabs",
 * see PROTOCOL.md) needs this instead of strtok since the trailing
 * key=value tail must be kept as one raw chunk, not tokenized further. */
static char *next_field(char **cursor)
{
    char *p = skip_ws(*cursor);
    if (!*p) {
        *cursor = p;
        return NULL;
    }
    char *start = p;
    while (*p && *p != ' ' && *p != '\t') {
        p++;
    }
    if (*p) {
        *p = '\0';
        p++;
    }
    *cursor = p;
    return start;
}

static void load_xispanel_conf(PanelRec *panels, int *n_panels, WidgetRec *widgets, int *n_widgets,
                                 ThemeRec *themes, int *n_themes)
{
    *n_panels = *n_widgets = *n_themes = 0;
    char path[PATH_MAX];
    resolve_path("xispanel.conf", path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) {
        return;
    }
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        char *l = line;
        size_t len = strlen(l);
        while (len > 0 && (l[len - 1] == '\n' || l[len - 1] == '\r')) {
            l[--len] = '\0';
        }
        char *t = skip_ws(l);
        if (!*t || *t == '#') {
            continue;
        }
        char *cursor = t;
        char *tag = next_field(&cursor);
        if (!tag) {
            continue;
        }
        if (!strcmp(tag, "PANEL") && *n_panels < MAX_PANELS) {
            char *name = next_field(&cursor);
            char *output = next_field(&cursor);
            if (!name || !output) {
                continue;
            }
            PanelRec *r = &panels[(*n_panels)++];
            snprintf(r->name, sizeof(r->name), "%s", name);
            snprintf(r->output, sizeof(r->output), "%s", output);
            snprintf(r->options, sizeof(r->options), "%s", skip_ws(cursor));
        } else if (!strcmp(tag, "WIDGET") && *n_widgets < MAX_WIDGETS) {
            char *panel = next_field(&cursor);
            char *order = next_field(&cursor); /* positional only, see PROTOCOL.md */
            char *type = next_field(&cursor);
            if (!panel || !order || !type) {
                continue;
            }
            WidgetRec *r = &widgets[(*n_widgets)++];
            snprintf(r->panel, sizeof(r->panel), "%s", panel);
            snprintf(r->type, sizeof(r->type), "%s", type);
            snprintf(r->options, sizeof(r->options), "%s", skip_ws(cursor));
        } else if (!strcmp(tag, "THEME") && *n_themes < MAX_PANELS) {
            char *panel = next_field(&cursor);
            if (!panel) {
                continue;
            }
            ThemeRec *r = &themes[(*n_themes)++];
            snprintf(r->panel, sizeof(r->panel), "%s", panel);
            snprintf(r->options, sizeof(r->options), "%s", skip_ws(cursor));
        }
    }
    fclose(f);
}

static void save_xispanel_conf(void)
{
    char path[PATH_MAX];
    resolve_path("xispanel.conf", path, sizeof(path));
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) {
        g_warning("kiconf: could not write '%s': %s", tmp, strerror(errno));
        return;
    }

    GtkTreeIter it;
    gboolean valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(g_panels_store), &it);
    while (valid) {
        gchar *name, *output, *opts;
        gtk_tree_model_get(GTK_TREE_MODEL(g_panels_store), &it,
                            COL_PANEL_NAME, &name, COL_PANEL_OUTPUT, &output, COL_PANEL_OPTIONS, &opts, -1);
        if (name && *name) {
            fprintf(f, "PANEL\t%s\t%s\t%s\n", name, output && *output ? output : "*", opts ? opts : "");
        }
        g_free(name);
        g_free(output);
        g_free(opts);
        valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(g_panels_store), &it);
    }

    /* order is positional-only (see PROTOCOL.md), so it's just recomputed
     * here as each panel's 0-based count in file-write order, not edited
     * directly anywhere in the UI. */
    GHashTable *counters = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(g_widgets_store), &it);
    while (valid) {
        gchar *panel, *type, *opts;
        gtk_tree_model_get(GTK_TREE_MODEL(g_widgets_store), &it,
                            COL_WIDGET_PANEL, &panel, COL_WIDGET_TYPE, &type, COL_WIDGET_OPTIONS, &opts, -1);
        if (panel && *panel && type && *type) {
            gpointer countp = g_hash_table_lookup(counters, panel);
            int count = countp ? GPOINTER_TO_INT(countp) : 0;
            fprintf(f, "WIDGET\t%s\t%d\t%s\t%s\n", panel, count, type, opts ? opts : "");
            g_hash_table_insert(counters, g_strdup(panel), GINT_TO_POINTER(count + 1));
        }
        g_free(panel);
        g_free(type);
        g_free(opts);
        valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(g_widgets_store), &it);
    }
    g_hash_table_destroy(counters);

    char *theme_opts = gtk_editable_get_chars(GTK_EDITABLE(g_theme_options_entry), 0, -1);
    if (g_selected_panel[0] && theme_opts && theme_opts[0]) {
        fprintf(f, "THEME\t%s\t%s\n", g_selected_panel, theme_opts);
    }
    g_free(theme_opts);

    fclose(f);
    rename(tmp, path);
}

static void xispanel_ctl_path(char *out, size_t outsz)
{
    const char *rundir = getenv("XDG_RUNTIME_DIR");
    snprintf(out, outsz, "%s/xispanel-ctl.sock", (rundir && *rundir) ? rundir : "/tmp");
}

static void save_panels_cb(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    save_xispanel_conf();
    char path[PATH_MAX];
    xispanel_ctl_path(path, sizeof(path));
    char resp[JSON_BUF_LEN];
    json_line_send(path, "{\"cmd\":\"RELOAD\"}", resp, sizeof(resp));
}

static void add_panel_cb(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    GtkTreeIter it;
    gtk_list_store_append(g_panels_store, &it);
    gtk_list_store_set(g_panels_store, &it,
                        COL_PANEL_NAME, "novo-painel", COL_PANEL_OUTPUT, "*",
                        COL_PANEL_OPTIONS, "edge=top pct=100 thickness=32 mode=dock", -1);
}

static void remove_panel_cb(GtkWidget *widget, gpointer data)
{
    GtkTreeView *view = GTK_TREE_VIEW(data);
    (void)widget;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(view);
    GtkTreeIter it;
    if (gtk_tree_selection_get_selected(sel, NULL, &it)) {
        gtk_list_store_remove(g_panels_store, &it);
    }
}

static void panel_cell_edited(GtkCellRendererText *cell, gchar *path_str, gchar *new_text, gpointer data)
{
    (void)cell;
    gint col = GPOINTER_TO_INT(data);
    GtkTreePath *path = gtk_tree_path_new_from_string(path_str);
    GtkTreeIter it;
    if (gtk_tree_model_get_iter(GTK_TREE_MODEL(g_panels_store), &it, path)) {
        gtk_list_store_set(g_panels_store, &it, col, new_text, -1);
    }
    gtk_tree_path_free(path);
}

static void add_widget_cb(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    GtkTreeIter it;
    gtk_list_store_append(g_widgets_store, &it);
    gtk_list_store_set(g_widgets_store, &it,
                        COL_WIDGET_PANEL, g_selected_panel[0] ? g_selected_panel : "novo-painel",
                        COL_WIDGET_TYPE, "spacer", COL_WIDGET_OPTIONS, "", -1);
}

static void remove_widget_cb(GtkWidget *widget, gpointer data)
{
    GtkTreeView *view = GTK_TREE_VIEW(data);
    (void)widget;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(view);
    GtkTreeIter it;
    if (gtk_tree_selection_get_selected(sel, NULL, &it)) {
        gtk_list_store_remove(g_widgets_store, &it);
    }
}

static void widget_cell_edited(GtkCellRendererText *cell, gchar *path_str, gchar *new_text, gpointer data)
{
    (void)cell;
    gint col = GPOINTER_TO_INT(data);
    GtkTreePath *path = gtk_tree_path_new_from_string(path_str);
    GtkTreeIter it;
    if (gtk_tree_model_get_iter(GTK_TREE_MODEL(g_widgets_store), &it, path)) {
        gtk_list_store_set(g_widgets_store, &it, col, new_text, -1);
    }
    gtk_tree_path_free(path);
}

static GtkWidget *build_editable_list(GtkListStore *store, int ncols, const char *const *titles)
{
    GtkWidget *view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    for (int col = 0; col < ncols; col++) {
        GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
        g_object_set(renderer, "editable", TRUE, NULL);
        const char *sig = "edited";
        GCallback cb = store == g_panels_store ? G_CALLBACK(panel_cell_edited) : G_CALLBACK(widget_cell_edited);
        g_signal_connect(renderer, sig, cb, GINT_TO_POINTER(col));
        GtkTreeViewColumn *tvcol = gtk_tree_view_column_new_with_attributes(titles[col], renderer, "text", col, NULL);
        gtk_tree_view_column_set_expand(tvcol, TRUE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(view), tvcol);
    }
    return view;
}

static GtkWidget *build_paineis_tab(void)
{
    PanelRec panels[MAX_PANELS];
    WidgetRec widgets[MAX_WIDGETS];
    ThemeRec themes[MAX_PANELS];
    int n_panels, n_widgets, n_themes;
    load_xispanel_conf(panels, &n_panels, widgets, &n_widgets, themes, &n_themes);

    GtkWidget *outer = gtk_vbox_new(FALSE, 8);
    gtk_container_set_border_width(GTK_CONTAINER(outer), 12);

    GtkWidget *note = gtk_label_new(
        "Editor direto de xispanel.conf (PANEL/WIDGET/THEME). Opcoes sao\n"
        "a string key=value bruta de cada linha -- ver xispanel/PROTOCOL.md.");
    gtk_misc_set_alignment(GTK_MISC(note), 0.0, 0.5);
    gtk_box_pack_start(GTK_BOX(outer), note, FALSE, FALSE, 0);

    g_panels_store = gtk_list_store_new(N_PANEL_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    for (int i = 0; i < n_panels; i++) {
        GtkTreeIter it;
        gtk_list_store_append(g_panels_store, &it);
        gtk_list_store_set(g_panels_store, &it,
                            COL_PANEL_NAME, panels[i].name, COL_PANEL_OUTPUT, panels[i].output,
                            COL_PANEL_OPTIONS, panels[i].options, -1);
    }
    const char *panel_titles[N_PANEL_COLS] = {"Nome", "Output", "Opcoes"};
    GtkWidget *panels_view = build_editable_list(g_panels_store, N_PANEL_COLS, panel_titles);
    GtkWidget *panels_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(panels_scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(panels_scroll, -1, 100);
    gtk_container_add(GTK_CONTAINER(panels_scroll), panels_view);
    GtkWidget *panels_box = gtk_vbox_new(FALSE, 4);
    gtk_box_pack_start(GTK_BOX(panels_box), panels_scroll, TRUE, TRUE, 0);
    GtkWidget *panels_btnbox = gtk_hbox_new(FALSE, 6);
    GtkWidget *padd = gtk_button_new_with_label("Adicionar painel");
    GtkWidget *prem = gtk_button_new_with_label("Remover painel");
    g_signal_connect(padd, "clicked", G_CALLBACK(add_panel_cb), NULL);
    g_signal_connect(prem, "clicked", G_CALLBACK(remove_panel_cb), panels_view);
    gtk_box_pack_start(GTK_BOX(panels_btnbox), padd, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(panels_btnbox), prem, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(panels_box), panels_btnbox, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), frame_with("Paineis", panels_box), FALSE, FALSE, 0);

    g_widgets_store = gtk_list_store_new(N_WIDGET_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    for (int i = 0; i < n_widgets; i++) {
        GtkTreeIter it;
        gtk_list_store_append(g_widgets_store, &it);
        gtk_list_store_set(g_widgets_store, &it,
                            COL_WIDGET_PANEL, widgets[i].panel, COL_WIDGET_TYPE, widgets[i].type,
                            COL_WIDGET_OPTIONS, widgets[i].options, -1);
    }
    const char *widget_titles[N_WIDGET_COLS] = {"Painel", "Tipo", "Opcoes"};
    GtkWidget *widgets_view = build_editable_list(g_widgets_store, N_WIDGET_COLS, widget_titles);
    GtkWidget *widgets_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(widgets_scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(widgets_scroll, -1, 140);
    gtk_container_add(GTK_CONTAINER(widgets_scroll), widgets_view);
    GtkWidget *widgets_box = gtk_vbox_new(FALSE, 4);
    gtk_box_pack_start(GTK_BOX(widgets_box), widgets_scroll, TRUE, TRUE, 0);
    GtkWidget *widgets_btnbox = gtk_hbox_new(FALSE, 6);
    GtkWidget *wadd = gtk_button_new_with_label("Adicionar widget");
    GtkWidget *wrem = gtk_button_new_with_label("Remover widget");
    g_signal_connect(wadd, "clicked", G_CALLBACK(add_widget_cb), NULL);
    g_signal_connect(wrem, "clicked", G_CALLBACK(remove_widget_cb), widgets_view);
    gtk_box_pack_start(GTK_BOX(widgets_btnbox), wadd, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(widgets_btnbox), wrem, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(widgets_box), widgets_btnbox, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), frame_with("Widgets (coluna Painel = nome do painel dono)", widgets_box), TRUE, TRUE, 0);

    /* Theme editing is intentionally reduced to "one raw options string
     * for the first panel found with a THEME line" rather than a proper
     * per-panel selector -- xispanel already defaults to the live system
     * theme when no THEME line exists at all (see PROTOCOL.md), so most
     * setups will simply leave this blank. */
    if (n_themes > 0) {
        snprintf(g_selected_panel, sizeof(g_selected_panel), "%s", themes[0].panel);
    } else if (n_panels > 0) {
        snprintf(g_selected_panel, sizeof(g_selected_panel), "%s", panels[0].name);
    }
    GtkWidget *theme_table = gtk_table_new(1, 2, FALSE);
    g_theme_options_entry = gtk_entry_new();
    if (n_themes > 0) {
        gtk_entry_set_text(GTK_ENTRY(g_theme_options_entry), themes[0].options);
    }
    char theme_label_text[NAME_LEN + 16];
    snprintf(theme_label_text, sizeof(theme_label_text), "THEME de '%s':", g_selected_panel[0] ? g_selected_panel : "?");
    labeled_row(theme_table, 0, theme_label_text, g_theme_options_entry);
    gtk_box_pack_start(GTK_BOX(outer), frame_with("Tema (cores/fonte do painel)", theme_table), FALSE, FALSE, 0);

    GtkWidget *save_btn = gtk_button_new_with_label("Salvar e recarregar xispanel");
    g_signal_connect(save_btn, "clicked", G_CALLBACK(save_panels_cb), NULL);
    GtkWidget *save_btnbox = gtk_hbox_new(FALSE, 0);
    gtk_box_pack_end(GTK_BOX(save_btnbox), save_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), save_btnbox, FALSE, FALSE, 0);

    return outer;
}

/* ---- Wallpaper tab: xisback socket client ----------------------------- */

static void xisback_socket_path(char *out, size_t outsz)
{
    const char *rundir = getenv("XDG_RUNTIME_DIR");
    snprintf(out, outsz, "%s/xisback.sock", (rundir && *rundir) ? rundir : "/tmp");
}

/* Sends one line, reads until EOF, same wire format as PROTOCOL.md's
 * Python example. If the daemon isn't reachable, starts it (no args ->
 * comes up as an empty daemon, see xisback's own doc comment) and retries
 * once, same as xisconf.py's _xisback_send(). */
static int xisback_send(const char *cmd, char *resp, size_t respsz)
{
    resp[0] = '\0';
    char path[PATH_MAX];
    xisback_socket_path(path, sizeof(path));

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return 0;
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        pid_t pid = fork();
        if (pid == 0) {
            execlp("xisback", "xisback", (char *)NULL);
            _exit(127);
        }
        if (pid > 0) {
            int status;
            waitpid(pid, &status, 0);
        }
        usleep(300000);
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0 || connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
            if (fd >= 0) {
                close(fd);
            }
            return 0;
        }
    }
    char line[PATH_MAX + 128];
    snprintf(line, sizeof(line), "%s\n", cmd);
    if (write(fd, line, strlen(line)) < 0) {
        close(fd);
        return 0;
    }
    shutdown(fd, SHUT_WR);
    size_t total = 0;
    ssize_t n;
    while (total + 1 < respsz && (n = read(fd, resp + total, respsz - 1 - total)) > 0) {
        total += (size_t)n;
    }
    resp[total] = '\0';
    close(fd);
    return 1;
}

static int xisback_list_layers(WpLayer *layers, int max)
{
    char resp[16384];
    if (!xisback_send("LIST", resp, sizeof(resp))) {
        return 0;
    }
    int n = 0;
    char *save = NULL;
    char *line = strtok_r(resp, "\n", &save);
    while (line && n < max) {
        char *save2 = NULL;
        char *output = strtok_r(line, "\t", &save2);
        char *desktop = output ? strtok_r(NULL, "\t", &save2) : NULL;
        char *mode = desktop ? strtok_r(NULL, "\t", &save2) : NULL;
        char *interval = mode ? strtok_r(NULL, "\t", &save2) : NULL;
        char *shuffle = interval ? strtok_r(NULL, "\t", &save2) : NULL;
        char *fade = shuffle ? strtok_r(NULL, "\t", &save2) : NULL;
        char *path = fade ? strtok_r(NULL, "", &save2) : NULL;
        if (output && desktop && mode && interval && shuffle && fade && path) {
            WpLayer *l = &layers[n++];
            snprintf(l->output, sizeof(l->output), "%s", output);
            snprintf(l->desktop, sizeof(l->desktop), "%s", desktop);
            snprintf(l->mode, sizeof(l->mode), "%s", mode);
            l->interval = atoi(interval);
            l->shuffle = atoi(shuffle);
            l->fade_ms = atoi(fade);
            snprintf(l->path, sizeof(l->path), "%s", path);
        }
        line = strtok_r(NULL, "\n", &save);
    }
    return n;
}

static void refresh_wp_layers(void)
{
    gtk_list_store_clear(g_wp_layers_store);
    g_n_wp_layers = xisback_list_layers(g_wp_layers, MAX_WP_LAYERS);
    for (int i = 0; i < g_n_wp_layers; i++) {
        WpLayer *l = &g_wp_layers[i];
        char interval_s[16], shuffle_s[8], fade_s[16];
        snprintf(interval_s, sizeof(interval_s), "%d", l->interval);
        snprintf(shuffle_s, sizeof(shuffle_s), "%s", l->shuffle ? "sim" : "nao");
        snprintf(fade_s, sizeof(fade_s), "%.1fs", l->fade_ms / 1000.0);
        GtkTreeIter it;
        gtk_list_store_append(g_wp_layers_store, &it);
        gtk_list_store_set(g_wp_layers_store, &it,
                            COL_WP_OUTPUT, l->output, COL_WP_DESKTOP, l->desktop, COL_WP_MODE, l->mode,
                            COL_WP_INTERVAL, interval_s, COL_WP_SHUFFLE, shuffle_s, COL_WP_FADE, fade_s,
                            COL_WP_PATH, l->path, -1);
    }
    char status[64];
    snprintf(status, sizeof(status), "%d camada(s) ativa(s).", g_n_wp_layers);
    gtk_label_set_text(GTK_LABEL(g_wp_status_label), status);
}

static void wp_load_layer_into_form(const WpLayer *l)
{
    gtk_entry_set_text(GTK_ENTRY(g_wp_output_entry), l->output);
    gtk_entry_set_text(GTK_ENTRY(g_wp_desktop_entry), l->desktop);
    gtk_combo_box_set_active(GTK_COMBO_BOX(g_wp_mode_combo), strcmp(l->mode, "stretch") == 0 ? 1 : 0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_wp_interval_spin), l->interval);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_wp_shuffle_chk), l->shuffle);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_wp_fade_spin), l->fade_ms / 1000.0);
    gtk_entry_set_text(GTK_ENTRY(g_wp_path_entry), l->path);
}

static void on_wp_row_selected(GtkTreeSelection *sel, gpointer data)
{
    (void)data;
    GtkTreeIter it;
    GtkTreeModel *model;
    if (!gtk_tree_selection_get_selected(sel, &model, &it)) {
        return;
    }
    GtkTreePath *path = gtk_tree_model_get_path(model, &it);
    int idx = gtk_tree_path_get_indices(path)[0];
    gtk_tree_path_free(path);
    if (idx >= 0 && idx < g_n_wp_layers) {
        wp_load_layer_into_form(&g_wp_layers[idx]);
    }
}

static int wp_selected_index(void)
{
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(g_wp_layers_view));
    GtkTreeIter it;
    GtkTreeModel *model;
    if (!gtk_tree_selection_get_selected(sel, &model, &it)) {
        return -1;
    }
    GtkTreePath *path = gtk_tree_model_get_path(model, &it);
    int idx = gtk_tree_path_get_indices(path)[0];
    gtk_tree_path_free(path);
    return idx;
}

static void on_wp_set(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    const char *output = gtk_entry_get_text(GTK_ENTRY(g_wp_output_entry));
    const char *desktop = gtk_entry_get_text(GTK_ENTRY(g_wp_desktop_entry));
    gchar *mode = gtk_combo_box_get_active_text(GTK_COMBO_BOX(g_wp_mode_combo));
    int interval = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_wp_interval_spin));
    int shuffle = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_wp_shuffle_chk));
    int fade_ms = (int)(gtk_spin_button_get_value(GTK_SPIN_BUTTON(g_wp_fade_spin)) * 1000);
    const char *path = gtk_entry_get_text(GTK_ENTRY(g_wp_path_entry));

    char cmd[PATH_MAX + 256];
    snprintf(cmd, sizeof(cmd), "SET\t%s\t%s\t%s\t%d\t%d\t%d\t%s",
              output[0] ? output : "*", desktop[0] ? desktop : "*", mode ? mode : "fill",
              interval, shuffle, fade_ms, path);
    char resp[256];
    xisback_send(cmd, resp, sizeof(resp));
    g_free(mode);
    refresh_wp_layers();
}

static void on_wp_next(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    int idx = wp_selected_index();
    if (idx < 0 || idx >= g_n_wp_layers) {
        return;
    }
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "NEXT\t%s\t%s", g_wp_layers[idx].output, g_wp_layers[idx].desktop);
    char resp[256];
    xisback_send(cmd, resp, sizeof(resp));
    refresh_wp_layers();
}

static void on_wp_clear_selected(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    int idx = wp_selected_index();
    if (idx < 0 || idx >= g_n_wp_layers) {
        return;
    }
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "CLEAR\t%s\t%s", g_wp_layers[idx].output, g_wp_layers[idx].desktop);
    char resp[256];
    xisback_send(cmd, resp, sizeof(resp));
    refresh_wp_layers();
}

static void on_wp_clear_all(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    char resp[256];
    xisback_send("CLEARALL", resp, sizeof(resp));
    refresh_wp_layers();
}

static void on_wp_refresh(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    refresh_wp_layers();
}

static void on_wp_global(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    char resp[256];
    xisback_send("CLEARALL", resp, sizeof(resp));
    on_wp_set(NULL, NULL);
}

static void on_wp_pick_file(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    GtkWidget *dlg = gtk_file_chooser_dialog_new("Escolher imagem", NULL, GTK_FILE_CHOOSER_ACTION_OPEN,
                                                   GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
                                                   GTK_STOCK_OPEN, GTK_RESPONSE_ACCEPT, NULL);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        gtk_entry_set_text(GTK_ENTRY(g_wp_path_entry), filename);
        g_free(filename);
    }
    gtk_widget_destroy(dlg);
}

static void on_wp_pick_folder(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    GtkWidget *dlg = gtk_file_chooser_dialog_new("Escolher pasta de slideshow", NULL,
                                                   GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
                                                   GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
                                                   GTK_STOCK_OPEN, GTK_RESPONSE_ACCEPT, NULL);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        gtk_entry_set_text(GTK_ENTRY(g_wp_path_entry), filename);
        g_free(filename);
    }
    gtk_widget_destroy(dlg);
}

static void on_wp_actions_save(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    const char *left = gtk_entry_get_text(GTK_ENTRY(g_wp_action_left));
    const char *right = gtk_entry_get_text(GTK_ENTRY(g_wp_action_right));
    const char *middle = gtk_entry_get_text(GTK_ENTRY(g_wp_action_middle));
    const char *dbl = gtk_entry_get_text(GTK_ENTRY(g_wp_action_double));
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "SETACTIONS\t%s\t%s\t%s\t%s", left, right, middle, dbl);
    char resp[256];
    xisback_send(cmd, resp, sizeof(resp));
}

static void load_wp_actions(void)
{
    char resp[1024];
    if (!xisback_send("ACTIONS", resp, sizeof(resp))) {
        return;
    }
    char *save = NULL;
    char *left = strtok_r(resp, "\t", &save);
    char *right = left ? strtok_r(NULL, "\t", &save) : NULL;
    char *middle = right ? strtok_r(NULL, "\t", &save) : NULL;
    char *dbl = middle ? strtok_r(NULL, "\n", &save) : NULL;
    gtk_entry_set_text(GTK_ENTRY(g_wp_action_left), left ? left : "");
    gtk_entry_set_text(GTK_ENTRY(g_wp_action_right), right ? right : "");
    gtk_entry_set_text(GTK_ENTRY(g_wp_action_middle), middle ? middle : "");
    gtk_entry_set_text(GTK_ENTRY(g_wp_action_double), dbl ? dbl : "");
}

static GtkWidget *build_wallpaper_tab(void)
{
    GtkWidget *outer = gtk_vbox_new(FALSE, 8);
    gtk_container_set_border_width(GTK_CONTAINER(outer), 12);

    g_wp_status_label = gtk_label_new("-");
    gtk_misc_set_alignment(GTK_MISC(g_wp_status_label), 0.0, 0.5);
    gtk_box_pack_start(GTK_BOX(outer), g_wp_status_label, FALSE, FALSE, 0);

    g_wp_layers_store = gtk_list_store_new(N_WP_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                             G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    g_wp_layers_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(g_wp_layers_store));
    const char *wp_titles[N_WP_COLS] = {"Output", "Desktop", "Modo", "Intervalo", "Shuffle", "Fade", "Caminho"};
    for (int col = 0; col < N_WP_COLS; col++) {
        GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
        GtkTreeViewColumn *tvcol = gtk_tree_view_column_new_with_attributes(wp_titles[col], renderer, "text", col, NULL);
        gtk_tree_view_column_set_expand(tvcol, col == N_WP_COLS - 1);
        gtk_tree_view_append_column(GTK_TREE_VIEW(g_wp_layers_view), tvcol);
    }
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(g_wp_layers_view)), "changed",
                       G_CALLBACK(on_wp_row_selected), NULL);
    GtkWidget *wp_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(wp_scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(wp_scroll, -1, 140);
    gtk_container_add(GTK_CONTAINER(wp_scroll), g_wp_layers_view);

    GtkWidget *layers_box = gtk_vbox_new(FALSE, 4);
    gtk_box_pack_start(GTK_BOX(layers_box), wp_scroll, TRUE, TRUE, 0);
    GtkWidget *layers_btnbox = gtk_hbox_new(FALSE, 6);
    GtkWidget *next_btn = gtk_button_new_with_label("Avancar slide");
    GtkWidget *clear_sel_btn = gtk_button_new_with_label("Limpar selecionada");
    GtkWidget *clear_all_btn = gtk_button_new_with_label("Limpar todas");
    GtkWidget *refresh_btn = gtk_button_new_with_label("Atualizar");
    GtkWidget *global_btn = gtk_button_new_with_label("Mesmo papel de parede pra tudo");
    g_signal_connect(next_btn, "clicked", G_CALLBACK(on_wp_next), NULL);
    g_signal_connect(clear_sel_btn, "clicked", G_CALLBACK(on_wp_clear_selected), NULL);
    g_signal_connect(clear_all_btn, "clicked", G_CALLBACK(on_wp_clear_all), NULL);
    g_signal_connect(refresh_btn, "clicked", G_CALLBACK(on_wp_refresh), NULL);
    g_signal_connect(global_btn, "clicked", G_CALLBACK(on_wp_global), NULL);
    gtk_box_pack_start(GTK_BOX(layers_btnbox), next_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(layers_btnbox), clear_sel_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(layers_btnbox), clear_all_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(layers_btnbox), refresh_btn, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(layers_btnbox), global_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(layers_box), layers_btnbox, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), frame_with("Camadas ativas (clique numa linha pra editar)", layers_box), TRUE, TRUE, 0);

    GtkWidget *set_table = gtk_table_new(6, 2, FALSE);
    g_wp_output_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(g_wp_output_entry), "*");
    labeled_row(set_table, 0, "Output ('*' = tudo):", g_wp_output_entry);
    g_wp_desktop_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(g_wp_desktop_entry), "*");
    labeled_row(set_table, 1, "Desktop ('*' = todos):", g_wp_desktop_entry);
    g_wp_mode_combo = gtk_combo_box_new_text();
    gtk_combo_box_append_text(GTK_COMBO_BOX(g_wp_mode_combo), "fill");
    gtk_combo_box_append_text(GTK_COMBO_BOX(g_wp_mode_combo), "stretch");
    gtk_combo_box_set_active(GTK_COMBO_BOX(g_wp_mode_combo), 0);
    labeled_row(set_table, 2, "Modo:", g_wp_mode_combo);
    g_wp_interval_spin = gtk_spin_button_new_with_range(5, 86400, 5);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_wp_interval_spin), 300);
    labeled_row(set_table, 3, "Intervalo do slideshow (s):", g_wp_interval_spin);
    g_wp_shuffle_chk = gtk_check_button_new_with_label("Ordem aleatoria (em vez de alfabetica)");
    gtk_table_attach(GTK_TABLE(set_table), g_wp_shuffle_chk, 0, 2, 4, 5, GTK_FILL, GTK_FILL, 4, 2);
    g_wp_fade_spin = gtk_spin_button_new_with_range(0.0, 5.0, 0.1);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(g_wp_fade_spin), 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_wp_fade_spin), 1.0);
    labeled_row(set_table, 5, "Crossfade (s, 0=instantaneo):", g_wp_fade_spin);

    GtkWidget *path_row = gtk_hbox_new(FALSE, 4);
    g_wp_path_entry = gtk_entry_new();
    gtk_box_pack_start(GTK_BOX(path_row), g_wp_path_entry, TRUE, TRUE, 0);
    GtkWidget *file_btn = gtk_button_new_with_label("Arquivo...");
    GtkWidget *folder_btn = gtk_button_new_with_label("Pasta...");
    g_signal_connect(file_btn, "clicked", G_CALLBACK(on_wp_pick_file), NULL);
    g_signal_connect(folder_btn, "clicked", G_CALLBACK(on_wp_pick_folder), NULL);
    gtk_box_pack_start(GTK_BOX(path_row), file_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(path_row), folder_btn, FALSE, FALSE, 0);

    GtkWidget *set_box = gtk_vbox_new(FALSE, 4);
    gtk_box_pack_start(GTK_BOX(set_box), set_table, FALSE, FALSE, 0);
    GtkWidget *path_label = gtk_label_new("Imagem ou pasta:");
    gtk_misc_set_alignment(GTK_MISC(path_label), 0.0, 0.5);
    gtk_box_pack_start(GTK_BOX(set_box), path_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(set_box), path_row, FALSE, FALSE, 0);
    GtkWidget *set_btn = gtk_button_new_with_label("Definir camada");
    g_signal_connect(set_btn, "clicked", G_CALLBACK(on_wp_set), NULL);
    GtkWidget *set_btnbox = gtk_hbox_new(FALSE, 0);
    gtk_box_pack_end(GTK_BOX(set_btnbox), set_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(set_box), set_btnbox, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), frame_with("Definir/substituir uma camada", set_box), FALSE, FALSE, 0);

    GtkWidget *actions_table = gtk_table_new(4, 2, FALSE);
    g_wp_action_left = gtk_entry_new();
    labeled_row(actions_table, 0, "Clique esquerdo:", g_wp_action_left);
    g_wp_action_right = gtk_entry_new();
    labeled_row(actions_table, 1, "Clique direito:", g_wp_action_right);
    g_wp_action_middle = gtk_entry_new();
    labeled_row(actions_table, 2, "Clique do meio:", g_wp_action_middle);
    g_wp_action_double = gtk_entry_new();
    labeled_row(actions_table, 3, "Clique duplo:", g_wp_action_double);
    GtkWidget *actions_box = gtk_vbox_new(FALSE, 4);
    gtk_box_pack_start(GTK_BOX(actions_box), actions_table, FALSE, FALSE, 0);
    GtkWidget *actions_save_btn = gtk_button_new_with_label("Salvar acoes de clique");
    g_signal_connect(actions_save_btn, "clicked", G_CALLBACK(on_wp_actions_save), NULL);
    gtk_box_pack_start(GTK_BOX(actions_box), actions_save_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), frame_with("Acoes de clique", actions_box), FALSE, FALSE, 0);

    load_wp_actions();
    refresh_wp_layers();

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_add_with_viewport(GTK_SCROLLED_WINDOW(scroll), outer);
    return scroll;
}

/* ---- Telas tab: xrandr layout, draggable canvas ----------------------- */
/*
 * Ported subset of xisconf.py's Screens tab: connect/enable/disable,
 * resolution+refresh rate, position (via drag on the canvas, snapping to
 * other outputs' edges), rotation, primary output, mirror/DPI/scale, all
 * diffed against a baseline before Aplicar sends only what changed (see
 * apply_output_diff(), which mirrors xisconf.py's _output_diff_args()
 * field for field). Deliberately NOT ported -- xisconf.py's generic
 * "advanced driver properties" system (TearFree, underscan, PRIME Sync,
 * etc., discovered from `xrandr --verbose`'s per-output "supported:"/
 * "range:" sub-lines): needs parsing --verbose output (a second xrandr
 * call with a much richer, driver-dependent format) and a dynamic
 * per-property widget system, which didn't fit this pass. Only plain
 * `xrandr` (no --verbose) is parsed here -- which also means mirror/DPI/
 * scale can't be read back from hardware, only written (see the
 * ScreenOutput struct's comment and screens_redetect_preserving_extras()).
 */

static int is_xid_token(const char *t)
{
    size_t n = strlen(t);
    if (n < 4 || t[0] != '(' || t[n - 1] != ')') {
        return 0;
    }
    if (strncmp(t + 1, "0x", 2) != 0) {
        return 0;
    }
    for (size_t i = 3; i < n - 1; i++) {
        if (!isxdigit((unsigned char)t[i])) {
            return 0;
        }
    }
    return 1;
}

static int is_rotation_word(const char *t)
{
    return !strcmp(t, "normal") || !strcmp(t, "left") || !strcmp(t, "right") || !strcmp(t, "inverted");
}

static int tokenize_ws(char *line, char *tokens[], int max)
{
    int n = 0;
    char *save = NULL;
    char *tok = strtok_r(line, " \t", &save);
    while (tok && n < max) {
        tokens[n++] = tok;
        tok = strtok_r(NULL, " \t", &save);
    }
    return n;
}

/* Mirrors xisconf.py's _parse_header(): NAME connected|disconnected
 * [primary] [WxH+X+Y] [(0xID)] [rotation] (...) [WWmm x HHmm]. Only the
 * fields this tab actually uses are extracted. */
static void parse_output_header(char *line, ScreenOutput *o)
{
    memset(o, 0, sizeof(*o));
    snprintf(o->rotation, sizeof(o->rotation), "normal");
    o->scale_x = 1.0;
    o->scale_y = 1.0;
    char *tokens[32];
    int n = tokenize_ws(line, tokens, 32);
    if (n < 2) {
        return;
    }
    snprintf(o->name, sizeof(o->name), "%s", tokens[0]);
    o->connected = !strcmp(tokens[1], "connected");
    int i = 2;
    if (i < n && !strcmp(tokens[i], "primary")) {
        o->primary = 1;
        i++;
    }
    if (i < n) {
        int w, h, x, y;
        if (sscanf(tokens[i], "%dx%d+%d+%d", &w, &h, &x, &y) == 4) {
            o->width = w;
            o->height = h;
            o->x = x;
            o->y = y;
            o->enabled = 1;
            i++;
        }
    }
    if (i < n && is_xid_token(tokens[i])) {
        i++;
    }
    if (i < n && is_rotation_word(tokens[i])) {
        snprintf(o->rotation, sizeof(o->rotation), "%s", tokens[i]);
    }
}

static OutMode *find_or_add_mode(ScreenOutput *o, const char *name)
{
    for (int i = 0; i < o->n_modes; i++) {
        if (!strcmp(o->modes[i].name, name)) {
            return &o->modes[i];
        }
    }
    if (o->n_modes >= MAX_MODES) {
        return &o->modes[0];
    }
    OutMode *m = &o->modes[o->n_modes++];
    snprintf(m->name, sizeof(m->name), "%s", name);
    m->n_rates = 0;
    return m;
}

/* Mode lines are indented exactly 3 spaces ("   1920x1080   60.00*+ ..."),
 * same delimiter xisconf.py's _MODE_LINE_RE uses to tell them apart from
 * anything else. */
static int parse_mode_line(const char *line, int *w, int *h, char *rest, size_t restsz)
{
    if (strncmp(line, "   ", 3) != 0 || line[3] == ' ') {
        return 0;
    }
    int consumed = 0;
    if (sscanf(line + 3, "%dx%d%n", w, h, &consumed) != 2) {
        return 0;
    }
    const char *r = line + 3 + consumed;
    while (*r == ' ') {
        r++;
    }
    snprintf(rest, restsz, "%s", r);
    return 1;
}

static void parse_rate_tokens(ScreenOutput *o, const char *modename, char *rest)
{
    OutMode *m = find_or_add_mode(o, modename);
    char *save = NULL;
    char *tok = strtok_r(rest, " \t", &save);
    while (tok) {
        int is_cur = strchr(tok, '*') != NULL;
        char rate[16];
        int k = 0;
        for (const char *p = tok; *p && k < 15; p++) {
            if (*p == '*' || *p == '+') {
                continue;
            }
            rate[k++] = *p;
        }
        rate[k] = '\0';
        if (m->n_rates < MAX_RATES) {
            snprintf(m->rates[m->n_rates].rate, sizeof(m->rates[0].rate), "%s", rate);
            m->rates[m->n_rates].is_current = is_cur;
            m->n_rates++;
        }
        if (is_cur) {
            snprintf(o->current_mode, sizeof(o->current_mode), "%s", modename);
            snprintf(o->current_rate, sizeof(o->current_rate), "%s", rate);
        }
        tok = strtok_r(NULL, " \t", &save);
    }
}

static int detect_outputs(ScreenOutput *outs, int max)
{
    char *argv[] = {"xrandr", NULL};
    char out[16384];
    if (!run_capture(argv, out, sizeof(out))) {
        return 0;
    }
    int n = 0;
    ScreenOutput *cur = NULL;
    char *save = NULL;
    char *line = strtok_r(out, "\n", &save);
    while (line) {
        if (!isspace((unsigned char)line[0])) {
            char tmp[512];
            snprintf(tmp, sizeof(tmp), "%s", line);
            char *save2 = NULL;
            char *t1 = strtok_r(tmp, " \t", &save2);
            char *t2 = t1 ? strtok_r(NULL, " \t", &save2) : NULL;
            if (t2 && (!strcmp(t2, "connected") || !strcmp(t2, "disconnected")) && n < max) {
                cur = &outs[n++];
                char linecopy[512];
                snprintf(linecopy, sizeof(linecopy), "%s", line);
                parse_output_header(linecopy, cur);
            } else {
                cur = NULL;
            }
        } else if (cur) {
            int w, h;
            char rest[256];
            if (parse_mode_line(line, &w, &h, rest, sizeof(rest))) {
                char modename[16];
                snprintf(modename, sizeof(modename), "%dx%d", w, h);
                parse_rate_tokens(cur, modename, rest);
            }
        }
        line = strtok_r(NULL, "\n", &save);
    }
    return n;
}

/* Mirrors xisconf.py's _output_args()/_output_diff_args() field for
 * field, including the write-only mirror/scale/DPI fields (see
 * ScreenOutput's comment on why their "baseline" isn't hardware truth). */
static void apply_output_diff(const ScreenOutput *o, const ScreenOutput *base)
{
    if (!o->connected) {
        return;
    }
    char *argv[24];
    int ac = 0;
    argv[ac++] = "xrandr";
    argv[ac++] = "--output";
    argv[ac++] = (char *)o->name;

    char modebuf[32], ratebuf[16], posbuf[32], scalebuf[32], dpibuf[16];

    if (o->enabled != base->enabled) {
        if (!o->enabled) {
            argv[ac++] = "--off";
            argv[ac] = NULL;
            run_fire(argv);
            return;
        }
        /* Turning an output back on needs a full description -- there's
         * no sensible "diff" starting from a disabled state. */
        if (o->mirror_of[0]) {
            argv[ac++] = "--same-as";
            argv[ac++] = (char *)o->mirror_of;
        } else {
            if (o->current_mode[0]) {
                argv[ac++] = "--mode";
                snprintf(modebuf, sizeof(modebuf), "%s", o->current_mode);
                argv[ac++] = modebuf;
            }
            if (o->current_rate[0]) {
                argv[ac++] = "--rate";
                snprintf(ratebuf, sizeof(ratebuf), "%s", o->current_rate);
                argv[ac++] = ratebuf;
            }
            snprintf(posbuf, sizeof(posbuf), "%dx%d", o->x, o->y);
            argv[ac++] = "--pos";
            argv[ac++] = posbuf;
        }
        argv[ac++] = "--rotate";
        argv[ac++] = (char *)o->rotation;
        argv[ac++] = o->primary ? "--primary" : "--noprimary";
        double sx = fabs(o->scale_x) > 1e-6 ? o->scale_x : 1.0;
        double sy = fabs(o->scale_y) > 1e-6 ? o->scale_y : 1.0;
        snprintf(scalebuf, sizeof(scalebuf), "%.4fx%.4f", sx, sy);
        argv[ac++] = "--scale";
        argv[ac++] = scalebuf;
        if (o->dpi) {
            argv[ac++] = "--set";
            argv[ac++] = "DPI";
            snprintf(dpibuf, sizeof(dpibuf), "%d", o->dpi);
            argv[ac++] = dpibuf;
        }
        argv[ac] = NULL;
        run_fire(argv);
        return;
    }
    if (!o->enabled) {
        return;
    }

    int changed = 0;
    int mirror_changed = strcmp(o->mirror_of, base->mirror_of) != 0;
    if (mirror_changed) {
        if (o->mirror_of[0]) {
            argv[ac++] = "--same-as";
            argv[ac++] = (char *)o->mirror_of;
        } else {
            if (o->current_mode[0]) {
                argv[ac++] = "--mode";
                snprintf(modebuf, sizeof(modebuf), "%s", o->current_mode);
                argv[ac++] = modebuf;
            }
            if (o->current_rate[0]) {
                argv[ac++] = "--rate";
                snprintf(ratebuf, sizeof(ratebuf), "%s", o->current_rate);
                argv[ac++] = ratebuf;
            }
            snprintf(posbuf, sizeof(posbuf), "%dx%d", o->x, o->y);
            argv[ac++] = "--pos";
            argv[ac++] = posbuf;
        }
        changed = 1;
    } else if (!o->mirror_of[0]) {
        if (strcmp(o->current_mode, base->current_mode)) {
            argv[ac++] = "--mode";
            snprintf(modebuf, sizeof(modebuf), "%s", o->current_mode);
            argv[ac++] = modebuf;
            changed = 1;
        }
        if (strcmp(o->current_rate, base->current_rate)) {
            argv[ac++] = "--rate";
            snprintf(ratebuf, sizeof(ratebuf), "%s", o->current_rate);
            argv[ac++] = ratebuf;
            changed = 1;
        }
        if (o->x != base->x || o->y != base->y) {
            snprintf(posbuf, sizeof(posbuf), "%dx%d", o->x, o->y);
            argv[ac++] = "--pos";
            argv[ac++] = posbuf;
            changed = 1;
        }
    }
    if (strcmp(o->rotation, base->rotation)) {
        argv[ac++] = "--rotate";
        argv[ac++] = (char *)o->rotation;
        changed = 1;
    }
    if (o->primary != base->primary) {
        argv[ac++] = o->primary ? "--primary" : "--noprimary";
        changed = 1;
    }
    if (lround(o->scale_x * 10000) != lround(base->scale_x * 10000) ||
        lround(o->scale_y * 10000) != lround(base->scale_y * 10000)) {
        double sx = fabs(o->scale_x) > 1e-6 ? o->scale_x : 1.0;
        double sy = fabs(o->scale_y) > 1e-6 ? o->scale_y : 1.0;
        snprintf(scalebuf, sizeof(scalebuf), "%.4fx%.4f", sx, sy);
        argv[ac++] = "--scale";
        argv[ac++] = scalebuf;
        changed = 1;
    }
    if (o->dpi != base->dpi && o->dpi) {
        argv[ac++] = "--set";
        argv[ac++] = "DPI";
        snprintf(dpibuf, sizeof(dpibuf), "%d", o->dpi);
        argv[ac++] = dpibuf;
        changed = 1;
    }
    argv[ac] = NULL;
    if (changed) {
        run_fire(argv);
    }
}

static void output_visual_size(const ScreenOutput *o, int *w, int *h)
{
    *w = o->width;
    *h = o->height;
    if (!strcmp(o->rotation, "left") || !strcmp(o->rotation, "right")) {
        int t = *w;
        *w = *h;
        *h = t;
    }
}

static void compute_world(double *ox, double *oy, double *scale, int canvas_w, int canvas_h)
{
    int min_x = INT_MAX, min_y = INT_MAX, max_x = INT_MIN, max_y = INT_MIN;
    int any = 0;
    for (int i = 0; i < g_n_outputs; i++) {
        ScreenOutput *o = &g_outputs[i];
        if (!o->connected || !o->enabled) {
            continue;
        }
        any = 1;
        int w, h;
        output_visual_size(o, &w, &h);
        if (o->x < min_x) {
            min_x = o->x;
        }
        if (o->y < min_y) {
            min_y = o->y;
        }
        if (o->x + w > max_x) {
            max_x = o->x + w;
        }
        if (o->y + h > max_y) {
            max_y = o->y + h;
        }
    }
    if (!any) {
        *ox = 0;
        *oy = 0;
        *scale = 0.05;
        return;
    }
    *ox = min_x;
    *oy = min_y;
    double world_w = max_x - min_x > 0 ? max_x - min_x : 1;
    double world_h = max_y - min_y > 0 ? max_y - min_y : 1;
    double sx = (canvas_w - 40) / world_w;
    double sy = (canvas_h - 40) / world_h;
    *scale = sx < sy ? sx : sy;
    if (*scale <= 0) {
        *scale = 0.01;
    }
}

static int screens_hit_test(double px, double py, int canvas_w, int canvas_h)
{
    double ox, oy, scale;
    compute_world(&ox, &oy, &scale, canvas_w, canvas_h);
    for (int i = g_n_outputs - 1; i >= 0; i--) {
        ScreenOutput *o = &g_outputs[i];
        if (!o->connected || !o->enabled) {
            continue;
        }
        int w, h;
        output_visual_size(o, &w, &h);
        double rx = 20 + (o->x - ox) * scale, ry = 20 + (o->y - oy) * scale;
        double rw = w * scale, rh = h * scale;
        if (px >= rx && px <= rx + rw && py >= ry && py <= ry + rh) {
            return i;
        }
    }
    return -1;
}

/* Snaps `*x`/`*y` (the dragged output's candidate top-left, in xrandr
 * world units) to align an edge with any other connected+enabled
 * output's edges, if within SNAP_CANVAS_PX *canvas* pixels at the
 * current zoom -- converted to world units via `scale` so the snap
 * distance feels the same regardless of how zoomed out the layout is.
 * Left/right and top/bottom are snapped independently, each picking
 * whichever candidate-edge/other-edge pairing is closest. */
#define SNAP_CANVAS_PX 10

static void screens_snap(int idx, int *x, int *y, int w, int h, double scale)
{
    if (scale <= 0) {
        return;
    }
    double thresh = SNAP_CANVAS_PX / scale;
    double best_dx = 0, best_dx_dist = thresh + 1;
    double best_dy = 0, best_dy_dist = thresh + 1;
    int have_dx = 0, have_dy = 0;

    for (int i = 0; i < g_n_outputs; i++) {
        if (i == idx) {
            continue;
        }
        ScreenOutput *o = &g_outputs[i];
        if (!o->connected || !o->enabled) {
            continue;
        }
        int ow, oh;
        output_visual_size(o, &ow, &oh);
        int ol = o->x, orr = o->x + ow, ot = o->y, ob = o->y + oh;
        int cl = *x, cr = *x + w, ct = *y, cb = *y + h;

        int xpairs[4][2] = {{cl, ol}, {cl, orr}, {cr, ol}, {cr, orr}};
        for (int k = 0; k < 4; k++) {
            double d = fabs((double)(xpairs[k][0] - xpairs[k][1]));
            if (d <= thresh && d < best_dx_dist) {
                best_dx_dist = d;
                best_dx = *x + (xpairs[k][1] - xpairs[k][0]);
                have_dx = 1;
            }
        }
        int ypairs[4][2] = {{ct, ot}, {ct, ob}, {cb, ot}, {cb, ob}};
        for (int k = 0; k < 4; k++) {
            double d = fabs((double)(ypairs[k][0] - ypairs[k][1]));
            if (d <= thresh && d < best_dy_dist) {
                best_dy_dist = d;
                best_dy = *y + (ypairs[k][1] - ypairs[k][0]);
                have_dy = 1;
            }
        }
    }
    if (have_dx) {
        *x = (int)lround(best_dx);
    }
    if (have_dy) {
        *y = (int)lround(best_dy);
    }
}

static void sync_screens_form(void);

static gboolean screens_canvas_expose(GtkWidget *widget, GdkEventExpose *event, gpointer data)
{
    (void)event;
    (void)data;
    cairo_t *cr = gdk_cairo_create(widget->window);
    int cw = widget->allocation.width, ch = widget->allocation.height;
    cairo_set_source_rgb(cr, 0.14, 0.15, 0.16);
    cairo_paint(cr);

    double ox, oy, scale;
    compute_world(&ox, &oy, &scale, cw, ch);

    for (int i = 0; i < g_n_outputs; i++) {
        ScreenOutput *o = &g_outputs[i];
        if (!o->connected || !o->enabled) {
            continue;
        }
        int w, h;
        output_visual_size(o, &w, &h);
        double rx = 20 + (o->x - ox) * scale, ry = 20 + (o->y - oy) * scale;
        double rw = w * scale, rh = h * scale;

        int selected = (g_screens_selected == i);
        cairo_set_source_rgb(cr, selected ? 0.20 : 0.16, selected ? 0.45 : 0.30, selected ? 0.75 : 0.45);
        cairo_rectangle(cr, rx, ry, rw, rh);
        cairo_fill_preserve(cr);
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_set_line_width(cr, selected ? 2 : 1);
        cairo_stroke(cr);

        cairo_move_to(cr, rx + 6, ry + 16);
        cairo_show_text(cr, o->name);
        if (o->primary) {
            cairo_move_to(cr, rx + 6, ry + 32);
            cairo_show_text(cr, "(primary)");
        }
    }
    cairo_destroy(cr);
    return TRUE;
}

static gboolean screens_canvas_press(GtkWidget *widget, GdkEventButton *event, gpointer data)
{
    (void)data;
    int cw = widget->allocation.width, ch = widget->allocation.height;
    int idx = screens_hit_test(event->x, event->y, cw, ch);
    if (idx >= 0) {
        double ox, oy, scale;
        compute_world(&ox, &oy, &scale, cw, ch);
        ScreenOutput *o = &g_outputs[idx];
        double rx = 20 + (o->x - ox) * scale, ry = 20 + (o->y - oy) * scale;
        g_screens_selected = idx;
        g_screens_dragging = 1;
        g_screens_drag_dx = event->x - rx;
        g_screens_drag_dy = event->y - ry;
        sync_screens_form();
        gtk_widget_queue_draw(widget);
    }
    return TRUE;
}

static gboolean screens_canvas_motion(GtkWidget *widget, GdkEventMotion *event, gpointer data)
{
    (void)data;
    if (!g_screens_dragging || g_screens_selected < 0) {
        return TRUE;
    }
    int cw = widget->allocation.width, ch = widget->allocation.height;
    double ox, oy, scale;
    compute_world(&ox, &oy, &scale, cw, ch);
    if (scale <= 0) {
        return TRUE;
    }
    double new_rx = event->x - g_screens_drag_dx, new_ry = event->y - g_screens_drag_dy;
    ScreenOutput *o = &g_outputs[g_screens_selected];
    o->x = (int)lround(ox + (new_rx - 20) / scale);
    o->y = (int)lround(oy + (new_ry - 20) / scale);
    if (!o->mirror_of[0]) {
        int w, h;
        output_visual_size(o, &w, &h);
        screens_snap(g_screens_selected, &o->x, &o->y, w, h, scale);
    }
    gtk_widget_queue_draw(widget);
    return TRUE;
}

static gboolean screens_canvas_release(GtkWidget *widget, GdkEventButton *event, gpointer data)
{
    (void)widget;
    (void)event;
    (void)data;
    g_screens_dragging = 0;
    return TRUE;
}

static void populate_rate_combo(void)
{
    if (g_screens_selected < 0) {
        return;
    }
    ScreenOutput *o = &g_outputs[g_screens_selected];
    gchar *modename = gtk_combo_box_get_active_text(GTK_COMBO_BOX(g_screens_res_combo));
    if (!modename) {
        return;
    }
    /* rebuild rate combo (GTK2 has no "remove all" for the text-combo
     * convenience API, so destroy and recreate its model instead) */
    GtkTreeModel *model = gtk_combo_box_get_model(GTK_COMBO_BOX(g_screens_rate_combo));
    gtk_list_store_clear(GTK_LIST_STORE(model));
    int active = -1;
    for (int i = 0; i < o->n_modes; i++) {
        if (strcmp(o->modes[i].name, modename)) {
            continue;
        }
        for (int r = 0; r < o->modes[i].n_rates; r++) {
            gtk_combo_box_append_text(GTK_COMBO_BOX(g_screens_rate_combo), o->modes[i].rates[r].rate);
            if (!strcmp(o->modes[i].rates[r].rate, o->current_rate)) {
                active = r;
            }
        }
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(g_screens_rate_combo), active >= 0 ? active : 0);
    g_free(modename);
}

static void sync_screens_form(void)
{
    if (g_screens_selected < 0) {
        return;
    }
    ScreenOutput *o = &g_outputs[g_screens_selected];
    g_screens_syncing = 1;

    GtkTreeModel *resmodel = gtk_combo_box_get_model(GTK_COMBO_BOX(g_screens_res_combo));
    gtk_list_store_clear(GTK_LIST_STORE(resmodel));
    int active = -1;
    for (int i = 0; i < o->n_modes; i++) {
        gtk_combo_box_append_text(GTK_COMBO_BOX(g_screens_res_combo), o->modes[i].name);
        if (!strcmp(o->modes[i].name, o->current_mode)) {
            active = i;
        }
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(g_screens_res_combo), active >= 0 ? active : (o->n_modes > 0 ? 0 : -1));
    populate_rate_combo();

    const char *rots[] = {"normal", "left", "right", "inverted"};
    int rot_idx = 0;
    for (int i = 0; i < 4; i++) {
        if (!strcmp(rots[i], o->rotation)) {
            rot_idx = i;
        }
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(g_screens_rot_combo), rot_idx);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_screens_enabled_chk), o->enabled);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_screens_primary_chk), o->primary);

    GtkTreeModel *mirmodel = gtk_combo_box_get_model(GTK_COMBO_BOX(g_screens_mirror_combo));
    gtk_list_store_clear(GTK_LIST_STORE(mirmodel));
    gtk_combo_box_append_text(GTK_COMBO_BOX(g_screens_mirror_combo), "(nenhum)");
    int mirror_idx = 0;
    for (int i = 0, pos = 1; i < g_n_outputs; i++) {
        if (i == g_screens_selected || !g_outputs[i].connected) {
            continue;
        }
        gtk_combo_box_append_text(GTK_COMBO_BOX(g_screens_mirror_combo), g_outputs[i].name);
        if (o->mirror_of[0] && !strcmp(g_outputs[i].name, o->mirror_of)) {
            mirror_idx = pos;
        }
        pos++;
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(g_screens_mirror_combo), mirror_idx);

    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_screens_dpi_spin), o->dpi > 0 ? o->dpi : 96);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_screens_scale_spin), fabs(o->scale_x) > 1e-6 ? o->scale_x : 1.0);

    g_screens_syncing = 0;
}

static void on_screens_res_changed(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    if (g_screens_syncing || g_screens_selected < 0) {
        return;
    }
    populate_rate_combo();
    gchar *rate = gtk_combo_box_get_active_text(GTK_COMBO_BOX(g_screens_rate_combo));
    gchar *mode = gtk_combo_box_get_active_text(GTK_COMBO_BOX(g_screens_res_combo));
    ScreenOutput *o = &g_outputs[g_screens_selected];
    if (mode) {
        snprintf(o->current_mode, sizeof(o->current_mode), "%s", mode);
    }
    if (rate) {
        snprintf(o->current_rate, sizeof(o->current_rate), "%s", rate);
    }
    g_free(mode);
    g_free(rate);
    gtk_widget_queue_draw(g_screens_canvas);
}

static void on_screens_rate_changed(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    if (g_screens_syncing || g_screens_selected < 0) {
        return;
    }
    gchar *rate = gtk_combo_box_get_active_text(GTK_COMBO_BOX(g_screens_rate_combo));
    if (rate) {
        snprintf(g_outputs[g_screens_selected].current_rate, sizeof(g_outputs[0].current_rate), "%s", rate);
    }
    g_free(rate);
}

static void on_screens_rot_changed(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    if (g_screens_syncing || g_screens_selected < 0) {
        return;
    }
    gchar *rot = gtk_combo_box_get_active_text(GTK_COMBO_BOX(g_screens_rot_combo));
    if (rot) {
        snprintf(g_outputs[g_screens_selected].rotation, sizeof(g_outputs[0].rotation), "%s", rot);
    }
    g_free(rot);
    gtk_widget_queue_draw(g_screens_canvas);
}

static void on_screens_enabled_toggled(GtkWidget *widget, gpointer data)
{
    (void)data;
    if (g_screens_syncing || g_screens_selected < 0) {
        return;
    }
    g_outputs[g_screens_selected].enabled = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
    gtk_widget_queue_draw(g_screens_canvas);
}

static void on_screens_primary_toggled(GtkWidget *widget, gpointer data)
{
    (void)data;
    if (g_screens_syncing || g_screens_selected < 0) {
        return;
    }
    int active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
    if (active) {
        for (int i = 0; i < g_n_outputs; i++) {
            g_outputs[i].primary = (i == g_screens_selected);
        }
    } else {
        g_outputs[g_screens_selected].primary = 0;
    }
    gtk_widget_queue_draw(g_screens_canvas);
}

static void on_screens_mirror_changed(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    if (g_screens_syncing || g_screens_selected < 0) {
        return;
    }
    gchar *sel = gtk_combo_box_get_active_text(GTK_COMBO_BOX(g_screens_mirror_combo));
    ScreenOutput *o = &g_outputs[g_screens_selected];
    if (!sel || !strcmp(sel, "(nenhum)")) {
        o->mirror_of[0] = '\0';
    } else {
        snprintf(o->mirror_of, sizeof(o->mirror_of), "%s", sel);
    }
    g_free(sel);
    gtk_widget_queue_draw(g_screens_canvas);
}

static void on_screens_dpi_changed(GtkWidget *widget, gpointer data)
{
    (void)data;
    if (g_screens_syncing || g_screens_selected < 0) {
        return;
    }
    g_outputs[g_screens_selected].dpi = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(widget));
}

static void on_screens_scale_changed(GtkWidget *widget, gpointer data)
{
    (void)data;
    if (g_screens_syncing || g_screens_selected < 0) {
        return;
    }
    double v = gtk_spin_button_get_value(GTK_SPIN_BUTTON(widget));
    g_outputs[g_screens_selected].scale_x = v;
    g_outputs[g_screens_selected].scale_y = v;
}

/* mirror_of/dpi/scale aren't detected from plain `xrandr` (see
 * ScreenOutput's comment), so a raw detect_outputs() call would silently
 * wipe them back to blank/1.0 every time -- these snapshot them by output
 * name before re-detecting and restore them after, so Apply/Detectar
 * novamente only ever lose what actually vanished (the output itself). */
typedef struct {
    char name[NAME_LEN];
    char mirror_of[NAME_LEN];
    int dpi;
    double scale_x, scale_y;
} ScreensExtra;

static void screens_redetect_preserving_extras(void)
{
    static ScreensExtra snap[MAX_OUTPUTS];
    int n_snap = 0;
    for (int i = 0; i < g_n_outputs && n_snap < MAX_OUTPUTS; i++) {
        snprintf(snap[n_snap].name, sizeof(snap[n_snap].name), "%s", g_outputs[i].name);
        snprintf(snap[n_snap].mirror_of, sizeof(snap[n_snap].mirror_of), "%s", g_outputs[i].mirror_of);
        snap[n_snap].dpi = g_outputs[i].dpi;
        snap[n_snap].scale_x = g_outputs[i].scale_x;
        snap[n_snap].scale_y = g_outputs[i].scale_y;
        n_snap++;
    }

    g_n_outputs = detect_outputs(g_outputs, MAX_OUTPUTS);

    for (int i = 0; i < g_n_outputs; i++) {
        for (int j = 0; j < n_snap; j++) {
            if (!strcmp(snap[j].name, g_outputs[i].name)) {
                snprintf(g_outputs[i].mirror_of, sizeof(g_outputs[i].mirror_of), "%s", snap[j].mirror_of);
                g_outputs[i].dpi = snap[j].dpi;
                g_outputs[i].scale_x = snap[j].scale_x;
                g_outputs[i].scale_y = snap[j].scale_y;
                break;
            }
        }
    }
    memcpy(g_outputs_baseline, g_outputs, sizeof(g_outputs));
}

static void on_screens_apply(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    for (int i = 0; i < g_n_outputs; i++) {
        apply_output_diff(&g_outputs[i], &g_outputs_baseline[i]);
    }
    screens_redetect_preserving_extras();
    if (g_screens_selected >= g_n_outputs) {
        g_screens_selected = -1;
    }
    char status[64];
    snprintf(status, sizeof(status), "%d saida(s) detectada(s).", g_n_outputs);
    gtk_label_set_text(GTK_LABEL(g_screens_status_label), status);
    if (g_screens_selected >= 0) {
        sync_screens_form();
    }
    gtk_widget_queue_draw(g_screens_canvas);
}

static void on_screens_refresh(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    screens_redetect_preserving_extras();
    if (g_screens_selected >= g_n_outputs) {
        g_screens_selected = -1;
    }
    char status[64];
    snprintf(status, sizeof(status), "%d saida(s) detectada(s).", g_n_outputs);
    gtk_label_set_text(GTK_LABEL(g_screens_status_label), status);
    if (g_screens_selected >= 0) {
        sync_screens_form();
    }
    gtk_widget_queue_draw(g_screens_canvas);
}

static GtkWidget *build_telas_tab(void)
{
    g_n_outputs = detect_outputs(g_outputs, MAX_OUTPUTS);
    memcpy(g_outputs_baseline, g_outputs, sizeof(g_outputs));
    for (int i = 0; i < g_n_outputs; i++) {
        if (g_outputs[i].connected && g_outputs[i].enabled) {
            g_screens_selected = i;
            break;
        }
    }

    GtkWidget *outer = gtk_vbox_new(FALSE, 8);
    gtk_container_set_border_width(GTK_CONTAINER(outer), 12);

    GtkWidget *note = gtk_label_new(
        "Arraste as caixas pra reposicionar -- encaixa nas bordas de outras\n"
        "saidas automaticamente. Espelho/DPI/Escala nao sao detectados do\n"
        "hardware (xrandr sem --verbose nao expoe isso), so escritos ao\n"
        "Aplicar; propriedades avancadas do driver nao foram portadas.");
    gtk_misc_set_alignment(GTK_MISC(note), 0.0, 0.5);
    gtk_box_pack_start(GTK_BOX(outer), note, FALSE, FALSE, 0);

    g_screens_status_label = gtk_label_new("-");
    gtk_misc_set_alignment(GTK_MISC(g_screens_status_label), 0.0, 0.5);
    gtk_box_pack_start(GTK_BOX(outer), g_screens_status_label, FALSE, FALSE, 0);
    {
        char status[64];
        snprintf(status, sizeof(status), "%d saida(s) detectada(s).", g_n_outputs);
        gtk_label_set_text(GTK_LABEL(g_screens_status_label), status);
    }

    g_screens_canvas = gtk_drawing_area_new();
    gtk_widget_set_size_request(g_screens_canvas, -1, 220);
    gtk_widget_add_events(g_screens_canvas, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK);
    g_signal_connect(g_screens_canvas, "expose-event", G_CALLBACK(screens_canvas_expose), NULL);
    g_signal_connect(g_screens_canvas, "button-press-event", G_CALLBACK(screens_canvas_press), NULL);
    g_signal_connect(g_screens_canvas, "motion-notify-event", G_CALLBACK(screens_canvas_motion), NULL);
    g_signal_connect(g_screens_canvas, "button-release-event", G_CALLBACK(screens_canvas_release), NULL);
    gtk_box_pack_start(GTK_BOX(outer), frame_with("Layout (arraste pra mover)", g_screens_canvas), TRUE, TRUE, 0);

    GtkWidget *form_table = gtk_table_new(8, 2, FALSE);
    g_screens_res_combo = gtk_combo_box_new_text();
    g_signal_connect(g_screens_res_combo, "changed", G_CALLBACK(on_screens_res_changed), NULL);
    labeled_row(form_table, 0, "Resolucao:", g_screens_res_combo);
    g_screens_rate_combo = gtk_combo_box_new_text();
    g_signal_connect(g_screens_rate_combo, "changed", G_CALLBACK(on_screens_rate_changed), NULL);
    labeled_row(form_table, 1, "Taxa de atualizacao:", g_screens_rate_combo);
    g_screens_rot_combo = gtk_combo_box_new_text();
    gtk_combo_box_append_text(GTK_COMBO_BOX(g_screens_rot_combo), "normal");
    gtk_combo_box_append_text(GTK_COMBO_BOX(g_screens_rot_combo), "left");
    gtk_combo_box_append_text(GTK_COMBO_BOX(g_screens_rot_combo), "right");
    gtk_combo_box_append_text(GTK_COMBO_BOX(g_screens_rot_combo), "inverted");
    g_signal_connect(g_screens_rot_combo, "changed", G_CALLBACK(on_screens_rot_changed), NULL);
    labeled_row(form_table, 2, "Rotacao:", g_screens_rot_combo);
    g_screens_enabled_chk = gtk_check_button_new_with_label("Saida ligada");
    g_signal_connect(g_screens_enabled_chk, "toggled", G_CALLBACK(on_screens_enabled_toggled), NULL);
    gtk_table_attach(GTK_TABLE(form_table), g_screens_enabled_chk, 0, 2, 3, 4, GTK_FILL, GTK_FILL, 4, 2);
    g_screens_primary_chk = gtk_check_button_new_with_label("Saida primaria");
    g_signal_connect(g_screens_primary_chk, "toggled", G_CALLBACK(on_screens_primary_toggled), NULL);
    gtk_table_attach(GTK_TABLE(form_table), g_screens_primary_chk, 0, 2, 4, 5, GTK_FILL, GTK_FILL, 4, 2);
    g_screens_mirror_combo = gtk_combo_box_new_text();
    g_signal_connect(g_screens_mirror_combo, "changed", G_CALLBACK(on_screens_mirror_changed), NULL);
    labeled_row(form_table, 5, "Espelhar (mirror):", g_screens_mirror_combo);
    g_screens_dpi_spin = gtk_spin_button_new_with_range(48, 960, 12);
    g_signal_connect(g_screens_dpi_spin, "value-changed", G_CALLBACK(on_screens_dpi_changed), NULL);
    labeled_row(form_table, 6, "DPI:", g_screens_dpi_spin);
    g_screens_scale_spin = gtk_spin_button_new_with_range(0.25, 4.0, 0.05);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(g_screens_scale_spin), 2);
    g_signal_connect(g_screens_scale_spin, "value-changed", G_CALLBACK(on_screens_scale_changed), NULL);
    labeled_row(form_table, 7, "Escala:", g_screens_scale_spin);
    gtk_box_pack_start(GTK_BOX(outer), frame_with("Saida selecionada", form_table), FALSE, FALSE, 0);

    GtkWidget *btnbox = gtk_hbox_new(FALSE, 6);
    GtkWidget *refresh_btn = gtk_button_new_with_label("Detectar novamente");
    GtkWidget *apply_btn = gtk_button_new_with_label("Aplicar");
    g_signal_connect(refresh_btn, "clicked", G_CALLBACK(on_screens_refresh), NULL);
    g_signal_connect(apply_btn, "clicked", G_CALLBACK(on_screens_apply), NULL);
    gtk_box_pack_start(GTK_BOX(btnbox), refresh_btn, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(btnbox), apply_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), btnbox, FALSE, FALSE, 0);

    if (g_screens_selected >= 0) {
        sync_screens_form();
    }

    return outer;
}

#define KICONF_VERSION "0.1.0"

int main(int argc, char **argv)
{
    /* Checked before gtk_init() so `kiconf --version` works even without
     * a display (X connection), same as most CLI-invokable GTK tools. */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--version") || !strcmp(argv[i], "-V")) {
            printf("kiconf %s\n", KICONF_VERSION);
            return 0;
        }
    }

    gtk_init(&argc, &argv);

    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "kiconf");
    gtk_window_set_default_size(GTK_WINDOW(window), 640, 660);
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *notebook = gtk_notebook_new();
    /* Stacked on the left instead of GTK's top-tab default -- with 8 tabs
     * the top row was starting to wrap/crowd at the window's default
     * width; a left column scales to more tabs without eating vertical
     * space from the (often taller) tab content below it. */
    gtk_notebook_set_tab_pos(GTK_NOTEBOOK(notebook), GTK_POS_LEFT);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), build_appearance_tab(), gtk_label_new("Aparencia"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), build_shortcuts_tab(), gtk_label_new("Atalhos"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), build_telas_tab(), gtk_label_new("Telas"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), build_entrada_tab(), gtk_label_new("Entrada"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), build_wallpaper_tab(), gtk_label_new("Wallpaper"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), build_outras_tab(), gtk_label_new("Outras"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), build_paineis_tab(), gtk_label_new("Paineis"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), build_permissoes_tab(), gtk_label_new("Permissoes"));

    gtk_container_add(GTK_CONTAINER(window), notebook);
    gtk_widget_show_all(window);
    gtk_main();
    return 0;
}
