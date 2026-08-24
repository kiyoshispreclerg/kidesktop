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

/* Shared by handle_map_request (app remaps an already-managed window via
 * a fresh MapRequest -- can't happen normally since MapRequest only
 * fires for children of *root*, but kept for whatever unmanage/remanage
 * edge case might route back through here) and handle_map_notify (the
 * common real-world case: an app just calls XMapWindow directly on its
 * own already-reparented-into-our-frame window to re-show a popup it
 * kept around and only hid via XUnmapWindow -- e.g. Plasma's kickoff
 * menu toggling the same window every open/close instead of recreating
 * it. Root only redirects MapRequest for its own direct children, so
 * once a window is reparented into our frame, further XMapWindow calls
 * on it go straight through as a plain MapNotify, no MapRequest -- if we
 * only ever handled MapRequest, the frame (still unmapped from the
 * previous hide) would never come back, so the popup would appear to
 * work exactly once and then silently stop responding to its own toggle
 * until the app that owns it is restarted). */
static void remap_existing_client(Client *c)
{
    if (wm.outputs[c->output].desktop == c->desktop) {
        xcb_map_window(wm.conn, c->frame);
        c->mapped = true;
    }
    c->minimized = false;
    set_icccm_wm_state(c, WM_STATE_NORMAL);
    ewmh_update_wm_state(c);
}

static void handle_map_request(xcb_map_request_event_t *ev)
{
    Client *c = find_client_window(ev->window);
    if (!c)
        manage(ev->window);
    else
        remap_existing_client(c);
    xcb_flush(wm.conn);
}

