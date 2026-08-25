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

#endif
