/*
 * xisserve_page.h - everything a page needs from xisserve, and the one
 * thing an installable page module (<flag>.so) must export.
 *
 * Self-contained (GTK2 only, no KiDesktop-internal includes) so it can
 * be installed and built against outside this tree. The built-in pages
 * (pages/<name>.c) use the same functions through xisserve.h, which includes
 * this; a module calls them the same way, resolved against the running
 * xisserve binary (see pages-api.sym -- only what is declared here is
 * exported to modules).
 *
 * A module is one shared object named after its mode flag, e.g.
 * kiremote.so for `xisserve --kiremote`, in one of the directories
 * PROTOCOL.md's "Page modules" section lists. xisserve only lists those
 * directories at startup; the .so is dlopen()ed the first time its page
 * is actually opened, so an installed module that is never used costs
 * nothing. Build it with -fPIC -shared against gtk+-2.0, and export:
 *
 *     const XisservePageModule *xisserve_page_entry(void);
 *
 * returning a static descriptor whose abi is XISSERVE_PAGE_ABI. A module
 * built against another ABI is refused (the page shows why instead).
 * Bump XISSERVE_PAGE_ABI on any incompatible change to this header.
 */
#ifndef XISSERVE_PAGE_H
#define XISSERVE_PAGE_H

#include <gtk/gtk.h>

#define XISSERVE_PAGE_ABI 1
#define XISSERVE_PAGE_ENTRY "xisserve_page_entry"

typedef struct {
    int abi;                    /* XISSERVE_PAGE_ABI */
    const char *title;          /* header-strip label, untranslated msgid */
    const char *gettext_domain; /* catalog `title` is translated with; NULL shows it as-is */
    int min_width;              /* forced minimum window size while this page is up... */
    int min_height;             /* ...or 0/0 for the page's own natural size */
    GtkWidget *(*build)(void);  /* once, on first open; returns the page's root widget */
    void (*on_show)(void);      /* each time the page becomes visible; may be NULL */
    void (*on_hide)(void);      /* each time it stops being visible; may be NULL */
} XisservePageModule;

typedef const XisservePageModule *(*XisservePageEntryFn)(void);

/* Accessible name/description for a screen reader, for widgets whose
 * visible text alone doesn't say what they are (an entry with no label,
 * an icon-only or "×" button, a slider under a section header, several
 * identical "Ativar" buttons in a list). Either may be NULL. */
static inline void xisserve_a11y(GtkWidget *w, const char *name, const char *desc)
{
    AtkObject *acc = gtk_widget_get_accessible(w);
    if (name) atk_object_set_name(acc, name);
    if (desc) atk_object_set_description(acc, desc);
}

/* Launches `cmd` as an independent user application: double-forked,
 * setsid()'d, never waited on, and -- on a systemd session -- inside its
 * own transient scope, so the launched app isn't a member of xisserve's
 * (i.e. xispanel's) cgroup and doesn't die with it. One shared
 * implementation, shared/xis_spawn.h, which has the full reasoning;
 * exported here so plugins that launch something don't duplicate it. */
void run_detached(const char *cmd);

/* Single-quotes `in` for safe use inside an `sh -c` command string. */
void shell_quote(const char *in, char *out, size_t outsz);

/* A page's own widget (GtkComboBox's dropdown, most commonly) is about
 * to take the X grab away from g_window, the same way the launcher's
 * own right-click context menu already does -- see xisserve.c's
 * on_window_grab_broken(). Call _begin() right before showing that
 * widget's popup and _end() once it closes (e.g. a GtkComboBox's
 * "notify::popup-shown" going FALSE), so on_window_grab_broken() knows
 * this is our own popup stealing the grab, not something to hide the
 * window over, and _end() reclaims the grab afterwards. Nest freely
 * (paired begin/end calls only, not meant to overlap two different
 * popups at once). */
void xisserve_transient_popup_begin(void);
void xisserve_transient_popup_end(void);

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

/* For pages that act on another window (--clipboard): the invocation's
 * target window (--for-window=<id>, else whatever was active when
 * xisserve was called, before the popup took focus; 0 if none), whether
 * the page was asked to open filtered to it (--for-active/--for-window),
 * and closing the popup the same way Escape does. */
unsigned long xisserve_target_window(void);
gboolean xisserve_target_filter(void);
/* --filter=: "all"/"app"/"window"/"doc"/"auto", or "" when not given. */
const char *xisserve_target_scope(void);
void xisserve_hide(void);

#endif
