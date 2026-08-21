/* Client lifecycle: classification, manage/unmanage, focus/stacking,
 * and the map/move/resize/maximize/minimize state transitions. */
#include "client.h"
#include "wm.h"
#include "output.h"
#include "decoration.h"
#include "ewmh.h"

#include <stdlib.h>
#include <string.h>

static bool client_supports_protocol(xcb_window_t window, xcb_atom_t proto)
{
    xcb_get_property_reply_t *reply = xcb_get_property_reply(wm.conn,
        xcb_get_property(wm.conn, 0, window, wm.atoms.wm_protocols, XCB_ATOM_ATOM, 0, 64), NULL);
    if (!reply)
        return false;

    bool found = false;
    if (reply->type == XCB_ATOM_ATOM && reply->format == 32) {
        xcb_atom_t *atoms = xcb_get_property_value(reply);
        int n = xcb_get_property_value_length(reply) / (int)sizeof(xcb_atom_t);
        for (int i = 0; i < n; i++)
            if (atoms[i] == proto) { found = true; break; }
    }
    free(reply);
    return found;
}

static xcb_atom_t get_window_type(xcb_window_t window)
{
    xcb_get_property_reply_t *reply = xcb_get_property_reply(wm.conn,
        xcb_get_property(wm.conn, 0, window, wm.atoms.net_wm_window_type, XCB_ATOM_ATOM, 0, 32), NULL);
    if (!reply)
        return XCB_ATOM_NONE;

    xcb_atom_t type = XCB_ATOM_NONE;
    if (reply->type == XCB_ATOM_ATOM && reply->format == 32 &&
        xcb_get_property_value_length(reply) > 0) {
        xcb_atom_t *atoms = xcb_get_property_value(reply);
        type = atoms[0];
    }
    free(reply);
    return type;
}

static bool should_manage_decorated(xcb_window_t window)
{
    xcb_atom_t t = get_window_type(window);
    if (t == XCB_ATOM_NONE)
        return true;
    return !(t == wm.atoms.net_wm_window_type_dock ||
             t == wm.atoms.net_wm_window_type_desktop ||
             t == wm.atoms.net_wm_window_type_toolbar ||
             t == wm.atoms.net_wm_window_type_menu);
}

Client *find_client_window(xcb_window_t window)
{
    for (Client *c = wm.clients; c; c = c->next)
        if (c->window == window || c->frame == window)
            return c;
    return NULL;
}

void configure_frame(Client *c)
{
    bool deco = client_deco_visible(c);
    int th = deco ? TITLEBAR_H : 0;

    c->frame_width = c->width;
    c->frame_height = c->height + th;

    uint32_t fv[] = {
        (uint32_t)c->x, (uint32_t)c->y,
        (uint32_t)c->frame_width, (uint32_t)c->frame_height
    };
    xcb_configure_window(wm.conn, c->frame,
                         XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                         XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, fv);

    uint32_t cv[] = { 0, (uint32_t)th, (uint32_t)c->width, (uint32_t)c->height };
    xcb_configure_window(wm.conn, c->window,
                         XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                         XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, cv);

    draw_decoration(c);
}

void focus_client(Client *c)
{
    if (!c)
        return;

    Client *old = wm.focused;

    if (old != c) {
        wm.focused = c;
        xcb_set_input_focus(wm.conn, XCB_INPUT_FOCUS_POINTER_ROOT,
                            c->window, XCB_CURRENT_TIME);
        if (old)
            draw_decoration(old);
        ewmh_update_active_window();
    }

    xcb_configure_window(wm.conn, c->frame, XCB_CONFIG_WINDOW_STACK_MODE,
                         (uint32_t[]){ XCB_STACK_MODE_ABOVE });

    draw_decoration(c);
    ewmh_update_client_list();
    xcb_flush(wm.conn);
}

