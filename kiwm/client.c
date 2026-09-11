/* Client lifecycle: classification, manage/unmanage, focus/stacking,
 * and the map/move/resize/maximize/minimize state transitions. */
#include "client.h"
#include "wm.h"
#include "output.h"
#include "decoration.h"
#include "density.h"
#include "ewmh.h"
#include "osd.h"
#include "menu.h"
#include "outline.h"
#include "shape.h"
#include "selection.h"
#include "grip.h"
#include "atoms.h"

#include <xcb/shape.h>

#include <xcb/xcb_icccm.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Whether the window exports an application menu -- i.e. carries the
 * _KDE_NET_WM_APPMENU_* pair Qt/KF5 apps set (xispanel's globalmenu
 * widget reads the same thing). Only their presence matters here: kiwm
 * shows or hides the appmenu titlebar button by it and hands the actual
 * menu to kiwm.conf's appmenu_command=, never speaking DBus itself. */
void client_refresh_appmenu(Client *c)
{
    bool found = false;
    const xcb_atom_t props[] = {
        wm.atoms.kde_net_wm_appmenu_service_name,
        wm.atoms.kde_net_wm_appmenu_object_path,
    };
    for (size_t i = 0; i < sizeof(props) / sizeof(props[0]) && !found; i++) {
        if (props[i] == XCB_ATOM_NONE)
            continue;
        xcb_get_property_reply_t *r = xcb_get_property_reply(wm.conn,
            xcb_get_property(wm.conn, 0, c->window, props[i], XCB_GET_PROPERTY_TYPE_ANY, 0, 1), NULL);
        if (r) {
            found = xcb_get_property_value_length(r) > 0;
            free(r);
        }
    }
    c->has_appmenu = found;
}

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
                        t == wm.atoms.net_wm_window_type_splash ||
                        t == wm.atoms.kde_net_wm_window_type_applet_popup);
        }
    }
    free(reply);
    return excluded;
}

/* Whether `type` appears anywhere in the window's _NET_WM_WINDOW_TYPE
 * list. Same "check every entry, not just the first" rule
 * window_type_excluded_from_decoration() above explains -- a window whose
 * primary type is one kiwm doesn't recognize still has to be seen for the
 * types it lists after it. */
static bool window_has_type(xcb_window_t window, xcb_atom_t type)
{
    xcb_get_property_reply_t *reply = xcb_get_property_reply(wm.conn,
        xcb_get_property(wm.conn, 0, window, wm.atoms.net_wm_window_type, XCB_ATOM_ATOM, 0, 32), NULL);
    if (!reply)
        return false;

    bool found = false;
    if (reply->type == XCB_ATOM_ATOM && reply->format == 32) {
        xcb_atom_t *atoms = xcb_get_property_value(reply);
        int n = xcb_get_property_value_length(reply) / (int)sizeof(xcb_atom_t);
        for (int i = 0; i < n && !found; i++)
            found = (atoms[i] == type);
    }
    free(reply);
    return found;
}

static bool should_manage_decorated(xcb_window_t window)
{
    return !window_type_excluded_from_decoration(window);
}

/* Hands the keyboard to an unframed popup kiwm just mapped, if it's the
 * kind that needs one.
 *
 * These windows are excluded from framing (see
 * window_type_excluded_from_decoration()) but they are *not*
 * override-redirect: they're ordinary top-levels whose app expects the WM
 * to focus them, exactly as it would a dialog. Skipping that leaves a
 * Plasma application launcher that opens, draws and takes clicks but
 * silently swallows every keystroke typed into its search field, because
 * the keyboard is still pointed at whatever was focused before.
 *
 * Only the interactive kinds qualify. A tooltip, notification, splash or
 * drag icon must never take the keyboard away from the window the user is
 * actually working in -- that's the opposite bug, and a far more annoying
 * one. Docks are excluded for the same reason: a panel takes clicks
 * without ever wanting the keyboard. */
static void focus_unframed_popup(xcb_window_t window)
{
    bool focusable = window_has_type(window, wm.atoms.kde_net_wm_window_type_applet_popup) ||
                     window_has_type(window, wm.atoms.net_wm_window_type_popup_menu) ||
                     window_has_type(window, wm.atoms.net_wm_window_type_dropdown_menu) ||
                     window_has_type(window, wm.atoms.net_wm_window_type_combo) ||
                     window_has_type(window, wm.atoms.net_wm_window_type_menu);
    if (!focusable)
        return;

    xcb_set_input_focus(wm.conn, XCB_INPUT_FOCUS_POINTER_ROOT, window, XCB_CURRENT_TIME);
    wm.focused_popup = window;
}

/* The other half of focus_unframed_popup(): a popup that held the keyboard
 * has gone away (unmapped or destroyed), so hand focus back to the client
 * that had it. Without this the X input focus stays pointed at a window
 * that no longer exists and the session goes deaf until something else
 * happens to focus a window. Called from events.c for every unmap/destroy
 * of a non-Client window; a no-op unless that window is the one tracked. */
void popup_focus_released(xcb_window_t window)
{
    if (window != wm.focused_popup)
        return;
    wm.focused_popup = XCB_NONE;

    if (wm.focused)
        xcb_set_input_focus(wm.conn, XCB_INPUT_FOCUS_POINTER_ROOT,
                            wm.focused->window, XCB_CURRENT_TIME);
    else
        xcb_set_input_focus(wm.conn, XCB_INPUT_FOCUS_POINTER_ROOT,
                            wm.root, XCB_CURRENT_TIME);
    xcb_flush(wm.conn);
}

/* Whether a window that *is* managed normally nonetheless wants kiwm to
 * draw no titlebar/border around it (Client::undecorated, see wm.h). Two
 * independent ways to say it, both honored:
 *
 *   - _MOTIF_WM_HINTS with the decorations field flagged as present and
 *     set to 0. Never standardized, predates EWMH by a decade, and is
 *     still what Qt's Qt::FramelessWindowHint, GTK's
 *     gtk_window_set_decorated(false) and SDL's borderless windows all
 *     actually put on the wire -- so a WM that ignores it draws a titlebar
 *     on top of windows that already drew their own.
 *   - _KDE_NET_WM_WINDOW_TYPE_OVERRIDE anywhere in _NET_WM_WINDOW_TYPE,
 *     KDE's equivalent (kwin's "noBorder"). VirtualBox's VM window sets
 *     exactly this.
 *
 * The Motif struct is { flags, functions, decorations, input_mode,
 * status } as 5 32-bit values; flags bit 1 (MWM_HINTS_DECORATIONS) says
 * the decorations field is meaningful at all, and a *nonzero* decorations
 * value means "decorate me" (possibly with a specific subset kiwm doesn't
 * model), so only an explicit 0 counts as a refusal. */
static bool window_wants_no_decoration(xcb_window_t window)
{
    xcb_get_property_reply_t *reply = xcb_get_property_reply(wm.conn,
        xcb_get_property(wm.conn, 0, window, wm.atoms.motif_wm_hints,
                         XCB_GET_PROPERTY_TYPE_ANY, 0, 5), NULL);
    if (reply) {
        bool undecorated = false;
        if (reply->format == 32 && xcb_get_property_value_length(reply) >= 3 * (int)sizeof(uint32_t)) {
            uint32_t *hints = xcb_get_property_value(reply);
            const uint32_t MWM_HINTS_DECORATIONS = 1u << 1;
            undecorated = (hints[0] & MWM_HINTS_DECORATIONS) && hints[2] == 0;
        }
        free(reply);
        if (undecorated)
            return true;
    }

    reply = xcb_get_property_reply(wm.conn,
        xcb_get_property(wm.conn, 0, window, wm.atoms.net_wm_window_type, XCB_ATOM_ATOM, 0, 32), NULL);
    if (!reply)
        return false;

    bool override_type = false;
    if (reply->type == XCB_ATOM_ATOM && reply->format == 32) {
        xcb_atom_t *atoms = xcb_get_property_value(reply);
        int n = xcb_get_property_value_length(reply) / (int)sizeof(xcb_atom_t);
        for (int i = 0; i < n && !override_type; i++)
            override_type = (atoms[i] == wm.atoms.kde_net_wm_window_type_override);
    }
    free(reply);
    return override_type;
}

/* _MOTIF_WM_HINTS' `functions` field -> Client::allow_*. Same prehistoric
 * struct window_wants_no_decoration() reads for `decorations`, one field
 * over: { flags, functions, decorations, input_mode, status }, with flags
 * bit 0 (MWM_HINTS_FUNCTIONS) saying `functions` is meaningful at all.
 *
 * The one trap is MWM_FUNC_ALL: when that bit is set, the remaining bits
 * are the functions to *remove*, not the ones to allow. Reading it the
 * naive way inverts the whole thing -- a window saying "everything except
 * resize" would come out as "nothing but resize". */
static void apply_motif_functions(Client *c)
{
    xcb_get_property_reply_t *reply = xcb_get_property_reply(wm.conn,
        xcb_get_property(wm.conn, 0, c->window, wm.atoms.motif_wm_hints,
                         XCB_GET_PROPERTY_TYPE_ANY, 0, 5), NULL);
    if (!reply)
        return;

    if (reply->format == 32 && xcb_get_property_value_length(reply) >= 2 * (int)sizeof(uint32_t)) {
        uint32_t *hints = xcb_get_property_value(reply);
        const uint32_t MWM_HINTS_FUNCTIONS = 1u << 0;
        const uint32_t MWM_FUNC_ALL        = 1u << 0;
        const uint32_t MWM_FUNC_RESIZE     = 1u << 1;
        const uint32_t MWM_FUNC_MOVE       = 1u << 2;
        const uint32_t MWM_FUNC_MINIMIZE   = 1u << 3;
        const uint32_t MWM_FUNC_MAXIMIZE   = 1u << 4;
        const uint32_t MWM_FUNC_CLOSE      = 1u << 5;

        if (hints[0] & MWM_HINTS_FUNCTIONS) {
            uint32_t f = hints[1];
            bool listed_are_excluded = (f & MWM_FUNC_ALL) != 0;
            bool move     = (f & MWM_FUNC_MOVE) != 0;
            bool resize   = (f & MWM_FUNC_RESIZE) != 0;
            bool minimize = (f & MWM_FUNC_MINIMIZE) != 0;
            bool maximize = (f & MWM_FUNC_MAXIMIZE) != 0;
            bool close    = (f & MWM_FUNC_CLOSE) != 0;
            if (listed_are_excluded) {
                move = !move; resize = !resize; minimize = !minimize;
                maximize = !maximize; close = !close;
            }
            c->allow_move     = c->allow_move     && move;
            c->allow_resize   = c->allow_resize   && resize;
            c->allow_minimize = c->allow_minimize && minimize;
            c->allow_maximize = c->allow_maximize && maximize;
            c->allow_close    = c->allow_close    && close;
        }
    }
    free(reply);
}

/* Recomputes what this client permits (Client::allow_*) from its current
 * hints, and republishes _NET_WM_ALLOWED_ACTIONS. Everything starts
 * allowed and only gets taken away, so a client that declares nothing
 * behaves exactly as it always did.
 *
 * Two independent sources, both of which real apps use: ICCCM
 * WM_NORMAL_HINTS (min size == max size means unresizable, which also
 * rules out maximizing and tiling -- there'd be nothing to resize *to*),
 * and _MOTIF_WM_HINTS' functions field, still the only way a toolkit can
 * say "this dialog has no maximize button". Call after get_size_hints(),
 * and again whenever WM_NORMAL_HINTS changes. */
void update_client_actions(Client *c)
{
    c->allow_move = true;
    c->allow_resize = true;
    c->allow_minimize = true;
    c->allow_maximize = true;
    c->allow_close = true;

    if (c->hints_fixed_size) {
        c->allow_resize = false;
        c->allow_maximize = false;
    }

    apply_motif_functions(c);

    ewmh_update_allowed_actions(c);
}

/* Whether `state` is listed in the window's _NET_WM_STATE right now. */
static bool window_has_state(xcb_window_t window, xcb_atom_t state)
{
    xcb_get_property_reply_t *reply = xcb_get_property_reply(wm.conn,
        xcb_get_property(wm.conn, 0, window, wm.atoms.net_wm_state, XCB_ATOM_ATOM, 0, 32), NULL);
    if (!reply)
        return false;

    bool found = false;
    if (reply->type == XCB_ATOM_ATOM && reply->format == 32) {
        xcb_atom_t *atoms = xcb_get_property_value(reply);
        int n = xcb_get_property_value_length(reply) / (int)sizeof(xcb_atom_t);
        for (int i = 0; i < n && !found; i++)
            found = (atoms[i] == state);
    }
    free(reply);
    return found;
}

