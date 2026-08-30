#include "atoms.h"
#include "wm.h"

#include <stdlib.h>
#include <string.h>

xcb_atom_t intern_atom(const char *name)
{
    xcb_intern_atom_cookie_t cookie =
        xcb_intern_atom(wm.conn, 0, (uint16_t)strlen(name), name);
    xcb_intern_atom_reply_t *reply =
        xcb_intern_atom_reply(wm.conn, cookie, NULL);
    if (!reply)
        return XCB_ATOM_NONE;
    xcb_atom_t atom = reply->atom;
    free(reply);
    return atom;
}

void ewmh_init_atoms(void)
{
    wm.atoms.wm_protocols = intern_atom("WM_PROTOCOLS");
    wm.atoms.wm_delete_window = intern_atom("WM_DELETE_WINDOW");
    wm.atoms.wm_take_focus = intern_atom("WM_TAKE_FOCUS");
    wm.atoms.wm_state = intern_atom("WM_STATE");
    wm.atoms.wm_change_state = intern_atom("WM_CHANGE_STATE");
    wm.atoms.net_wm_name = intern_atom("_NET_WM_NAME");
    wm.atoms.net_wm_pid = intern_atom("_NET_WM_PID");
    wm.atoms.utf8_string = intern_atom("UTF8_STRING");

    wm.atoms.net_supported = intern_atom("_NET_SUPPORTED");
    wm.atoms.net_supporting_wm_check = intern_atom("_NET_SUPPORTING_WM_CHECK");
    wm.atoms.net_client_list = intern_atom("_NET_CLIENT_LIST");
    wm.atoms.net_client_list_stacking = intern_atom("_NET_CLIENT_LIST_STACKING");
    wm.atoms.net_active_window = intern_atom("_NET_ACTIVE_WINDOW");
    wm.atoms.net_close_window = intern_atom("_NET_CLOSE_WINDOW");
    wm.atoms.net_number_of_desktops = intern_atom("_NET_NUMBER_OF_DESKTOPS");
    wm.atoms.net_current_desktop = intern_atom("_NET_CURRENT_DESKTOP");
    wm.atoms.net_wm_desktop = intern_atom("_NET_WM_DESKTOP");
    wm.atoms.net_workarea = intern_atom("_NET_WORKAREA");
    wm.atoms.net_desktop_geometry = intern_atom("_NET_DESKTOP_GEOMETRY");
    wm.atoms.net_desktop_viewport = intern_atom("_NET_DESKTOP_VIEWPORT");
    wm.atoms.net_wm_full_placement = intern_atom("_NET_WM_FULL_PLACEMENT");
    wm.atoms.net_frame_extents = intern_atom("_NET_FRAME_EXTENTS");
    wm.atoms.net_wm_strut = intern_atom("_NET_WM_STRUT");
    wm.atoms.net_wm_strut_partial = intern_atom("_NET_WM_STRUT_PARTIAL");

    wm.atoms.net_wm_state = intern_atom("_NET_WM_STATE");
    wm.atoms.net_wm_state_hidden = intern_atom("_NET_WM_STATE_HIDDEN");
    wm.atoms.net_wm_state_maximized_vert = intern_atom("_NET_WM_STATE_MAXIMIZED_VERT");
    wm.atoms.net_wm_state_maximized_horz = intern_atom("_NET_WM_STATE_MAXIMIZED_HORZ");
    wm.atoms.net_wm_state_skip_taskbar = intern_atom("_NET_WM_STATE_SKIP_TASKBAR");
    wm.atoms.net_wm_state_shaded = intern_atom("_NET_WM_STATE_SHADED");
    wm.atoms.net_wm_state_above = intern_atom("_NET_WM_STATE_ABOVE");
    wm.atoms.net_wm_state_sticky = intern_atom("_NET_WM_STATE_STICKY");
    wm.atoms.net_wm_state_fullscreen = intern_atom("_NET_WM_STATE_FULLSCREEN");
    wm.atoms.net_wm_state_below = intern_atom("_NET_WM_STATE_BELOW");
    wm.atoms.net_wm_icon = intern_atom("_NET_WM_ICON");

    wm.atoms.net_wm_allowed_actions = intern_atom("_NET_WM_ALLOWED_ACTIONS");
    wm.atoms.net_wm_moveresize = intern_atom("_NET_WM_MOVERESIZE");
    wm.atoms.net_wm_action_move = intern_atom("_NET_WM_ACTION_MOVE");
    wm.atoms.net_wm_action_resize = intern_atom("_NET_WM_ACTION_RESIZE");
    wm.atoms.net_wm_action_minimize = intern_atom("_NET_WM_ACTION_MINIMIZE");
    wm.atoms.net_wm_action_shade = intern_atom("_NET_WM_ACTION_SHADE");
    wm.atoms.net_wm_action_maximize_horz = intern_atom("_NET_WM_ACTION_MAXIMIZE_HORZ");
    wm.atoms.net_wm_action_maximize_vert = intern_atom("_NET_WM_ACTION_MAXIMIZE_VERT");
    wm.atoms.net_wm_action_fullscreen = intern_atom("_NET_WM_ACTION_FULLSCREEN");
    wm.atoms.net_wm_action_change_desktop = intern_atom("_NET_WM_ACTION_CHANGE_DESKTOP");
    wm.atoms.net_wm_action_close = intern_atom("_NET_WM_ACTION_CLOSE");

    wm.atoms.net_wm_window_type = intern_atom("_NET_WM_WINDOW_TYPE");
    wm.atoms.net_wm_window_type_normal = intern_atom("_NET_WM_WINDOW_TYPE_NORMAL");
    wm.atoms.net_wm_window_type_dock = intern_atom("_NET_WM_WINDOW_TYPE_DOCK");
    wm.atoms.net_wm_window_type_desktop = intern_atom("_NET_WM_WINDOW_TYPE_DESKTOP");
    wm.atoms.net_wm_window_type_toolbar = intern_atom("_NET_WM_WINDOW_TYPE_TOOLBAR");
    wm.atoms.net_wm_window_type_menu = intern_atom("_NET_WM_WINDOW_TYPE_MENU");
    wm.atoms.net_wm_window_type_popup_menu = intern_atom("_NET_WM_WINDOW_TYPE_POPUP_MENU");
    wm.atoms.net_wm_window_type_dropdown_menu = intern_atom("_NET_WM_WINDOW_TYPE_DROPDOWN_MENU");
    wm.atoms.net_wm_window_type_tooltip = intern_atom("_NET_WM_WINDOW_TYPE_TOOLTIP");
    wm.atoms.net_wm_window_type_notification = intern_atom("_NET_WM_WINDOW_TYPE_NOTIFICATION");
    wm.atoms.net_wm_window_type_combo = intern_atom("_NET_WM_WINDOW_TYPE_COMBO");
    wm.atoms.net_wm_window_type_dnd = intern_atom("_NET_WM_WINDOW_TYPE_DND");
    wm.atoms.net_wm_window_type_splash = intern_atom("_NET_WM_WINDOW_TYPE_SPLASH");

    /* KDE-specific window types Plasma uses on its own popups, and the
     * Motif hint every toolkit still speaks for "don't decorate me". */
    wm.atoms.kde_net_wm_window_type_applet_popup = intern_atom("_KDE_NET_WM_WINDOW_TYPE_APPLET_POPUP");
    wm.atoms.kde_net_wm_window_type_override = intern_atom("_KDE_NET_WM_WINDOW_TYPE_OVERRIDE");
    wm.atoms.motif_wm_hints = intern_atom("_MOTIF_WM_HINTS");
    wm.atoms.wm_client_leader = intern_atom("WM_CLIENT_LEADER");

    /* Custom, per kiwm-kicomp-projeto.md section 6: independent virtual
     * desktops per output, since EWMH itself has no such concept. */
    wm.atoms.kiwm_outputs = intern_atom("_KIWM_OUTPUTS");
    wm.atoms.kiwm_output_desktop = intern_atom("_KIWM_OUTPUT_DESKTOP");
    wm.atoms.kiwm_num_output_desktops = intern_atom("_KIWM_NUM_OUTPUT_DESKTOPS");
    wm.atoms.kiwm_set_output_desktop = intern_atom("_KIWM_SET_OUTPUT_DESKTOP");
    wm.atoms.kiwm_wm_output = intern_atom("_KIWM_WM_OUTPUT");
    wm.atoms.kiwm_minimized_geometry = intern_atom("_KIWM_MINIMIZED_GEOMETRY");
}
