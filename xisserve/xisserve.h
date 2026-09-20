/*
 * xisserve.h - what xisserve.c exports for the plugins/ sources to build on: the
 * ResultEntry shape a search result is, the SearchPluginFn signature a
 * plugin's own entry point matches, and the handful of helpers (process
 * spawning, shell quoting, terminal launching, icon resolution) plugins
 * would otherwise have to duplicate. Adding a new plugin is: write
 * plugins/<name>.c defining one function matching SearchPluginFn,
 * declare it below, add one line to xisserve.c's kSearchPlugins table,
 * and one line to the Makefile's SRCS -- nothing else in the search path
 * needs to change.
 */
#ifndef XISSERVE_H
#define XISSERVE_H

#include <gtk/gtk.h>
#include <X11/Xlib.h>

/* Every result row's icon (app icon, or a plugin's own) is resolved to
 * this fixed square size -- see xisserve_resolve_icon(). */
#define XISSERVE_ICON_PX 22

typedef struct ResultEntry ResultEntry;

/* Called instead of run_detached(exec) when a result is launched
 * (click, Enter) -- lets a plugin do something other than "run a shell
 * command" (see plugins/globalmenu.c, which sends a DBusMenu Event
 * instead). Leave both NULL on a ResultEntry to fall back to the
 * default exec-based launch. */
typedef void (*ResultActivateFn)(ResultEntry *self);

/* One search result, real (from_desktop=TRUE, backed by a scanned
 * .desktop file, persists across searches) or plugin-synthetic
 * (from_desktop=FALSE, lives only for one search -- see
 * plugins/terminal.c, plugins/globalmenu.c). */
struct ResultEntry {
    char id[160];           /* .desktop basename ("firefox.desktop"); "" for plugin results */
    char name[256];
    char exec[1300];        /* shell command; used by the default activate when activate_fn is NULL */
    char category_key[32];  /* bucket key, e.g. "Development"; "" for plugin results */
    char subtitle[128];     /* small text shown under the name: category label, or the plugin's name */
    gboolean is_favorite;
    gboolean from_desktop;
    GdkPixbuf *icon;        /* owned; NULL = no icon drawn for this row */
    char icon_spec[256];    /* raw Icon= value icon was resolved from, "" if none; kept around
                              * so the on-disk apps cache can persist it (a pixbuf itself can't) --
                              * see xisserve.c's apps-cache section */

    ResultActivateFn activate_fn;         /* optional, see above */
    gpointer activate_data;               /* opaque payload for activate_fn */
    void (*activate_data_free)(gpointer); /* optional destructor for activate_data, called by result_entry_free() */
};

/* Frees an entry's icon (unref) and activate_data (via its destructor,
 * if set) before freeing the struct itself -- the only correct way to
 * free a ResultEntry, since a plain g_free() would leak both. */
void result_entry_free(ResultEntry *e);

/* A plugin gets the current (non-empty) query text and appends whatever
 * freshly g_new0'd ResultEntry* it wants to `results` (ownership passes
 * to the caller, xisserve.c's rebuild_results()). Called on every
 * keystroke while searching -- keep it fast, or at least bounded (see
 * plugins/globalmenu.c's synchronous-but-200ms-capped DBus call). */
typedef void (*SearchPluginFn)(const char *query, GPtrArray *results);

void plugin_terminal_search(const char *query, GPtrArray *results);

/* One entry of $XDG_DATA_HOME/recently-used.xbel (the XDG "recent
 * files" list every GTK/Qt app already reads and writes) -- see
 * xisserve.c's "recently-used.xbel" section for the GMarkup parser
 * behind xisserve_load_recent_xbel() below. */
typedef struct {
    char path[4096];     /* PATH_MAX, spelled out since limits.h isn't pulled in here */
    char modified[32];   /* raw ISO-8601 modified= timestamp; sorts correctly as a plain string */
    GPtrArray *apps;      /* g_strdup'd bookmark:application name= values that opened this item */
} RecentXbelItem;

