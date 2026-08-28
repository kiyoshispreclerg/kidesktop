/* X event dispatch: the WM's whole runtime behavior after startup lives
 * here, delegating actual state changes to client.c/output.c/ewmh.c. */
#include "events.h"
#include "wm.h"
#include "client.h"
#include "output.h"
#include "decoration.h"
#include "ewmh.h"
#include "keybind.h"
#include "osd.h"
#include "shape.h"

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
    if (c->sticky || wm.outputs[c->output].desktop == c->desktop) {
        xcb_map_window(wm.conn, c->frame);
        c->mapped = true;
    }
    c->minimized = false;
    set_icccm_wm_state(c, WM_STATE_NORMAL);
    ewmh_update_wm_state(c);
    /* Mapping a frame doesn't restack it, so a window coming back after
     * being hidden reappears at whatever stacking position it was left in
     * -- which for a transient that was hidden while its parent got raised
     * (VirtualBox's auto-hiding mini-toolbar, exactly) means underneath the
     * very window it's supposed to float over. */
    restack_all();
}

static void handle_map_request(xcb_map_request_event_t *ev)
{
    Client *c = find_client_window(ev->window);
    if (!c)
        manage(ev->window, true);
    else
        remap_existing_client(c);
    xcb_flush(wm.conn);
}

static void handle_map_notify(xcb_map_notify_event_t *ev)
{
    Client *c = find_client_window(ev->window);
    if (c && ev->window == c->window && c->ignore_map > 0) {
        /* The server's own re-map at the end of a reparent, not the app
         * asking to be shown -- see wm.h's Client::ignore_map. */
        c->ignore_map--;
        return;
    }
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

    /* `c` is only ever found here for an *already-reparented* window (see
     * client.c's manage(): the frame's own SubstructureRedirect is what
     * makes this fire at all, and that's only selected on a frame that
     * already has the client reparented into it -- there's no window where
     * find_client_window() could match a still-child-of-root window). So
     * ev->x/ev->y are parent-relative to the *frame*, but the app that
     * issued this ConfigureWindow on its own (reparented, from its own
     * point of view still-a-root-child per ICCCM's transparency
     * requirement) window meant them as its own absolute *screen* position
     * -- i.e. where it wants its *content*, not the frame, to end up.
     * Converting that back to c->x/c->y (which this whole codebase treats
     * as the frame's own top-left) needs subtracting the same insets
     * deco_insets() everywhere else adds going the other way. Getting this
     * backwards (assigning ev->x/y to c->x/c->y directly, as if they were
     * already frame-relative) is exactly the "content displaced by
     * whatever the window's old position used to be, cut off in a corner"
     * bug this fixes -- width/height need no such translation, since
     * those the app really is requesting for its own content, unaffected
     * by insets. */
    int bt, th;
    deco_insets(c, &bt, &th);

    /* Same ICCCM gravity rule manage() applies when it first frames a
     * window (see client.c): with StaticGravity -- what every Qt/GTK
     * window asks for, and the case that made this matter -- the position
     * names where the *content* should end up, so the frame goes above and
     * left of it by the decoration insets. Under NorthWest (the default
     * for anything that doesn't ask) the position names the frame itself,
     * and no translation applies. */
    int gx = (c->gravity == XCB_GRAVITY_STATIC) ? bt : 0;
    int gy = (c->gravity == XCB_GRAVITY_STATIC) ? th : 0;

    if (ev->value_mask & XCB_CONFIG_WINDOW_X)      c->x = ev->x - gx;
    if (ev->value_mask & XCB_CONFIG_WINDOW_Y)      c->y = ev->y - gy;
    if (ev->value_mask & XCB_CONFIG_WINDOW_WIDTH)  c->width = ev->width < c->min_w ? c->min_w : ev->width;
    if (ev->value_mask & XCB_CONFIG_WINDOW_HEIGHT) c->height = ev->height < c->min_h ? c->min_h : ev->height;

    configure_frame(c);

    /* A plain XRaiseWindow/XLowerWindow arrives here too, and used to be
     * dropped on the floor -- an app asking to be raised (VirtualBox's
     * mini-toolbar does exactly that when it slides back into view) simply
     * never was. Applied to the *frame*, since that's what actually sits in
     * the root's stacking order, and then handed to restack_all() so the
     * request still lands within whatever layer the client belongs to
     * rather than jumping the whole stack. The requested sibling is
     * deliberately ignored: it's expressed in terms of the client's
     * would-be siblings, which under a reparenting WM aren't the frame's. */
    if (ev->value_mask & XCB_CONFIG_WINDOW_STACK_MODE) {
        uint32_t mode = ev->stack_mode;
        if (mode == XCB_STACK_MODE_ABOVE || mode == XCB_STACK_MODE_TOP_IF ||
            mode == XCB_STACK_MODE_BELOW || mode == XCB_STACK_MODE_BOTTOM_IF) {
            uint32_t v = (mode == XCB_STACK_MODE_ABOVE || mode == XCB_STACK_MODE_TOP_IF)
                             ? XCB_STACK_MODE_ABOVE : XCB_STACK_MODE_BELOW;
            xcb_configure_window(wm.conn, c->frame, XCB_CONFIG_WINDOW_STACK_MODE, &v);
            restack_all();
        }
    }

    xcb_flush(wm.conn);
}

