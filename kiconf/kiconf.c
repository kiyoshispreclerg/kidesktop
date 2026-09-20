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
 *     "button-release-event" on the same widget cover dragging -- unlike
 *     xisconf.py's free-floating canvas, outputs always dock flush
 *     against their nearest neighbor while dragging (screens_dock()), so
 *     the layout stays gap-free and the fit-to-canvas zoom, frozen for
 *     the drag's duration, never jumps around. Aplicar also persists the
 *     resulting layout to kiconfd-screens.conf for kiconfd to replay via
 *     xrandr at the next session's start (save_screens_layout()), since
 *     plain xrandr state doesn't survive a logout/login on its own.
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

#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "i18n.h"
#include "tabs.h"

#define KICONF_VERSION "0.2.17"

/* ---- lazy tab construction ---------------------------------------------
 * Each build_X_tab() was cheap at first, but several now do real I/O the
 * moment they're built -- build_permissoes_tab() connects to xisguard's
 * control socket, build_paineis_tab()/build_wallpaper_tab() talk to
 * xispanel/xisback the same way, build_telas_tab() shells out to `xrandr`
 * -- so building all 8 up front made every kiconf launch pay for tabs the
 * user may never open, including blocking on daemons that aren't running.
 * Instead, each notebook page starts as an empty placeholder box, and the
 * real build_X_tab() call happens on first visit (GtkNotebook's
 * "switch-page").
 *
 * Only ever one tab's content is kept built at a time: GtkNotebook sizes
 * itself to fit the largest of *all* its pages, built or not, not just the
 * current one, so once a wide tab (e.g. Telas) had been visited, the
 * window could never be resized narrower than it again even after
 * switching to a smaller tab. Tearing the previous tab's content down on
 * every switch (back to an empty placeholder) keeps the notebook's size
 * request bounded by whatever single tab is actually showing. The cost is
 * that switching away and back re-does that tab's build_X_tab() (and its
 * I/O) and discards any unapplied edits in it -- same tradeoff control
 * panels like GNOME Settings make with their per-module pages. */
typedef GtkWidget *(*TabBuilder)(void);

typedef struct {
    const char *label;
    const char *stock_icon;
    TabBuilder build;
    GtkWidget *placeholder;
    int built;
    int dirty;
} LazyTab;

static LazyTab g_tabs[] = {
    {N_("Aparencia"), GTK_STOCK_SELECT_COLOR, build_appearance_tab, NULL, 0, 0},
    {N_("Atalhos"), GTK_STOCK_JUMP_TO, build_shortcuts_tab, NULL, 0, 0},
    {N_("Telas"), GTK_STOCK_FULLSCREEN, build_telas_tab, NULL, 0, 0},
    {N_("Entrada"), GTK_STOCK_EDIT, build_entrada_tab, NULL, 0, 0},
    {N_("Wallpaper"), GTK_STOCK_FILE, build_wallpaper_tab, NULL, 0, 0},
    {N_("Outras"), GTK_STOCK_PREFERENCES, build_outras_tab, NULL, 0, 0},
    {N_("Paineis"), GTK_STOCK_JUSTIFY_FILL, build_paineis_tab, NULL, 0, 0},
    {N_("Permissoes"), GTK_STOCK_DIALOG_AUTHENTICATION, build_permissoes_tab, NULL, 0, 0},
    {N_("Gerenciamento de janelas"), GTK_STOCK_DND_MULTIPLE, build_janelas_tab, NULL, 0, 0},
    {N_("Efeitos do compositor"), GTK_STOCK_CONVERT, build_efeitos_tab, NULL, 0, 0},
    {N_("Sistema"), GTK_STOCK_HARDDISK, build_sistema_tab, NULL, 0, 0},
    {N_("Energia"), GTK_STOCK_QUIT, build_energia_tab, NULL, 0, 0},
    {N_("Programas padrao"), GTK_STOCK_EXECUTE, build_programas_tab, NULL, 0, 0},
    {N_("Iniciar automaticamente"), GTK_STOCK_MEDIA_PLAY, build_autostart_tab, NULL, 0, 0},
};
#define N_TABS ((int)(sizeof(g_tabs) / sizeof(g_tabs[0])))