void cycle_focus(int direction)
{
    int output_idx = wm.focused ? wm.focused->output : output_for_pointer();
    if (output_idx < 0)
        return;
    int desktop = wm.outputs[output_idx].desktop;

    Client *eligible[MAX_CLIENTS];
    int n = 0;
    int current_idx = -1;

    for (Client *c = wm.clients; c && n < MAX_CLIENTS; c = c->next) {
        if (c->output == output_idx && c->desktop == desktop && c->mapped && !c->minimized) {
            if (c == wm.focused)
                current_idx = n;
            eligible[n++] = c;
        }
    }
    if (n == 0)
        return;

    int next = (current_idx < 0) ? 0 : (current_idx + direction + n) % n;
    focus_client(eligible[next]);
}

static void send_delete(Client *c)
{
    if (wm.atoms.wm_protocols == XCB_ATOM_NONE ||
        wm.atoms.wm_delete_window == XCB_ATOM_NONE)
        return;

    xcb_client_message_event_t ev = {
        .response_type = XCB_CLIENT_MESSAGE,
        .format = 32,
        .window = c->window,
        .type = wm.atoms.wm_protocols
    };
    ev.data.data32[0] = wm.atoms.wm_delete_window;
    ev.data.data32[1] = XCB_CURRENT_TIME;

    xcb_send_event(wm.conn, 0, c->window, XCB_EVENT_MASK_NO_EVENT, (const char *)&ev);
}

void close_client(Client *c)
{
    if (client_supports_protocol(c->window, wm.atoms.wm_delete_window))
        send_delete(c);
    else
        xcb_kill_client(wm.conn, c->window);
    xcb_flush(wm.conn);
}

void toggle_maximize(Client *c, int want /* -1=toggle 0=unmax 1=max */)
{
    bool target = (want == -1) ? !c->maximized : (want == 1);
    if (target == c->maximized)
        return;

    if (target) {
        c->saved_x = c->x;
        c->saved_y = c->y;
        c->saved_w = c->width;
        c->saved_h = c->height;

        /* Fill the output's usable area (screen minus any dock/panel
         * struts, see output.c's compute_output_workarea), not the raw
         * output rect -- a maximized window must never cover a taskbar. */
        int wx, wy, ww, wh;
        compute_output_workarea(c->output >= 0 ? c->output : 0, &wx, &wy, &ww, &wh);
        c->maximized = true;
        c->x = wx;
        c->y = wy;
        c->width = ww;
        c->height = wh - (wm.hide_deco_on_maximize ? 0 : TITLEBAR_H);
    } else {
        c->maximized = false;
        c->x = c->saved_x;
        c->y = c->saved_y;
        c->width = c->saved_w;
        c->height = c->saved_h;
    }

    configure_frame(c);
    ewmh_update_wm_state(c);
    ewmh_update_frame_extents(c);
    xcb_flush(wm.conn);
}

void minimize_client(Client *c)
{
    if (c->minimized)
        return;

    c->minimized = true;
    if (c->mapped) {
        xcb_unmap_window(wm.conn, c->frame);
        c->mapped = false;
    }
    if (wm.focused == c)
        wm.focused = NULL;

    ewmh_update_wm_state(c);
    ewmh_update_active_window();
    xcb_flush(wm.conn);
}

void restore_client(Client *c)
{
    if (!c->minimized)
        return;
    c->minimized = false;

    if (wm.outputs[c->output].desktop == c->desktop) {
        xcb_map_window(wm.conn, c->frame);
        c->mapped = true;
    }
    ewmh_update_wm_state(c);
    xcb_flush(wm.conn);
}

void activate_client(Client *c)
{
    if (c->minimized)
        restore_client(c);
    if (c->output >= 0 && wm.outputs[c->output].desktop != c->desktop)
        switch_workspace(c->output, c->desktop);
    if (c->mapped)
        focus_client(c);
}

