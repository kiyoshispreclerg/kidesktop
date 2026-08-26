/* EWMH/ICCCM state kiwm publishes on behalf of managed clients: just
 * enough for a taskbar (xispanel's tasklist widget) to list/activate/
 * close/minimize/maximize windows -- not full spec conformance. */
#include "ewmh.h"
#include "wm.h"
#include "output.h"
#include "decoration.h"

#include <xcb/xcb_icccm.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

void ewmh_update_client_list(void)
{
    xcb_window_t list[MAX_CLIENTS];
    int n = 0;
    for (Client *c = wm.clients; c && n < MAX_CLIENTS; c = c->next)
        list[n++] = c->window;

    xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE, wm.root,
                        wm.atoms.net_client_list, XCB_ATOM_WINDOW, 32, (uint32_t)n, list);
    xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE, wm.root,
                        wm.atoms.net_client_list_stacking, XCB_ATOM_WINDOW, 32, (uint32_t)n, list);
}

void ewmh_update_active_window(void)
{
    xcb_window_t w = wm.focused ? wm.focused->window : XCB_NONE;
    xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE, wm.root,
                        wm.atoms.net_active_window, XCB_ATOM_WINDOW, 32, 1, &w);
}

void set_icccm_wm_state(Client *c, uint32_t state)
{
    uint32_t data[] = { state, XCB_ATOM_NONE };
    xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE, c->window,
                        wm.atoms.wm_state, wm.atoms.wm_state, 32, 2, data);
}

/* Rewrites _NET_WM_STATE preserving any atoms we don't manage ourselves
 * (e.g. an app's own _NET_WM_STATE_SKIP_TASKBAR), only replacing the
 * hidden/maximized bits we own. */
void ewmh_update_wm_state(Client *c)
{
    xcb_atom_t keep[32];
    int nkeep = 0;

    xcb_get_property_reply_t *reply = xcb_get_property_reply(wm.conn,
        xcb_get_property(wm.conn, 0, c->window, wm.atoms.net_wm_state, XCB_ATOM_ATOM, 0, 32), NULL);
    if (reply) {
        if (reply->type == XCB_ATOM_ATOM && reply->format == 32) {
            xcb_atom_t *atoms = xcb_get_property_value(reply);
            int n = xcb_get_property_value_length(reply) / (int)sizeof(xcb_atom_t);
            for (int i = 0; i < n && nkeep < 32; i++) {
                xcb_atom_t a = atoms[i];
                if (a == wm.atoms.net_wm_state_hidden ||
                    a == wm.atoms.net_wm_state_maximized_vert ||
                    a == wm.atoms.net_wm_state_maximized_horz ||
                    a == wm.atoms.net_wm_state_shaded ||
                    a == wm.atoms.net_wm_state_above ||
                    a == wm.atoms.net_wm_state_sticky ||
                    a == wm.atoms.net_wm_state_fullscreen ||
                    a == wm.atoms.net_wm_state_below)
                    continue;
                keep[nkeep++] = a;
            }
        }
        free(reply);
    }

    xcb_atom_t out[40];
    int n = 0;
    for (int i = 0; i < nkeep; i++)
        out[n++] = keep[i];
    if (c->minimized)
        out[n++] = wm.atoms.net_wm_state_hidden;
    if (c->maximized) {
        out[n++] = wm.atoms.net_wm_state_maximized_vert;
        out[n++] = wm.atoms.net_wm_state_maximized_horz;
    }
    if (c->shaded)
        out[n++] = wm.atoms.net_wm_state_shaded;
    if (c->keep_above)
        out[n++] = wm.atoms.net_wm_state_above;
    if (c->sticky)
        out[n++] = wm.atoms.net_wm_state_sticky;
    if (c->fullscreen)
        out[n++] = wm.atoms.net_wm_state_fullscreen;
    if (c->keep_below)
        out[n++] = wm.atoms.net_wm_state_below;

    xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE, c->window,
                        wm.atoms.net_wm_state, XCB_ATOM_ATOM, 32, (uint32_t)n, out);

    set_icccm_wm_state(c, c->minimized ? WM_STATE_ICONIC : WM_STATE_NORMAL);
}

void ewmh_update_wm_desktop(Client *c)
{
    uint32_t v = (uint32_t)c->desktop;
    xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE, c->window,
                        wm.atoms.net_wm_desktop, XCB_ATOM_CARDINAL, 32, 1, &v);
}

/* Companion to _NET_WM_DESKTOP (see PROTOCOL.md): _NET_WM_DESKTOP alone is
 * only unique *within* a client's own output, so a pager needs this too to
 * reconstruct which (output, desktop) cell a window actually belongs to. */
void ewmh_update_wm_output(Client *c)
{
    uint32_t v = (uint32_t)c->output;
    xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE, c->window,
                        wm.atoms.kiwm_wm_output, XCB_ATOM_CARDINAL, 32, 1, &v);
}

/* Tells toolkits how big kiwm's decoration is around this client, so their
 * own "position relative to my window" logic (context menus, tooltips,
 * tray-icon popups, etc.) accounts for it instead of assuming an
 * undecorated 0-offset frame. Without this, Qt/GTK apps that read
 * _NET_FRAME_EXTENTS to compute their true on-screen origin get it wrong
 * by exactly the titlebar height whenever kiwm draws one. */
void ewmh_update_frame_extents(Client *c)
{
    bool deco = client_deco_visible(c);
    uint32_t top = deco ? TITLEBAR_H : 0;
    uint32_t side = deco ? (uint32_t)wm.border_thickness : 0;
    uint32_t extents[4] = { side, side, top, side }; /* left, right, top, bottom */
    xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE, c->window,
                        wm.atoms.net_frame_extents, XCB_ATOM_CARDINAL, 32, 4, extents);
}