/* Page 0 is the icon-grid home page (see build_home_page()); module i
 * (0-based, into g_tabs[]) lives at notebook page i + 1. */
#define HOME_PAGE 0

static GtkWidget *g_notebook;
static GtkWidget *g_window;
static int g_current_tab = -1;

static int confirm_leave_current_tab(void);

static void on_back_clicked(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    if (!confirm_leave_current_tab()) {
        return;
    }
    gtk_notebook_set_current_page(GTK_NOTEBOOK(g_notebook), HOME_PAGE);
}

/* One row, left-aligned, added above every module's own content -- kept
 * here rather than in each tabs/ file so all 8 modules get it the same
 * way and none of them need to know they're being shown inside a
 * "navigate back to home" scheme at all (same reasoning as the tab-strip
 * navigation itself: one shared mechanism, not a per-tab opt-in). */
static GtkWidget *make_back_button(void)
{
    GtkWidget *btn = gtk_button_new_from_stock(GTK_STOCK_GO_BACK);
    g_signal_connect(btn, "clicked", G_CALLBACK(on_back_clicked), NULL);
    GtkWidget *row = gtk_hbox_new(FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), btn, FALSE, FALSE, 0);
    return row;
}

/* ---- pending-changes tracking -------------------------------------------
 * One shared mechanism for all 14 tabs rather than a per-tab dirty flag
 * each build_X_tab() has to remember to set/clear by hand: after a tab is
 * built, connect_dirty_tracking() walks its whole widget tree and hooks
 * the "value changed" signal of every editable widget type used anywhere
 * in tabs/ (entries, spin buttons, combo/color/font/file buttons, text
 * views, and tree view models -- which also covers list-based tabs'
 * Adicionar/Remover buttons, since those mutate the model directly) to
 * mark that tab dirty. Buttons whose label starts with "Aplicar"/"Salvar"
 * -- the actual persist-to-disk action in every tab, see the grep of
 * gtk_button_new_with_label() calls across tabs/ -- clear it again once
 * their own "clicked" handler (connected first, inside build_X_tab()) has
 * run. Connecting happens after build() returns so a tab's *initial*
 * population (loading current config into its widgets) never itself
 * counts as a pending edit. */
static void mark_dirty_cb(GtkWidget *w, gpointer data)
{
    (void)w;
    int idx = GPOINTER_TO_INT(data);
    if (idx >= 0 && idx < N_TABS) {
        g_tabs[idx].dirty = 1;
    }
}

static void clear_dirty_cb(GtkWidget *w, gpointer data)
{
    (void)w;
    int idx = GPOINTER_TO_INT(data);
    if (idx >= 0 && idx < N_TABS) {
        g_tabs[idx].dirty = 0;
    }
}

static int is_commit_label(const char *label)
{
    return label && (!strncmp(label, "Aplicar", 7) || !strncmp(label, "Salvar", 6));
}