void set_client_desktop(Client *c, int desktop)
{
    if (desktop < 0)
        desktop = 0;
    if (desktop >= wm.num_desktops)
        desktop = wm.num_desktops - 1;
    if (desktop == c->desktop)
        return;

    bool was_visible = (c->output >= 0 && wm.outputs[c->output].desktop == c->desktop);
    bool now_visible = (c->output >= 0 && wm.outputs[c->output].desktop == desktop);

    c->desktop = desktop;

    if (was_visible && !now_visible && c->mapped)
        xcb_unmap_window(wm.conn, c->frame);
    else if (!was_visible && now_visible && c->mapped)
        xcb_map_window(wm.conn, c->frame);

    ewmh_update_wm_desktop(c);
    xcb_flush(wm.conn);
}

static void remove_client(Client *client)
{
    Client **pp = &wm.clients;
    while (*pp) {
        if (*pp == client) {
            *pp = client->next;
            if (wm.focused == client)
                wm.focused = NULL;
            free(client);
            return;
        }
        pp = &(*pp)->next;
    }
}

void unmanage(Client *c)
{
    int fx = c->x, fy = c->y, fw = c->frame_width, fh = c->frame_height;

    if (wm.focused == c)
        wm.focused = NULL;
    if (wm.drag_client == c) {
        wm.drag_client = NULL;
        wm.drag_mode = DRAG_NONE;
    }

    xcb_unmap_window(wm.conn, c->frame);

    xcb_void_cookie_t reparent_cookie =
        xcb_reparent_window_checked(wm.conn, c->window, wm.root, c->x, c->y);
    xcb_generic_error_t *err = xcb_request_check(wm.conn, reparent_cookie);
    if (err)
        free(err);

    xcb_destroy_window(wm.conn, c->frame);

    if (fw > 0 && fh > 0)
        xcb_clear_area(wm.conn, 0, wm.root, fx, fy, (uint16_t)fw, (uint16_t)fh);

    remove_client(c);
    ewmh_update_client_list();
    ewmh_update_active_window();
    xcb_flush(wm.conn);
}

