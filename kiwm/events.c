/* X event dispatch: the WM's whole runtime behavior after startup lives
 * here, delegating actual state changes to client.c/output.c/ewmh.c. */
#include "events.h"
#include "wm.h"
#include "client.h"
#include "output.h"
#include "decoration.h"
#include "ewmh.h"

#include <xcb/randr.h>

#include <stdio.h>

static void handle_map_request(xcb_map_request_event_t *ev)
{
    Client *c = find_client_window(ev->window);
    if (!c) {
        manage(ev->window);
    } else {
        if (wm.outputs[c->output].desktop == c->desktop) {
            xcb_map_window(wm.conn, c->frame);
            c->mapped = true;
        }
        c->minimized = false;
        set_icccm_wm_state(c, WM_STATE_NORMAL);
        ewmh_update_wm_state(c);
    }
    xcb_flush(wm.conn);
}

static void handle_configure_request(xcb_configure_request_event_t *ev)
{
    Client *c = find_client_window(ev->window);

    if (!c) {
        uint16_t mask = ev->value_mask;
        uint32_t values[7];
        int n = 0;
        if (mask & XCB_CONFIG_WINDOW_X)            values[n++] = (uint32_t)ev->x;
        if (mask & XCB_CONFIG_WINDOW_Y)            values[n++] = (uint32_t)ev->y;
        if (mask & XCB_CONFIG_WINDOW_WIDTH)        values[n++] = ev->width;
        if (mask & XCB_CONFIG_WINDOW_HEIGHT)       values[n++] = ev->height;
        if (mask & XCB_CONFIG_WINDOW_BORDER_WIDTH) values[n++] = ev->border_width;
        if (mask & XCB_CONFIG_WINDOW_SIBLING)      values[n++] = ev->sibling;
        if (mask & XCB_CONFIG_WINDOW_STACK_MODE)   values[n++] = ev->stack_mode;
        xcb_configure_window(wm.conn, ev->window, mask, values);
        xcb_flush(wm.conn);
        return;
    }

    if (c->maximized) {
        /* Ignore geometry requests while maximized; just re-affirm current state. */
        configure_frame(c);
        xcb_flush(wm.conn);
        return;
    }

    if (ev->value_mask & XCB_CONFIG_WINDOW_X)      c->x = ev->x;
    if (ev->value_mask & XCB_CONFIG_WINDOW_Y)      c->y = ev->y;
    if (ev->value_mask & XCB_CONFIG_WINDOW_WIDTH)  c->width = ev->width < MIN_CLIENT_W ? MIN_CLIENT_W : ev->width;
    if (ev->value_mask & XCB_CONFIG_WINDOW_HEIGHT) c->height = ev->height < MIN_CLIENT_H ? MIN_CLIENT_H : ev->height;

    configure_frame(c);
    xcb_flush(wm.conn);
}

static void handle_button_press(xcb_button_press_event_t *ev)
{
    Client *c = find_client_window(ev->event);
    if (!c)
        c = find_client_window(ev->child);
    if (!c)
        return;

    focus_client(c);

    int rel_x = ev->root_x - c->x;
    int rel_y = ev->root_y - c->y;

    if (client_deco_visible(c) && ev->detail == 1 && rel_y >= 0 && rel_y < TITLEBAR_H) {
        int w = c->frame_width;

        if (rel_x >= w - BUTTON_W) {
            close_client(c);
            return;
        }
        if (rel_x >= w - BUTTON_W * 2) {
            toggle_maximize(c, -1);
            return;
        }
        if (rel_x >= w - BUTTON_W * 3) {
            minimize_client(c);
            return;
        }

        wm.drag_mode = DRAG_MOVE;
        wm.drag_client = c;
        wm.drag_start_root_x = ev->root_x;
        wm.drag_start_root_y = ev->root_y;
        wm.drag_start_x = c->x;
        wm.drag_start_y = c->y;
        wm.drag_start_w = c->width;
        wm.drag_start_h = c->height;

        xcb_grab_pointer(wm.conn, 0, wm.root,
                         XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_POINTER_MOTION,
                         XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
                         XCB_NONE, XCB_NONE, XCB_CURRENT_TIME);
        xcb_flush(wm.conn);
        return;
    }

    /* Meta+drag (any button) and Alt+drag move the window; Alt+Right
     * additionally resizes -- see kiwm-kicomp-projeto.md's request for
     * hardcoded Meta+mouse-down-to-move alongside the existing Alt binds. */
    if (ev->state & (MOD_ALT | MOD_META)) {
        if (ev->detail == 1)
            wm.drag_mode = DRAG_MOVE;
        else if (ev->detail == 3 && (ev->state & MOD_ALT))
            wm.drag_mode = DRAG_RESIZE;
        else
            return;

        wm.drag_client = c;
        wm.drag_start_root_x = ev->root_x;
        wm.drag_start_root_y = ev->root_y;
        wm.drag_start_x = c->x;
        wm.drag_start_y = c->y;
        wm.drag_start_w = c->width;
        wm.drag_start_h = c->height;

        xcb_grab_pointer(wm.conn, 0, wm.root,
                         XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_POINTER_MOTION,
                         XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
                         XCB_NONE, XCB_NONE, XCB_CURRENT_TIME);
        xcb_flush(wm.conn);
        return;
    }

    if (rel_y >= (client_deco_visible(c) ? TITLEBAR_H : 0)) {
        xcb_allow_events(wm.conn, XCB_ALLOW_REPLAY_POINTER, ev->time);
        xcb_flush(wm.conn);
    }
}