/* ICCCM window group: WM_HINTS' window_group field, falling back to
 * WM_CLIENT_LEADER for clients that set only that one. See wm.h's
 * Client::group_leader. */
static xcb_window_t window_group_leader(xcb_window_t window)
{
    xcb_icccm_wm_hints_t hints;
    if (xcb_icccm_get_wm_hints_reply(wm.conn, xcb_icccm_get_wm_hints(wm.conn, window), &hints, NULL) &&
        (hints.flags & XCB_ICCCM_WM_HINT_WINDOW_GROUP) && hints.window_group != XCB_NONE)
        return hints.window_group;

    xcb_get_property_reply_t *reply = xcb_get_property_reply(wm.conn,
        xcb_get_property(wm.conn, 0, window, wm.atoms.wm_client_leader, XCB_ATOM_WINDOW, 0, 1), NULL);
    if (!reply)
        return XCB_NONE;

    xcb_window_t leader = XCB_NONE;
    if (reply->type == XCB_ATOM_WINDOW && reply->format == 32 &&
        xcb_get_property_value_length(reply) >= (int)sizeof(xcb_window_t))
        leader = *(xcb_window_t *)xcb_get_property_value(reply);
    free(reply);
    return leader;
}

/* ICCCM WM_TRANSIENT_FOR (a predefined atom, no interning needed) -- the
 * window `window` is a transient of, or XCB_NONE. See wm.h's
 * Client::transient_for for what kiwm does with it. */
static xcb_window_t window_transient_for(xcb_window_t window)
{
    xcb_get_property_reply_t *reply = xcb_get_property_reply(wm.conn,
        xcb_get_property(wm.conn, 0, window, XCB_ATOM_WM_TRANSIENT_FOR, XCB_ATOM_WINDOW, 0, 1), NULL);
    if (!reply)
        return XCB_NONE;

    xcb_window_t parent = XCB_NONE;
    if (reply->type == XCB_ATOM_WINDOW && reply->format == 32 &&
        xcb_get_property_value_length(reply) >= (int)sizeof(xcb_window_t))
        parent = *(xcb_window_t *)xcb_get_property_value(reply);
    free(reply);

    /* Some toolkits point a transient at the root window to mean "transient
     * for the whole group" -- meaningless as a stacking relationship, and
     * root isn't a Client anyway. */
    return parent == wm.root ? XCB_NONE : parent;
}

/* ICCCM WM_STATE == IconicState: the window is minimized, and stays that
 * way across a WM handoff (both kiwm and every other WM set it). */
static bool window_is_iconic(xcb_window_t window)
{
    xcb_get_property_reply_t *reply = xcb_get_property_reply(wm.conn,
        xcb_get_property(wm.conn, 0, window, wm.atoms.wm_state, wm.atoms.wm_state, 0, 2), NULL);
    if (!reply)
        return false;

    bool iconic = false;
    if (reply->type == wm.atoms.wm_state && reply->format == 32 &&
        xcb_get_property_value_length(reply) >= (int)sizeof(uint32_t))
        iconic = (*(uint32_t *)xcb_get_property_value(reply) == WM_STATE_ICONIC);
    free(reply);
    return iconic;
}

/* Re-reads WM_TRANSIENT_FOR and restacks if it actually changed. Toolkits
 * don't all set the property before the window is mapped -- Qt in
 * particular can attach a transient parent after the fact -- and manage()
 * reading it once would then see nothing, leaving the window with no
 * stacking relationship at all. That's the whole difference between
 * VirtualBox's mini-toolbar working when kiwm adopts an already-running VM
 * (property long since set) and not working when the VM starts under a
 * running kiwm (property set moments after the map). Called from events.c
 * on every WM_TRANSIENT_FOR PropertyNotify. */
void client_refresh_transient_for(Client *c)
{
    xcb_window_t parent = window_transient_for(c->window);
    if (parent == c->transient_for)
        return;
    c->transient_for = parent;
    restack_all();
    xcb_flush(wm.conn);
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
    apply_frame_geometry_told(c, true);
}

void apply_frame_geometry_told(Client *c, bool tell_client)
{
    int bt, th;
    deco_insets(c, &bt, &th);

    c->frame_width = c->width + bt * 2;
    /* Shaded: only the titlebar shows, content stays unmapped (see
     * toggle_shade()) -- the frame collapses to exactly th tall, no
     * bottom border either since there's no content edge to border. */
    c->frame_height = c->shaded ? th : (c->height + th + bt);

    /* Both windows are configured only when their own geometry actually
     * changed, and they are asked separately because they do not change
     * together.
     *
     * The frame's is x/y/w/h in root coordinates: a move changes it, a
     * resize changes it, and this is the request that has to go out for
     * the window to follow the pointer at all.
     *
     * The content window's is x/y/w/h *inside the frame*, and that is
     * bt/th/width/height -- the insets are constant for a given
     * decoration state, so a plain move does not change a single one of
     * the four. Sending it anyway, as this used to on every motion event
     * of a move, hands the client a ConfigureNotify saying nothing
     * changed; Qt and GTK both re-run layout on that. Skipping it halves
     * the requests a move step costs and the client stops being woken for
     * news it already has.
     *
     * A cache rather than a "skip this in DRAG_MOVE" flag because it is
     * the honest test: whatever the reason the geometry is the same --
     * a move, a repeated motion event with the pointer barely moving, a
     * client asking to be resized to the size it already is -- the
     * request is equally pointless, and whenever something does change
     * (the decoration is toggled mid-drag and the insets move) it goes
     * out without anyone having to have thought of that case. */
    if (!c->geom_sent ||
        c->sent_frame_x != c->x || c->sent_frame_y != c->y ||
        c->sent_frame_w != c->frame_width || c->sent_frame_h != c->frame_height) {
        uint32_t fv[] = {
            (uint32_t)c->x, (uint32_t)c->y,
            (uint32_t)c->frame_width, (uint32_t)c->frame_height
        };
        xcb_configure_window(wm.conn, c->frame,
                             XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                             XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, fv);
        c->sent_frame_x = c->x;
        c->sent_frame_y = c->y;
        c->sent_frame_w = c->frame_width;
        c->sent_frame_h = c->frame_height;
    }

    if (!c->geom_sent ||
        c->sent_client_x != bt || c->sent_client_y != th ||
        c->sent_client_w != c->width || c->sent_client_h != c->height) {
        uint32_t cv[] = { (uint32_t)bt, (uint32_t)th, (uint32_t)c->width, (uint32_t)c->height };
        xcb_configure_window(wm.conn, c->window,
                             XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                             XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, cv);
        c->sent_client_x = bt;
        c->sent_client_y = th;
        c->sent_client_w = c->width;
        c->sent_client_h = c->height;
    }

    c->geom_sent = true;

    /* Not skipped for being unchanged, whatever the two above did. This
     * one carries the client's *root-relative* position, which a move
     * changes even though nothing about the client's geometry within the
     * frame did -- and it is the only thing that tells a client its
     * request was denied when handle_configure_request() re-affirms the
     * geometry of a maximized window (ICCCM 4.1.5).
     *
     * It is a SendEvent and costs kiwm no drawing. It costs the *client*
     * something, though: a toolkit answers a ConfigureNotify by
     * re-deriving its screen position and whatever depends on it, and
     * during a move drag this went out on every motion event -- 125 a
     * second from an ordinary mouse, for a position the display shows
     * 60 of. Measured on a move drag of a Kate window: 1.5 points of
     * GPU busy. So a drag sends it once per frame (tell_client from
     * handle_motion(), true on the frame's due step and false between),
     * and finish_drag()'s unconditional apply sends the final one. Every
     * other caller passes true: outside a drag each apply is one
     * event, and the client is owed it. */
    if (tell_client)
        send_synthetic_configure(c, bt, th);

    /* The invisible resize ring lives outside this frame, so it has to
     * follow it (grip.h). A no-op mid-drag, where the ring is unreachable
     * anyway and this function runs on every motion event -- finish_drag()
     * catches it up once at the end. */
    grip_sync(c);
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

    shape_update_frame(c);
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

/* Asks the X server to re-expose everything that sits inside `rect` and is
 * now on top of it, so those windows repaint.
 *
 * A window dropping down the stacking order uncovers whatever was beneath
 * it, and X normally sends those windows an Expose for the uncovered part
 * -- but only for regions it knows became visible. A window that was fully
 * covered by a fullscreen window and is *still* fully covered by it in
 * screen terms (a panel, say, that just moved above it in the stack) gets
 * no such notification, and is left showing the pixels the fullscreen
 * window painted over it: the "the game's picture stays on the panels
 * after it loses focus" symptom. ClearArea with exposures=1 asks for those
 * Expose events explicitly. It only ever *clears* to the window's own
 * background (None for practically every toolkit window, so nothing is
 * painted at all) -- the point is purely the Expose it generates. */
static void expose_windows_over(int rx, int ry, int rw, int rh)
{
    for (int i = 0; i < wm.dock_count; i++)
        xcb_clear_area(wm.conn, 1, wm.docks[i].window, 0, 0, 0, 0);

    for (Client *o = wm.clients; o; o = o->next) {
        if (!o->mapped || o->minimized)
            continue;
        if (o->x >= rx + rw || o->x + o->frame_width <= rx ||
            o->y >= ry + rh || o->y + o->frame_height <= ry)
            continue;
        xcb_clear_area(wm.conn, 1, o->frame, 0, 0, 0, 0);
        xcb_clear_area(wm.conn, 1, o->window, 0, 0, 0, 0);
    }
    xcb_flush(wm.conn);
}

/* One round of expose_windows_over() isn't enough for what a *fullscreen*
 * window leaves behind, because of how the X server presents one without a
 * compositor: a window that covers a whole output and is topmost and
 * unobscured gets page-flipped straight to the scanout (DRI3/Present), so
 * while it's up the server's own screen pixmap is stale -- everything
 * other clients draw during that time goes into a buffer nobody is looking
 * at. When the window stops qualifying (kiwm drops it out of
 * LAYER_ACTIVE_FULLSCREEN the moment it loses focus, see client_layer()),
 * the server "unflips" by copying that last flipped frame -- the video
 * frame -- into the screen pixmap, and *then* starts scanning the screen
 * pixmap out again. Anything repainted between the restack and that copy
 * is wiped by it, which is exactly when the immediate expose round lands:
 * the panel/other windows dutifully repaint, and then get overwritten with
 * the video's pixels, leaving them looking corrupted until something else
 * happens to make them repaint on their own (this is the "às vezes" in the
 * bug report -- it depends on whether a flip was actually in effect).
 *
 * So the exposes are also re-sent a couple of times over the next half
 * second, after the unflip has certainly settled. Verified live: an
 * xrefresh over the same region right away leaves the window corrupted,
 * the same xrefresh a second later restores it.
 *
 * Three rounds in all, then: the immediate one, +150ms and +500ms. Which
 * of them actually happen is kiwm.conf's force_unflip= -- see
 * force_unflip_wanted() below. */
static struct {
    bool active;
    int rx, ry, rw, rh;
    double due[2];   /* monotonic_ms() deadlines, ascending */
    int next;        /* index into due[] of the next round to fire */
} pending_expose;

/* Whether the server is one that page-flips at all, which is the whole
 * premise of the delayed rounds: no Present, no flip, no unflip, nothing
 * to repair. Which *outputs* it flips separately (the modeset driver's
 * PerCRTCFlip, the case that actually bites here, since it flips a window
 * covering one output of several) is a driver option no client can ask
 * about -- Present's own capabilities are about async/fence/UST -- so
 * "the server can flip" is as close as a runtime probe gets.
 *
 * Asked once: an extension does not appear or disappear under a running
 * connection. */
static bool server_page_flips(void)
{
    static int cached = -1;
    if (cached >= 0)
        return cached != 0;

    xcb_query_extension_reply_t *r = xcb_query_extension_reply(wm.conn,
        xcb_query_extension(wm.conn, 7, "Present"), NULL);
    cached = (r && r->present) ? 1 : 0;
    free(r);
    return cached != 0;
}

/* kiwm.conf's force_unflip=.
 *
 * A compositor redirects every window offscreen and paints the screen
 * itself, so no client window is ever the topmost unobscured thing the
 * server could flip, and none of this repair is needed -- hence the
 * compositor question, which selection.h answers for the whole of kiwm
 * (this file used to ask it privately, re-interning the selection atom on
 * every call). */
static bool force_unflip_wanted(void)
{
    switch (wm.force_unflip) {
    case FORCE_UNFLIP_NEVER:
        return false;
    case FORCE_UNFLIP_ALWAYS:
        return true;
    default:
        return !compositor_running() && server_page_flips();
    }
}

static void queue_expose_windows_over(int rx, int ry, int rw, int rh)
{
    expose_windows_over(rx, ry, rw, rh);

    if (!force_unflip_wanted()) {
        pending_expose.active = false;
        return;
    }

    double now = monotonic_ms();
    pending_expose.active = true;
    pending_expose.rx = rx;
    pending_expose.ry = ry;
    pending_expose.rw = rw;
    pending_expose.rh = rh;
    pending_expose.due[0] = now + 150.0;
    pending_expose.due[1] = now + 500.0;
    pending_expose.next = 0;
}

int client_pending_expose_timeout_ms(void)
{
    if (!pending_expose.active)
        return -1;
    double left = pending_expose.due[pending_expose.next] - monotonic_ms();
    return left <= 0 ? 0 : (int)(left + 0.5);
}

/* ------------------------------------------------------------------ */
/* holding a hidden window up to be photographed (client.h)            */
/* ------------------------------------------------------------------ */

/* As long as anyone could reasonably want, and short enough that a
 * requester which dies mid-picture is not noticed. Whoever wants longer
 * asks again. */
#define HOLD_MAX_MS 2000

static void hold_input_shape(Client *c, bool none)
{
    if (!wm.shape_ext_present)
        return;

    if (none) {
        /* No input rectangles at all: the frame is on screen but cannot
         * be clicked, which is what keeps a window that is only being
         * looked at from taking a click meant for the desktop under it. */
        xcb_shape_rectangles(wm.conn, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_INPUT,
                             XCB_CLIP_ORDERING_UNSORTED, c->frame, 0, 0, 0, NULL);
    } else {
        /* And back to "the whole window", which is what no input shape
         * at all means. */
        xcb_shape_mask(wm.conn, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_INPUT,
                       c->frame, 0, 0, XCB_PIXMAP_NONE);
    }
}

void client_hold(xcb_window_t window, int ms)
{
    Client *c = find_client_window(window);
    if (!c)
        return;

    /* Only a window that is away with its desktop. Everything else is
     * either already on screen or put away for a reason of its own, and
     * neither is this request's business.
     *
     * Which desktop its output is showing is the whole test: `mapped` is
     * not the frame's map state but the client's own "belongs on screen"
     * (see switch_workspace, which unmaps the frame and leaves the flag
     * alone), so a window away with its desktop still has it set. */
    if (c->minimized || c->shaded || c->sticky)
        return;
    if (c->output < 0 || c->output >= wm.output_count)
        return;
    if (wm.outputs[c->output].desktop == c->desktop)
        return;

    if (ms <= 0)
        return;
    if (ms > HOLD_MAX_MS)
        ms = HOLD_MAX_MS;

    if (c->hold_until == 0.0) {
        /* Marked before it is mapped, so that a compositor reading the
         * events in order knows what the map is before it sees it. */
        uint32_t one = 1;
        xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE, c->frame,
                            wm.atoms.kiwm_held, XCB_ATOM_CARDINAL, 32, 1, &one);

        hold_input_shape(c, true);

        /* And it is left exactly where it is in the stack.
         *
         * Twice now the tidying was the bug. Lowering it to the bottom
         * put it under the wallpaper -- the bottom belongs to the
         * desktop layers -- and lowering it to just above "the topmost
         * layer that is mapped" is no better with one wallpaper per
         * monitor, since that answer is a single window and the layer
         * covering *this* monitor may be a different one. Either way the
         * window ends up behind a wallpaper, invisible in the very grid
         * that asked to see it, and invisible again when its desktop
         * comes back, because nothing restacks it afterwards.
         *
         * There is nothing to tidy anyway: whoever asks for this is
         * drawing the screen themselves for the second or two it lasts,
         * and a window that keeps its place is a window nobody has to
         * put back. */
        xcb_map_window(wm.conn, c->frame);

        /* And ask for it to be *drawn*. X threw the contents away when
         * the frame was unmapped, but the application was never told
         * anything went missing -- as far as it knows its window is
         * intact -- so it repaints only what it happens to change next,
         * which for a terminal is one line at a time over a window that
         * is otherwise empty. Exposing it is the standard way of saying
         * "you have lost your contents, draw them again". */
        draw_decoration(c);
        xcb_clear_area(wm.conn, 1, c->window, 0, 0, 0, 0);
    }

    c->hold_until = monotonic_ms() + ms;
    xcb_flush(wm.conn);
}

