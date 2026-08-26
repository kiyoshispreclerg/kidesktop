/* Client lifecycle: classification, manage/unmanage, focus/stacking,
 * and the map/move/resize/maximize/minimize state transitions. */
#include "client.h"
#include "wm.h"
#include "output.h"
#include "decoration.h"
#include "ewmh.h"
#include "osd.h"

#include <stdio.h>
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

/* Whether any of a window's *listed* _NET_WM_WINDOW_TYPE atoms (there can
 * be more than one, in priority order -- e.g. a specific type followed by
 * NORMAL as a generic fallback for WMs that don't recognize the first
 * one) names one kiwm never frames/decorates (see wm.h's Atoms doc
 * comment on net_wm_window_type_popup_menu and friends). Checking every
 * entry, not just get_window_type()'s first one, matters here: a window
 * whose primary type kiwm doesn't recognize at all would otherwise fall
 * through to "must be NORMAL, frame it" even when a later entry in the
 * same list says otherwise -- exactly what was giving KDE Plasma's own
 * popups (application launcher, applet popups, panel tooltips) a
 * titlebar they were never supposed to have, once Plasma is talking to a
 * WM that isn't KWin (which has private, kiwm-invisible handling for its
 * own popups regardless of what's actually in this property). */
static bool window_type_excluded_from_decoration(xcb_window_t window)
{
    xcb_get_property_reply_t *reply = xcb_get_property_reply(wm.conn,
        xcb_get_property(wm.conn, 0, window, wm.atoms.net_wm_window_type, XCB_ATOM_ATOM, 0, 32), NULL);
    if (!reply)
        return false; /* no type at all -- ICCCM default is effectively NORMAL */

    bool excluded = false;
    if (reply->type == XCB_ATOM_ATOM && reply->format == 32) {
        xcb_atom_t *atoms = xcb_get_property_value(reply);
        int n = xcb_get_property_value_length(reply) / (int)sizeof(xcb_atom_t);
        for (int i = 0; i < n && !excluded; i++) {
            xcb_atom_t t = atoms[i];
            excluded = (t == wm.atoms.net_wm_window_type_dock ||
                        t == wm.atoms.net_wm_window_type_desktop ||
                        t == wm.atoms.net_wm_window_type_toolbar ||
                        t == wm.atoms.net_wm_window_type_menu ||
                        t == wm.atoms.net_wm_window_type_popup_menu ||
                        t == wm.atoms.net_wm_window_type_dropdown_menu ||
                        t == wm.atoms.net_wm_window_type_tooltip ||
                        t == wm.atoms.net_wm_window_type_notification ||
                        t == wm.atoms.net_wm_window_type_combo ||
                        t == wm.atoms.net_wm_window_type_dnd ||
                        t == wm.atoms.net_wm_window_type_splash);
        }
    }
    free(reply);
    return excluded;
}

static bool should_manage_decorated(xcb_window_t window)
{
    return !window_type_excluded_from_decoration(window);
}

/* A passive xcb_grab_button() with a specific (non-ANY) modifier only
 * matches an *exact* modifier state, extra lock bits included -- with
 * NumLock or CapsLock active, ev->state also carries XCB_MOD_MASK_2/LOCK,
 * which would silently mismatch a grab registered for `mod` alone. Grab
 * all 4 combinations of "with/without each lock" so the resize grab (see
 * manage()) actually fires regardless of lock key state, same technique
 * every other X11 WM uses for this. */
static void grab_button3_with_locks(xcb_window_t window, uint16_t mod)
{
    static const uint16_t locks[] = { 0, XCB_MOD_MASK_LOCK, XCB_MOD_MASK_2,
                                      XCB_MOD_MASK_LOCK | XCB_MOD_MASK_2 };
    for (size_t i = 0; i < sizeof(locks) / sizeof(locks[0]); i++)
        xcb_grab_button(wm.conn, 0, window, XCB_EVENT_MASK_BUTTON_PRESS,
                        XCB_GRAB_MODE_SYNC, XCB_GRAB_MODE_ASYNC,
                        XCB_NONE, XCB_NONE, XCB_BUTTON_INDEX_3, (uint16_t)(mod | locks[i]));
}