void get_title(Client *c)
{
    memset(c->title, 0, sizeof(c->title));

    xcb_get_property_reply_t *reply = xcb_get_property_reply(wm.conn,
        xcb_get_property(wm.conn, 0, c->window, wm.atoms.net_wm_name, wm.atoms.utf8_string, 0, 256), NULL);

    int len = reply ? xcb_get_property_value_length(reply) : 0;
    if (reply && len > 0) {
        if ((size_t)len >= sizeof(c->title))
            len = (int)sizeof(c->title) - 1;
        memcpy(c->title, xcb_get_property_value(reply), (size_t)len);
        c->title[len] = '\0';
    }
    free(reply);

    if (!c->title[0]) {
        reply = xcb_get_property_reply(wm.conn,
            xcb_get_property(wm.conn, 0, c->window, XCB_ATOM_WM_NAME, XCB_GET_PROPERTY_TYPE_ANY, 0, 256), NULL);
        len = reply ? xcb_get_property_value_length(reply) : 0;
        if (reply && len > 0) {
            if ((size_t)len >= sizeof(c->title))
                len = (int)sizeof(c->title) - 1;
            memcpy(c->title, xcb_get_property_value(reply), (size_t)len);
            c->title[len] = '\0';
        }
        free(reply);
    }

    if (!c->title[0])
        snprintf(c->title, sizeof(c->title), "Untitled");
}

/* ICCCM WM_NORMAL_HINTS' minimum size (PMinSize), if the client set one --
 * read once in client.c's manage() and again whenever WM_NORMAL_HINTS
 * changes (events.c's handle_property_notify()), since some toolkits only
 * set it after the initial map. Always floored to MIN_CLIENT_W/H: a hint
 * of 0 (or no hint at all) must not make a window shrinkable to nothing,
 * and a hint smaller than kiwm's own absolute floor is pointless to honor
 * literally. */
void get_size_hints(Client *c)
{
    c->min_w = MIN_CLIENT_W;
    c->min_h = MIN_CLIENT_H;

    xcb_size_hints_t hints;
    xcb_get_property_cookie_t cookie = xcb_icccm_get_wm_normal_hints(wm.conn, c->window);
    if (!xcb_icccm_get_wm_normal_hints_reply(wm.conn, cookie, &hints, NULL))
        return;

    if (hints.flags & XCB_ICCCM_SIZE_HINT_P_MIN_SIZE) {
        if (hints.min_width > c->min_w)
            c->min_w = hints.min_width;
        if (hints.min_height > c->min_h)
            c->min_h = hints.min_height;
    }
}

void ewmh_init_supported(void)
{
    xcb_atom_t supported[] = {
        wm.atoms.net_supported, wm.atoms.net_supporting_wm_check,
        wm.atoms.net_client_list, wm.atoms.net_client_list_stacking,
        wm.atoms.net_active_window, wm.atoms.net_close_window,
        wm.atoms.net_number_of_desktops, wm.atoms.net_current_desktop,
        wm.atoms.net_wm_desktop, wm.atoms.net_workarea, wm.atoms.net_frame_extents,
        wm.atoms.net_wm_strut, wm.atoms.net_wm_strut_partial,
        wm.atoms.net_wm_state, wm.atoms.net_wm_state_hidden,
        wm.atoms.net_wm_state_maximized_vert, wm.atoms.net_wm_state_maximized_horz,
        wm.atoms.net_wm_state_skip_taskbar, wm.atoms.net_wm_state_shaded,
        wm.atoms.net_wm_state_above, wm.atoms.net_wm_state_sticky,
        wm.atoms.net_wm_state_fullscreen, wm.atoms.net_wm_state_below,
        wm.atoms.net_wm_icon,
        wm.atoms.net_wm_window_type, wm.atoms.net_wm_window_type_normal,
        wm.atoms.net_wm_window_type_dock, wm.atoms.net_wm_window_type_desktop,
        wm.atoms.net_wm_window_type_toolbar, wm.atoms.net_wm_window_type_menu,
        wm.atoms.net_wm_window_type_popup_menu, wm.atoms.net_wm_window_type_dropdown_menu,
        wm.atoms.net_wm_window_type_tooltip, wm.atoms.net_wm_window_type_notification,
        wm.atoms.net_wm_window_type_combo, wm.atoms.net_wm_window_type_dnd,
        wm.atoms.net_wm_window_type_splash,
        wm.atoms.net_wm_name,
    };
    xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE, wm.root,
                        wm.atoms.net_supported, XCB_ATOM_ATOM, 32,
                        sizeof(supported) / sizeof(supported[0]), supported);

    uint32_t numws = (uint32_t)wm.num_desktops;
    xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE, wm.root,
                        wm.atoms.net_number_of_desktops, XCB_ATOM_CARDINAL, 32, 1, &numws);
    ewmh_set_current_desktop(0);
}

void ewmh_init_supporting_wm_check(void)
{
    wm.check_win = xcb_generate_id(wm.conn);
    xcb_create_window(wm.conn, XCB_COPY_FROM_PARENT, wm.check_win, wm.root,
                      -1, -1, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      wm.screen->root_visual, 0, NULL);

    xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE, wm.check_win,
                        wm.atoms.net_supporting_wm_check, XCB_ATOM_WINDOW, 32, 1, &wm.check_win);
    xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE, wm.root,
                        wm.atoms.net_supporting_wm_check, XCB_ATOM_WINDOW, 32, 1, &wm.check_win);

    xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE, wm.check_win,
                        wm.atoms.net_wm_name, wm.atoms.utf8_string, 8, 4, "kiwm");
}
