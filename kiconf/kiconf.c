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

#include <string.h>

#include "tabs.h"

#define KICONF_VERSION "0.1.3"

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
    TabBuilder build;
    GtkWidget *placeholder;
    int built;
} LazyTab;

static LazyTab g_tabs[] = {
    {"Aparencia", build_appearance_tab, NULL, 0},
    {"Atalhos", build_shortcuts_tab, NULL, 0},
    {"Telas", build_telas_tab, NULL, 0},
    {"Entrada", build_entrada_tab, NULL, 0},
    {"Wallpaper", build_wallpaper_tab, NULL, 0},
    {"Outras", build_outras_tab, NULL, 0},
    {"Paineis", build_paineis_tab, NULL, 0},
    {"Permissoes", build_permissoes_tab, NULL, 0},
};
#define N_TABS ((int)(sizeof(g_tabs) / sizeof(g_tabs[0])))

static int g_current_tab = -1;

static void ensure_tab_built(int idx)
{
    if (idx < 0 || idx >= N_TABS || g_tabs[idx].built) {
        return;
    }
    GtkWidget *content = g_tabs[idx].build();
    gtk_box_pack_start(GTK_BOX(g_tabs[idx].placeholder), content, TRUE, TRUE, 0);
    gtk_widget_show_all(g_tabs[idx].placeholder);
    g_tabs[idx].built = 1;
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
}

static void on_switch_page(GtkNotebook *notebook, GtkNotebookPage *page, guint page_num, gpointer data)
{
    (void)notebook;
    (void)page;
    (void)data;
    if (g_current_tab >= 0 && g_current_tab != (int)page_num) {
        unbuild_tab(g_current_tab);
    }
    ensure_tab_built((int)page_num);
    g_current_tab = (int)page_num;
}

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
    for (int i = 0; i < N_TABS; i++) {
        g_tabs[i].placeholder = gtk_vbox_new(FALSE, 0);
        gtk_notebook_append_page(GTK_NOTEBOOK(notebook), g_tabs[i].placeholder, gtk_label_new(g_tabs[i].label));
    }
    g_signal_connect(notebook, "switch-page", G_CALLBACK(on_switch_page), NULL);

    gtk_container_add(GTK_CONTAINER(window), notebook);
    gtk_widget_show_all(window);
    /* The initially-selected page (0, "Aparencia") never gets a
     * "switch-page" emission for itself -- it's already current when the
     * handler is connected -- so it's built explicitly here. */
    g_current_tab = gtk_notebook_get_current_page(GTK_NOTEBOOK(notebook));
    ensure_tab_built(g_current_tab);
    gtk_main();
    return 0;
}