Client *find_client_window(xcb_window_t window)
{
    for (Client *c = wm.clients; c; c = c->next)
        if (c->window == window || c->frame == window)
            return c;
    return NULL;
}

/* How much frame space the titlebar (top) and the flat side/bottom border
 * (left/right/bottom, kiwm.conf's border_thickness=) currently take up --
 * both collapse to 0 together via client_deco_visible() (maximized with
 * hide_deco_on_maximize=1), so a maximized/hidden-deco window's frame is
 * exactly its content size, no partial state. */
void deco_insets(Client *c, int *bt, int *th)
{
    bool deco = client_deco_visible(c);
    *th = deco ? TITLEBAR_H : 0;
    *bt = deco ? wm.border_thickness : 0;
}

/* ICCCM 4.1.5: a real ConfigureNotify only reaches the client when *its
 * own* geometry relative to its immediate parent (the frame) changes --
 * moving/resizing the frame itself never generates one for the reparented
 * child, even though the child's on-screen (root-relative) position just
 * changed right along with it. Toolkits use ConfigureNotify to learn their
 * true screen position for placing context menus, tooltips and popups;
 * without this synthetic event (which real ConfigureNotify already looks
 * identical to, per spec) they keep using a stale root-relative origin
 * every time kiwm moves/resizes a window by moving its frame, which is
 * every move, drag-resize, snap and maximize -- exactly what made menus
 * and tooltips show up in the wrong place. */
static void send_synthetic_configure(Client *c, int bt, int th)
{
    xcb_configure_notify_event_t ev = { 0 };
    ev.response_type = XCB_CONFIGURE_NOTIFY;
    ev.event = c->window;
    ev.window = c->window;
    ev.above_sibling = XCB_NONE;
    ev.x = (int16_t)(c->x + bt);
    ev.y = (int16_t)(c->y + th);
    ev.width = (uint16_t)c->width;
    ev.height = (uint16_t)c->height;
    ev.border_width = 0;
    ev.override_redirect = 0;
    xcb_send_event(wm.conn, 0, c->window, XCB_EVENT_MASK_STRUCTURE_NOTIFY, (const char *)&ev);
}

/* The cheap part of configure_frame(): move/resize the actual frame and
 * content windows and tell the client its new position, but skip the two
 * expensive parts (XShape re-clip, off-screen decoration repaint) --
 * neither of which needs to happen on every single event for the window to
 * visibly track the pointer. Used directly, uncapped, on every motion event
 * of a move or resize drag (events.c's handle_motion()) so the window's own
 * outline keeps up with the mouse at full input rate the way kwin's
 * uncomposited opaque move/resize does; the throttled apply_rounded_shape()
 * + draw_decoration() pair (still gated to the output's refresh rate) just
 * makes the *painted chrome* -- corners, title, buttons -- catch up
 * shortly after, which is far less noticeable than the window border itself
 * lagging the pointer. */
void apply_frame_geometry(Client *c)
{
    int bt, th;
    deco_insets(c, &bt, &th);

    c->frame_width = c->width + bt * 2;
    /* Shaded: only the titlebar shows, content stays unmapped (see
     * toggle_shade()) -- the frame collapses to exactly th tall, no
     * bottom border either since there's no content edge to border. */
    c->frame_height = c->shaded ? th : (c->height + th + bt);

    uint32_t fv[] = {
        (uint32_t)c->x, (uint32_t)c->y,
        (uint32_t)c->frame_width, (uint32_t)c->frame_height
    };
    xcb_configure_window(wm.conn, c->frame,
                         XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                         XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, fv);

    uint32_t cv[] = { (uint32_t)bt, (uint32_t)th, (uint32_t)c->width, (uint32_t)c->height };
    xcb_configure_window(wm.conn, c->window,
                         XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                         XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, cv);

    send_synthetic_configure(c, bt, th);
}