static void connect_dirty_tracking(GtkWidget *w, int idx)
{
    gpointer d = GINT_TO_POINTER(idx);

    if (GTK_IS_SPIN_BUTTON(w)) {
        g_signal_connect(w, "value-changed", G_CALLBACK(mark_dirty_cb), d);
    } else if (GTK_IS_ENTRY(w)) {
        g_signal_connect(w, "changed", G_CALLBACK(mark_dirty_cb), d);
    } else if (GTK_IS_TOGGLE_BUTTON(w)) {
        g_signal_connect(w, "toggled", G_CALLBACK(mark_dirty_cb), d);
    } else if (GTK_IS_COMBO_BOX(w)) {
        g_signal_connect(w, "changed", G_CALLBACK(mark_dirty_cb), d);
    } else if (GTK_IS_COLOR_BUTTON(w)) {
        g_signal_connect(w, "color-set", G_CALLBACK(mark_dirty_cb), d);
    } else if (GTK_IS_FONT_BUTTON(w)) {
        g_signal_connect(w, "font-set", G_CALLBACK(mark_dirty_cb), d);
    } else if (GTK_IS_FILE_CHOOSER_BUTTON(w)) {
        g_signal_connect(w, "file-set", G_CALLBACK(mark_dirty_cb), d);
    } else if (GTK_IS_TEXT_VIEW(w)) {
        GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(w));
        g_signal_connect(buf, "changed", G_CALLBACK(mark_dirty_cb), d);
    } else if (GTK_IS_TREE_VIEW(w)) {
        GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(w));
        if (model) {
            g_signal_connect(model, "row-changed", G_CALLBACK(mark_dirty_cb), d);
            g_signal_connect(model, "row-inserted", G_CALLBACK(mark_dirty_cb), d);
            g_signal_connect(model, "row-deleted", G_CALLBACK(mark_dirty_cb), d);
        }
    }

    if (GTK_IS_BUTTON(w) && !GTK_IS_TOGGLE_BUTTON(w) && is_commit_label(gtk_button_get_label(GTK_BUTTON(w)))) {
        g_signal_connect_after(w, "clicked", G_CALLBACK(clear_dirty_cb), d);
    }

    if (GTK_IS_CONTAINER(w)) {
        GList *children = gtk_container_get_children(GTK_CONTAINER(w));
        for (GList *l = children; l; l = l->next) {
            connect_dirty_tracking(GTK_WIDGET(l->data), idx);
        }
        g_list_free(children);
    }
}

/* Asks the user to confirm discarding g_current_tab's pending edits, if
 * any. Returns 1 if it's fine to leave the tab now (nothing pending, or
 * the user confirmed), 0 if the caller must not proceed (stay put). */
static int confirm_leave_current_tab(void)
{
    if (g_current_tab < 0 || !g_tabs[g_current_tab].dirty) {
        return 1;
    }
    GtkWidget *dlg = gtk_message_dialog_new(GTK_WINDOW(g_window), GTK_DIALOG_MODAL,
                                             GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO, "%s",
                                             _("Ha alteracoes nao aplicadas nesta aba. "
                                               "Tem certeza que deseja sair sem aplica-las?"));
    gtk_window_set_title(GTK_WINDOW(dlg), _("Alteracoes pendentes"));
    int response = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    return response == GTK_RESPONSE_YES;
}

/* "delete-event" fires when the user tries to close the window (titlebar X,
 * Alt+F4, ...) before GTK's default handler destroys it -- returning TRUE
 * here stops that default handler, keeping the window open. */
static gboolean on_window_delete(GtkWidget *widget, GdkEvent *event, gpointer data)
{
    (void)widget;
    (void)event;
    (void)data;
    return confirm_leave_current_tab() ? FALSE : TRUE;
}

static void ensure_tab_built(int idx)
{
    if (idx < 0 || idx >= N_TABS || g_tabs[idx].built) {
        return;
    }
    GtkWidget *content = g_tabs[idx].build();
    gtk_box_pack_start(GTK_BOX(g_tabs[idx].placeholder), make_back_button(), FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(g_tabs[idx].placeholder), content, TRUE, TRUE, 0);
    gtk_widget_show_all(g_tabs[idx].placeholder);
    g_tabs[idx].built = 1;
    g_tabs[idx].dirty = 0;
    connect_dirty_tracking(g_tabs[idx].placeholder, idx);
}

/* Destroys idx's content widget (recursively, along with everything it
 * owns -- combobox models, list stores, signal handlers), leaving its
 * placeholder empty again. Safe to call whether or not the tab is built. */
static void unbuild_tab(int idx)
{
    if (idx < 0 || idx >= N_TABS || !g_tabs[idx].built) {
        return;
    }
    GList *children = gtk_container_get_children(GTK_CONTAINER(g_tabs[idx].placeholder));
    for (GList *l = children; l; l = l->next) {
        gtk_widget_destroy(GTK_WIDGET(l->data));
    }
    g_list_free(children);
    g_tabs[idx].built = 0;
    g_tabs[idx].dirty = 0;
}