void client_release_hold(Client *c)
{
    if (c->hold_until == 0.0)
        return;

    c->hold_until = 0.0;

    /* Back where it was: unmapped, its own input shape, and no mark. The
     * mark is deleted *after* the unmap, so the same reader that saw the
     * map explained still has the explanation when the window goes. */
    xcb_unmap_window(wm.conn, c->frame);
    hold_input_shape(c, false);
    xcb_delete_property(wm.conn, c->frame, wm.atoms.kiwm_held);
}

int client_hold_timeout_ms(void)
{
    double soonest = 0.0;

    for (Client *c = wm.clients; c; c = c->next)
        if (c->hold_until != 0.0 && (soonest == 0.0 || c->hold_until < soonest))
            soonest = c->hold_until;
    if (soonest == 0.0)
        return -1;

    double left = soonest - monotonic_ms();
    return left <= 0.0 ? 0 : (int)(left + 0.5);
}

void client_run_holds(void)
{
    double now = monotonic_ms();
    bool any = false;

    for (Client *c = wm.clients; c; c = c->next) {
        if (c->hold_until == 0.0 || now < c->hold_until)
            continue;
        client_release_hold(c);
        any = true;
    }

    if (any)
        xcb_flush(wm.conn);
}

void client_run_pending_expose(void)
{
    if (!pending_expose.active)
        return;
    if (monotonic_ms() < pending_expose.due[pending_expose.next])
        return;

    expose_windows_over(pending_expose.rx, pending_expose.ry,
                        pending_expose.rw, pending_expose.rh);
    if (++pending_expose.next >= (int)(sizeof(pending_expose.due) / sizeof(pending_expose.due[0])))
        pending_expose.active = false;
}

/* ICCCM's WM_TAKE_FOCUS: for a client that lists it in WM_PROTOCOLS,
 * SetInputFocus alone is only half of handing over the keyboard -- the
 * client also has to be told, so it can route the focus internally (to the
 * right sub-window, and, for a toolkit, so it updates its own idea of
 * which window is active at all). Every Qt/KDE window asks for this;
 * krunner is where skipping it actually shows, opening with its input
 * field dead even though X focus is already on it. The message must carry
 * a real timestamp, never CurrentTime, which is what wm.last_event_time
 * exists for. */
static void send_take_focus(Client *c)
{
    if (!c->takes_focus || wm.atoms.wm_protocols == XCB_ATOM_NONE ||
        wm.atoms.wm_take_focus == XCB_ATOM_NONE)
        return;

    xcb_client_message_event_t ev = {
        .response_type = XCB_CLIENT_MESSAGE,
        .format = 32,
        .window = c->window,
        .type = wm.atoms.wm_protocols
    };
    ev.data.data32[0] = wm.atoms.wm_take_focus;
    ev.data.data32[1] = wm.last_event_time;

    xcb_send_event(wm.conn, 0, c->window, XCB_EVENT_MASK_NO_EVENT, (const char *)&ev);
}

/* _NET_WM_USER_TIME for a window: the server timestamp of the last user
 * interaction with it, which is the whole basis of focus-stealing
 * prevention -- a window whose last interaction predates the focused
 * window's is asking for focus on its own behalf, not the user's.
 *
 * Read live rather than cached: a focus decision happens when a window
 * maps or when an application sends _NET_ACTIVE_WINDOW, both rare, and
 * reading on the spot means kiwm doesn't have to track PropertyNotify on
 * the property *or* on the separate _NET_WM_USER_TIME_WINDOW some
 * toolkits point it at (which would need its own event mask selected on
 * a window kiwm otherwise never touches).
 *
 * *found is false when the window carries no such property at all, which
 * is a different thing from a time of 0: 0 is EWMH's explicit "do not
 * focus me when I map". */
static xcb_timestamp_t client_user_time(Client *c, bool *found)
{
    *found = false;
    if (wm.atoms.net_wm_user_time == XCB_ATOM_NONE)
        return 0;

    /* The property may live on a window of the client's choosing. */
    xcb_window_t src = c->window;
    xcb_get_property_reply_t *w = xcb_get_property_reply(wm.conn,
        xcb_get_property(wm.conn, 0, c->window, wm.atoms.net_wm_user_time_window,
                         XCB_ATOM_WINDOW, 0, 1), NULL);
    if (w) {
        if (w->type == XCB_ATOM_WINDOW && w->format == 32 &&
            xcb_get_property_value_length(w) >= (int)sizeof(xcb_window_t))
            src = *(xcb_window_t *)xcb_get_property_value(w);
        free(w);
    }

    xcb_get_property_reply_t *t = xcb_get_property_reply(wm.conn,
        xcb_get_property(wm.conn, 0, src, wm.atoms.net_wm_user_time,
                         XCB_ATOM_CARDINAL, 0, 1), NULL);
    xcb_timestamp_t out = 0;
    if (t) {
        if (t->type == XCB_ATOM_CARDINAL && t->format == 32 &&
            xcb_get_property_value_length(t) >= 4) {
            out = *(uint32_t *)xcb_get_property_value(t);
            *found = true;
        }
        free(t);
    }
    return out;
}

/* Whether `a` and `b` belong to the same application, as far as anything
 * X hands a WM can tell: the same ICCCM group leader, or one being a
 * transient of the other (a dialog and the window it belongs to). */
static bool same_application(Client *a, Client *b)
{
    if (!a || !b)
        return false;
    if (a == b)
        return true;
    if (a->group_leader != XCB_NONE && a->group_leader == b->group_leader)
        return true;
    return a->transient_for == b->window || b->transient_for == a->window;
}

bool focus_request_allowed(Client *c, bool user_driven)
{
    /* The user asking is never "stealing", and neither is the very first
     * window on an empty desktop -- there is nothing to steal from. */
    if (user_driven || wm.focus_stealing_prevention == FSP_NONE || !wm.focused || wm.focused == c)
        return true;

    bool found;
    xcb_timestamp_t ut = client_user_time(c, &found);

    /* EWMH's explicit opt-out, honored at every level that is on at all:
     * a window that maps with _NET_WM_USER_TIME 0 is telling the WM it
     * does not want focus (a mail client starting into the tray, a
     * session-restored window). */
    if (found && ut == 0)
        return false;

    if (wm.focus_stealing_prevention == FSP_LOW)
        return true;

    if (wm.focus_stealing_prevention == FSP_EXTREME)
        return false;

    if (wm.focus_stealing_prevention == FSP_HIGH)
        return same_application(c, wm.focused);

    /* FSP_NORMAL: the window may have focus if the user has interacted
     * with it at least as recently as with the focused window. A window
     * carrying no user time at all gets the benefit of the doubt here --
     * plenty of small/old apps never set the property, and refusing all
     * of them at the *default-ish* level would be its own kind of
     * annoying. Timestamps are compared as signed differences so the
     * server's 32-bit millisecond clock wrapping (every ~49 days of
     * uptime) doesn't invert the comparison. */
    if (!found)
        return true;
    bool active_found;
    xcb_timestamp_t active_ut = client_user_time(wm.focused, &active_found);
    if (!active_found)
        return true;
    return (int32_t)(ut - active_ut) >= 0;
}

/* A refused request, turned into something the user can act on: EWMH's
 * _NET_WM_STATE_DEMANDS_ATTENTION, which is what makes a taskbar
 * highlight the entry (xispanel's tasklist reads it). The window stays
 * exactly where it is -- mapped, unfocused, unraised. */
void deny_focus_request(Client *c)
{
    if (c->demands_attention)
        return;
    c->demands_attention = true;
    ewmh_update_wm_state(c);
    xcb_flush(wm.conn);
}