/* Populates wm.resize_neighbors_x/y and wm.resize_edge_x/y_start for a
 * DRAG_RESIZE about to start on `c`, once wm.resize_right/resize_bottom
 * are already decided (see begin_drag(), which calls this right after) --
 * see wm.h's ResizeNeighbor for the overall idea. Scans every other
 * mapped, visible client on c's own output (same-output only, unlike
 * magnet_snap_move()'s cross-output candidates -- resizing a neighbor only
 * makes sense against a window that could plausibly share the same
 * screen) whose frame edge sits within RESIZE_NEIGHBOR_EPSILON_PX of the
 * specific edge about to be dragged, gated by the same perpendicular-
 * overlap test magnet_snap_move()/_resize() use. wm.resize_right/
 * resize_bottom already say which *of our own* edges is moving, so which
 * of the *neighbor's* edges could plausibly be the touching one follows
 * directly -- a neighbor a resize_right drag could touch sits to our
 * right, so only its left edge is tested (never its right edge, which
 * would mean it's overlapping us, not adjacent to us). */
static void detect_resize_neighbors(Client *c)
{
    wm.resize_neighbors_x_count = 0;
    wm.resize_neighbors_y_count = 0;

    int moving_x = wm.resize_right  ? (c->x + c->frame_width)  : c->x;
    int moving_y = wm.resize_bottom ? (c->y + c->frame_height) : c->y;
    wm.resize_edge_x_start = moving_x;
    wm.resize_edge_y_start = moving_y;

    for (Client *o2 = wm.clients; o2; o2 = o2->next) {
        if (o2 == c || !o2->mapped || o2->minimized || o2->shaded || o2->output != c->output)
            continue;
        if (!o2->sticky && wm.outputs[o2->output].desktop != o2->desktop)
            continue;

        int rl = o2->x, rr = o2->x + o2->frame_width;
        int rt = o2->y, rb = o2->y + o2->frame_height;

        bool y_overlap = (c->y + c->frame_height > rt) && (c->y < rb);
        if (y_overlap && wm.resize_neighbors_x_count < MAX_RESIZE_NEIGHBORS) {
            int edge = wm.resize_right ? rl : rr;
            int d = edge - moving_x;
            if ((d < 0 ? -d : d) <= RESIZE_NEIGHBOR_EPSILON_PX) {
                ResizeNeighbor *n = &wm.resize_neighbors_x[wm.resize_neighbors_x_count++];
                n->client = o2;
                n->orig_x = o2->x; n->orig_y = o2->y;
                n->orig_w = o2->width; n->orig_h = o2->height;
            }
        }

        bool x_overlap = (c->x + c->frame_width > rl) && (c->x < rr);
        if (x_overlap && wm.resize_neighbors_y_count < MAX_RESIZE_NEIGHBORS) {
            int edge = wm.resize_bottom ? rt : rb;
            int d = edge - moving_y;
            if ((d < 0 ? -d : d) <= RESIZE_NEIGHBOR_EPSILON_PX) {
                ResizeNeighbor *n = &wm.resize_neighbors_y[wm.resize_neighbors_y_count++];
                n->client = o2;
                n->orig_x = o2->x; n->orig_y = o2->y;
                n->orig_w = o2->width; n->orig_h = o2->height;
            }
        }
    }
}

/* Whether a DRAG_RESIZE about to start on `c` should preserve its half-snap
 * state instead of detiling back to its pre-snap floating size (see wm.h's
 * KiWM::drag_preserve_snap) -- true only when link_resize_neighbors is on,
 * c is currently half-snapped (Client::snap_side LEFT/RIGHT -- SNAP_TOP/
 * maximized never qualifies, there's no "other half" to preserve against),
 * the click landed on c's *shared* edge (the one touching its counterpart
 * -- e.g. a SNAP_LEFT window's right edge), and there's actually a same-
 * output neighbor half-snapped to the complementary side still touching
 * that edge. Must be checked *before* client.c's detile_for_drag() runs --
 * once it does, c->snap_side is already cleared and there's nothing left
 * to check. */