static void on_switch_page(GtkNotebook *notebook, GtkNotebookPage *page, guint page_num, gpointer data)
{
    (void)notebook;
    (void)page;
    (void)data;
    if (g_current_tab >= 0) {
        unbuild_tab(g_current_tab);
        g_current_tab = -1;
    }
    if ((int)page_num != HOME_PAGE) {
        int idx = (int)page_num - 1;
        ensure_tab_built(idx);
        g_current_tab = idx;
    }
}

/* ---- home page: a small "control panel" of module icons --------------- */

static void on_module_icon_clicked(GtkWidget *widget, gpointer data)
{
    (void)widget;
    if (!confirm_leave_current_tab()) {
        return;
    }
    gtk_notebook_set_current_page(GTK_NOTEBOOK(g_notebook), GPOINTER_TO_INT(data));
}

static GtkWidget *make_module_button(const LazyTab *tab, int page_num)
{
    GtkWidget *btn = gtk_button_new();
    gtk_container_set_border_width(GTK_CONTAINER(btn), 6);

    GtkWidget *box = gtk_vbox_new(FALSE, 4);
    GtkWidget *icon = gtk_image_new_from_stock(tab->stock_icon, GTK_ICON_SIZE_DIALOG);
    GtkWidget *label = gtk_label_new(_(tab->label));
    gtk_box_pack_start(GTK_BOX(box), icon, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(btn), box);

    g_signal_connect(btn, "clicked", G_CALLBACK(on_module_icon_clicked), GINT_TO_POINTER(page_num));
    return btn;
}

/* Plain icon grid, 4 per row -- same idea as GNOME Settings/Windows'
 * Control Panel "home": no state of its own, so unlike the module tabs it
 * stays built for the whole session instead of going through
 * ensure_tab_built()/unbuild_tab(). */
static GtkWidget *build_home_page(void)
{
    const int cols = 4;
    const int rows = (N_TABS + cols - 1) / cols;
    GtkWidget *grid = gtk_table_new(rows, cols, TRUE);
    gtk_table_set_row_spacings(GTK_TABLE(grid), 8);
    gtk_table_set_col_spacings(GTK_TABLE(grid), 8);
    for (int i = 0; i < N_TABS; i++) {
        int r = i / cols, c = i % cols;
        GtkWidget *btn = make_module_button(&g_tabs[i], i + 1);
        gtk_table_attach(GTK_TABLE(grid), btn, c, c + 1, r, r + 1,
                          GTK_EXPAND | GTK_FILL, GTK_EXPAND | GTK_FILL, 0, 0);
    }
    GtkWidget *outer = gtk_vbox_new(FALSE, 0);
    gtk_container_set_border_width(GTK_CONTAINER(outer), 16);
    gtk_box_pack_start(GTK_BOX(outer), grid, TRUE, TRUE, 0);
    return outer;
}

/* ---- --tab=NAME argument resolution ------------------------------------
 * Matches against g_tabs[].label itself (the untranslated, plain-ASCII
 * strings, e.g. "Atalhos", "Wallpaper") rather than the gettext()'d text
 * shown on screen, so a --tab argument works the same regardless of the
 * user's locale -- same reasoning as process_running()/signal_daemon()
 * matching on a fixed process name rather than anything localized. Also
 * accepts a plain 1-based index, matching the order --list-tabs prints. */
static int resolve_tab_index(const char *arg)
{
    if (!arg || !*arg) {
        return -1;
    }
    char *end;
    long n = strtol(arg, &end, 10);
    if (end != arg && *end == '\0') {
        return (n >= 1 && n <= N_TABS) ? (int)(n - 1) : -1;
    }
    for (int i = 0; i < N_TABS; i++) {
        if (!strcasecmp(g_tabs[i].label, arg)) {
            return i;
        }
    }
    /* Unambiguous case-insensitive prefix, e.g. "wall" -> "Wallpaper". */
    int found = -1;
    size_t len = strlen(arg);
    for (int i = 0; i < N_TABS; i++) {
        if (!strncasecmp(g_tabs[i].label, arg, len)) {
            if (found >= 0) {
                return -1;
            }
            found = i;
        }
    }
    return found;
}

static void list_tabs(void)
{
    for (int i = 0; i < N_TABS; i++) {
        printf("%2d  %s\n", i + 1, g_tabs[i].label);
    }
}

