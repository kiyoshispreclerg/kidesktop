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

#endif