static void handle_map_notify(xcb_map_notify_event_t *ev)
{
    Client *c = find_client_window(ev->window);
    if (c && ev->window == c->window && !c->mapped) {
        remap_existing_client(c);
        xcb_flush(wm.conn);
    }
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

/* Shared by both drag-start sites below (titlebar-click-move and
 * wm.mod_cycle/wm.mod_control-drag): detiles c first (see client.c's
 * detile_for_drag() -- no-op if already floating) so the whole rest of
 * the drag builds on a floating baseline from the very first motion
 * event, then captures wm.drag_start_x/y/w/h and grabs the pointer. mode
 * == DRAG_RESIZE additionally picks which corner grows from the press
 * position (nearest corner, kwin/compiz-style) -- the opposite corner
 * stays fixed for the whole resize (see handle_motion). */
static void begin_drag(Client *c, DragMode mode, xcb_button_press_event_t *ev)
{
    detile_for_drag(c, ev->root_x, ev->root_y);

    wm.drag_mode = mode;
    wm.drag_client = c;
    wm.drag_snap_side = SNAP_NONE;
    wm.drag_start_root_x = ev->root_x;
    wm.drag_start_root_y = ev->root_y;
    wm.drag_start_x = c->x;
    wm.drag_start_y = c->y;
    wm.drag_start_w = c->width;
    wm.drag_start_h = c->height;

    if (mode == DRAG_RESIZE) {
        wm.resize_right = (ev->root_x - c->x) > c->frame_width / 2;
        wm.resize_bottom = (ev->root_y - c->y) > c->frame_height / 2;
    }

    xcb_grab_pointer(wm.conn, 0, wm.root,
                     XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_POINTER_MOTION,
                     XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
                     XCB_NONE, XCB_NONE, XCB_CURRENT_TIME);
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
    bool on_titlebar = client_deco_visible(c) && rel_y >= 0 && rel_y < TITLEBAR_H;

    /* Scroll wheel over the titlebar (detail 4 = up, 5 = down) shades/
     * unshades -- X has no separate "scroll" event, wheel motion is just
     * ButtonPress with these detail values. */
    if (on_titlebar && (ev->detail == 4 || ev->detail == 5)) {
        toggle_shade(c, ev->detail == 4 ? 1 : 0);
        return;
    }

    if (on_titlebar && ev->detail == 1) {
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
        if (rel_x >= w - BUTTON_W * 4) {
            toggle_shade(c, -1);
            return;
        }

        /* Plain titlebar area (no button under the click): double-click
         * toggles maximize, same as most desktops -- X has no double-click
         * event of its own, so this compares consecutive ButtonPress
         * timestamps by hand (see wm.h's DOUBLE_CLICK_MS). */
        bool is_double = (c->frame == wm.last_titlebar_click_frame) &&
                         (xcb_timestamp_t)(ev->time - wm.last_titlebar_click_time) < DOUBLE_CLICK_MS;
        wm.last_titlebar_click_frame = c->frame;
        wm.last_titlebar_click_time = ev->time;
        if (is_double) {
            wm.last_titlebar_click_time = 0; /* consume -- don't chain into a triple-click */
            toggle_maximize(c, -1);
            return;
        }

        begin_drag(c, DRAG_MOVE, ev);
        return;
    }

    /* wm.mod_control-drag (any button, e.g. Meta by default) and
     * wm.mod_cycle-drag (e.g. Alt by default) both move the window with
     * the left button; either one with the right button resizes instead,
     * from whichever corner is nearest the click -- see kiwm.conf's
     * mod_cycle=/mod_control= keys. */
    if (ev->state & (wm.mod_cycle | wm.mod_control)) {
        if (ev->detail == 1)
            begin_drag(c, DRAG_MOVE, ev);
        else if (ev->detail == 3)
            begin_drag(c, DRAG_RESIZE, ev);
        return;
    }

    if (rel_y >= (client_deco_visible(c) ? TITLEBAR_H : 0)) {
        xcb_allow_events(wm.conn, XCB_ALLOW_REPLAY_POINTER, ev->time);
        xcb_flush(wm.conn);
    }
}

/* Windows7/kwin-style edge snap while dragging a window by its titlebar or
 * via mod_control-drag: the pointer getting within kiwm.conf's
 * snap_threshold= of an output workarea edge snaps the window there (top =
 * maximize, left/right = half-width); moving the pointer back out of that
 * zone before releasing the button restores the exact pre-drag floating
 * geometry, offset by however far the pointer has moved since -- so the
 * window keeps following the cursor as if it had never been snapped. Only
 * engages/disengages on a *change* of which edge (if any) the pointer is
 * currently within threshold of, so a snapped window doesn't jitter while
 * the pointer sits still inside the same edge zone. */
static bool try_edge_snap(Client *c, xcb_motion_notify_event_t *ev, int dx, int dy)
{
    if (wm.snap_threshold <= 0)
        return false;

    int output_idx = output_index_for_point(ev->root_x, ev->root_y);
    if (output_idx < 0)
        output_idx = c->output >= 0 ? c->output : 0;

    int wx, wy, ww, wh;
    compute_output_workarea(output_idx, &wx, &wy, &ww, &wh);

    SnapSide want;
    if (ev->root_y - wy <= wm.snap_threshold)
        want = SNAP_TOP;
    else if (ev->root_x - wx <= wm.snap_threshold)
        want = SNAP_LEFT;
    else if (wx + ww - ev->root_x <= wm.snap_threshold)
        want = SNAP_RIGHT;
    else
        want = SNAP_NONE;

    if (want == wm.drag_snap_side)
        return want != SNAP_NONE; /* already settled into this state (or none); nothing to do */

    wm.drag_snap_side = want;
    c->output = output_idx;

    if (want != SNAP_NONE) {
        /* Remember the true pre-drag floating geometry as the maximize
         * "restore" target too, so a later plain un-maximize (titlebar
         * button, Meta+Up) after this drag restores to it correctly
         * instead of to wherever the window happened to be mid-drag. */
        c->saved_x = wm.drag_start_x;
        c->saved_y = wm.drag_start_y;
        c->saved_w = wm.drag_start_w;
        c->saved_h = wm.drag_start_h;
    }

    switch (want) {
    case SNAP_TOP: {
        unshade_now(c);
        c->snap_side = SNAP_NONE;
        c->maximized = true;
        int bt, th;
        bool deco = client_deco_visible(c);
        th = deco ? TITLEBAR_H : 0;
        bt = deco ? wm.border_thickness : 0;
        c->x = wx;
        c->y = wy;
        c->width = ww - bt * 2;
        c->height = wh - th - bt;
        if (c->width < MIN_CLIENT_W) c->width = MIN_CLIENT_W;
        if (c->height < MIN_CLIENT_H) c->height = MIN_CLIENT_H;
        break;
    }
    case SNAP_LEFT:
    case SNAP_RIGHT:
        snap_client_to_side(c, want);
        break;
    case SNAP_NONE:
    default:
        unsnap_client(c, wm.drag_start_x + dx, wm.drag_start_y + dy,
                      wm.drag_start_w, wm.drag_start_h);
        break;
    }

    configure_frame(c);
    ewmh_update_wm_state(c);
    ewmh_update_frame_extents(c);
    xcb_flush(wm.conn);
    return true; /* transition (into or out of a snap) already fully handled above */
}

/* Tracks which titlebar button (if any) the pointer currently sits over,
 * for btns.png's hover row (see decoration.c's draw_button()) -- a no-op
 * whenever no button theme is loaded, so plain-fallback decoration
 * doesn't pay for tracking/repainting it never uses. */
static void update_button_hover(xcb_motion_notify_event_t *ev)
{
    if (!wm.deco_btns)
        return;

    Client *c = find_client_window(ev->event);
    int slot = -1;
    if (c && client_deco_visible(c)) {
        int rel_x = ev->root_x - c->x;
        int rel_y = ev->root_y - c->y;
        int w = c->frame_width;
        if (rel_y >= 0 && rel_y < TITLEBAR_H) {
            if (rel_x >= w - BUTTON_W)          slot = BTNSLOT_CLOSE;
            else if (rel_x >= w - BUTTON_W * 2)  slot = BTNSLOT_MAXIMIZE;
            else if (rel_x >= w - BUTTON_W * 3)  slot = BTNSLOT_MINIMIZE;
            else if (rel_x >= w - BUTTON_W * 4)  slot = BTNSLOT_SHADE;
        }
    }

    if (c == wm.hover_client && slot == wm.hover_btn)
        return;

    Client *old = wm.hover_client;
    wm.hover_client = (slot >= 0) ? c : NULL;
    wm.hover_btn = slot;

    if (old && old != wm.hover_client)
        draw_decoration(old);
    if (wm.hover_client)
        draw_decoration(wm.hover_client);
    xcb_flush(wm.conn);
}

static void handle_motion(xcb_motion_notify_event_t *ev)
{
    if (!wm.drag_client || wm.drag_mode == DRAG_NONE) {
        update_button_hover(ev);
        return;
    }

    Client *c = wm.drag_client;
    int dx = ev->root_x - wm.drag_start_root_x;
    int dy = ev->root_y - wm.drag_start_root_y;

    if (wm.drag_mode == DRAG_MOVE && try_edge_snap(c, ev, dx, dy))
        return; /* settled into a snapped state this motion event; nothing else to do */

    if (c->maximized)
        toggle_maximize(c, 0);
    c->snap_side = SNAP_NONE;

    if (wm.drag_mode == DRAG_MOVE) {
        c->x = wm.drag_start_x + dx;
        c->y = wm.drag_start_y + dy;
    } else {
        /* Resize from whichever corner was nearest the initial click
         * (wm.resize_right/resize_bottom, decided once in
         * handle_button_press's begin_drag()) -- the *opposite* corner
         * stays fixed: recompute x/y from the (possibly MIN_CLIENT_*-
         * clamped) new size so that fixed corner's absolute position
         * never drifts, kwin/compiz-style, instead of always anchoring
         * top-left and growing toward bottom-right regardless of which
         * corner was actually grabbed. */
        int new_w = wm.resize_right ? wm.drag_start_w + dx : wm.drag_start_w - dx;
        int new_h = wm.resize_bottom ? wm.drag_start_h + dy : wm.drag_start_h - dy;
        if (new_w < MIN_CLIENT_W) new_w = MIN_CLIENT_W;
        if (new_h < MIN_CLIENT_H) new_h = MIN_CLIENT_H;

        c->width = new_w;
        c->height = new_h;
        if (!wm.resize_right)
            c->x = wm.drag_start_x + (wm.drag_start_w - new_w);
        if (!wm.resize_bottom)
            c->y = wm.drag_start_y + (wm.drag_start_h - new_h);
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
            ewmh_update_wm_output(c);
        }
        xcb_ungrab_pointer(wm.conn, XCB_CURRENT_TIME);
        wm.drag_client = NULL;
        wm.drag_mode = DRAG_NONE;
        wm.drag_snap_side = SNAP_NONE;
        xcb_flush(wm.conn);
    }
}