/* ---- single instance: flock'd lock file + a tiny Unix control socket --
 * Mirrors xisback.c's own main()'s pattern exactly (lock file decides
 * daemon-vs-client, same rundir/fallback): whichever kiconf process
 * grabs the flock first keeps running as the one GUI instance and listens
 * on kiconf-ctl.sock for the rest; every later invocation just connects
 * to it, sends what it was asked to do (focus, or focus-and-switch-tab),
 * and exits -- no window of its own, no "pending changes" question to
 * ask, since it never builds any UI. */
static void lock_and_sock_paths(char *lockpath, char *sockpath, size_t sz)
{
    const char *rundir = getenv("XDG_RUNTIME_DIR");
    if (!rundir || !*rundir) {
        rundir = "/tmp";
    }
    snprintf(lockpath, sz, "%s/kiconf.lock", rundir);
    snprintf(sockpath, sz, "%s/kiconf-ctl.sock", rundir);
}

/* Tries to hand `tab_idx` (-1 for "just focus") off to an already-running
 * kiconf instance. Returns 1 if one was found and notified (caller should
 * exit without touching GTK at all), 0 if this process should become the
 * running instance itself. */
static int notify_running_instance(const char *sockpath, int tab_idx)
{
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
    char line[64];
    if (tab_idx >= 0) {
        snprintf(line, sizeof(line), "TAB %d\n", tab_idx);
    } else {
        snprintf(line, sizeof(line), "FOCUS\n");
    }
    if (write(fd, line, strlen(line)) < 0) {
        /* Fall through anyway -- the other instance is still alive and
         * holding the lock, so this process must not also become GUI. */
    }
    close(fd);
    return 1;
}

static int g_listenfd = -1;
static char g_sockpath[PATH_MAX];

/* Applies one line read from the control socket ("FOCUS" or "TAB <n>") to
 * the already-running instance's window/notebook. */
static void apply_ctl_line(char *line)
{
    char *nl = strchr(line, '\n');
    if (nl) {
        *nl = '\0';
    }
    if (!strncmp(line, "TAB ", 4)) {
        int idx = atoi(line + 4);
        if (idx >= 0 && idx < N_TABS && confirm_leave_current_tab()) {
            gtk_notebook_set_current_page(GTK_NOTEBOOK(g_notebook), idx + 1);
        }
    }
    gtk_window_present(GTK_WINDOW(g_window));
}

static gboolean on_ctl_accept(GIOChannel *source, GIOCondition condition, gpointer data)
{
    (void)condition;
    (void)data;
    int listenfd = g_io_channel_unix_get_fd(source);
    int fd = accept(listenfd, NULL, NULL);
    if (fd >= 0) {
        char buf[64] = {0};
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        if (n > 0) {
            apply_ctl_line(buf);
        }
        close(fd);
    }
    return TRUE; /* keep watching */
}

/* Binds+listens on sockpath and hooks it into GTK's main loop -- run once
 * this process has won the flock and is about to become the GUI
 * instance. Stale sockets from a kiconf that crashed without cleaning up
 * are harmless to unlink first: the lock file, not the socket's mere
 * existence, is what decided single-instance-ness. */
static void start_ctl_listener(const char *sockpath)
{
    snprintf(g_sockpath, sizeof(g_sockpath), "%s", sockpath);
    unlink(sockpath);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return;
    }
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sockpath);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(fd, 8) != 0) {
        close(fd);
        return;
    }
    g_listenfd = fd;

    GIOChannel *chan = g_io_channel_unix_new(fd);
    g_io_add_watch(chan, G_IO_IN, on_ctl_accept, NULL);
    g_io_channel_unref(chan);
}

static void stop_ctl_listener(void)
{
    if (g_listenfd >= 0) {
        close(g_listenfd);
        g_listenfd = -1;
        unlink(g_sockpath);
    }
}

