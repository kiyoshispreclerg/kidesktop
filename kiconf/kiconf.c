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

#define KICONF_VERSION "0.1.1"

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