void focus_client(Client *c)
{
    if (!c)
        return;

    Client *old = wm.focused;

    /* Whatever it was asking for attention about, it has it now. */
    if (c->demands_attention) {
        c->demands_attention = false;
        ewmh_update_wm_state(c);
    }

    if (old != c) {
        wm.focused = c;
        /* kiwm's focus history, for the switcher's most-recently-used
         * order (osd.c) -- see Client::last_focus_serial. */
        c->last_focus_serial = ++wm.focus_serial;
        xcb_set_input_focus(wm.conn, XCB_INPUT_FOCUS_POINTER_ROOT,
                            c->window, XCB_CURRENT_TIME);
        send_take_focus(c);
        if (old)
            draw_decoration(old);
        ewmh_update_active_window();
    }

    restack_all_raising(c);

    /* A fullscreen window that just lost focus also just left
     * LAYER_ACTIVE_FULLSCREEN (see client_layer()), so everything it was
     * covering -- panels above all -- is now on top of it and has to
     * repaint over the pixels it left behind. */
    if (old && old != c && old->fullscreen)
        queue_expose_windows_over(old->x, old->y, old->frame_width, old->frame_height);

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
    /* A window that declares no close function (Motif's MWM_FUNC_CLOSE) is
     * saying it must be dismissed through its own UI -- an installer's
     * progress dialog, say. kiwm hides the close button for it, and
     * refuses here too so a taskbar or a shortcut can't do what the
     * titlebar won't. */
    if (!c->allow_close)
        return;

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

/* The top of a client's WM_TRANSIENT_FOR chain -- itself for a plain
 * top-level. A transient's layer is its *parent's* layer (see
 * client_layer() below), so this is what actually gets asked about state. */
static Client *transient_root(Client *c)
{
    for (int guard = 0; guard < MAX_CLIENTS && c->transient_for; guard++) {
        Client *p = find_client_window(c->transient_for);
        if (!p || p == c)
            break;
        c = p;
    }
    return c;
}

/* A client's stacking layer, derived from its state -- see wm.h's WmLayer.
 *
 * Everything is answered about the *transient root*, not the client
 * itself, so a window and its dialogs/toolbars always land in the same
 * layer and lift_transients() can then order them within it. Splitting
 * them across layers is what made VirtualBox's mini-toolbar sink behind
 * the VM window: the VM going fullscreen+focused moved it to a layer the
 * toolbar (a transient, plain-normal on its own) couldn't be lifted into,
 * so no amount of within-layer ordering could keep the two together.
 *
 * A fullscreen root claims LAYER_ACTIVE_FULLSCREEN (above docks and
 * everything else) only while it or one of its transients holds focus --
 * see WmLayer. keep_above/keep_below are kept mutually exclusive by
 * toggle_keep_above()/toggle_keep_below(), so this never has to arbitrate
 * between them. */
/* Whether two clients are the same "application unit" for stacking: one is
 * a transient of the other (directly or up the chain), or they declare the
 * same ICCCM window group. */
static bool same_window_unit(Client *a, Client *b)
{
    if (!a || !b)
        return false;
    if (a == b || transient_root(a) == transient_root(b))
        return true;
    return a->group_leader != XCB_NONE && a->group_leader == b->group_leader;
}

static WmLayer client_layer(Client *c)
{
    Client *root = transient_root(c);

    /* The active-fullscreen layer is claimed by the whole unit, not just
     * the one window: whichever member has focus lifts every window that
     * belongs with it, as long as some member is actually fullscreen.
     * Without that, VirtualBox's mini-toolbar (same group, no transient
     * relation, itself marked fullscreen) sinks below the panels the
     * instant the VM window it floats over takes focus -- and focusing the
     * toolbar would drop the VM out from under it in the same way. */
    if (wm.focused && same_window_unit(c, wm.focused)) {
        for (Client *o = wm.clients; o; o = o->next)
            if (o->fullscreen && same_window_unit(o, c))
                return LAYER_ACTIVE_FULLSCREEN;
    }
    if (root->fullscreen && wm.focused && transient_root(wm.focused) == root)
        return LAYER_ACTIVE_FULLSCREEN;
    if (root->keep_above)
        return LAYER_ABOVE;
    if (root->keep_below)
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
/* One window taking part in the stacking order: a managed client (placed
 * via its frame, `client` set) or a tracked dock/panel (`client` NULL --
 * a dock is never a Client, but still has to be ordered against them). */
typedef struct {
    xcb_window_t window;
    Client *client;
} StackEntry;

/* Reorders one layer's bottom-to-top list so every transient window sits
 * above the window it's transient for (ICCCM WM_TRANSIENT_FOR, see wm.h's
 * Client::transient_for). Without this, focusing the parent raises it to
 * the top of the layer and buries its own dialog -- or, the case that
 * prompted it, VirtualBox's mini-toolbar, which is a transient of the VM
 * window it floats above and would vanish behind it the moment the VM got
 * focus (the toolbar is only ever "on top" because a WM keeps transients
 * there; VirtualBox never restacks it itself).
 *
 * Repeatedly moves the lowest offending transient to just above its
 * parent, which also settles chains (a transient of a transient) since
 * each move only ever pushes a window upward. Capped at one move per
 * client so a pathological WM_TRANSIENT_FOR cycle can't spin here. */
static void lift_transients(StackEntry *b, int n)
{
    for (int guard = 0; guard < n; guard++) {
        bool moved = false;
        for (int i = 0; i < n && !moved; i++) {
            if (!b[i].client || !b[i].client->transient_for)
                continue;
            for (int p = i + 1; p < n; p++) {
                if (!b[p].client || b[p].client->window != b[i].client->transient_for)
                    continue;
                StackEntry t = b[i];
                for (int k = i; k < p; k++)
                    b[k] = b[k + 1];
                b[p] = t;
                moved = true;
                break;
            }
        }
        if (!moved)
            return;
    }
}

/* Second ordering pass, for windows that belong together but declare no
 * transient relationship: within a layer, an auxiliary window (one marked
 * _NET_WM_STATE_SKIP_TASKBAR -- the client's own "I'm not a window you
 * switch to") is kept above the ordinary windows of its group.
 *
 * VirtualBox's fullscreen mini-toolbar is the case this exists for: same
 * ICCCM group as the VM window, skip-taskbar, no WM_TRANSIENT_FOR at all,
 * and floating over a window that gets raised every time it's focused. The
 * app expects the WM to keep its chrome on top and never restacks it
 * itself, so without this it disappears under the VM the moment you click
 * into the VM. */
static void lift_group_aux(StackEntry *b, int n)
{
    for (int guard = 0; guard < n; guard++) {
        bool moved = false;
        for (int i = 0; i < n && !moved; i++) {
            Client *aux = b[i].client;
            if (!aux || !aux->skip_taskbar || aux->group_leader == XCB_NONE)
                continue;
            for (int p = n - 1; p > i; p--) {
                Client *other = b[p].client;
                if (!other || other->skip_taskbar || other->group_leader != aux->group_leader)
                    continue;
                StackEntry t = b[i];
                for (int k = i; k < p; k++)
                    b[k] = b[k + 1];
                b[p] = t;
                moved = true;
                break;
            }
        }
        if (!moved)
            return;
    }
}

/* Moves `promote`'s entry within its own bucket to the top (or the
 * bottom) of it, leaving every other entry's relative order alone. This
 * is the in-memory half of restack_all_raising/lowering(): see client.h
 * for why it must not be a STACK_MODE_ABOVE sent to the server first. */
static void bucket_promote(StackEntry *b, int n, Client *promote, bool to_top)
{
    int at = -1;
    for (int i = 0; i < n; i++) {
        if (b[i].client == promote) {
            at = i;
            break;
        }
    }
    if (at < 0)
        return;

    StackEntry moved = b[at];

    /* Buckets are bottom-to-top, so the top of a layer is the end of its
     * array. */
    if (to_top) {
        for (int i = at; i < n - 1; i++)
            b[i] = b[i + 1];
        b[n - 1] = moved;
    } else {
        for (int i = at; i > 0; i--)
            b[i] = b[i - 1];
        b[0] = moved;
    }
}

static void restack_all_core(Client *promote, bool to_top)
{
    xcb_query_tree_reply_t *tree =
        xcb_query_tree_reply(wm.conn, xcb_query_tree(wm.conn, wm.root), NULL);
    if (!tree)
        return;

    xcb_window_t *kids = xcb_query_tree_children(tree);
    int nkids = xcb_query_tree_children_length(tree);

    StackEntry buckets[LAYER_COUNT][MAX_CLIENTS + MAX_DOCKS];
    int bn[LAYER_COUNT] = { 0 };
    const int bmax = MAX_CLIENTS + MAX_DOCKS;

    /* xcb_query_tree()'s children come back bottom-to-top, so walking them
     * in order and appending each one to its layer's bucket naturally
     * preserves that same relative order within the bucket. Two kinds of
     * window take part: a managed client (via its frame) and a tracked
     * dock/panel, which is never a Client but still has to be placed
     * relative to them -- see wm.h's LAYER_DOCK. */
    for (int i = 0; i < nkids; i++) {
        Client *c = find_client_window(kids[i]);
        WmLayer l;

        if (c && c->frame == kids[i]) {
            l = client_layer(c);
        } else if (!c && dock_is_tracked(kids[i])) {
            l = LAYER_DOCK;
        } else if (!c && (osd_owns_window(kids[i]) || window_menu_owns_window(kids[i]))) {
            l = LAYER_OSD;
        } else if (!c && outline_owns_window(kids[i])) {
            l = LAYER_OUTLINE;
        } else if (!c && grip_owns_window(kids[i])) {
            /* A resize ring is placed relative to its own frame, not
             * sorted into a layer -- the chaining loop below emits it
             * directly under the frame it belongs to (grip.h). */
            continue;
        } else {
            continue;
        }

        if (bn[l] < bmax)
            buckets[l][bn[l]++] = (StackEntry){ .window = kids[i], .client = c };
    }
    free(tree);

    for (int l = 0; l < LAYER_COUNT; l++) {
        /* Before the layer's own ordering rules, not after: a client
         * being raised goes to the top of its layer, and then
         * lift_transients()/lift_group_aux() get to pull its transients
         * and its group's chrome back above it. Doing it the other way
         * round is what put the mini-toolbar below the VM window. */
        if (promote)
            bucket_promote(buckets[l], bn[l], promote, to_top);
        lift_transients(buckets[l], bn[l]);
        lift_group_aux(buckets[l], bn[l]);
    }

    /* Chain every window to sit directly above the previous one, walking
     * layers bottom to top -- one xcb_configure_window() each (besides the
     * very first, which is left wherever it already is; nothing needs to
     * be below it). */
    xcb_window_t prev = XCB_NONE;
    for (int l = 0; l < LAYER_COUNT; l++) {
        for (int i = 0; i < bn[l]; i++) {
            /* A client's invisible resize ring goes immediately under its
             * frame, so a click in the margin between two adjacent windows
             * reaches whichever of them is actually on top (grip.h).
             * Chained here rather than left where it was created, because
             * everything else in the stack moves around it. */
            xcb_window_t ring = buckets[l][i].client
                                    ? grip_ring_window(buckets[l][i].client)
                                    : XCB_NONE;

            if (ring != XCB_NONE) {
                if (prev != XCB_NONE) {
                    uint32_t values[] = { prev, XCB_STACK_MODE_ABOVE };
                    xcb_configure_window(wm.conn, ring,
                                         XCB_CONFIG_WINDOW_SIBLING | XCB_CONFIG_WINDOW_STACK_MODE, values);
                }
                prev = ring;
            }

            if (prev != XCB_NONE) {
                uint32_t values[] = { prev, XCB_STACK_MODE_ABOVE };
                xcb_configure_window(wm.conn, buckets[l][i].window,
                                     XCB_CONFIG_WINDOW_SIBLING | XCB_CONFIG_WINDOW_STACK_MODE, values);
            }
            prev = buckets[l][i].window;
        }
    }
}

void restack_all(void)
{
    restack_all_core(NULL, true);
}

void restack_all_raising(Client *c)
{
    restack_all_core(c, true);
}

void restack_all_lowering(Client *c)
{
    restack_all_core(c, false);
}

void toggle_keep_above(Client *c, int want /* -1=toggle 0=off 1=on */)
{
    bool target = (want == -1) ? !c->keep_above : (want == 1);
    if (target == c->keep_above)
        return;
    c->keep_above = target;
    if (target) {
        c->keep_below = false; /* mutually exclusive, see client_layer() */
        restack_all_raising(c);
    } else {
        restack_all();
    }
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
        restack_all_lowering(c);
    } else {
        restack_all();
    }
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

    /* State first, then the geometry that carries it out -- see the note
     * above ewmh_update_wm_state()'s other callers in this file. */
    ewmh_update_wm_state(c);

    if (c->mapped) {
        if (target)
            xcb_unmap_window(wm.conn, c->window);
        else
            xcb_map_window(wm.conn, c->window);
    }

    configure_frame(c);
    xcb_flush(wm.conn);
}

/* The geometry a maximized client should have *right now*: the output's
 * usable area (screen minus any dock/panel struts, see output.c's
 * compute_output_workarea()), not the raw output rect -- a maximized
 * window must never cover a taskbar. Split out from toggle_maximize()
 * because the answer changes over a window's lifetime, whenever a panel
 * appears, disappears, moves output or changes its strut -- see
 * refit_tiled_clients(). Sets geometry only; the caller owns the axis
 * flags
 * and the redraw. */
static void apply_maximized_geometry(Client *c)
{
    int wx, wy, ww, wh;
    compute_output_workarea(c->output >= 0 ? c->output : 0, &wx, &wy, &ww, &wh);

    int bt, th;
    deco_insets(c, &bt, &th);

    /* Per axis: a horizontally-maximized window keeps whatever height and
     * y it had, and vice versa. Both together is the ordinary maximize. */
    if (c->max_horz) {
        c->x = wx;
        c->width = ww - bt * 2;
        if (c->width < c->min_w) c->width = c->min_w;
    }
    if (c->max_vert) {
        c->y = wy;
        c->height = wh - th - bt;
        if (c->height < c->min_h) c->height = c->min_h;
    }
}

/* A fullscreen window's geometry is its output's, whole -- not the
 * workarea, since fullscreen covers docks too. Shared by everything that
 * has to (re-)apply it: adoption, a workarea/output change
 * (refit_tiled_clients() below), and dragging a fullscreen window to a
 * different output (events.c). */
void client_apply_fullscreen_geometry(Client *c)
{
    if (c->output < 0 || c->output >= wm.output_count)
        return;
    c->x = wm.outputs[c->output].x;
    c->y = wm.outputs[c->output].y;
    c->width = wm.outputs[c->output].width;
    c->height = wm.outputs[c->output].height;
}

/* Re-derives the geometry of every client whose size isn't its own choice
 * -- maximized, half-tiled or fullscreen -- from the *current* outputs and
 * workarea. Two things need this:
 *
 *   - Startup/adoption: manage_existing_windows() frames windows in
 *     xcb_query_tree() order, and a panel is just another window in that
 *     list, so a maximized window adopted *before* the panel it shares a
 *     screen with computed its size against a workarea that didn't have
 *     that panel's strut in it yet -- coming up covering the taskbar. This
 *     runs once at the end, when every dock is known.
 *   - Any later workarea change (a panel appearing, quitting, moving,
 *     resizing, or changing its strut) -- output.c's ewmh_set_workarea()
 *     calls this for the same reason, so a maximized window follows a
 *     panel that shows up long after it did.
 *
 * Floating windows are deliberately untouched: their geometry is the
 * user's, not kiwm's to recompute. */
void refit_tiled_clients(void)
{
    bool any = false;

    for (Client *c = wm.clients; c; c = c->next) {
        if (c->fullscreen) {
            client_apply_fullscreen_geometry(c);
        } else if (c->max_horz || c->max_vert) {
            apply_maximized_geometry(c);
        } else if (c->snap_side != SNAP_NONE) {
            snap_client_to_side(c, c->snap_side);
        } else {
            continue;
        }

        configure_frame(c);
        ewmh_update_frame_extents(c);
        any = true;
    }

    if (any)
        xcb_flush(wm.conn);
}

/* The one place maximization state changes (see client.h): sets the two axis flags to
 * `horz`/`vert` and makes the geometry match, capturing (or handing back)
 * the floating geometry as the window enters or leaves maximization.
 *
 * "Enters" means going from neither axis to either one, which is what
 * makes the single-axis states compose the way they should: maximizing
 * horizontally and then fully keeps the *original* floating geometry as
 * the restore target, rather than replacing it with the
 * horizontally-maximized one halfway through. Only the axes being given
 * up are restored, so unmaximizing one axis of a fully maximized window
 * leaves the other where it is. */
void client_set_maximized(Client *c, bool horz, bool vert)
{
    bool was_any = c->max_horz || c->max_vert;
    bool want_any = horz || vert;

    if (horz == c->max_horz && vert == c->max_vert)
        return;

    /* Refused for a window that says it can't be maximized (see
     * update_client_actions()) -- the titlebar button for it isn't even
     * drawn, but the same operation is reachable from a taskbar, a
     * shortcut, the window menu and a _NET_WM_STATE client message, and
     * all of them have to agree. Un-maximizing is always allowed:
     * whatever put the window in that state, it has to be possible to get
     * out of it. */
    if (want_any && !was_any && !c->allow_maximize)
        return;

    if (want_any && !was_any) {
        unshade_now(c);

        /* Only capture the "restore" geometry when currently floating --
         * if the window is already half-snapped (Client::snap_side), that
         * state's own saved_x/y/w/h already holds the true pre-tiling
         * geometry from whenever tiling was first entered, and clicking
         * maximize directly (no drag involved) must not clobber it with
         * the half-snapped size instead. See the drag-path equivalent in
         * events.c's try_edge_snap(). */
        if (c->snap_side == SNAP_NONE) {
            c->saved_x = c->x;
            c->saved_y = c->y;
            c->saved_w = c->width;
            c->saved_h = c->height;
        }
        c->snap_side = SNAP_NONE;
    }

    /* Give back the axes being dropped before applying the ones being
     * taken, so a window that swaps one axis for the other lands on the
     * floating geometry for the axis it just gave up. */
    if (c->max_horz && !horz) {
        c->x = c->saved_x;
        c->width = c->saved_w;
    }
    if (c->max_vert && !vert) {
        c->y = c->saved_y;
        c->height = c->saved_h;
    }

    c->max_horz = horz;
    c->max_vert = vert;
    apply_maximized_geometry(c);

/* The state a window is in is published *before* the geometry (or the
 * map/unmap) that carries it out, in every path below. It reads
 * backwards, and it matters: a compositor watching from outside sees the
 * ConfigureNotify and has to know what it means -- a shade, a maximize, a
 * fullscreen, or just a resize -- and the only evidence is _NET_WM_STATE.
 * Announced afterwards, that evidence arrives after the event it
 * explains, and by then the wrong animation is already running (kicomp's
 * geometry effect sliding a window that was being rolled up). Announcing
 * first costs nothing here and makes the order say what happened. */
    ewmh_update_wm_state(c);

    configure_frame(c);
    ewmh_update_frame_extents(c);
    xcb_flush(wm.conn);
}

void toggle_maximize(Client *c, int want /* -1=toggle 0=unmax 1=max */)
{
    /* Plain "maximize": both axes. From a single-axis state this takes
     * the window the rest of the way (kwin does the same) rather than
     * restoring it -- restoring is then the *next* press, and it goes
     * back to the geometry from before any of it, since that's what
     * saved_x/y/w/h has held all along. */
    bool target = (want == -1) ? !client_maximized(c) : (want == 1);
    client_set_maximized(c, target, target);
}

void toggle_maximize_horz(Client *c, int want)
{
    bool target = (want == -1) ? !c->max_horz : (want == 1);
    client_set_maximized(c, target, c->max_vert);
}

void toggle_maximize_vert(Client *c, int want)
{
    bool target = (want == -1) ? !c->max_vert : (want == 1);
    client_set_maximized(c, c->max_horz, target);
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
        c->fs_was_max_horz = c->max_horz;
        c->fs_was_max_vert = c->max_vert;
        c->fs_saved_snap_side = c->snap_side;

        c->fullscreen = true;
        c->max_horz = false;
        c->max_vert = false;
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
        restack_all_raising(c);
        ewmh_update_wm_state(c);
        ewmh_update_frame_extents(c);
        xcb_flush(wm.conn);
        return;
    }

    c->fullscreen = false;
    bool restore_horz = c->fs_was_max_horz;
    bool restore_vert = c->fs_was_max_vert;
    c->fs_was_max_horz = false;
    c->fs_was_max_vert = false;

    c->x = c->fs_saved_x;
    c->y = c->fs_saved_y;
    c->width = c->fs_saved_w;
    c->height = c->fs_saved_h;
    c->snap_side = c->fs_saved_snap_side;

    if (restore_horz || restore_vert) {
        /* saved_x/y/w/h still holds the floating geometry from before the
         * window was *maximized*, which is what a later unmaximize has to
         * restore -- so hand that back as the current geometry before
         * re-maximizing. What fs_saved_* restored just above is the
         * maximized geometry itself (that's what the window looked like at
         * the moment fullscreen was entered); letting toggle_maximize()
         * capture *that* as the restore geometry -- which is what happened
         * before, whether it captured it itself or was handed it here --
         * made fullscreen-and-back silently forget the real floating size,
         * leaving a later unmaximize restoring to a maximized-sized
         * window. toggle_maximize()'s own configure_frame()/ewmh update/
         * flush covers the rest. */
        c->x = c->saved_x;
        c->y = c->saved_y;
        c->width = c->saved_w;
        c->height = c->saved_h;
        client_set_maximized(c, restore_horz, restore_vert);
        restack_all(); /* re-affirms its position in LAYER_NORMAL -- harmless no-op if nothing else changed */
        xcb_flush(wm.conn);
        return;
    }

    /* State before geometry, as everywhere else here. */
    ewmh_update_wm_state(c);

    configure_frame(c);
    restack_all(); /* ditto */
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
    /* Half-screen tiling is a resize, so an unresizable window can't do it
     * any more than it can maximize. */
    if (!c->allow_resize)
        return;

    unshade_now(c);

    int wx, wy, ww, wh;
    compute_output_workarea(c->output >= 0 ? c->output : 0, &wx, &wy, &ww, &wh);

    int bt, th;
    deco_insets(c, &bt, &th);

    int half = ww / 2;
    c->max_horz = false;
    c->max_vert = false;
    c->snap_side = side;
    c->y = wy;
    c->height = wh - th - bt;
    c->x = (side == SNAP_LEFT) ? wx : wx + (ww - half);
    c->width = half - bt * 2;
    if (c->width < c->min_w) c->width = c->min_w;
    if (c->height < c->min_h) c->height = c->min_h;
}