/* Parses recently-used.xbel into a fresh GPtrArray of RecentXbelItem*,
 * newest-modified first, skipping entries whose file no longer exists.
 * Caller owns the array: g_ptr_array_free(arr, TRUE) frees every item
 * too. Never NULL, empty if the file is missing or unparseable. Used by
 * xisserve.c's own "recent files opened with this app" context-menu
 * entries and by plugins/recent.c's search plugin. */
GPtrArray *xisserve_load_recent_xbel(void);

/* Search plugin over the same recent-files list, filename/path
 * substring match -- see plugins/recent.c. */
void plugin_recent_search(const char *query, GPtrArray *results);

/* Search plugin matching the query against the current user's own
 * running processes, offering terminate/kill/kill-tree results -- see
 * plugins/process.c. */
void plugin_process_search(const char *query, GPtrArray *results);

/* Search plugin: if the whole query parses as a basic math expression,
 * offers one result showing its value -- see plugins/calculator.c. */
void plugin_calculator_search(const char *query, GPtrArray *results);

/* The com.canonical.dbusmenu busname+object path a window exports
 * (_KDE_NET_WM_APPMENU_SERVICE_NAME/_OBJECT_PATH, what every Qt/KF5 app
 * sets), or FALSE when it exports none. `window` of 0 means the current
 * _NET_ACTIVE_WINDOW. Implemented in plugins/globalmenu.c, which already
 * owns those atoms; used by the search plugin there and by appmenu.c's
 * --menu mode. */
gboolean xisserve_window_appmenu(unsigned long window, char *busname, size_t bn_sz,
                                 char *objpath, size_t op_sz);

/* --menu mode: pops up `window`'s exported application menu as a real
 * cascading GTK menu at root coordinates (x, y) and runs its own
 * gtk_main() until the menu is dismissed or an item is activated (which
 * is sent back to the app as a DBusMenu Event). `window` of 0 falls back
 * to the active window. Returns the process exit code.
 *
 * Deliberately outside the launcher's singleton/control-socket
 * machinery: this is a one-shot popup, launched per click by whatever
 * drew the button (kiwm's appmenu titlebar element, via kiwm.conf's
 * appmenu_command=), and it must not disturb a running launcher's own
 * window. */
int appmenu_run(unsigned long window, int x, int y);
void plugin_globalmenu_search(const char *query, GPtrArray *results);

/* --question mode: a Zenity-style question popup, entirely outside the
 * launcher's singleton/control-socket machinery (like --menu above, so
 * any number of instances can be up at once). `argv`/`argc` are handed
 * over as-is -- question.c does its own getopt_long pass for its own
 * --text/--button flags, independent of parse_argv()'s LaunchArgs.
 * Returns the process exit code: the chosen button's value (low byte),
 * 1 if the dialog was dismissed with no button clicked, or 2 on a usage
 * error (missing --text or no --button). See PROTOCOL.md. */
int question_run(int argc, char **argv);

/* --applications: a one-shot cascading menu of every installed app,
 * grouped by freedesktop category, at root coordinates (x, y) -- like
 * --menu above but listing every scanned .desktop entry instead of one
 * window's own menu. Meant as an xisback click-action command (see
 * xisback's XISBACK_CLICK_X/XISBACK_CLICK_Y), so right-clicking bare
 * desktop space can pop an "Applications" list the way Plasma/kickoff-
 * style shells do. Same "no singleton, no control socket, own
 * gtk_main()" reasoning as --menu/--question. Returns the process exit
 * code. See applications.c and PROTOCOL.md. */
int applications_run(int x, int y);