static bool should_preserve_snap_resize(Client *c, xcb_button_press_event_t *ev)
{
    if (!wm.link_resize_neighbors || (c->snap_side != SNAP_LEFT && c->snap_side != SNAP_RIGHT))
        return false;

    bool resize_right = (ev->root_x - c->x) > c->frame_width / 2;
    bool shared_edge = (c->snap_side == SNAP_LEFT && resize_right) ||
                       (c->snap_side == SNAP_RIGHT && !resize_right);
    if (!shared_edge)
        return false;

    SnapSide want_side = (c->snap_side == SNAP_LEFT) ? SNAP_RIGHT : SNAP_LEFT;
    int edge = resize_right ? (c->x + c->frame_width) : c->x;

    for (Client *o2 = wm.clients; o2; o2 = o2->next) {
        if (o2 == c || !o2->mapped || o2->minimized || o2->output != c->output)
            continue;
        if (o2->snap_side != want_side)
            continue;
        if (!o2->sticky && wm.outputs[o2->output].desktop != o2->desktop)
            continue;
        int other_edge = resize_right ? o2->x : (o2->x + o2->frame_width);
        int d = other_edge - edge;
        if ((d < 0 ? -d : d) <= RESIZE_NEIGHBOR_EPSILON_PX)
            return true;
    }
    return false;
}

/* Shared by both drag-start sites below (titlebar-click-move and
 * wm.mod_cycle/wm.mod_control-drag): detiles c first (see client.c's
 * detile_for_drag() -- no-op if already floating, and skipped entirely
 * when should_preserve_snap_resize() says so) so the whole rest of the
 * drag builds on a floating baseline from the very first motion event,
 * then captures wm.drag_start_x/y/w/h and grabs the pointer. mode ==
 * DRAG_RESIZE additionally picks which corner grows from the press
 * position (nearest corner, kwin/compiz-style) -- the opposite corner
 * stays fixed for the whole resize (see handle_motion). */
static void begin_drag(Client *c, DragMode mode, xcb_button_press_event_t *ev)
{
    /* A window that declares it can't be moved or resized (Motif's
     * functions field, or a fixed min==max size -- see client.c's
     * update_client_actions()) doesn't get dragged either, whether the
     * drag started on its titlebar or via a modifier from anywhere on it. */
    if (mode == DRAG_MOVE && !c->allow_move)
        return;
    if (mode == DRAG_RESIZE && !c->allow_resize)
        return;

    wm.drag_preserve_snap = (mode == DRAG_RESIZE) && should_preserve_snap_resize(c, ev);
    if (!wm.drag_preserve_snap)
        detile_for_drag(c, ev->root_x, ev->root_y);

    wm.drag_mode = mode;
    wm.drag_client = c;
    wm.drag_snap_side = SNAP_NONE;
    wm.last_drag_apply_ms = 0; /* don't let a previous drag's timestamp throttle this new one's first frame */
    wm.drag_start_root_x = ev->root_x;
    wm.drag_start_root_y = ev->root_y;
    wm.drag_start_x = c->x;
    wm.drag_start_y = c->y;
    wm.drag_start_w = c->width;
    wm.drag_start_h = c->height;

    xcb_cursor_t cursor;
    if (mode == DRAG_RESIZE) {
        wm.resize_right = (ev->root_x - c->x) > c->frame_width / 2;
        wm.resize_bottom = (ev->root_y - c->y) > c->frame_height / 2;
        /* Corner nearest the click (same one that stays fixed's opposite,
         * see handle_motion) picks the matching diagonal resize cursor. */
        if (wm.resize_right)
            cursor = wm.resize_bottom ? wm.cursor_resize_se : wm.cursor_resize_ne;
        else
            cursor = wm.resize_bottom ? wm.cursor_resize_sw : wm.cursor_resize_nw;
        if (wm.link_resize_neighbors)
            detect_resize_neighbors(c);
        else {
            wm.resize_neighbors_x_count = 0;
            wm.resize_neighbors_y_count = 0;
        }
    } else {
        cursor = wm.cursor_move;
        wm.resize_neighbors_x_count = 0;
        wm.resize_neighbors_y_count = 0;
    }

    xcb_grab_pointer(wm.conn, 0, wm.root,
                     XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_POINTER_MOTION,
                     XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
                     XCB_NONE, cursor, XCB_CURRENT_TIME);
    xcb_flush(wm.conn);
}

/* Which configured titlebar element (see wm.h's DecoElemKind/
 * wm.deco_layout) sits at a given rel_x, if any -- shared by
 * handle_button_press's hit-testing and update_button_hover(), so they
 * can never disagree about where a button actually is. Fills `slots` with
 * the whole computed layout (caller may only care about the one index
 * returned, but computing the rest is nearly free and callers that do
 * care can just index it). */