void configure_frame(Client *c)
{
    /* Fine-grained breakdown under wm.debug_resize (KIWM_DEBUG_RESIZE=1),
     * gated to the resize drag specifically (per-motion-event cost during
     * a plain move is rarely the complaint) -- see main.c's event loop
     * for the coarser "whole handle_event() took Xms" number this
     * complements. */
    bool dbg = wm.debug_resize && wm.drag_mode == DRAG_RESIZE;
    double t_start = dbg ? monotonic_ms() : 0;

    apply_frame_geometry(c);
    double t_configure = dbg ? monotonic_ms() : 0;

    apply_rounded_shape(c);
    double t_shape = dbg ? monotonic_ms() : 0;

    draw_decoration(c);
    double t_deco = dbg ? monotonic_ms() : 0;

    if (dbg) {
        double t_end = monotonic_ms();
        fprintf(stderr, "kiwm: [resize-debug] configure_frame: geometry=%.2fms shape=%.2fms "
                        "draw_decoration=%.2fms total=%.2fms\n",
                t_configure - t_start, t_shape - t_configure, t_deco - t_shape, t_end - t_start);
    }
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
    restack_all();

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
        if (c->output == output_idx && (c->sticky || c->desktop == desktop) && c->mapped && !c->minimized) {
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

/* If c is currently shaded, remap its content and clear the flag -- called
 * before any tiling transition (maximize, edge-snap) engages, since tiling
 * assumes the content is actually visible. Exported (client.h) because
 * events.c's SNAP_TOP handling in try_edge_snap() needs it too. */
void unshade_now(Client *c)
{
    if (!c->shaded)
        return;
    c->shaded = false;
    if (c->mapped)
        xcb_map_window(wm.conn, c->window);
}

/* A client's stacking layer, derived from its state -- see wm.h's WmLayer.
 * fullscreen is deliberately *not* checked here -- see WmLayer's doc
 * comment for why -- so a fullscreen client falls through to whichever of
 * keep_above/keep_below/LAYER_NORMAL its other state says, same as if it
 * weren't fullscreen at all. keep_above/keep_below are themselves kept
 * mutually exclusive by toggle_keep_above()/toggle_keep_below() so this
 * never has to arbitrate between them. */
static WmLayer client_layer(Client *c)
{
    if (c->keep_above)
        return LAYER_ABOVE;
    if (c->keep_below)
        return LAYER_BELOW;
    return LAYER_NORMAL;
}

/* Rebuilds the real X stacking order to match every client's current
 * WmLayer, bottom to top (LAYER_BELOW, LAYER_NORMAL, LAYER_ABOVE -- see
 * wm.h), while preserving each client's relative order
 * *within* its own layer exactly as xcb_query_tree() currently reports it.
 * That's what lets a caller put one specific client at the top or bottom
 * of its own layer without disturbing everyone else's relative order: raise
 * (or lower) that one client to the very top (or bottom) of the whole X
 * stack first with a plain STACK_MODE_ABOVE/BELOW, *then* call this --
 * since query_tree now reports it topmost (or bottommost) overall, it's
 * still topmost (or bottommost) once partitioned into just its own layer's
 * bucket below. See client.h's comment for the call sites. */
void restack_all(void)
{
    xcb_query_tree_reply_t *tree =
        xcb_query_tree_reply(wm.conn, xcb_query_tree(wm.conn, wm.root), NULL);
    if (!tree)
        return;

    xcb_window_t *kids = xcb_query_tree_children(tree);
    int nkids = xcb_query_tree_children_length(tree);

    Client *buckets[LAYER_COUNT][MAX_CLIENTS];
    int bn[LAYER_COUNT] = { 0 };

    /* xcb_query_tree()'s children come back bottom-to-top, so walking them
     * in order and appending each one to its layer's bucket naturally
     * preserves that same relative order within the bucket. */
    for (int i = 0; i < nkids; i++) {
        Client *c = find_client_window(kids[i]);
        if (!c || c->frame != kids[i])
            continue;
        WmLayer l = client_layer(c);
        if (bn[l] < MAX_CLIENTS)
            buckets[l][bn[l]++] = c;
    }
    free(tree);

    /* Chain every client's frame to sit directly above the previous one,
     * walking layers bottom to top -- one xcb_configure_window() per
     * client (besides the very first, which is left wherever it already
     * is; nothing needs to be below it). */
    xcb_window_t prev = XCB_NONE;
    for (int l = 0; l < LAYER_COUNT; l++) {
        for (int i = 0; i < bn[l]; i++) {
            Client *c = buckets[l][i];
            if (prev != XCB_NONE) {
                uint32_t values[] = { prev, XCB_STACK_MODE_ABOVE };
                xcb_configure_window(wm.conn, c->frame,
                                     XCB_CONFIG_WINDOW_SIBLING | XCB_CONFIG_WINDOW_STACK_MODE, values);
            }
            prev = c->frame;
        }
    }
}

void toggle_keep_above(Client *c, int want /* -1=toggle 0=off 1=on */)
{
    bool target = (want == -1) ? !c->keep_above : (want == 1);
    if (target == c->keep_above)
        return;
    c->keep_above = target;
    if (target) {
        c->keep_below = false; /* mutually exclusive, see client_layer() */
        xcb_configure_window(wm.conn, c->frame, XCB_CONFIG_WINDOW_STACK_MODE,
                             (uint32_t[]){ XCB_STACK_MODE_ABOVE });
    }
    restack_all();
    ewmh_update_wm_state(c);
    xcb_flush(wm.conn);
}

void toggle_keep_below(Client *c, int want /* -1=toggle 0=off 1=on */)
{
    bool target = (want == -1) ? !c->keep_below : (want == 1);
    if (target == c->keep_below)
        return;
    c->keep_below = target;
    if (target) {
        c->keep_above = false; /* mutually exclusive, see client_layer() */
        xcb_configure_window(wm.conn, c->frame, XCB_CONFIG_WINDOW_STACK_MODE,
                             (uint32_t[]){ XCB_STACK_MODE_BELOW });
    }
    restack_all();
    ewmh_update_wm_state(c);
    xcb_flush(wm.conn);
}

void toggle_sticky(Client *c, int want /* -1=toggle 0=off 1=on */)
{
    bool target = (want == -1) ? !c->sticky : (want == 1);
    if (target == c->sticky)
        return;
    c->sticky = target;

    /* Toggling this can change whether the client should currently be
     * visible at all -- becoming sticky can reveal a window that was
     * hidden by a desktop mismatch, and un-sticking one can hide a window
     * that only stayed visible because it used to be sticky. */
    if (!c->minimized) {
        bool should_show = target || (c->output >= 0 && wm.outputs[c->output].desktop == c->desktop);
        if (should_show && !c->mapped) {
            xcb_map_window(wm.conn, c->frame);
            c->mapped = true;
        } else if (!should_show && c->mapped) {
            xcb_unmap_window(wm.conn, c->frame);
            c->mapped = false;
        }
    }

    ewmh_update_wm_state(c);
    xcb_flush(wm.conn);
}

void toggle_shade(Client *c, int want /* -1=toggle 0=unshade 1=shade */)
{
    bool target = (want == -1) ? !c->shaded : (want == 1);
    if (target == c->shaded)
        return;

    c->shaded = target;
    if (c->mapped) {
        if (target)
            xcb_unmap_window(wm.conn, c->window);
        else
            xcb_map_window(wm.conn, c->window);
    }

    configure_frame(c);
    ewmh_update_wm_state(c);
    xcb_flush(wm.conn);
}

void toggle_maximize(Client *c, int want /* -1=toggle 0=unmax 1=max */)
{
    bool target = (want == -1) ? !c->maximized : (want == 1);
    if (target == c->maximized)
        return;

    if (target) {
        unshade_now(c);

        /* Only capture the "restore" geometry when currently floating --
         * if the window is already half-snapped (Client::snap_side), that
         * state's own saved_x/y/w/h already holds the true pre-tiling
         * geometry from whenever tiling was first entered, and clicking
         * maximize directly (no drag involved) must not clobber it with
         * the half-snapped size instead. See PROTOCOL notes in
         * events.c's try_edge_snap() for the drag-path equivalent. */
        if (c->snap_side == SNAP_NONE) {
            c->saved_x = c->x;
            c->saved_y = c->y;
            c->saved_w = c->width;
            c->saved_h = c->height;
        }

        /* Fill the output's usable area (screen minus any dock/panel
         * struts, see output.c's compute_output_workarea), not the raw
         * output rect -- a maximized window must never cover a taskbar. */
        int wx, wy, ww, wh;
        compute_output_workarea(c->output >= 0 ? c->output : 0, &wx, &wy, &ww, &wh);
        c->maximized = true;
        c->snap_side = SNAP_NONE;

        int bt, th;
        deco_insets(c, &bt, &th);
        c->x = wx;
        c->y = wy;
        c->width = ww - bt * 2;
        c->height = wh - th - bt;
        if (c->width < c->min_w) c->width = c->min_w;
        if (c->height < c->min_h) c->height = c->min_h;
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

/* _NET_WM_STATE_FULLSCREEN: unlike toggle_maximize's workarea fill, covers
 * the output's whole rectangle -- docks/panels included -- with the
 * decoration unconditionally hidden (see decoration.c's
 * client_deco_visible()), the way a video player or browser expects.
 * Remembers whether the window was maximized (or half-snapped) before
 * going fullscreen so leaving it restores that exact prior state instead
 * of always dropping to floating -- toggling a maximized window fullscreen
 * and back should look like nothing happened. */
void toggle_fullscreen(Client *c, int want /* -1=toggle 0=unfullscreen 1=fullscreen */)
{
    bool target = (want == -1) ? !c->fullscreen : (want == 1);
    if (target == c->fullscreen)
        return;

    if (target) {
        unshade_now(c);

        c->fs_saved_x = c->x;
        c->fs_saved_y = c->y;
        c->fs_saved_w = c->width;
        c->fs_saved_h = c->height;
        c->fs_was_maximized = c->maximized;
        c->fs_saved_snap_side = c->snap_side;

        c->fullscreen = true;
        c->maximized = false;
        c->snap_side = SNAP_NONE;

        int ox = 0, oy = 0, ow = 0, oh = 0;
        if (c->output >= 0 && c->output < wm.output_count) {
            ox = wm.outputs[c->output].x;
            oy = wm.outputs[c->output].y;
            ow = wm.outputs[c->output].width;
            oh = wm.outputs[c->output].height;
        }
        c->x = ox;
        c->y = oy;
        c->width = ow;
        c->height = oh;

        configure_frame(c);
        xcb_configure_window(wm.conn, c->frame, XCB_CONFIG_WINDOW_STACK_MODE,
                             (uint32_t[]){ XCB_STACK_MODE_ABOVE });
        restack_all();
        ewmh_update_wm_state(c);
        ewmh_update_frame_extents(c);
        xcb_flush(wm.conn);
        return;
    }

    c->fullscreen = false;
    bool restore_maximized = c->fs_was_maximized;
    c->fs_was_maximized = false;

    c->x = c->fs_saved_x;
    c->y = c->fs_saved_y;
    c->width = c->fs_saved_w;
    c->height = c->fs_saved_h;
    c->snap_side = c->fs_saved_snap_side;

    if (restore_maximized) {
        /* toggle_maximize()'s own configure_frame()/ewmh update/flush
         * covers the rest -- just hand it the pre-fullscreen floating
         * geometry as its "restore" baseline first. */
        c->saved_x = c->x;
        c->saved_y = c->y;
        c->saved_w = c->width;
        c->saved_h = c->height;
        toggle_maximize(c, 1);
        restack_all(); /* re-affirms its position in LAYER_NORMAL -- harmless no-op if nothing else changed */
        xcb_flush(wm.conn);
        return;
    }

    configure_frame(c);
    restack_all(); /* ditto */
    ewmh_update_wm_state(c);
    ewmh_update_frame_extents(c);
    xcb_flush(wm.conn);
}

/* Windows7/kwin-style edge snap: fills exactly the left or right half of
 * the client's output workarea. A separate concept from toggle_maximize's
 * full-area fill (which Client::maximized already models and which the
 * top-edge drag snap in events.c's handle_motion reuses directly) --
 * tracked via its own Client::snap_side since a half-snapped window is
 * neither "maximized" nor "floating". Caller (handle_motion) is
 * responsible for configure_frame()/ewmh updates/flush afterward, since it
 * always needs to do that anyway for whichever snap state it applies. */
void snap_client_to_side(Client *c, SnapSide side)
{
    unshade_now(c);

    int wx, wy, ww, wh;
    compute_output_workarea(c->output >= 0 ? c->output : 0, &wx, &wy, &ww, &wh);

    int bt, th;
    deco_insets(c, &bt, &th);

    int half = ww / 2;
    c->maximized = false;
    c->snap_side = side;
    c->y = wy;
    c->height = wh - th - bt;
    c->x = (side == SNAP_LEFT) ? wx : wx + (ww - half);
    c->width = half - bt * 2;
    if (c->width < c->min_w) c->width = c->min_w;
    if (c->height < c->min_h) c->height = c->min_h;
}

/* Restores explicit floating geometry (typically the pre-drag geometry
 * the caller tracked itself, offset by however far the pointer has moved
 * since), clearing whatever snap/maximize state was engaged. Also just
 * the caller's own responsibility to configure_frame()/flush afterward. */
void unsnap_client(Client *c, int x, int y, int width, int height)
{
    c->maximized = false;
    c->snap_side = SNAP_NONE;
    c->x = x;
    c->y = y;
    c->width = width;
    c->height = height;
}

/* Starting a titlebar/mod drag on a window that's currently maximized or
 * half-snapped must restore it to its pre-tiling floating size *right
 * then*, not partway through the drag -- otherwise handle_motion's normal
 * "c->x = wm.drag_start_x + dx" math keeps using whatever geometry was
 * current at button-press time (the tiled one), so the window would
 * appear to move but stay the tiled size, only "restoring" for real once
 * some other snap-transition code path happened to run. Also repositions
 * the window so the press point stays under the same relative fraction
 * of the restored frame it was at within the tiled one (kwin/Windows-
 * style: grabbing a maximized window's titlebar and dragging keeps the
 * cursor under roughly the same spot instead of jumping the window's
 * origin to wherever its old tiled corner was). No-op if c is already
 * floating. Caller (events.c's handle_button_press) must call this
 * *before* capturing wm.drag_start_x/y/w/h, so the whole rest of the drag
 * -- including any further snap-side transitions in try_edge_snap() --
 * builds on this restored geometry as its baseline. */
void detile_for_drag(Client *c, int press_root_x, int press_root_y)
{
    if (!c->maximized && c->snap_side == SNAP_NONE)
        return;

    int old_fw = c->frame_width, old_fh = c->frame_height;
    double frac_x = old_fw > 0 ? (press_root_x - c->x) / (double)old_fw : 0.5;
    double frac_y = old_fh > 0 ? (press_root_y - c->y) / (double)old_fh : 0.0;
    if (frac_x < 0.0) frac_x = 0.0; else if (frac_x > 1.0) frac_x = 1.0;
    if (frac_y < 0.0) frac_y = 0.0; else if (frac_y > 1.0) frac_y = 1.0;

    c->maximized = false;
    c->snap_side = SNAP_NONE;
    c->width = c->saved_w;
    c->height = c->saved_h;

    int bt, th;
    deco_insets(c, &bt, &th);
    int new_fw = c->width + bt * 2;
    int new_fh = c->height + th + bt;

    c->x = press_root_x - (int)(frac_x * new_fw);
    c->y = press_root_y - (int)(frac_y * new_fh);

    configure_frame(c);
    ewmh_update_wm_state(c);
    ewmh_update_frame_extents(c);
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

    if (c->sticky || wm.outputs[c->output].desktop == c->desktop) {
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
    if (!c->sticky && c->output >= 0 && wm.outputs[c->output].desktop != c->desktop)
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

    bool was_visible = c->sticky || (c->output >= 0 && wm.outputs[c->output].desktop == c->desktop);
    bool now_visible = c->sticky || (c->output >= 0 && wm.outputs[c->output].desktop == desktop);

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
        wm.drag_snap_side = SNAP_NONE;
        wm.resize_neighbors_x_count = 0;
        wm.resize_neighbors_y_count = 0;
    } else {
        /* c isn't the client actually being dragged, but a resize in
         * progress might still be dragging it along as a resize-neighbor
         * (see wm.h's ResizeNeighbor) -- remove it from whichever list
         * references it (swap-with-last, order doesn't matter here) so
         * the rest of the drag doesn't touch this about-to-be-freed
         * Client again. */
        for (int i = 0; i < wm.resize_neighbors_x_count; i++) {
            if (wm.resize_neighbors_x[i].client == c) {
                wm.resize_neighbors_x[i] = wm.resize_neighbors_x[--wm.resize_neighbors_x_count];
                break;
            }
        }
        for (int i = 0; i < wm.resize_neighbors_y_count; i++) {
            if (wm.resize_neighbors_y[i].client == c) {
                wm.resize_neighbors_y[i] = wm.resize_neighbors_y[--wm.resize_neighbors_y_count];
                break;
            }
        }
    }
    if (wm.hover_client == c) {
        wm.hover_client = NULL;
        wm.hover_btn = -1;
    }

    /* A window closing mid-Alt+Tab-hold (osd.c's window-switcher OSD) must
     * not be left in its list -- it could otherwise get focused (dangling
     * pointer) or drawn (use-after-free) on the next repaint/commit. */
    osd_client_destroyed(c);

    xcb_unmap_window(wm.conn, c->frame);

    xcb_void_cookie_t reparent_cookie =
        xcb_reparent_window_checked(wm.conn, c->window, wm.root, c->x, c->y);
    xcb_generic_error_t *err = xcb_request_check(wm.conn, reparent_cookie);
    if (err)
        free(err);

    xcb_destroy_window(wm.conn, c->frame);

    if (fw > 0 && fh > 0)
        xcb_clear_area(wm.conn, 0, wm.root, fx, fy, (uint16_t)fw, (uint16_t)fh);

    if (c->icon)
        cairo_surface_destroy(c->icon);

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

        /* _NET_WM_WINDOW_TYPE_DESKTOP (e.g. xisback's wallpaper/fade
         * windows) must stay clustered at the very bottom of the whole
         * stack, below every normal window -- X's default "a newly
         * mapped window goes on top of its siblings" would otherwise put
         * it above everything, which is exactly the bug this fixes (see
         * xisback's create_fade_window() comment: it deliberately avoids
         * restacking itself and just trusts a "compliant WM" to do this).
         * Chaining each new one directly above the previous one (rather
         * than flatly below all current siblings) keeps the group's own
         * creation order intact -- e.g. xisback's fade_win needs to stay
         * visually above the older wallpaper window it's cross-fading
         * over, not buried under it.
         *
         * Restacking *before* mapping matters: stacking order applies to
         * unmapped windows too, so doing it first means the window is
         * already in its correct position the instant it becomes visible.
         * Map-then-restack (even flushed together) still makes the X
         * server perform two separate state transitions, and a screen
         * refresh landing between them is a real one-frame flash at the
         * wrong (default: topmost) stacking position -- reproduced with
         * xisback's own slideshow crossfade before this reordering. */
        if (get_window_type(window) == wm.atoms.net_wm_window_type_desktop) {
            if (wm.last_desktop_window != XCB_NONE) {
                uint32_t values[] = { wm.last_desktop_window, XCB_STACK_MODE_ABOVE };
                xcb_configure_window(wm.conn, window,
                                     XCB_CONFIG_WINDOW_SIBLING | XCB_CONFIG_WINDOW_STACK_MODE, values);
            } else {
                uint32_t values[] = { XCB_STACK_MODE_BELOW };
                xcb_configure_window(wm.conn, window, XCB_CONFIG_WINDOW_STACK_MODE, values);
            }
            wm.last_desktop_window = window;
        }

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
    get_size_hints(c);
    c->width = geo->width < c->min_w ? c->min_w : geo->width;
    c->height = geo->height < c->min_h ? c->min_h : geo->height;
    free(geo);

    c->output = output_index_for_point(c->x + c->width / 2, c->y + c->height / 2);
    if (c->output < 0)
        c->output = 0;
    c->desktop = wm.output_count > 0 ? wm.outputs[c->output].desktop : 0;

    get_title(c);
    load_client_icon(c);

    c->frame = xcb_generate_id(wm.conn);

    /* xcb_create_window's value-list must appear in ascending bit order of
     * the CW_* flags in the mask, NOT the order they're OR'd together in
     * source -- CW_BORDER_PIXEL (0x08) sorts before CW_EVENT_MASK (0x800),
     * so border-pixel goes first. Getting this backwards (as an earlier
     * version of this code did) silently sends 0 as the *event mask* and
     * the intended event-mask bits as the border pixel instead: the frame
     * window then never receives Expose at all, so an unfocused window's
     * titlebar/border never gets cleared or redrawn once something else
     * has been drawn over it -- exactly the "keeps whatever was drawn over
     * it, like a background-None window" symptom this fixes. */
    uint32_t values[] = {
        0, /* border_pixel: unused, frame's X border_width is 0 */
        XCB_EVENT_MASK_EXPOSURE |
        XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE |
        XCB_EVENT_MASK_POINTER_MOTION |
        XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_LEAVE_WINDOW |
        /* SubstructureRedirect on the frame too, not just root: once a
         * client's top-level window is reparented into the frame, it's no
         * longer a direct child of root, so root's own SubstructureRedirect
         * no longer covers it -- any ConfigureWindow the app later issues
         * on *itself* (many toolkits do this to restore a remembered
         * position/size well after being mapped, unaware it's reparented at
         * all, per ICCCM's transparency requirement) would otherwise apply
         * directly against its real parent (the frame) with no redirect at
         * all: the app's intended *absolute screen* x/y lands as a raw
         * frame-relative offset instead, shoving the content way off inside
         * the frame -- exactly the "content displaced by however far the
         * window used to be from (0,0), cut off in a corner" bug this
         * fixes. With this selected, that request instead comes back to us
         * as a ConfigureRequest (handle_configure_request(), which now
         * knows to treat it as the *content's* intended position, not the
         * frame's, since that's what the app actually meant). */
        XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT
    };
    int bt = wm.border_thickness; /* deco is always visible on a freshly-managed window */
    xcb_create_window(wm.conn, wm.screen->root_depth, c->frame, wm.root,
                      c->x, c->y, c->width + bt * 2, c->height + TITLEBAR_H + bt, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, wm.screen->root_visual,
                      XCB_CW_BORDER_PIXEL | XCB_CW_EVENT_MASK, values);

    uint32_t client_mask = XCB_EVENT_MASK_PROPERTY_CHANGE |
                           XCB_EVENT_MASK_STRUCTURE_NOTIFY |
                           XCB_EVENT_MASK_FOCUS_CHANGE;
    xcb_change_window_attributes(wm.conn, window, XCB_CW_EVENT_MASK, &client_mask);

    xcb_grab_button(wm.conn, 0, window, XCB_EVENT_MASK_BUTTON_PRESS,
                    XCB_GRAB_MODE_SYNC, XCB_GRAB_MODE_ASYNC,
                    XCB_NONE, XCB_NONE, XCB_BUTTON_INDEX_1, XCB_MOD_MASK_ANY);

    /* mod_cycle/mod_control + right-click resize (events.c's
     * handle_button_press) only ever worked when the click happened to
     * land on the frame itself (e.g. the titlebar) -- everywhere else on
     * a window is its *content* child, which never had button 3 grabbed
     * at all, so kiwm never even saw the event; it went straight to the
     * app (typically opening its own context menu) instead. The button-1
     * grab above already works from anywhere on content because it's an
     * unconditional (MOD_MASK_ANY) passive grab that the handler then
     * decides what to do with (move if a mod is held, otherwise
     * xcb_allow_events() replays it straight through) -- button 3 needs
     * its own passive grabs, one per modifier, since we only want to
     * steal right-clicks that actually have one of them held, not every
     * right-click on the window. */
    grab_button3_with_locks(window, wm.mod_cycle);
    grab_button3_with_locks(window, wm.mod_control);

    xcb_reparent_window(wm.conn, window, c->frame, bt, TITLEBAR_H);

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