/* --keyboard: a mouse-driven on-screen QWERTY keyboard docked to the
 * bottom of the (output_x, output_y, output_w, output_h) rectangle --
 * output_w/output_h <= 0 falls back to the default screen's size. Own
 * "run again to toggle it off" singleton, separate from the launcher's
 * (see keyboard.c's file comment for why); still no LaunchArgs-page
 * relation to the launcher window itself. Returns the process exit
 * code. See keyboard.c and PROTOCOL.md. */
int keyboard_run(int output_x, int output_y, int output_w, int output_h);

/* Resolves the RandR "Monitor" rectangle (xrandr --listmonitors) that
 * currently matters most: the one containing the focused window's
 * center, falling back to wherever the pointer is if there's no usable
 * _NET_ACTIVE_WINDOW. Returns FALSE if RandR is unavailable or neither
 * lookup resolves (caller should fall back to the full screen then).
 * Implemented in keyboard.c, which made this same lookup first for
 * --keyboard's own docking; exported so session.c's --session picker can
 * center on the same monitor without a second copy of it. */
gboolean xisserve_resolve_active_output(Display *dpy, Window root, int *ox, int *oy, int *ow, int *oh);

/* --session: a centered, always-on-top "what do you want to do with this
 * session" picker -- Desligar/Reiniciar/Suspender/Sair/Trocar
 * usuario/Bloquear tela, the same actions and confirmation dialogs the
 * launcher's own footer buttons use (see xisserve_n_power_actions() et
 * al. below), reused here so there is exactly one place each command
 * lives. Escape, a "Cancelar"/"X" button, or the WM close button dismiss
 * it with no action taken. Like --menu/--question, this stays outside
 * the launcher's singleton/control-socket machinery: it's a one-shot
 * popup, not part of the xispanel contract. Returns the process exit
 * code. See session.c and PROTOCOL.md. */
int session_run(void);

/* The launcher's own power-action table (Desligar/Reiniciar/Suspender/
 * Sair/Trocar usuario/Bloquear tela) -- shared with session.c's
 * --session picker so both draw the same buttons from the same source
 * rather than keeping two copies of the command list in sync by hand.
 * `i` ranges over [0, xisserve_n_power_actions()); an action whose
 * required binary (per its probe) isn't installed is still counted but
 * xisserve_power_action_visible() reports it as not shown, matching how
 * the launcher's own footer already skips it. xisserve_power_action_run()
 * shows the same Yes/No confirmation dialog (parented on `parent`, which
 * may be NULL) and, on Yes, runs the action's command via run_detached()
 * -- it does not hide/close `parent` itself, that's each caller's own
 * job (the launcher hides itself first, session.c exits after). */
int xisserve_n_power_actions(void);
gboolean xisserve_power_action_visible(int i);
const char *xisserve_power_action_label(int i);
/* Returns TRUE if the user answered Yes and a command actually ran (FALSE
 * for No/dismissed, or for a stub action with no command of its own e.g.
 * "Sair" today) -- session.c uses this to know whether to close itself. */
gboolean xisserve_power_action_run(int i, GtkWidget *parent);

/* Fresh .desktop scan across $XDG_DATA_DIRS + ~/.local/share/applications,
 * independent of the launcher's own persistent app list -- used by
 * applications.c, a one-shot process that never touches the launcher's
 * state (favorites included: is_favorite is always FALSE on these
 * entries). Caller owns the returned array: result_entry_free() each
 * element, then g_ptr_array_free() the array itself. Sorted by name;
 * category_key is bucketed the same way the launcher's own scan does. */
GPtrArray *xisserve_scan_apps(void);

/* Sidebar/menu label for a category_key as bucketed by
 * xisserve_scan_apps() (e.g. "Development" -> "Desenvolvimento"), or
 * "Outros" for an unrecognized key. Never returns NULL. */
const char *xisserve_category_label(const char *key);

/* Same fork+setsid+execl-via-sh-c "don't wait" pattern xispanel.c's own
 * run_detached() uses -- defined once in xisserve.c, exported so plugins
 * that launch something themselves don't duplicate it. */