static void handle_motion(xcb_motion_notify_event_t *ev)
{
    if (!wm.drag_client || wm.drag_mode == DRAG_NONE)
        return;

    Client *c = wm.drag_client;
    int dx = ev->root_x - wm.drag_start_root_x;
    int dy = ev->root_y - wm.drag_start_root_y;

    if (c->maximized)
        toggle_maximize(c, 0);

    if (wm.drag_mode == DRAG_MOVE) {
        c->x = wm.drag_start_x + dx;
        c->y = wm.drag_start_y + dy;
    } else {
        c->width = wm.drag_start_w + dx;
        c->height = wm.drag_start_h + dy;
        if (c->width < MIN_CLIENT_W) c->width = MIN_CLIENT_W;
        if (c->height < MIN_CLIENT_H) c->height = MIN_CLIENT_H;
    }

    configure_frame(c);
    xcb_flush(wm.conn);
}

static void handle_button_release(xcb_button_release_event_t *ev)
{
    (void)ev;
    if (wm.drag_client) {
        Client *c = wm.drag_client;
        int new_output = output_index_for_point(c->x + c->width / 2, c->y + c->height / 2);
        if (new_output >= 0 && new_output != c->output) {
            c->output = new_output;
            c->desktop = wm.outputs[new_output].desktop;
            ewmh_update_wm_desktop(c);
        }
        xcb_ungrab_pointer(wm.conn, XCB_CURRENT_TIME);
        wm.drag_client = NULL;
        wm.drag_mode = DRAG_NONE;
        xcb_flush(wm.conn);
    }
}

static void handle_property_notify(xcb_property_notify_event_t *ev)
{
    Client *c = find_client_window(ev->window);
    if (!c)
        return;

    if (ev->atom == wm.atoms.net_wm_name || ev->atom == XCB_ATOM_WM_NAME) {
        get_title(c);
        draw_decoration(c);
        xcb_flush(wm.conn);
    }
}

static void handle_enter_notify(xcb_enter_notify_event_t *ev)
{
    Client *c = find_client_window(ev->event);
    if (!c)
        c = find_client_window(ev->child);
    if (c && c->mapped && !c->minimized && wm.outputs[c->output].desktop == c->desktop)
        focus_client(c);
}

static void handle_key_press(xcb_key_press_event_t *ev)
{
    uint16_t mods = ev->state & (XCB_MOD_MASK_SHIFT | XCB_MOD_MASK_LOCK |
                                 XCB_MOD_MASK_CONTROL | XCB_MOD_MASK_1 |
                                 XCB_MOD_MASK_2 | XCB_MOD_MASK_3 |
                                 XCB_MOD_MASK_4 | XCB_MOD_MASK_5);
    uint16_t clean_alt = mods & (MOD_ALT | XCB_MOD_MASK_SHIFT);
    uint16_t clean_meta = mods & (MOD_META | XCB_MOD_MASK_SHIFT);

    /* Alt+Tab / Alt+Shift+Tab: cycle focused window.
     * Meta+Tab / Meta+Shift+Tab: cycle the focused output's virtual desktop. */
    if (ev->detail == wm.key_tab) {
        if (clean_alt == MOD_ALT_SHIFT)        { cycle_focus(-1); return; }
        if (clean_alt == MOD_ALT)              { cycle_focus(+1); return; }
        if (clean_meta == MOD_META_SHIFT)      { cycle_output_desktop(-1); return; }
        if (clean_meta == MOD_META)            { cycle_output_desktop(+1); return; }
        return;
    }

    /* Meta+Up: maximize/restore the focused window. */
    if (ev->detail == wm.key_up && clean_meta == MOD_META) {
        if (wm.focused)
            toggle_maximize(wm.focused, -1);
        return;
    }

    if (clean_alt == MOD_ALT) {
        int output_idx = wm.focused ? wm.focused->output : output_for_pointer();
        if (output_idx < 0)
            return;
        if (ev->detail == wm.key_1) { switch_workspace(output_idx, 0); return; }
        if (ev->detail == wm.key_2) { switch_workspace(output_idx, 1); return; }
        if (ev->detail == wm.key_3) { switch_workspace(output_idx, 2); return; }
        if (ev->detail == wm.key_4) { switch_workspace(output_idx, 3); return; }
    }
}