void manage(xcb_window_t window)
{
    if (find_client_window(window))
        return;

    xcb_get_window_attributes_reply_t *attr = xcb_get_window_attributes_reply(
        wm.conn, xcb_get_window_attributes(wm.conn, window), NULL);
    if (!attr)
        return;
    if (attr->override_redirect) {
        free(attr);
        return;
    }
    bool was_viewable = attr->map_state == XCB_MAP_STATE_VIEWABLE;
    free(attr);

    if (!should_manage_decorated(window)) {
        /* Panels, docks, desktops: managed just enough to be mapped and
         * to show up wherever _NET_WM_WINDOW_TYPE says they belong,
         * never framed or added to the taskbar client list. Still watch
         * for _NET_WM_STRUT(_PARTIAL) changes and destruction so a panel
         * reserving screen edge space (see output.c's dock_track) keeps
         * maximize/_NET_WORKAREA out of its way even though it's never a
         * Client. */
        uint32_t dock_mask = XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_STRUCTURE_NOTIFY;
        xcb_change_window_attributes(wm.conn, window, XCB_CW_EVENT_MASK, &dock_mask);
        dock_track(window);
        xcb_map_window(wm.conn, window);
        xcb_flush(wm.conn);
        return;
    }

    xcb_get_geometry_reply_t *geo =
        xcb_get_geometry_reply(wm.conn, xcb_get_geometry(wm.conn, window), NULL);
    if (!geo)
        return;

    Client *c = calloc(1, sizeof(*c));
    if (!c) {
        free(geo);
        return;
    }

    c->window = window;
    c->x = geo->x;
    c->y = geo->y;
    c->width = geo->width < MIN_CLIENT_W ? MIN_CLIENT_W : geo->width;
    c->height = geo->height < MIN_CLIENT_H ? MIN_CLIENT_H : geo->height;
    free(geo);

    c->output = output_index_for_point(c->x + c->width / 2, c->y + c->height / 2);
    if (c->output < 0)
        c->output = 0;
    c->desktop = wm.output_count > 0 ? wm.outputs[c->output].desktop : 0;

    get_title(c);

    c->frame = xcb_generate_id(wm.conn);

    uint32_t values[] = {
        XCB_EVENT_MASK_EXPOSURE |
        XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE |
        XCB_EVENT_MASK_POINTER_MOTION |
        XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_LEAVE_WINDOW,
        0
    };
    xcb_create_window(wm.conn, wm.screen->root_depth, c->frame, wm.root,
                      c->x, c->y, c->width, c->height + TITLEBAR_H, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, wm.screen->root_visual,
                      XCB_CW_EVENT_MASK | XCB_CW_BORDER_PIXEL, values);

    uint32_t client_mask = XCB_EVENT_MASK_PROPERTY_CHANGE |
                           XCB_EVENT_MASK_STRUCTURE_NOTIFY |
                           XCB_EVENT_MASK_FOCUS_CHANGE;
    xcb_change_window_attributes(wm.conn, window, XCB_CW_EVENT_MASK, &client_mask);

    xcb_grab_button(wm.conn, 0, window, XCB_EVENT_MASK_BUTTON_PRESS,
                    XCB_GRAB_MODE_SYNC, XCB_GRAB_MODE_ASYNC,
                    XCB_NONE, XCB_NONE, XCB_BUTTON_INDEX_1, XCB_MOD_MASK_ANY);

    xcb_reparent_window(wm.conn, window, c->frame, 0, TITLEBAR_H);

    xcb_map_window(wm.conn, window);
    xcb_map_window(wm.conn, c->frame);

    c->mapped = true;
    /* Reparenting an already-mapped window generates its one automatic
     * unmap as *two* UnmapNotify events: one via StructureNotify on the
     * window itself, one via SubstructureNotify on root (the window's
     * parent at that instant) -- both must be swallowed, not just one. */
    c->ignore_unmap = was_viewable ? 2 : 0;
    set_icccm_wm_state(c, WM_STATE_NORMAL);

    c->next = wm.clients;
    wm.clients = c;

    configure_frame(c);
    ewmh_update_wm_desktop(c);
    ewmh_update_wm_output(c);
    ewmh_update_wm_state(c);
    ewmh_update_frame_extents(c);
    ewmh_update_client_list();
    focus_client(c);
}

/* Called once at startup so windows already open before kiwm starts (or
 * left over from a --replace'd WM) get framed too, not just windows
 * mapped afterward. A window qualifies if it's currently viewable, or if
 * it carries an ICCCM WM_STATE property (an app that was managed before
 * and expects to be picked back up, even if briefly unmapped). */
void manage_existing_windows(void)
{
    xcb_query_tree_reply_t *tree =
        xcb_query_tree_reply(wm.conn, xcb_query_tree(wm.conn, wm.root), NULL);
    if (!tree)
        return;

    xcb_window_t *children = xcb_query_tree_children(tree);
    int n = xcb_query_tree_children_length(tree);

    for (int i = 0; i < n; i++) {
        xcb_window_t w = children[i];
        if (w == wm.check_win || w == wm.sel_win || find_client_window(w))
            continue;

        xcb_get_window_attributes_reply_t *attr = xcb_get_window_attributes_reply(
            wm.conn, xcb_get_window_attributes(wm.conn, w), NULL);
        if (!attr)
            continue;
        bool viewable = attr->map_state == XCB_MAP_STATE_VIEWABLE;
        bool override = attr->override_redirect;
        free(attr);
        if (override)
            continue;

        bool has_wm_state = false;
        xcb_get_property_reply_t *st = xcb_get_property_reply(wm.conn,
            xcb_get_property(wm.conn, 0, w, wm.atoms.wm_state, wm.atoms.wm_state, 0, 2), NULL);
        if (st) {
            if (st->type == wm.atoms.wm_state)
                has_wm_state = true;
            free(st);
        }

        if (viewable || has_wm_state)
            manage(w);
    }

    free(tree);
    xcb_flush(wm.conn);
}