void run_detached(const char *cmd);

/* Single-quotes `in` for safe use inside an `sh -c` command string. */
void shell_quote(const char *in, char *out, size_t outsz);

/* Wraps `cmd` in the session's terminal-exec fallback chain (xdg-
 * terminal-exec, then $TERMINAL, then x-terminal-emulator, then xterm),
 * routed through `sh -c` so a multi-word cmd is split by the shell
 * rather than handed to the terminal's `-e` as one literal argv word. */
void build_terminal_exec(const char *cmd, char *out, size_t outsz);

/* Icon resolution shared by app icons (.desktop Icon=) and any plugin
 * that wants one (e.g. globalmenu's per-item icon-name): `spec` starting
 * with '/' is loaded as a path to an image file, anything else is
 * looked up as a themed icon name via GtkIconTheme. Cached process-wide
 * by spec (including failed lookups, so a plugin can call this on every
 * keystroke without worrying about repeat theme lookups). Returns a
 * reference the caller owns (g_object_unref when done), or NULL if
 * `spec` is empty or nothing resolves it -- never a broken-image
 * placeholder. */
GdkPixbuf *xisserve_resolve_icon(const char *spec, int size);

/* The launcher's current --fg color (already parsed from argv/the
 * control socket), 0..1 components -- for a plugin's own generated
 * fallback icon (e.g. globalmenu's hamburger) to stay legible against
 * whatever theme is active instead of a fixed color. */
void xisserve_get_fg_rgba(double *r, double *g, double *b, double *a);
void xisserve_get_bg_rgba(double *r, double *g, double *b, double *a);

/* One integer setting from xisserve.conf, whose lines are all
 * "<SECTION>\t<key>\t<value>" (the same tab-delimited shape the PLUGIN
 * lines already use, and that xisback.conf uses for its own). Returns
 * `fallback` when the file, the line, or a parseable number is missing,
 * so every caller has a working default and no config file is ever
 * required. Reloaded on each open, so an edit takes effect on the next
 * toggle without restarting the daemon. */
int xisserve_config_get_int(const char *section, const char *key, int fallback);

/* ---- pages -------------------------------------------------------------
 *
 * A "page" is a whole-window alternative to the default launcher view,
 * selected by its own mode flag on the command line (PROTOCOL.md's
 * "--calendar", "--audio", ...). xisserve.c owns one root widget per
 * page, packed into the same box as the launcher's own widgets and
 * shown/hidden by apply_view_mode(); a page never touches the window,
 * the grab, positioning, or the singleton/toggle machinery.
 *
 * Adding a page is: write pages/<name>.c exposing the three functions
 * below, declare them here, add one row to xisserve.c's kPages table,
 * and one line to the Makefile's SRCS. The flag name, window sizing,
 * and everything else comes from that table row.
 */
typedef struct {
    const char *flag;      /* long-option name, e.g. "audio" -- also the JSON/control-socket value */
    const char *title;     /* header-strip label while this page is showing, e.g. "\xc3\x81udio" */
    int min_width;         /* forced minimum window size while this page is up... */
    int min_height;        /* ...or 0/0 to shrink the window to the page's own natural size */
    GtkWidget *(*build)(void); /* called once at startup; returns the page's root widget */
    void (*on_show)(void);     /* each time the page becomes visible (refresh live data here) */
    void (*on_hide)(void);     /* each time it stops being visible (stop timers here); may be NULL */
} XisservePage;

GtkWidget *page_calendar_build(void);
void page_calendar_on_show(void);

GtkWidget *page_audio_build(void);
void page_audio_on_show(void);
void page_audio_on_hide(void);

GtkWidget *page_energy_build(void);
void page_energy_on_show(void);
void page_energy_on_hide(void);

GtkWidget *page_notifications_build(void);
void page_notifications_on_show(void);
void page_notifications_on_hide(void);

#endif