/* setlocale()+bindtextdomain()+textdomain(): the three calls every
 * gettext program makes once, before building any UI, so _()/gettext()
 * knows both which language to look up (the user's LANG/LC_MESSAGES,
 * via setlocale(LC_ALL, "") -- GTK itself never calls this on its own)
 * and where the "kiconf" catalog's .mo files live (LOCALEDIR, baked in by
 * the Makefile from PREFIX, same as kiconfd.conf's own path resolution
 * follows XDG_CONFIG_HOME/HOME at runtime rather than a compiled-in
 * value). Translations live in po/ -- see po/README.md. */
void kiconf_i18n_init(void)
{
    setlocale(LC_ALL, "");
    bindtextdomain("kiconf", LOCALEDIR);
    bind_textdomain_codeset("kiconf", "UTF-8");
    textdomain("kiconf");
}

int main(int argc, char **argv)
{
    kiconf_i18n_init();

    /* Checked before gtk_init() so `kiconf --version`/`--list-tabs` work
     * even without a display (X connection), same as most CLI-invokable
     * GTK tools. */
    const char *tab_arg = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--version") || !strcmp(argv[i], "-V")) {
            printf("kiconf %s\n", KICONF_VERSION);
            return 0;
        } else if (!strcmp(argv[i], "--list-tabs")) {
            list_tabs();
            return 0;
        } else if ((!strcmp(argv[i], "--tab") || !strcmp(argv[i], "-t")) && i + 1 < argc) {
            tab_arg = argv[++i];
        } else if (!strncmp(argv[i], "--tab=", 6)) {
            tab_arg = argv[i] + 6;
        }
    }

    int tab_idx = -1;
    if (tab_arg) {
        tab_idx = resolve_tab_index(tab_arg);
        if (tab_idx < 0) {
            fprintf(stderr, "kiconf: unknown tab '%s' -- run --list-tabs to see valid names\n", tab_arg);
        }
    }

    char lockpath[PATH_MAX], sockpath[PATH_MAX];
    lock_and_sock_paths(lockpath, sockpath, sizeof(lockpath));

    int lockfd = open(lockpath, O_CREAT | O_RDWR, 0600);
    if (lockfd >= 0 && flock(lockfd, LOCK_EX | LOCK_NB) != 0) {
        /* Another kiconf already owns the lock: hand this request off to
         * it (focus it, switching tab if one was requested) instead of
         * opening a second window. */
        close(lockfd);
        if (notify_running_instance(sockpath, tab_idx)) {
            return 0;
        }
        /* The lock holder isn't answering its socket (crashed mid-init,
         * stale lock, ...) -- fall through and become the GUI ourselves
         * rather than doing nothing. */
    }

    gtk_init(&argc, &argv);

    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    g_window = window;
    gtk_window_set_title(GTK_WINDOW(window), "kiconf");
    gtk_window_set_default_size(GTK_WINDOW(window), 640, 660);
    g_signal_connect(window, "delete-event", G_CALLBACK(on_window_delete), NULL);
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *notebook = gtk_notebook_new();
    g_notebook = notebook;
    /* No visible tab strip -- navigation is the home page's icon grid (and
     * eventually a "Voltar" in each module), same as a regular OS control
     * panel/settings app, not a tabbed dialog. GtkNotebook itself is still
     * the simplest way to hold "one page visible, the rest torn down"
     * (see on_switch_page() above), it's just not shown as tabs. */
    gtk_notebook_set_show_tabs(GTK_NOTEBOOK(notebook), FALSE);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), build_home_page(), gtk_label_new(_("Inicio")));
    for (int i = 0; i < N_TABS; i++) {
        g_tabs[i].placeholder = gtk_vbox_new(FALSE, 0);
        gtk_notebook_append_page(GTK_NOTEBOOK(notebook), g_tabs[i].placeholder, gtk_label_new(_(g_tabs[i].label)));
    }
    g_signal_connect(notebook, "switch-page", G_CALLBACK(on_switch_page), NULL);

    gtk_container_add(GTK_CONTAINER(window), notebook);
    gtk_widget_show_all(window);

    if (tab_idx >= 0) {
        gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook), tab_idx + 1);
    }

    /* Now that we hold the lock and have a window to focus/switch on
     * later requests, start listening for them. */
    start_ctl_listener(sockpath);

    gtk_main();
    stop_ctl_listener();
    return 0;
}