static void handle_property_notify(xcb_property_notify_event_t *ev)
{
    Client *c = find_client_window(ev->window);
    if (!c) {
        if ((ev->atom == wm.atoms.net_wm_strut || ev->atom == wm.atoms.net_wm_strut_partial) &&
            dock_refresh_strut(ev->window))
            xcb_flush(wm.conn);
        return;
    }

    if (ev->atom == wm.atoms.net_wm_name || ev->atom == XCB_ATOM_WM_NAME) {
        get_title(c);
        draw_decoration(c);
        xcb_flush(wm.conn);
    }
}

static void handle_enter_notify(xcb_enter_notify_event_t *ev)
{
    if (!wm.focus_follows_mouse)
        return;
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
    uint16_t clean_cycle = mods & (wm.mod_cycle | XCB_MOD_MASK_SHIFT);
    uint16_t clean_control = mods & (wm.mod_control | XCB_MOD_MASK_SHIFT);

    /* mod_cycle+Tab / mod_cycle+Shift+Tab (Alt by default): cycle focused window.
     * mod_control+Tab / mod_control+Shift+Tab (Meta by default): cycle the
     * focused output's virtual desktop. */
    if (ev->detail == wm.key_tab) {
        if (clean_cycle == (uint16_t)(wm.mod_cycle | XCB_MOD_MASK_SHIFT))     { cycle_focus(-1); return; }
        if (clean_cycle == wm.mod_cycle)                                     { cycle_focus(+1); return; }
        if (clean_control == (uint16_t)(wm.mod_control | XCB_MOD_MASK_SHIFT)) { cycle_output_desktop(-1); return; }
        if (clean_control == wm.mod_control)                                 { cycle_output_desktop(+1); return; }
        return;
    }

    /* mod_control+Up (Meta by default): maximize/restore the focused window. */
    if (ev->detail == wm.key_up && clean_control == wm.mod_control) {
        if (wm.focused)
            toggle_maximize(wm.focused, -1);
        return;
    }

    if (clean_cycle == wm.mod_cycle) {
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
    bool is_shaded = (a1 == wm.atoms.net_wm_state_shaded || a2 == wm.atoms.net_wm_state_shaded);

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
    if (is_shaded) {
        int want = (action == 2) ? -1 : (action == 1 ? 1 : 0);
        toggle_shade(c, want);
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
    case XCB_MAP_NOTIFY:
        handle_map_notify((xcb_map_notify_event_t *)event);
        break;
    case XCB_CONFIGURE_REQUEST:
        handle_configure_request((xcb_configure_request_event_t *)event);
        break;
    case XCB_DESTROY_NOTIFY: {
        xcb_destroy_notify_event_t *ev = (xcb_destroy_notify_event_t *)event;
        Client *c = find_client_window(ev->window);
        if (c) {
            unmanage(c);
        } else {
            dock_forget(ev->window);
            /* Avoid chaining the next _NET_WM_WINDOW_TYPE_DESKTOP window
             * (see client.c's manage()) above a now-destroyed sibling --
             * that ConfigureWindow would just fail with BadWindow and
             * leave the new window unstacked (back to the original bug). */
            if (wm.last_desktop_window == ev->window)
                wm.last_desktop_window = XCB_NONE;
        }
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
            if (c->shaded) {
                /* Our own toggle_shade() unmapping the content window on
                 * purpose (c->shaded is already true by the time this
                 * event arrives, since toggle_shade sets it before
                 * unmapping) -- not a real withdrawal, must NOT also
                 * unmap the frame or mark the client unmapped, or the
                 * whole window (decoration included) vanishes and stays
                 * that way until something unrelated (e.g. a taskbar
                 * minimize+restore) happens to remap the frame again. */
                break;
            }
            if (c->mapped && wm.outputs[c->output].desktop == c->desktop && !c->minimized) {
                c->mapped = false;
                xcb_unmap_window(wm.conn, c->frame);
                xcb_flush(wm.conn);
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
    case XCB_LEAVE_NOTIFY: {
        /* Motion stops firing once the pointer leaves the frame entirely,
         * so button hover state (see update_button_hover()) needs its own
         * clear here or it'd stay stuck highlighted after the pointer
         * moves away. */
        xcb_leave_notify_event_t *ev = (xcb_leave_notify_event_t *)event;
        if (wm.hover_client && ev->event == wm.hover_client->frame) {
            Client *old = wm.hover_client;
            wm.hover_client = NULL;
            wm.hover_btn = -1;
            draw_decoration(old);
            xcb_flush(wm.conn);
        }
        break;
    }
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