static int deco_slot_at(Client *c, int rel_x, DecoSlot *slots, int max_slots)
{
    int n = compute_deco_layout(c, c->frame_width, slots, max_slots);
    for (int i = 0; i < n; i++)
        if (rel_x >= slots[i].x && rel_x < slots[i].x + slots[i].width)
            return i;
    return -1;
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
        DecoSlot slots[MAX_DECO_ELEMS];
        int idx = deco_slot_at(c, rel_x, slots, MAX_DECO_ELEMS);
        if (idx >= 0) {
            switch (slots[idx].kind) {
            case DECO_CLOSE:            close_client(c); return;
            case DECO_MAXIMIZE:         toggle_maximize(c, -1); return;
            case DECO_MINIMIZE:         minimize_client(c); return;
            case DECO_SHADE:            toggle_shade(c, -1); return;
            case DECO_KEEP_ABOVE:       toggle_keep_above(c, -1); return;
            case DECO_KEEP_ALL_DESKTOPS: toggle_sticky(c, -1); return;
            case DECO_TITLE:
            case DECO_ICON:
            default:
                break; /* not a button -- falls through to double-click/drag below */
            }
        }

        /* Plain titlebar area (icon/title, or no element under the
         * click): double-click
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
        if (c->width < c->min_w) c->width = c->min_w;
        if (c->height < c->min_h) c->height = c->min_h;
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
        if (rel_y >= 0 && rel_y < TITLEBAR_H) {
            DecoSlot slots[MAX_DECO_ELEMS];
            int idx = deco_slot_at(c, rel_x, slots, MAX_DECO_ELEMS);
            if (idx >= 0 && slots[idx].kind != DECO_TITLE && slots[idx].kind != DECO_ICON)
                slot = idx;
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

/* Nudges *edge toward cand if they're within wm.magnet_threshold and cand is
 * the closest candidate seen so far (tracked via best_delta/best_abs,
 * which the caller seeds to {0, wm.magnet_threshold + 1} so "nothing found"
 * naturally means "no delta applied"). Shared by both axes in
 * magnet_snap(). */
static void magnet_consider(int edge, int cand, int *best_delta, int *best_abs)
{
    int d = cand - edge;
    int ad = d < 0 ? -d : d;
    if (ad <= wm.magnet_threshold && ad < *best_abs) {
        *best_abs = ad;
        *best_delta = d;
    }
}

/* Upper bound for gather_magnet_edges_x/y()'s output buffer: the output's
 * own 2 screen edges, plus 2 per dock and 2 per client (left+right or
 * top+bottom). */
#define MAGNET_MAX_EDGES (2 + MAX_DOCKS * 2 + MAX_CLIENTS * 2)

/* Collects every edge coordinate (on the X axis; gather_magnet_edges_y()
 * below is the Y-axis mirror) that a window whose current frame spans
 * [ry, ry+rh) on the Y axis could plausibly snap against on this output:
 * the output's own left/right screen edges (always eligible -- they span
 * the whole output, so no Y-overlap gating needed), every dock/panel/
 * taskbar-type window's left/right edges *but only on this same output*
 * (a panel on a different monitor is irrelevant to a window that isn't
 * there -- unlike plain Clients below, which count regardless of output,
 * a dock's edges only ever matter to windows actually sharing its
 * screen), and every other visible Client's left/right edges (any client,
 * *any* output -- two windows on neighboring monitors can still plausibly
 * be lined up against each other, e.g. dragged across the boundary). A
 * dock or client's edges only make it in when its Y span actually
 * overlaps [ry, ry+rh) -- otherwise sharing an X coordinate is
 * coincidence, not two things that could be touching side by side.
 * `skip` excludes the dragged client itself from the Client scan. Returns
 * the number of edges written to `out` (caller-sized, see
 * MAGNET_MAX_EDGES). */
static int gather_magnet_edges_x(Client *skip, int for_output, int ry, int rh, int *out)
{
    int n = 0;

    if (for_output >= 0 && for_output < wm.output_count) {
        XisOutput *o = &wm.outputs[for_output];
        out[n++] = o->x;
        out[n++] = o->x + o->width;
    }

    for (int i = 0; i < wm.dock_count; i++) {
        DockWindow *d = &wm.docks[i];
        if (d->output != for_output || d->width <= 0 || d->height <= 0)
            continue;
        if (ry + rh > d->y && ry < d->y + d->height) {
            out[n++] = d->x;
            out[n++] = d->x + d->width;
        }
    }

    for (Client *o2 = wm.clients; o2; o2 = o2->next) {
        if (o2 == skip || !o2->mapped || o2->minimized)
            continue;
        if (!o2->sticky && wm.outputs[o2->output].desktop != o2->desktop)
            continue;
        if (ry + rh > o2->y && ry < o2->y + o2->frame_height) {
            out[n++] = o2->x;
            out[n++] = o2->x + o2->frame_width;
        }
    }

    return n;
}

/* Y-axis mirror of gather_magnet_edges_x() -- see its comment. */
static int gather_magnet_edges_y(Client *skip, int for_output, int rx, int rw, int *out)
{
    int n = 0;

    if (for_output >= 0 && for_output < wm.output_count) {
        XisOutput *o = &wm.outputs[for_output];
        out[n++] = o->y;
        out[n++] = o->y + o->height;
    }

    for (int i = 0; i < wm.dock_count; i++) {
        DockWindow *d = &wm.docks[i];
        if (d->output != for_output || d->width <= 0 || d->height <= 0)
            continue;
        if (rx + rw > d->x && rx < d->x + d->width) {
            out[n++] = d->y;
            out[n++] = d->y + d->height;
        }
    }

    for (Client *o2 = wm.clients; o2; o2 = o2->next) {
        if (o2 == skip || !o2->mapped || o2->minimized)
            continue;
        if (!o2->sticky && wm.outputs[o2->output].desktop != o2->desktop)
            continue;
        if (rx + rw > o2->x && rx < o2->x + o2->frame_width) {
            out[n++] = o2->y;
            out[n++] = o2->y + o2->frame_height;
        }
    }

    return n;
}

/* Window-to-window and window-to-screen magnetic edge snapping while
 * *moving* a window by its titlebar/mod-drag (kiwm.conf's
 * magnet_threshold=, default 10px, 0 disables) -- distinct from
 * try_edge_snap() above: that's a screen-edge *tiling* snap (maximize/
 * half-width, a whole different geometry engaging Client::maximized/
 * snap_side); this just nudges x/y a few pixels so the dragged window's
 * frame ends up touching another one exactly, instead of stopping just
 * short or just past it. Each axis snaps independently to whichever
 * nearby edge (of either side -- moving a window can snap its left *or*
 * right edge against the same candidate) is closest. `fw`/`fh` are the
 * dragged client's current frame size (unchanged during a move, so the
 * caller's already-computed c->frame_width/frame_height are exactly
 * right). */
static void magnet_snap_move(Client *c, int *x, int *y, int fw, int fh)
{
    if (wm.magnet_threshold <= 0 || c->output < 0 || c->output >= wm.output_count)
        return;

    int my_left = *x, my_right = *x + fw;
    int my_top = *y, my_bottom = *y + fh;

    int edges[MAGNET_MAX_EDGES];
    int n, i;

    int best_dx = 0, best_dx_abs = wm.magnet_threshold + 1;
    n = gather_magnet_edges_x(c, c->output, my_top, fh, edges);
    for (i = 0; i < n; i++) {
        magnet_consider(my_left,  edges[i], &best_dx, &best_dx_abs);
        magnet_consider(my_right, edges[i], &best_dx, &best_dx_abs);
    }

    int best_dy = 0, best_dy_abs = wm.magnet_threshold + 1;
    n = gather_magnet_edges_y(c, c->output, my_left, fw, edges);
    for (i = 0; i < n; i++) {
        magnet_consider(my_top,    edges[i], &best_dy, &best_dy_abs);
        magnet_consider(my_bottom, edges[i], &best_dy, &best_dy_abs);
    }

    if (best_dx_abs <= wm.magnet_threshold)
        *x += best_dx;
    if (best_dy_abs <= wm.magnet_threshold)
        *y += best_dy;
}

/* Same idea as magnet_snap_move(), but for a *resize* drag: only the one
 * edge actually being dragged (wm.resize_right/resize_bottom -- the same
 * flags the rest of the resize math already uses) is eligible to snap; the
 * anchored opposite corner never moves, so testing it against candidates
 * the way magnet_snap_move() tests both sides of a moving window wouldn't
 * make sense here. Adjusts new_w/new_h (content size, pre-min-clamp) in
 * place -- since the anchored edge never moves, shifting just the dragged
 * edge by `delta` changes the content size by exactly `delta` too, however
 * much decoration inset separates content from frame. `bt`/`th` (from
 * client.c's deco_insets()) are only needed to locate the dragged edge's
 * *current* absolute frame position for comparison against candidates,
 * which are already expressed in frame coordinates the same way every
 * other Client/dock/screen edge is. */
static void magnet_snap_resize(Client *c, int *new_w, int *new_h, int bt, int th)
{
    if (wm.magnet_threshold <= 0 || c->output < 0 || c->output >= wm.output_count)
        return;

    int frame_w = *new_w + bt * 2;
    int frame_h = c->shaded ? th : (*new_h + th + bt);
    int frame_left = wm.resize_right ? c->x : (wm.drag_start_x + wm.drag_start_w + bt * 2 - frame_w);
    int frame_top  = wm.resize_bottom ? c->y : (wm.drag_start_y + wm.drag_start_h + th + bt - frame_h);

    int moving_x = wm.resize_right ? (frame_left + frame_w) : frame_left;
    int moving_y = wm.resize_bottom ? (frame_top + frame_h) : frame_top;

    int edges[MAGNET_MAX_EDGES];
    int n, i;

    int best_dx = 0, best_dx_abs = wm.magnet_threshold + 1;
    n = gather_magnet_edges_x(c, c->output, frame_top, frame_h, edges);
    for (i = 0; i < n; i++)
        magnet_consider(moving_x, edges[i], &best_dx, &best_dx_abs);

    int best_dy = 0, best_dy_abs = wm.magnet_threshold + 1;
    n = gather_magnet_edges_y(c, c->output, frame_left, frame_w, edges);
    for (i = 0; i < n; i++)
        magnet_consider(moving_y, edges[i], &best_dy, &best_dy_abs);

    if (best_dx_abs <= wm.magnet_threshold)
        *new_w += wm.resize_right ? best_dx : -best_dx;
    if (best_dy_abs <= wm.magnet_threshold)
        *new_h += wm.resize_bottom ? best_dy : -best_dy;
}

/* Recomputes every registered resize-neighbor's geometry (wm.resize_
 * neighbors_x/y) from the total displacement of the dragged client's own
 * moving edge since detect_resize_neighbors() captured it -- see wm.h's
 * ResizeNeighbor comment. Only touches each neighbor Client's own x/y/
 * width/height fields; the caller still has to push that to the X server
 * per neighbor the same way it does for the dragged client itself
 * (apply_frame_geometry(), then the throttled apply_rounded_shape()/
 * draw_decoration() pair). `bt`/`th` are the *dragged* client's own insets
 * (the caller already has them, from deco_insets(), for
 * magnet_snap_resize()) -- each neighbor's own insets are looked up
 * individually since a neighbor's decoration state needn't match the
 * dragged client's. */
static void update_resize_neighbors(Client *c, int bt, int th)
{
    if (wm.resize_neighbors_x_count > 0) {
        int cur_x = wm.resize_right ? (c->x + c->width + bt * 2) : c->x;
        int delta = cur_x - wm.resize_edge_x_start;

        for (int i = 0; i < wm.resize_neighbors_x_count; i++) {
            ResizeNeighbor *n = &wm.resize_neighbors_x[i];
            Client *nc = n->client;
            int nbt, nth;
            deco_insets(nc, &nbt, &nth);

            int new_w;
            if (wm.resize_right) {
                /* Neighbor's left edge (the touching one) follows our
                 * moving right edge; its right edge is the anchor and
                 * never moves. */
                int anchor_right = n->orig_x + n->orig_w + nbt * 2;
                int new_left = n->orig_x + delta;
                new_w = (anchor_right - new_left) - nbt * 2;
                if (new_w < nc->min_w) new_w = nc->min_w;
                nc->x = anchor_right - (new_w + nbt * 2);
            } else {
                int anchor_left = n->orig_x;
                int new_right = n->orig_x + n->orig_w + nbt * 2 + delta;
                new_w = (new_right - anchor_left) - nbt * 2;
                if (new_w < nc->min_w) new_w = nc->min_w;
                nc->x = anchor_left;
            }
            nc->width = new_w;
        }
    }

    if (wm.resize_neighbors_y_count > 0) {
        int cur_y = wm.resize_bottom ? (c->y + c->height + th + bt) : c->y;
        int delta = cur_y - wm.resize_edge_y_start;

        for (int i = 0; i < wm.resize_neighbors_y_count; i++) {
            ResizeNeighbor *n = &wm.resize_neighbors_y[i];
            Client *nc = n->client;
            int nbt, nth;
            deco_insets(nc, &nbt, &nth);

            int new_h;
            if (wm.resize_bottom) {
                int anchor_bottom = n->orig_y + n->orig_h + nth + nbt;
                int new_top = n->orig_y + delta;
                new_h = (anchor_bottom - new_top) - nth - nbt;
                if (new_h < nc->min_h) new_h = nc->min_h;
                nc->y = anchor_bottom - (new_h + nth + nbt);
            } else {
                int anchor_top = n->orig_y;
                int new_bottom = n->orig_y + n->orig_h + nth + nbt + delta;
                new_h = (new_bottom - anchor_top) - nth - nbt;
                if (new_h < nc->min_h) new_h = nc->min_h;
                nc->y = anchor_top;
            }
            nc->height = new_h;
        }
    }
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
    /* A resize preserving a half-snap's state (wm.drag_preserve_snap, see
     * begin_drag()'s should_preserve_snap_resize()) must keep
     * Client::snap_side set for the whole drag -- clearing it here (like
     * every other drag does) is exactly the detile this feature exists to
     * skip. Only ever true for DRAG_RESIZE; a plain drag still clears it
     * every event same as before. */
    if (!wm.drag_preserve_snap)
        c->snap_side = SNAP_NONE;

    if (wm.drag_mode == DRAG_MOVE) {
        c->x = wm.drag_start_x + dx;
        c->y = wm.drag_start_y + dy;
        magnet_snap_move(c, &c->x, &c->y, c->frame_width, c->frame_height);
    } else {
        /* Resize from whichever corner was nearest the initial click
         * (wm.resize_right/resize_bottom, decided once in
         * handle_button_press's begin_drag()) -- the *opposite* corner
         * stays fixed: recompute x/y from the (possibly c->min_w/min_h-
         * clamped) new size so that fixed corner's absolute position
         * never drifts, kwin/compiz-style, instead of always anchoring
         * top-left and growing toward bottom-right regardless of which
         * corner was actually grabbed. */
        int new_w = wm.resize_right ? wm.drag_start_w + dx : wm.drag_start_w - dx;
        int new_h = wm.resize_bottom ? wm.drag_start_h + dy : wm.drag_start_h - dy;

        int bt, th;
        deco_insets(c, &bt, &th);
        magnet_snap_resize(c, &new_w, &new_h, bt, th);

        if (new_w < c->min_w) new_w = c->min_w;
        if (new_h < c->min_h) new_h = c->min_h;

        c->width = new_w;
        c->height = new_h;
        if (!wm.resize_right)
            c->x = wm.drag_start_x + (wm.drag_start_w - new_w);
        if (!wm.resize_bottom)
            c->y = wm.drag_start_y + (wm.drag_start_h - new_h);

        update_resize_neighbors(c, bt, th);
    }

    /* The window's own outline (frame + content geometry) tracks the
     * pointer on every single motion event, uncapped -- this is cheap
     * (a couple of xcb_configure_window() calls, no drawing), so there's
     * no reason to let it lag behind input the way the old code did by
     * throttling this together with the expensive part below. This is
     * what makes kiwm's move/resize track the mouse as immediately as
     * kwin's uncomposited opaque move/resize instead of visibly stepping
     * at the display's refresh rate. */
    apply_frame_geometry(c);
    for (int i = 0; i < wm.resize_neighbors_x_count; i++)
        apply_frame_geometry(wm.resize_neighbors_x[i].client);
    for (int i = 0; i < wm.resize_neighbors_y_count; i++)
        apply_frame_geometry(wm.resize_neighbors_y[i].client);

    /* The *painted* chrome -- rounded-corner XShape re-clip and the
     * off-screen decoration repaint (title, buttons, border) -- is capped
     * to this client's own output's refresh rate, independent of how often
     * the input device reports motion: see DRAG_REDRAW_FALLBACK_MS. There's
     * no point re-painting faster than the display can show it, and unlike
     * the geometry above, painting isn't free. Skipping it here just defers
     * catching the chrome up until the next due event, or until
     * handle_button_release()'s unconditional final apply if the drag ends
     * first -- the window itself already has the right size/position by
     * then regardless. */
    double interval_ms = DRAG_REDRAW_FALLBACK_MS;
    if (c->output >= 0 && c->output < wm.output_count && wm.outputs[c->output].refresh_hz > 0)
        interval_ms = 1000.0 / wm.outputs[c->output].refresh_hz;

    double now = monotonic_ms();
    if (now - wm.last_drag_apply_ms < interval_ms) {
        xcb_flush(wm.conn);
        return;
    }
    wm.last_drag_apply_ms = now;

    shape_update_frame(c);
    draw_decoration(c);
    for (int i = 0; i < wm.resize_neighbors_x_count; i++) {
        shape_update_frame(wm.resize_neighbors_x[i].client);
        draw_decoration(wm.resize_neighbors_x[i].client);
    }
    for (int i = 0; i < wm.resize_neighbors_y_count; i++) {
        shape_update_frame(wm.resize_neighbors_y[i].client);
        draw_decoration(wm.resize_neighbors_y[i].client);
    }
    xcb_flush(wm.conn);
}

static void handle_button_release(xcb_button_release_event_t *ev)
{
    (void)ev;
    if (wm.drag_client) {
        /* Force one final apply regardless of the redraw throttle above
         * -- otherwise the window could be left showing a stale size if
         * the very last motion event of the drag happened to land inside
         * the throttle window and got skipped. Same for every resize
         * neighbor that got dragged along (see update_resize_neighbors()). */
        configure_frame(wm.drag_client);
        for (int i = 0; i < wm.resize_neighbors_x_count; i++)
            configure_frame(wm.resize_neighbors_x[i].client);
        for (int i = 0; i < wm.resize_neighbors_y_count; i++)
            configure_frame(wm.resize_neighbors_y[i].client);
        wm.resize_neighbors_x_count = 0;
        wm.resize_neighbors_y_count = 0;

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
    if (ev->atom == wm.atoms.net_wm_icon) {
        load_client_icon(c);
        draw_decoration(c);
        xcb_flush(wm.conn);
    }
    if (ev->atom == XCB_ATOM_WM_TRANSIENT_FOR)
        client_refresh_transient_for(c);
    if (ev->atom == XCB_ATOM_WM_NORMAL_HINTS) {
        /* Some toolkits only set WM_NORMAL_HINTS after the initial map, so
         * a min-size hint that wasn't there yet in manage() can show up
         * later -- re-read it and clamp the current geometry up to match
         * if it's now too small (e.g. a terminal growing its min size once
         * a font/PTY dimension becomes known). */
        get_size_hints(c);
        /* Same hints decide which titlebar buttons exist at all -- a
         * toolkit that fixes its window size after mapping has just
         * withdrawn the maximize button. */
        update_client_actions(c);
        draw_decoration(c);
        bool grew = false;
        if (c->width < c->min_w)  { c->width = c->min_w;  grew = true; }
        if (c->height < c->min_h) { c->height = c->min_h; grew = true; }
        if (grew) {
            configure_frame(c);
            xcb_flush(wm.conn);
        }
    }
}

static void handle_enter_notify(xcb_enter_notify_event_t *ev)
{
    if (!wm.focus_follows_mouse)
        return;
    Client *c = find_client_window(ev->event);
    if (!c)
        c = find_client_window(ev->child);
    if (c && c->mapped && !c->minimized && (c->sticky || wm.outputs[c->output].desktop == c->desktop))
        focus_client(c);
}

static void handle_key_press(xcb_key_press_event_t *ev)
{
    /* Escape while either OSD (osd.c) is open cancels it without switching --
     * only meaningful during the active xcb_grab_keyboard() osd.c holds, but
     * osd_cancel() is a no-op otherwise so this is safe unconditionally.
     * Checked before the shortcut table so a hold can always be escaped
     * even if Escape happens to be bound to something as well. */
    if (ev->detail == wm.key_escape && osd_active()) {
        osd_cancel();
        return;
    }

    /* Everything else: kiwm.conf's key_* shortcuts (keybind.c), which is
     * also the only thing that grabbed any key on the root window in the
     * first place -- window/desktop switching, maximize, minimize,
     * half-screen tiling, direct desktop jumps, and whatever else the
     * user has bound. */
    keybind_handle_key_press(ev);
}

static void handle_net_wm_state(Client *c, uint32_t action, xcb_atom_t a1, xcb_atom_t a2)
{
    bool is_max = (a1 == wm.atoms.net_wm_state_maximized_vert || a1 == wm.atoms.net_wm_state_maximized_horz ||
                   a2 == wm.atoms.net_wm_state_maximized_vert || a2 == wm.atoms.net_wm_state_maximized_horz);
    bool is_hidden = (a1 == wm.atoms.net_wm_state_hidden || a2 == wm.atoms.net_wm_state_hidden);
    bool is_shaded = (a1 == wm.atoms.net_wm_state_shaded || a2 == wm.atoms.net_wm_state_shaded);
    bool is_above = (a1 == wm.atoms.net_wm_state_above || a2 == wm.atoms.net_wm_state_above);
    bool is_sticky = (a1 == wm.atoms.net_wm_state_sticky || a2 == wm.atoms.net_wm_state_sticky);
    bool is_fullscreen = (a1 == wm.atoms.net_wm_state_fullscreen || a2 == wm.atoms.net_wm_state_fullscreen);
    bool is_below = (a1 == wm.atoms.net_wm_state_below || a2 == wm.atoms.net_wm_state_below);

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
    if (is_above) {
        int want = (action == 2) ? -1 : (action == 1 ? 1 : 0);
        toggle_keep_above(c, want);
    }
    if (is_sticky) {
        int want = (action == 2) ? -1 : (action == 1 ? 1 : 0);
        toggle_sticky(c, want);
    }
    if (is_fullscreen) {
        int want = (action == 2) ? -1 : (action == 1 ? 1 : 0);
        toggle_fullscreen(c, want);
    }
    if (is_below) {
        int want = (action == 2) ? -1 : (action == 1 ? 1 : 0);
        toggle_keep_below(c, want);
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

    /* SHAPE's ShapeNotify: a client changed its own non-rectangular shape,
     * so the frame's has to follow (see shape.c). Like RandR's above, its
     * event number is assigned at runtime and can't be a case label. */
    if (shape_is_notify_event(type)) {
        shape_handle_notify(event);
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
            popup_focus_released(ev->window);
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
        if (!c)
            popup_focus_released(ev->window);
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
    case XCB_KEY_RELEASE:
        /* Only ever meaningful while osd.c holds its active
         * xcb_grab_keyboard() (see osd_windows_step()/osd_desktops_step()) --
         * a no-op otherwise, but delivered unconditionally since that's the
         * only way a bare modifier-key release (not tied to any specific
         * xcb_grab_key()) ever reaches kiwm at all. */
        osd_handle_key_release((xcb_key_release_event_t *)event);
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
        /* A single repaint-worthy change (e.g. one resize step) can be
         * reported as *several* Expose events, one per exposed
         * rectangle -- ev->count is how many more are still queued for
         * this same batch. kiwm always repaints the whole titlebar in
         * one go regardless of which rectangle triggered it, so there's
         * nothing to gain from repainting on every fragment; only the
         * last one in the batch (count == 0) actually needs to redraw.
         * Skipping this was making draw_decoration() fire many times per
         * single configure_frame() call during a resize -- confirmed via
         * KIWM_DEBUG_RESIZE instrumentation. */
        xcb_expose_event_t *ev = (xcb_expose_event_t *)event;
        if (ev->count != 0)
            break;
        osd_handle_expose(ev->window);
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