/* Keyboard half-screen tiling (kiwm.conf's key_tile_left=/key_tile_right=,
 * Meta+Left/Meta+Right by default) -- the same geometry the drag-to-edge
 * snap produces, just without the drag, plus the two things a keyboard
 * shortcut needs that the drag path handles elsewhere:
 *
 *   - capturing the restore geometry itself, since there's no button-press
 *     moment for detile_for_drag() to have done it. Only captured when
 *     coming from a genuinely floating window: a maximized or already-
 *     half-snapped one already has the true pre-tiling geometry in
 *     saved_x/y/w/h and must not have it clobbered with the tiled size
 *     (same rule toggle_maximize() follows, for the same reason).
 *   - toggling: pressing the shortcut for the side a window is *already*
 *     tiled to restores it instead of re-tiling it to where it already is,
 *     so one key both tiles and untiles.
 *
 * Unlike snap_client_to_side()/unsnap_client() (which leave the redraw to
 * their drag-path caller, which was going to do it anyway), this applies
 * everything itself -- there's no drag still in flight to defer to. */
void toggle_snap_side(Client *c, SnapSide side)
{
    if (c->snap_side == side) {
        unsnap_client(c, c->saved_x, c->saved_y, c->saved_w, c->saved_h);
    } else {
        if (c->snap_side == SNAP_NONE && !c->max_horz && !c->max_vert) {
            c->saved_x = c->x;
            c->saved_y = c->y;
            c->saved_w = c->width;
            c->saved_h = c->height;
        }
        snap_client_to_side(c, side);
    }

    configure_frame(c);
    ewmh_update_wm_state(c);
    ewmh_update_frame_extents(c);
    xcb_flush(wm.conn);
}

/* Restores explicit floating geometry (typically the pre-drag geometry
 * the caller tracked itself, offset by however far the pointer has moved
 * since), clearing whatever snap/maximize state was engaged. Also just
 * the caller's own responsibility to configure_frame()/flush afterward. */
