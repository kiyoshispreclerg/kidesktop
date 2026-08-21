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
    wm.atoms.wm_state = intern_atom("WM_STATE");
    wm.atoms.wm_change_state = intern_atom("WM_CHANGE_STATE");
    wm.atoms.net_wm_name = intern_atom("_NET_WM_NAME");
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
    wm.atoms.net_frame_extents = intern_atom("_NET_FRAME_EXTENTS");
    wm.atoms.net_wm_strut = intern_atom("_NET_WM_STRUT");
    wm.atoms.net_wm_strut_partial = intern_atom("_NET_WM_STRUT_PARTIAL");

    wm.atoms.net_wm_state = intern_atom("_NET_WM_STATE");
    wm.atoms.net_wm_state_hidden = intern_atom("_NET_WM_STATE_HIDDEN");
    wm.atoms.net_wm_state_maximized_vert = intern_atom("_NET_WM_STATE_MAXIMIZED_VERT");
    wm.atoms.net_wm_state_maximized_horz = intern_atom("_NET_WM_STATE_MAXIMIZED_HORZ");
    wm.atoms.net_wm_state_skip_taskbar = intern_atom("_NET_WM_STATE_SKIP_TASKBAR");

    wm.atoms.net_wm_window_type = intern_atom("_NET_WM_WINDOW_TYPE");
    wm.atoms.net_wm_window_type_normal = intern_atom("_NET_WM_WINDOW_TYPE_NORMAL");
    wm.atoms.net_wm_window_type_dock = intern_atom("_NET_WM_WINDOW_TYPE_DOCK");
    wm.atoms.net_wm_window_type_desktop = intern_atom("_NET_WM_WINDOW_TYPE_DESKTOP");
    wm.atoms.net_wm_window_type_toolbar = intern_atom("_NET_WM_WINDOW_TYPE_TOOLBAR");
    wm.atoms.net_wm_window_type_menu = intern_atom("_NET_WM_WINDOW_TYPE_MENU");

    /* Custom, per kiwm-kicomp-projeto.md section 6: independent virtual
     * desktops per output, since EWMH itself has no such concept. */
    wm.atoms.kiwm_outputs = intern_atom("_KIWM_OUTPUTS");
    wm.atoms.kiwm_output_desktop = intern_atom("_KIWM_OUTPUT_DESKTOP");
    wm.atoms.kiwm_num_output_desktops = intern_atom("_KIWM_NUM_OUTPUT_DESKTOPS");
    wm.atoms.kiwm_set_output_desktop = intern_atom("_KIWM_SET_OUTPUT_DESKTOP");
    wm.atoms.kiwm_wm_output = intern_atom("_KIWM_WM_OUTPUT");
}