static void handle_net_wm_state(Client *c, uint32_t action, xcb_atom_t a1, xcb_atom_t a2)
{
    bool is_max = (a1 == wm.atoms.net_wm_state_maximized_vert || a1 == wm.atoms.net_wm_state_maximized_horz ||
                   a2 == wm.atoms.net_wm_state_maximized_vert || a2 == wm.atoms.net_wm_state_maximized_horz);
    bool is_hidden = (a1 == wm.atoms.net_wm_state_hidden || a2 == wm.atoms.net_wm_state_hidden);

    /* action: 0=remove, 1=add, 2=toggle (_NET_WM_STATE_TOGGLE) */
    if (is_max) {
        int want = (action == 2) ? -1 : (action == 1 ? 1 : 0);
        toggle_maximize(c, want);
    }
    if (is_hidden) {
        bool want_hidden = (action == 2) ? !c->minimized : (action == 1);
        if (want_hidden)
            minimize_client(c);
        else
            restore_client(c);
    }
}

static void handle_client_message(xcb_client_message_event_t *ev)
{
    if (ev->type == wm.atoms.kiwm_set_output_desktop) {
        switch_workspace((int)ev->data.data32[0], (int)ev->data.data32[1]);
        return;
    }

    Client *c = find_client_window(ev->window);
    if (!c)
        return;

    if (ev->type == wm.atoms.net_active_window) {
        activate_client(c);
    } else if (ev->type == wm.atoms.net_close_window) {
        close_client(c);
    } else if (ev->type == wm.atoms.wm_change_state) {
        if (ev->data.data32[0] == WM_STATE_ICONIC)
            minimize_client(c);
    } else if (ev->type == wm.atoms.net_wm_state) {
        handle_net_wm_state(c, ev->data.data32[0],
                            (xcb_atom_t)ev->data.data32[1], (xcb_atom_t)ev->data.data32[2]);
    } else if (ev->type == wm.atoms.net_wm_desktop) {
        set_client_desktop(c, (int)ev->data.data32[0]);
    }
}

void handle_event(xcb_generic_event_t *event)
{
    uint8_t type = event->response_type & ~0x80;

    if (wm.randr_event_base && type == wm.randr_event_base + XCB_RANDR_SCREEN_CHANGE_NOTIFY) {
        outputs_refresh();
        return;
    }

    switch (type) {
    case XCB_MAP_REQUEST:
        handle_map_request((xcb_map_request_event_t *)event);
        break;
    case XCB_CONFIGURE_REQUEST:
        handle_configure_request((xcb_configure_request_event_t *)event);
        break;
    case XCB_DESTROY_NOTIFY: {
        xcb_destroy_notify_event_t *ev = (xcb_destroy_notify_event_t *)event;
        Client *c = find_client_window(ev->window);
        if (c)
            unmanage(c);
        break;
    }
    case XCB_UNMAP_NOTIFY: {
        xcb_unmap_notify_event_t *ev = (xcb_unmap_notify_event_t *)event;
        Client *c = find_client_window(ev->window);
        if (c && ev->window == c->window) {
            if (c->ignore_unmap > 0) {
                /* Spurious auto-unmap from reparenting an already-mapped
                 * pre-existing window at startup -- not a real withdrawal. */
                c->ignore_unmap--;
                break;
            }
            if (c->mapped && wm.outputs[c->output].desktop == c->desktop && !c->minimized) {
                c->mapped = false;
                xcb_unmap_window(wm.conn, c->frame);
            }
        }
        break;
    }
    case XCB_BUTTON_PRESS:
        handle_button_press((xcb_button_press_event_t *)event);
        break;
    case XCB_BUTTON_RELEASE:
        handle_button_release((xcb_button_release_event_t *)event);
        break;
    case XCB_MOTION_NOTIFY:
        handle_motion((xcb_motion_notify_event_t *)event);
        break;
    case XCB_PROPERTY_NOTIFY:
        handle_property_notify((xcb_property_notify_event_t *)event);
        break;
    case XCB_ENTER_NOTIFY:
        handle_enter_notify((xcb_enter_notify_event_t *)event);
        break;
    case XCB_KEY_PRESS:
        handle_key_press((xcb_key_press_event_t *)event);
        break;
    case XCB_CLIENT_MESSAGE:
        handle_client_message((xcb_client_message_event_t *)event);
        break;
    case XCB_SELECTION_CLEAR: {
        xcb_selection_clear_event_t *ev = (xcb_selection_clear_event_t *)event;
        if (ev->owner == wm.sel_win && ev->selection == wm.sn_atom) {
            /* Replaced by --replace: let go of SubstructureRedirect right
             * away so the new WM's acquire_wm_selection() probe succeeds,
             * then unwind normally through cleanup(). */
            fprintf(stderr, "kiwm: replaced by another window manager, exiting\n");
            uint32_t mask = XCB_EVENT_MASK_NO_EVENT;
            xcb_change_window_attributes(wm.conn, wm.root, XCB_CW_EVENT_MASK, &mask);
            xcb_flush(wm.conn);
            wm.running = false;
        }
        break;
    }
    case XCB_EXPOSE: {
        xcb_expose_event_t *ev = (xcb_expose_event_t *)event;
        Client *c = find_client_window(ev->window);
        if (c) {
            draw_decoration(c);
            xcb_flush(wm.conn);
        }
        break;
    }
    default:
        break;
    }
}