void unsnap_client(Client *c, int x, int y, int width, int height)
{
    c->max_horz = false;
    c->max_vert = false;
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
    if (!c->max_horz && !c->max_vert && c->snap_side == SNAP_NONE)
        return;

    int old_fw = c->frame_width, old_fh = c->frame_height;
    double frac_x = old_fw > 0 ? (press_root_x - c->x) / (double)old_fw : 0.5;
    double frac_y = old_fh > 0 ? (press_root_y - c->y) / (double)old_fh : 0.0;
    if (frac_x < 0.0) frac_x = 0.0; else if (frac_x > 1.0) frac_x = 1.0;
    if (frac_y < 0.0) frac_y = 0.0; else if (frac_y > 1.0) frac_y = 1.0;

    c->max_horz = false;
    c->max_vert = false;
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
    if (!c->allow_minimize)
        return;

    if (c->minimized)
        return;

    /* Where the window *was*. kiwm keeps that in the Client itself for
     * free -- minimizing only unmaps the frame, so c->x/y and the frame
     * size stay exactly as they were, which is what the switcher's outline
     * draws (osd.c) and what restoring puts back. Publishing it as well
     * hands the same answer to anything outside kiwm that needs to know
     * where a window it can no longer see used to be: kicomp, to animate a
     * minimize/restore from and to the right place, a taskbar wanting to
     * do the same. Removed again on restore, since it then says nothing
     * the window's real geometry doesn't. See PROTOCOL.md. */
    uint32_t geo[] = { (uint32_t)c->x, (uint32_t)c->y,
                       (uint32_t)c->frame_width, (uint32_t)c->frame_height };
    xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE, c->window,
                        wm.atoms.kiwm_minimized_geometry, XCB_ATOM_CARDINAL, 32, 4, geo);

    c->minimized = true;

    /* Before the unmap, so anything watching learns this disappearance is
     * a minimize rather than a close -- same reason as everywhere else in
     * this file. */
    ewmh_update_wm_state(c);

    if (c->mapped) {
        xcb_unmap_window(wm.conn, c->frame);
        c->mapped = false;
    }
    if (wm.focused == c)
        wm.focused = NULL;

    ewmh_update_active_window();
    xcb_flush(wm.conn);
}

void restore_client(Client *c)
{
    if (!c->minimized)
        return;
    c->minimized = false;
    xcb_delete_property(wm.conn, c->window, wm.atoms.kiwm_minimized_geometry);

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

/* An application asking for one of its windows to be activated
 * (_NET_ACTIVE_WINDOW). `user_driven` is EWMH's source indication: a
 * taskbar/pager sends 2, meaning the user clicked something, and that is
 * always honored; an application sending 1 (or 0, which is every older
 * client) is asking on its own behalf and goes through
 * focus_request_allowed(). Refused, the window is left exactly as it is
 * -- not unminimized, not pulled onto the current desktop -- and only
 * marked as demanding attention. */
void activate_client_requested(Client *c, bool user_driven)
{
    if (focus_request_allowed(c, user_driven))
        activate_client(c);
    else
        deny_focus_request(c);
}

/* Moves a client's *bookkeeping* to another output: which output it
 * belongs to and, with it, which of that output's desktops it is on.
 *
 * The second half is the whole point. A desktop number only means
 * something relative to an output (see PROTOCOL.md): desktop 0 of DP-3
 * and desktop 0 of HDMI-2 are different desktops. So carrying a window to
 * another screen while keeping its old desktop *number* can silently put
 * it on a desktop that screen isn't showing -- the window stays visible
 * where it was dropped, but kiwm now believes it belongs somewhere else,
 * and the next desktop switch (or any other visibility pass) unmaps it.
 * That is how a window ends up invisible with no way to get it back
 * except switching the other screen to the desktop it was stranded on.
 *
 * Re-basing to the destination output's *current* desktop is what a user
 * dragging a window to another screen means by it, and it's what the
 * drag-release path always did; the other three sites that reassigned
 * c->output (an edge snap mid-drag, a fullscreen window carried across,
 * a monitor layout change) didn't, which is the bug. Sticky windows keep
 * their desktop number -- they're on all of them anyway.
 *
 * Frame visibility is synced too, so this is safe for a client that was
 * hidden on its old output: it becomes visible if it now belongs to a
 * shown desktop, and hidden if it doesn't. */
void client_reassign_output(Client *c, int output_idx)
{
    if (output_idx < 0 || output_idx >= wm.output_count || output_idx == c->output)
        return;

    c->output = output_idx;
    if (!c->sticky)
        c->desktop = wm.outputs[output_idx].desktop;

    /* Frame visibility follows the same rule switch_workspace() applies,
     * and the same way: c->mapped is the client's own "I want to be on
     * screen" (cleared only by minimizing/withdrawing), while the frame's
     * actual mapped state also depends on whether its desktop is the one
     * being shown -- so this maps/unmaps without touching c->mapped. Both
     * requests are no-ops when the frame is already in that state. */
    if (c->mapped && !c->minimized) {
        if (c->sticky || wm.outputs[c->output].desktop == c->desktop)
            xcb_map_window(wm.conn, c->frame);
        else
            xcb_unmap_window(wm.conn, c->frame);
    }

    ewmh_update_wm_desktop(c);
    ewmh_update_wm_output(c);
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
    /* Frees the density pixmap and stops publishing for a frame that is
     * about to stop existing (density.h). */
    deco_density_forget(c);

    int fx = c->x, fy = c->y, fw = c->frame_width, fh = c->frame_height;

    if (wm.focused == c)
        wm.focused = NULL;
    if (wm.drag_client == c) {
        wm.drag_client = NULL;
        wm.drag_mode = DRAG_NONE;
        wm.drag_snap_side = SNAP_NONE;
        wm.drag_fullscreen_move = false;
        wm.resize_neighbors_x_count = 0;
        wm.resize_neighbors_y_count = 0;
        /* The drag is over whether the button was released or not, so a
         * snap or resize preview drawn for it has nothing left to
         * preview -- and nothing left to apply it to. */
        wm.resize_preview_active = false;
        outline_hide();
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
    if (wm.pressed_client == c) {
        /* An armed titlebar button on a window that's going away can't
         * fire on release any more (events.c's handle_button_release()). */
        wm.pressed_client = NULL;
        wm.pressed_btn = -1;
    }

    /* A window closing mid-Alt+Tab-hold (osd.c's window-switcher OSD) must
     * not be left in its list -- it could otherwise get focused (dangling
     * pointer) or drawn (use-after-free) on the next repaint/commit. */
    osd_client_destroyed(c);
    /* ...and neither can an open window menu keep pointing at it. */
    window_menu_client_destroyed(c);

    xcb_unmap_window(wm.conn, c->frame);

    /* The window stops being managed here, so the properties that say a WM
     * *is* managing it have to go with it: ICCCM says WM_STATE is removed
     * (or set to Withdrawn) when a window is withdrawn, and EWMH says the
     * same for _NET_WM_STATE/_NET_WM_DESKTOP. Leaving them behind is not
     * cosmetic -- manage_existing_windows() deliberately adopts an
     * unmapped window that still carries WM_STATE (that's how a window
     * minimized under the previous WM survives a --replace), so a stale
     * WM_STATE would resurrect every window the app had withdrawn as a
     * hidden client the next time kiwm starts. Unchecked on purpose: this
     * path also runs from DestroyNotify, where the window is already gone
     * and the resulting BadWindow is exactly what's expected. */
    xcb_delete_property(wm.conn, c->window, wm.atoms.wm_state);
    xcb_delete_property(wm.conn, c->window, wm.atoms.net_wm_state);
    xcb_delete_property(wm.conn, c->window, wm.atoms.net_wm_desktop);

    /* Out of the save-set on the way out: the window is going back to the
     * root under its own steam here, so there's nothing left for the
     * server to rescue at close-down (see manage()). */
    xcb_change_save_set(wm.conn, XCB_SET_MODE_DELETE, c->window);

    xcb_void_cookie_t reparent_cookie =
        xcb_reparent_window_checked(wm.conn, c->window, wm.root, c->x, c->y);
    xcb_generic_error_t *err = xcb_request_check(wm.conn, reparent_cookie);
    if (err)
        free(err);

    grip_destroy(c);
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

/* A plausible floating geometry for a window kiwm only ever sees already
 * maximized/fullscreen -- the "restore" size it'll get the first time it's
 * unmaximized. There's nothing to recover the *real* pre-maximize geometry
 * from: EWMH has no property for it, so the WM that maximized the window
 * held it in its own memory and took it along when it exited. Two thirds
 * of the output's workarea, centered, is the same shape most toolkits pick
 * for a fresh window and is at least obviously a restore rather than a
 * window that appears not to have restored at all. */
static void seed_restore_geometry(Client *c)
{
    int wx, wy, ww, wh;
    compute_output_workarea(c->output >= 0 ? c->output : 0, &wx, &wy, &ww, &wh);

    c->saved_w = ww * 2 / 3;
    c->saved_h = wh * 2 / 3;
    if (c->saved_w < c->min_w) c->saved_w = c->min_w;
    if (c->saved_h < c->min_h) c->saved_h = c->min_h;
    c->saved_x = wx + (ww - c->saved_w) / 2;
    c->saved_y = wy + (wh - c->saved_h) / 2;
}

/* Where a window that never asked to be anywhere goes: the middle of an
 * output's workarea (see wm.h's Client::hints_has_position).
 *
 * `bt`/`th` are the decoration insets, because what gets centered is the
 * *frame* -- centering the content instead leaves a decorated window
 * sitting a titlebar's height too low, which is exactly the kind of
 * almost-right that is worse than obviously wrong.
 *
 * Which output: a transient goes to its parent's, since a dialog belongs
 * to the window it came from and following the pointer could throw it onto
 * a different screen than the window it is asking about. Everything else
 * goes to the pointer's, which is where the user is looking.
 *
 * Clamped to the workarea rather than allowed to go negative: a window
 * taller than the screen would otherwise be centered with its titlebar
 * above the top edge, and a window whose titlebar is off-screen cannot be
 * dragged back. */
/* The top-left corner that centres a w x h box on an output's workarea.
 * The workarea rather than the whole monitor, so a centred window is not
 * partly behind a panel; clamped so the box never starts above or left of
 * it, since something bigger than the screen would otherwise be centred
 * with its top edge off it -- and for a window that means a titlebar
 * nobody can grab. */
static void centre_on_workarea(int output, int w, int h, int *out_x, int *out_y)
{
    if (output < 0 || output >= wm.output_count)
        output = 0;

    int wx, wy, ww, wh;
    compute_output_workarea(output, &wx, &wy, &ww, &wh);

    *out_x = wx + (ww - w) / 2;
    *out_y = wy + (wh - h) / 2;
    if (*out_x < wx) *out_x = wx;
    if (*out_y < wy) *out_y = wy;
}

static void place_client_centered(Client *c, int bt, int th)
{
    int output = -1;
    if (c->transient_for != XCB_NONE) {
        Client *parent = find_client_window(c->transient_for);
        if (parent)
            output = parent->output;
    }
    if (output < 0 || output >= wm.output_count)
        output = output_for_pointer();

    centre_on_workarea(output, c->width + bt * 2, c->height + th + bt, &c->x, &c->y);
}

/* Applies whatever _NET_WM_STATE the window already carries at the moment
 * kiwm starts managing it. Two quite different situations need this, and
 * both were broken without it:
 *
 *   - Adoption across a WM switch (kiwm --replace, or kiwm starting on a
 *     session that already has windows): the previous WM left each window
 *     maximized/fullscreen/shaded/above/sticky *and* recorded that in
 *     _NET_WM_STATE, which is precisely what the property is for. Ignoring
 *     it left every window "floating, but coincidentally the exact size of
 *     a maximized window" -- so unmaximizing did nothing, the maximize
 *     button showed the wrong glyph, and a fullscreen window came back
 *     windowed-but-screen-sized.
 *   - An app requesting an initial state *before* mapping, which EWMH says
 *     is done by setting _NET_WM_STATE on the window itself (a client
 *     message only works once the window is already managed, so there's no
 *     other way to ask). This is how VirtualBox's VM window asks to come
 *     up fullscreen, and why it never did under kiwm.
 *
 * Returns true if the window should come up minimized, which manage() acts
 * on instead of focusing it. Maximize is only honored when *both* axes are
 * listed: kiwm has no vertical/horizontal-only maximize state to map a
 * single-axis request onto, and treating a vertical-only maximize (which
 * some apps do use on their own) as a full one would resize windows nobody
 * asked to resize. */
static bool adopt_initial_wm_state(Client *c)
{
    xcb_get_property_reply_t *reply = xcb_get_property_reply(wm.conn,
        xcb_get_property(wm.conn, 0, c->window, wm.atoms.net_wm_state, XCB_ATOM_ATOM, 0, 32), NULL);
    if (!reply)
        return false;

    bool max_v = false, max_h = false, fullscreen = false, shaded = false;
    bool above = false, below = false, sticky = false, hidden = false;

    if (reply->type == XCB_ATOM_ATOM && reply->format == 32) {
        xcb_atom_t *atoms = xcb_get_property_value(reply);
        int n = xcb_get_property_value_length(reply) / (int)sizeof(xcb_atom_t);
        for (int i = 0; i < n; i++) {
            xcb_atom_t a = atoms[i];
            if      (a == wm.atoms.net_wm_state_maximized_vert) max_v = true;
            else if (a == wm.atoms.net_wm_state_maximized_horz) max_h = true;
            else if (a == wm.atoms.net_wm_state_fullscreen)     fullscreen = true;
            else if (a == wm.atoms.net_wm_state_shaded)         shaded = true;
            else if (a == wm.atoms.net_wm_state_above)          above = true;
            else if (a == wm.atoms.net_wm_state_below)          below = true;
            else if (a == wm.atoms.net_wm_state_sticky)         sticky = true;
            else if (a == wm.atoms.net_wm_state_hidden)         hidden = true;
        }
    }
    free(reply);

    if (!max_v && !max_h && !fullscreen && !shaded && !above && !below && !sticky && !hidden)
        return false;

    /* Maximize first, then fullscreen, so toggle_fullscreen() records
     * which axes were maximized and leaving fullscreen lands back on the
     * same state rather than a floating window -- exactly the nesting a
     * live toggle would have produced. Each axis is adopted on its own:
     * EWMH has always had them as two states, and a window the previous
     * WM left maximized in only one direction should come up that way. */
    if (max_h || max_v)
        client_set_maximized(c, max_h, max_v);
    if (fullscreen)
        toggle_fullscreen(c, 1);

    /* Both toggles above dutifully captured "the geometry this window had
     * before" as its restore geometry -- but here that geometry *is* the
     * tiled one the previous WM left behind, so restoring would visibly do
     * nothing. Overwrite it with a plausible floating one instead, after
     * the fact, rather than seeding it first (the toggles would just
     * overwrite it right back). */
    seed_restore_geometry(c);
    if (fullscreen && !max_h && !max_v) {
        /* Fullscreen with nothing to fall back to: leaving it restores
         * fs_saved_* directly, so that's the copy that needs seeding. */
        c->fs_saved_x = c->saved_x;
        c->fs_saved_y = c->saved_y;
        c->fs_saved_w = c->saved_w;
        c->fs_saved_h = c->saved_h;
    }

    if (shaded)
        toggle_shade(c, 1);
    if (above)
        toggle_keep_above(c, 1);
    else if (below)
        toggle_keep_below(c, 1);
    if (sticky)
        toggle_sticky(c, 1);

    return hidden;
}

/* Which depth the frame gets. Decided from the client's own depth and
 * from whether a compositor is running *now* -- and re-decided by
 * client_reframe() when the latter changes. */
static void frame_choose_depth(Client *c)
{
    /* The frame is an ARGB frame when its alpha channel has somewhere to
     * go, and the root's depth when it does not.
     *
     * For an ARGB client it is the only way its alpha survives: it draws
     * into the *frame's* backing pixmap, and a depth-24 pixmap has no
     * alpha channel to draw it into -- the transparency would be gone
     * before a compositor ever saw it, unrecoverable. Always 32 for one.
     *
     * For an ordinary depth-24 client it is what makes kiwm's *own*
     * decoration able to be translucent. The client's pixels are opaque
     * either way (it fills its own region), but the titlebar and borders
     * are drawn by kiwm, and they can only have real alpha if the surface
     * they land on has an alpha channel. Deciding this from the client's
     * depth -- as this did at first -- made a themed titlebar translucent
     * over Konsole and opaque over Kate, which is a property of the
     * theme, not of the application. So 32 for one too -- when there is
     * a compositor to give the alpha to.
     *
     * Without one there is not, and this used to say the 32-bit frame
     * "costs nothing on a plain X server: it is simply displayed as
     * opaque". It is displayed as opaque. It does not cost nothing. A
     * depth-32 window on a depth-24 screen goes through a format
     * conversion on every operation the server does to it, and a resize
     * is the server redoing the whole frame. Measured resizing a Kate
     * window on the bare server, sampling gpu_busy_percent: 18.6-19.0%
     * with a 32-bit frame, 10.5-10.8% with a 24-bit one -- and the 24-bit
     * one comes in under an uncomposited kwin's 11.1-11.8% on the same
     * drag. That was the whole of kiwm's gap against kwin, and more.
     *
     * Decided once, here, from the compositor's state at the time the
     * window is framed: a window's visual cannot change after it is
     * created. A compositor that arrives *later* finds the frames made
     * before it opaque, and they stay so until the window is re-managed
     * -- which is the same as kiwm on a screen with no 32-bit visual,
     * and nothing stops working (selection.c's rule for this answer).
     * Re-framing on a compositor's arrival is the follow-up, if the
     * session order turns out to need it. */
    bool argb_client = (c->client_depth == 32);
    if (wm.argb_visual && (argb_client || compositor_running())) {
        c->frame_depth = 32;
        c->frame_visual = wm.argb_visual;
    } else {
        /* No compositor to hand alpha to, or no 32-bit visual to hold it
         * in: root depth, and the decoration is flattened onto its own
         * colour when it is drawn (decoration.c). */
        c->frame_depth = wm.screen->root_depth;
        c->frame_visual = wm.visual;
    }
}

/* The frame window itself, at c->frame_depth, sized for the client plus
 * its insets. Split out of manage() so client_reframe() can make a second
 * one for a client that already has a Client. */
static void frame_create(Client *c, int bt, int th)
{
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
        /* PropertyChange for the frame's own properties: a compositor
         * writes _X_DENSITY_REQUESTED here to ask for a dense
         * decoration (density.h). */
        XCB_EVENT_MASK_PROPERTY_CHANGE |
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
    /* An ARGB frame (c->frame_depth == 32, see above) doesn't share its
     * parent's depth, so it needs a colormap of its own on top of the
     * border pixel -- and CW_COLORMAP (0x2000) sorts *after* CW_EVENT_MASK
     * (0x800) in the value list, same ascending-bit-order rule as the
     * comment above. Root-depth frames keep exactly the request they
     * always had. */
    if (c->frame_depth == 32) {
        uint32_t argb_values[] = { values[0], values[1], wm.argb_colormap };
        xcb_create_window(wm.conn, 32, c->frame, wm.root,
                          c->x, c->y, c->width + bt * 2, c->height + th + bt, 0,
                          XCB_WINDOW_CLASS_INPUT_OUTPUT, wm.argb_visual->visual_id,
                          XCB_CW_BORDER_PIXEL | XCB_CW_EVENT_MASK | XCB_CW_COLORMAP,
                          argb_values);
    } else {
        xcb_create_window(wm.conn, wm.screen->root_depth, c->frame, wm.root,
                          c->x, c->y, c->width + bt * 2, c->height + th + bt, 0,
                          XCB_WINDOW_CLASS_INPUT_OUTPUT, wm.screen->root_visual,
                          XCB_CW_BORDER_PIXEL | XCB_CW_EVENT_MASK, values);
    }

}

/* Moves a client into a new frame of the depth frame_choose_depth() now
 * wants, when that differs from the depth its frame has. A window's
 * visual is fixed at creation, so "change the frame's depth" can only
 * ever mean "make another frame and move the client into it" -- which
 * is what manage() does for a window adopted from a previous WM, done
 * here to a window kiwm is already managing, keeping its Client.
 *
 * Why it exists: without a compositor a 32-bit frame costs ~8 points of
 * GPU during a resize and buys nothing (frame_choose_depth); with one it
 * is what lets the decoration be translucent. A session that starts or
 * stops its compositor (kicomp --toggle) changes the right answer for
 * every window on screen, and this is how they follow it.
 *
 * What the reparent does that has to be accounted for: a mapped window
 * is unmapped and remapped by the server as it moves, which arrives as
 * the same two UnmapNotify manage() already swallows, then a MapNotify
 * that handle_map_notify() lets through harmlessly (c->mapped is still
 * true). The unmap can also make the server drop the focus, so a focused
 * window is focused again at the end. The new frame is stacked directly
 * above the old one before the old one goes, so the window keeps its
 * place in the order. Returns whether anything was done. */
bool client_reframe(Client *c)
{
    uint8_t had = c->frame_depth;
    frame_choose_depth(c);
    if (c->frame_depth == had)
        return false;

    xcb_window_t old_frame = c->frame;
    int bt, th;
    deco_insets(c, &bt, &th);

    /* Whatever a compositor was sampling lived on the old frame. Told
     * now, while c->frame still names it, so the properties are deleted
     * from the window that carried them. */
    deco_density_forget(c);

    frame_create(c, bt, th);

    uint32_t sv[] = { old_frame, XCB_STACK_MODE_ABOVE };
    xcb_configure_window(wm.conn, c->frame,
                         XCB_CONFIG_WINDOW_SIBLING | XCB_CONFIG_WINDOW_STACK_MODE, sv);

    /* One, not manage()'s two: the second UnmapNotify there comes by
     * SubstructureNotify from the window's parent, which for a fresh
     * adoption is the root. Here the parent is the old frame, and a
     * frame does not select SubstructureNotify. Counting one too many
     * would swallow the app's own next unmap. */
    if (c->mapped)
        c->ignore_unmap += 1;
    xcb_reparent_window(wm.conn, c->window, c->frame, bt, th);
    if (c->mapped)
        xcb_map_window(wm.conn, c->frame);

    xcb_destroy_window(wm.conn, old_frame);

    /* Everything that remembered the old frame's state starts over: it
     * has no geometry sent, no shape, and no decoration yet. */
    c->geom_sent = false;
    c->shape_sig_valid = false;
    c->frame_shaped = false;
    configure_frame(c);

    /* The server reverts the focus when the focused window is unmapped,
     * and the reparent unmapped it. Sent directly rather than through
     * focus_client(), which sees wm.focused == c and rightly sends
     * nothing for a window it believes already has the focus -- the
     * model is right; it is the server that was made to forget. */
    if (wm.focused == c) {
        xcb_set_input_focus(wm.conn, XCB_INPUT_FOCUS_POINTER_ROOT,
                            c->window, XCB_CURRENT_TIME);
        send_take_focus(c);
    }

    return true;
}

/* Every managed window, for a compositor arriving or leaving. Returns
 * how many actually changed frame. */
int clients_reframe_all(void)
{
    int n = 0;
    for (Client *c = wm.clients; c; c = c->next)
        if (client_reframe(c))
            n++;
    if (n) {
        restack_all();
        xcb_flush(wm.conn);
    }
    return n;
}

/* `map_requested` distinguishes the two ways a window gets here, and it
 * matters for exactly one thing: whether kiwm may map it.
 *
 *   - true: a MapRequest -- the client is asking to be shown right now,
 *     and mapping it is the whole point.
 *   - false: adoption at startup (manage_existing_windows()), where the
 *     window's *current* map state is the truth and kiwm's job is to take
 *     it over as it is. An adopted window that is unmapped is unmapped on
 *     purpose: it's a hidden-away popup an app keeps around between uses
 *     (krunner, every Plasma applet popup, VirtualBox's auto-hidden
 *     mini-toolbar), or a minimized window. Mapping it unconditionally --
 *     as this used to -- makes a WM switch spray the screen with windows
 *     nobody asked to see, at whatever stale position they were last left
 *     at, which is exactly the "Plasma windows showing up invisible and in
 *     the wrong places after kiwm --replace" symptom. */
void manage(xcb_window_t window, bool map_requested)
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

    /* See the doc comment above: a MapRequest is a request to be shown; an
     * adopted window keeps whatever state it already had. "Already
     * visible" can't be read off the map state alone, though: a WM that
     * hides a minimized window by unmapping its *frame* (kiwm included)
     * leaves the client window itself mapped, and the previous WM's own
     * shutdown -- reparenting every client back to root -- then makes it
     * genuinely viewable again moments before kiwm looks. ICCCM's
     * WM_STATE is the property that survives that intact and actually says
     * what the window is supposed to be: Iconic means minimized, whoever
     * put it there. */
    bool show = map_requested || (was_viewable && !window_is_iconic(window));

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

        /* Only actual dock/panel windows join the dock list. It used to
         * take every unframed window, which was harmless while that meant
         * "panels and the desktop" but stopped being so once popups,
         * menus, tooltips and notifications joined the unframed set: a
         * Plasma session churns through those constantly, and each one
         * permanently consumed one of the MAX_DOCKS slots until the real
         * panels couldn't be tracked at all. Struts are a dock concept
         * anyway; a tooltip has none. */
        if (window_has_type(window, wm.atoms.net_wm_window_type_dock))
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
        if (window_has_type(window, wm.atoms.net_wm_window_type_desktop)) {
            /* Above the topmost layer that is actually *visible*, not
             * merely the last one created: with one wallpaper per
             * desktop most of them are hidden at any moment, and a
             * crossfade window stacked above a hidden layer ends up
             * below the wallpaper it is fading over -- which looks
             * exactly like the fade not happening at all. */
            xcb_window_t below = desktop_layer_topmost_mapped();
            if (below == XCB_NONE)
                below = wm.last_desktop_window;

            if (below != XCB_NONE) {
                uint32_t values[] = { below, XCB_STACK_MODE_ABOVE };
                xcb_configure_window(wm.conn, window,
                                     XCB_CONFIG_WINDOW_SIBLING | XCB_CONFIG_WINDOW_STACK_MODE, values);
            } else {
                uint32_t values[] = { XCB_STACK_MODE_BELOW };
                xcb_configure_window(wm.conn, window, XCB_CONFIG_WINDOW_STACK_MODE, values);
            }
            wm.last_desktop_window = window;
        }

        /* A splash screen belongs in the middle of the screen, and it is
         * the one unframed window that is *not* placing itself: it sets no
         * position hint at all (LibreOffice's carries USSize, PMinSize,
         * PMaxSize and PWinGravity and nothing about where to be), so
         * ICCCM leaves the position to the window manager -- and with
         * nobody choosing one it came up in the corner at 0,0. Splash
         * windows are excluded from framing, which is right, so the
         * placement manage() does for a Client never reached them.
         *
         * Splash and only splash, deliberately -- not "any unframed window
         * with no position hint". A menu, dropdown, tooltip or
         * notification is placed by whoever owns it, with a plain
         * ConfigureWindow that sets no hint either, so the hint's absence
         * says nothing at all about those. Centring on it would drag every
         * context menu to the middle of the screen.
         *
         * Before the map, so it is never seen in the corner first. Only
         * for a MapRequest: an adopted splash is already on screen, and
         * moving windows around on a --replace is its own bug. */
        if (map_requested &&
            window_has_type(window, wm.atoms.net_wm_window_type_splash) &&
            !window_hints_have_position(window)) {
            xcb_get_geometry_reply_t *sg = xcb_get_geometry_reply(
                wm.conn, xcb_get_geometry(wm.conn, window), NULL);
            if (sg) {
                int sx, sy;
                centre_on_workarea(output_for_pointer(), sg->width, sg->height, &sx, &sy);
                uint32_t v[] = { (uint32_t)sx, (uint32_t)sy };
                xcb_configure_window(wm.conn, window,
                                     XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, v);
                free(sg);
            }
        }

        if (show) {
            xcb_map_window(wm.conn, window);
            focus_unframed_popup(window);
        }

        /* After the map, not before it: this decides whether the window
         * belongs on screen at all, and the unconditional map above would
         * simply undo it. xisback tags each wallpaper layer with
         * _NET_WM_DESKTOP and leaves the showing and hiding to the window
         * manager (output.h's desktop_layer_track). */
        if (window_has_type(window, wm.atoms.net_wm_window_type_desktop))
            desktop_layer_track(window);

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
    update_client_actions(c);
    c->width = geo->width < c->min_w ? c->min_w : geo->width;
    c->height = geo->height < c->min_h ? c->min_h : geo->height;

    c->client_depth = geo->depth;
    frame_choose_depth(c);
    free(geo);

    c->output = output_index_for_point(c->x + c->width / 2, c->y + c->height / 2);
    if (c->output < 0)
        c->output = 0;
    c->desktop = wm.output_count > 0 ? wm.outputs[c->output].desktop : 0;

    get_title(c);
    load_client_icon(c);
    /* Before the frame is sized/created: an undecorated client's frame is
     * exactly its content size, with no titlebar row to reparent below. */
    c->undecorated = window_wants_no_decoration(window);
    c->transient_for = window_transient_for(window);
    c->group_leader = window_group_leader(window);
    c->skip_taskbar = window_has_state(window, wm.atoms.net_wm_state_skip_taskbar);
    c->takes_focus = client_supports_protocol(window, wm.atoms.wm_take_focus);
    client_refresh_appmenu(c);

    /* No maximize/fullscreen state has been adopted yet at this point, so
     * this is just "the configured border/titlebar, unless the client
     * asked for none at all" (Client::undecorated above) -- adopting a
     * state that hides the decoration re-runs configure_frame() and
     * resizes the frame accordingly anyway. */
    int bt, th;
    deco_insets(c, &bt, &th);

    /* ICCCM 4.1.2.3: where the frame goes depends on the client's
     * win_gravity. c->x/c->y is the *frame's* origin everywhere in kiwm,
     * and geo->x/y (already copied into it above) is where the client's
     * own window currently is -- so for StaticGravity, which is what every
     * Qt/GTK window asks for, the frame has to move up and left by the
     * decoration insets to leave the content exactly where it is. Ignoring
     * this is invisible when a window is first mapped (nothing has been
     * drawn yet) but walks every window a titlebar's height down the
     * screen on each WM handoff, since adoption re-frames a window that's
     * already on screen. Every other gravity falls back to NorthWest, the
     * ICCCM default: the frame goes where the client asked and the content
     * lands below the titlebar. */
    if (c->gravity == XCB_GRAVITY_STATIC) {
        c->x -= bt;
        c->y -= th;

        /* ...but never far enough up that the titlebar lands off-screen
         * and the window can't be dragged again. Only relevant when there
         * *is* a titlebar; an undecorated client has no insets to subtract
         * in the first place. */
        if (th > 0) {
            int wx, wy, ww, wh;
            compute_output_workarea(c->output >= 0 ? c->output : 0, &wx, &wy, &ww, &wh);
            (void)wx; (void)ww; (void)wh;
            if (c->y < wy)
                c->y = wy;
        }
    }

    /* ...and all of that only matters if the client asked for a position
     * in the first place. If it didn't, ICCCM says the position is kiwm's
     * to pick, and whatever x/y the window happens to carry -- the origin,
     * for most toolkits -- is not a request to honour. Placed after the
     * gravity adjustment above so it has the last word: there is no
     * content position to preserve for a window that was never placed.
     *
     * A window adopted from a previous WM is exempt: it is already on
     * screen somewhere the user put it, and re-centering every window on
     * a --replace would be its own bug. `map_requested` is manage()'s
     * argument for "this is a fresh MapRequest". */
    if (map_requested && !c->hints_has_position) {
        place_client_centered(c, bt, th);

        /* c->output/c->desktop were derived from the position the client
         * came with, which is the one just discarded. */
        c->output = output_index_for_point(c->x + c->width / 2, c->y + c->height / 2);
        if (c->output < 0)
            c->output = 0;
        c->desktop = wm.output_count > 0 ? wm.outputs[c->output].desktop : 0;
    }

    frame_create(c, bt, th);

    /* POINTER_MOTION on the *client's* window, which a WM normally has no
     * reason to want. It was originally how events.c noticed the pointer
     * entering the invisible resize grip inside the window's edges; the
     * grip is a ring of its own windows outside the frame now (grip.h) and
     * needs nothing here. What still does is update_button_hover(): moving
     * off a titlebar button *downwards*, into the content, has to clear
     * that button's highlight, and this is the event that says so. Core
     * pointer selections aren't exclusive, so the app keeps getting its own
     * motion events exactly as before. */
    uint32_t client_mask = XCB_EVENT_MASK_PROPERTY_CHANGE |
                           XCB_EVENT_MASK_STRUCTURE_NOTIFY |
                           XCB_EVENT_MASK_FOCUS_CHANGE |
                           XCB_EVENT_MASK_POINTER_MOTION;
    xcb_change_window_attributes(wm.conn, window, XCB_CW_EVENT_MASK, &client_mask);

    xcb_grab_button(wm.conn, 0, window, XCB_EVENT_MASK_BUTTON_PRESS,
                    XCB_GRAB_MODE_SYNC, XCB_GRAB_MODE_ASYNC,
                    XCB_NONE, XCB_NONE, XCB_BUTTON_INDEX_1, XCB_MOD_MASK_ANY);

    /* mod_key + right-click resize (events.c's
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
    grab_button3_with_locks(window, wm.mod_key);

    /* ShapeNotify, so a client that carves up (or later changes) its own
     * silhouette has that forwarded onto the frame -- see shape.c. */
    shape_track_client(c);

    /* The X save-set: the server's own insurance for exactly the disaster
     * a reparenting WM can cause by dying. Destroying a window destroys
     * its children too, and the server destroys every window a client
     * created when that client's connection drops -- so a kiwm that goes
     * away without unwinding its frames (a crash, a SIGKILL, the terminal
     * that launched it closing, an X error) takes every window reparented
     * inside those frames with it: the whole session's apps quit at once,
     * everything in the taskbar gone, only the never-framed panels and
     * desktop left standing. Putting each client window in the save-set
     * makes the server reparent it back to the root and remap it at
     * close-down instead. cleanup()'s orderly unwind covers the normal
     * exit; this covers every other way kiwm can stop existing, and costs
     * one request per window. */
    xcb_change_save_set(wm.conn, XCB_SET_MODE_INSERT, window);

    xcb_reparent_window(wm.conn, window, c->frame, bt, th);

    if (show) {
        xcb_map_window(wm.conn, window);
        xcb_map_window(wm.conn, c->frame);
    }

    c->mapped = show;
    /* Adopted while hidden: ICCCM-wise that's an iconified window as far as
     * anything else (taskbars, the switcher, restore paths) is concerned --
     * ewmh_update_wm_state() below turns this into WM_STATE=Iconic and
     * _NET_WM_STATE_HIDDEN, and events.c's remap path clears it the moment
     * the app maps the window itself. */
    c->minimized = !show;
    /* Reparenting an already-mapped window generates its one automatic
     * unmap as *two* UnmapNotify events: one via StructureNotify on the
     * window itself, one via SubstructureNotify on root (the window's
     * parent at that instant) -- both must be swallowed, not just one. */
    c->ignore_unmap = was_viewable ? 2 : 0;
    /* ...and the automatic re-map that follows it, but only when we're
     * deliberately keeping this window hidden -- otherwise the frame gets
     * mapped by the remap path and the window we just decided not to show
     * appears anyway. See Client::ignore_map. */
    c->ignore_map = (was_viewable && !show) ? 1 : 0;
    set_icccm_wm_state(c, WM_STATE_NORMAL);

    c->next = wm.clients;
    wm.clients = c;

    /* Before the first ewmh_update_wm_state() below, which rewrites
     * _NET_WM_STATE from the Client's own (still all-false) fields and
     * would otherwise erase the very states being read here. */
    bool start_minimized = adopt_initial_wm_state(c);

    configure_frame(c);
    /* After configure_frame(), which is what first gives the frame a
     * width and height for the ring to be sized against, and after
     * adopt_initial_wm_state() above, so a window coming up already
     * maximized or fullscreen gets its ring created unmapped rather than
     * flashed across the screen it fills. */
    grip_create(c);
    ewmh_update_wm_desktop(c);
    ewmh_update_wm_output(c);
    ewmh_update_wm_state(c);
    ewmh_update_frame_extents(c);
    ewmh_update_client_list();

    if (start_minimized && c->mapped) {
        minimize_client(c);
    } else if (c->mapped) {
        /* A window mapping itself into focus is the commonest kind of
         * focus stealing there is, so it goes through the same policy an
         * _NET_ACTIVE_WINDOW request does (kiwm.conf's
         * focus_stealing_prevention=, off by default). Refused, it still
         * appears and is still stacked normally -- it just doesn't take
         * the keyboard, and says so via _NET_WM_STATE_DEMANDS_ATTENTION. */
        if (focus_request_allowed(c, false)) {
            focus_client(c);
        } else {
            deny_focus_request(c);
            restack_all();
        }
    }
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

        /* A wallpaper layer is adopted whether or not it is on screen.
         * One per desktop means most of them are unmapped at any moment
         * (output.h's desktop_layer_track), and a window manager
         * starting after them -- a --replace, a crash, a session
         * restarted around a running xisback -- would otherwise never
         * learn they exist: never tracked, never mapped again, and the
         * only way back was to restart the program that owns them. */
        bool is_desktop = window_has_type(w, wm.atoms.net_wm_window_type_desktop);

        if (viewable || has_wm_state || is_desktop)
            manage(w, false);
    }

    free(tree);

    /* Only now is every dock/panel in this session known, so only now can
     * a workarea-derived size be right -- a window adopted maximized
     * earlier in the loop sized itself against whatever struts had been
     * seen by then, which for anything framed before the panel meant none
     * at all (covering the taskbar). See refit_tiled_clients(). */
    refit_tiled_clients();

    xcb_flush(wm.conn);
}
