/* The compositor's mirror of root's children, in stacking order.
 *
 * kicomp learns everything from plain X events (SubstructureNotify on the
 * root window) -- no WM-specific protocol at all yet, which is what
 * kiwm-kicomp-projeto.md section 14 asks for in the first version. The
 * IPC of section 32 replaces only the *source* of these updates later;
 * the mirror itself stays exactly as it is here.
 */
#include "window.h"
#include "output.h"
#include "renderer.h"
#include "effect.h"
#include "animation.h"
#include "desktop.h"
#include "density.h"

#include <xcb/shape.h>

#include <stdlib.h>
#include <string.h>

/* How long after a desktop switch a window vanishing (or appearing) is
 * credited to that switch rather than to being closed (or opened). The
 * WM does both in one go, so this only has to cover the gap between the
 * root property and the maps/unmaps that follow it. */
#define DESKTOP_SWITCH_WINDOW_MS 250.0

/* Did this window vanish (or arrive) because its own output changed
 * desktop? The output matters: with a desktop per output (kiwm's model), a
 * switch on one monitor says nothing about a window closing on another,
 * and crediting it there would animate a closing window as a departing
 * one. desktop.h answers per output where the WM publishes enough to; the
 * screen-wide stopwatch is the fallback for one that doesn't. */
static bool left_with_a_desktop(CompWindow *w)
{
    CompRect r = window_rect(w);

    /* A window that says which desktop it is on answers this by itself,
     * and answers it whenever it is asked: it went away while its output
     * was showing a different desktop, so it went away *with* a desktop.
     * The clock below only ever approximated that -- it is right for the
     * ordinary case, where the window disappears in the same breath as
     * the switch, and wrong for a window the WM puts away at any other
     * moment (kiwm/PROTOCOL.md's prime, which maps a hidden wallpaper
     * just long enough to be photographed and then hides it again). */
    int desk = -1, out_index = -1;
    if (desktop_of_window(w, &desk, &out_index) && desk >= 0) {
        for (int i = 0; i < comp.output_count; i++) {
            CompOutput *o = &comp.outputs[i];
            CompRect hit;
            if (out_index >= 0) {
                if (desktop_output_index(o) != out_index)
                    continue;
            } else if (!rect_intersect(&r, &o->rect, &hit)) {
                continue;
            }

            int current = desktop_current_for_output(o);
            if (current >= 0 && current != desk)
                return true;
        }
    }

    int dx, dy;
    if (desktop_switch_for_rect(&r, &dx, &dy))
        return true;
    return comp_now_ms() - comp.desktop_changed_ms < DESKTOP_SWITCH_WINDOW_MS;
}

/* True only while windows_scan() adopts what was already on screen at
 * startup: those windows did not just appear, and must not be animated
 * as if they had. */
static bool adopting_existing;

static void window_destroy(CompWindow *w);
static uint32_t read_window_state(CompWindow *w);
static void read_window_group(CompWindow *w);

CompRect window_rect(const CompWindow *w)
{
    /* NameWindowPixmap covers the window *and* its X border, and the
     * border is drawn outside the window's origin -- so the pixmap's
     * top-left is (x - border, y - border). */
    CompRect r = {
        w->x - w->border,
        w->y - w->border,
        w->w + w->border * 2,
        w->h + w->border * 2
    };
    return r;
}

CompWindow *window_find_by_client(xcb_window_t client)
{
    for (CompWindow *w = comp.stack; w; w = w->next)
        if (!w->zombie && w->client == client)
            return w;
    return NULL;
}

CompWindow *window_find(xcb_window_t id)
{
    for (CompWindow *w = comp.stack; w; w = w->next)
        /* Zombies are skipped: the X window they mirror is gone, and X
         * recycles window ids -- a new window arriving with the same id
         * must not be mistaken for the corpse of the old one. */
        if (w->id == id && !w->zombie)
            return w;
    return NULL;
}

/* Unlinks without freeing. */
static void unlink_window(CompWindow *w)
{
    CompWindow **pp = &comp.stack;
    while (*pp) {
        if (*pp == w) {
            *pp = w->next;
            w->next = NULL;
            return;
        }
        pp = &(*pp)->next;
    }
}

/* Puts `w` at the very top of the stack. */
static void link_top(CompWindow *w)
{
    CompWindow **pp = &comp.stack;
    while (*pp)
        pp = &(*pp)->next;
    *pp = w;
    w->next = NULL;
}

/* Links `w` directly above `above`, with ConfigureNotify's above_sibling
 * semantics: XCB_NONE means the *bottom* of the stack, not the top. The
 * list runs bottom-most first, matching QueryTree's order. */
static void link_above(CompWindow *w, xcb_window_t above)
{
    if (above == XCB_NONE) {
        w->next = comp.stack;
        comp.stack = w;
        return;
    }

    CompWindow *prev = window_find(above);
    if (!prev) {
        /* Sibling we don't track (our own overlay window, or one that
         * went away between the event and now): the top is the safe
         * guess -- it's where X puts anything whose position we can't
         * reconstruct, and being one window too high is far less visible
         * than being buried under the desktop. */
        link_top(w);
        return;
    }

    w->next = prev->next;
    prev->next = w;
}

void window_update_opacity(CompWindow *w)
{
    double prev = w->opacity;
    w->opacity = 1.0;

    xcb_get_property_reply_t *r = xcb_get_property_reply(comp.conn,
        xcb_get_property(comp.conn, 0, w->id, comp.atoms.net_wm_window_opacity,
                         XCB_ATOM_CARDINAL, 0, 1), NULL);
    if (r) {
        if (r->type == XCB_ATOM_CARDINAL && r->format == 32 &&
            xcb_get_property_value_length(r) >= 4) {
            uint32_t v = *(uint32_t *)xcb_get_property_value(r);
            w->opacity = (double)v / (double)0xffffffffu;
        }
        free(r);
    }

    if (w->opacity != prev) {
        renderer_window_invalidate(w);   /* drops the cached alpha picture */
        CompRect r2 = window_rect(w);
        output_damage_window_rect(w, &r2);
    }
}

/* _KIWM_LAYER, kiwm's marking on its own overlay windows ("osd",
 * "outline"). Read once when the window is adopted: kiwm sets it at
 * creation and never changes it. */
static void read_wm_layer(CompWindow *w)
{
    w->wm_layer[0] = '\0';
    if (comp.atoms.kiwm_layer == XCB_NONE)
        return;

    xcb_get_property_reply_t *r = xcb_get_property_reply(comp.conn,
        xcb_get_property(comp.conn, 0, w->id, comp.atoms.kiwm_layer,
                         XCB_ATOM_STRING, 0, sizeof(w->wm_layer) / 4), NULL);
    if (!r)
        return;

    int len = xcb_get_property_value_length(r);
    if (r->type == XCB_ATOM_STRING && len > 0) {
        if ((size_t)len >= sizeof(w->wm_layer))
            len = (int)sizeof(w->wm_layer) - 1;
        memcpy(w->wm_layer, xcb_get_property_value(r), (size_t)len);
        w->wm_layer[len] = '\0';
        comp_log("window 0x%x is kiwm's \"%s\" layer%s", w->id, w->wm_layer,
                 comp.skip_wm_layers ? " (skipped)" : "");
    }
    free(r);
}

/* The client window that carries the EWMH properties. For an
 * override-redirect window (a menu, a tooltip, a dock that manages
 * itself) that's the window itself; for anything the WM framed it's the
 * child inside the frame. kiwm reparents exactly one client into each
 * frame, so "the first child" is the whole search -- and a frame with no
 * children yet simply isn't resolved until it has one. */
/* Does this window carry ICCCM's WM_STATE? The property a window manager
 * puts on the window it manages -- which is to say, on the *client*, not
 * on any frame it may have built around it. Reading it raw rather than
 * through window state, because this is what decides which window the
 * state is read from in the first place. */
/* Does this window say what kind of window it is? _NET_WM_WINDOW_TYPE is
 * a client's own statement about itself, and a WM's frame never carries
 * one. */
static bool window_declares_type(xcb_window_t win)
{
    if (win == XCB_NONE || comp.atoms.net_wm_window_type == XCB_NONE)
        return false;

    xcb_get_property_reply_t *r = xcb_get_property_reply(comp.conn,
        xcb_get_property(comp.conn, 0, win, comp.atoms.net_wm_window_type,
                         XCB_ATOM_ATOM, 0, 1), NULL);
    if (!r)
        return false;
    bool declared = xcb_get_property_value_length(r) >= 4;
    free(r);
    return declared;
}

static bool wm_state_present(xcb_window_t win)
{
    if (win == XCB_NONE || comp.atoms.wm_state == XCB_NONE)
        return false;

    xcb_get_property_reply_t *r = xcb_get_property_reply(comp.conn,
        xcb_get_property(comp.conn, 0, win, comp.atoms.wm_state,
                         comp.atoms.wm_state, 0, 2), NULL);
    if (!r)
        return false;
    bool present = xcb_get_property_value_length(r) >= 4;
    free(r);
    return present;
}

static xcb_window_t resolve_client(CompWindow *w)
{
    if (w->client != XCB_NONE)
        return w->client;

    /* A top-level window the WM manages *without* reparenting speaks for
     * itself, children or no children. Panels and desktop windows are
     * exactly that under kiwm -- there is nothing to decorate, so there
     * is no frame -- and they are also the windows most likely to have
     * children of their own, because they are Qt or GTK surfaces with
     * real subwindows inside.
     *
     * Taking the first child as "the client" without asking, which is
     * what this used to do, reads every property off a subwindow that
     * has none: Plasma's panel came back as type UNKNOWN instead of DOCK,
     * and its desktop window likewise, so both were treated as ordinary
     * windows -- laid out in show-windows' grid, and dodged, shadowed and
     * animated everywhere else. WM_STATE is the property that settles it,
     * and it is on the client by definition. */
    if (wm_state_present(w->id)) {
        w->client = w->id;
        read_window_group(w);
        return w->client;
    }

    /* Or a window that describes *itself* -- it declares a window type,
     * or it escaped the WM entirely. A frame describes nothing: it is a
     * container the WM built, and every property that says what a window
     * is lives on the client inside it. So a top-level carrying
     * _NET_WM_WINDOW_TYPE is the client, whatever it has underneath.
     *
     * Which matters because "has children" is not evidence of being a
     * frame. Plasma's desktop window is a plain top-level with one 1x1
     * child parked at (-1,-1) -- Qt's NET_WM user-time window -- and
     * reading its type off that child gave "unknown" for the desktop
     * itself: expo then had nothing to shrink into its cells and left the
     * wallpaper lying full-size behind the grid. */
    if (w->override_redirect || window_declares_type(w->id)) {
        w->client = w->id;
        read_window_group(w);
        return w->client;
    }

    xcb_query_tree_reply_t *tree =
        xcb_query_tree_reply(comp.conn, xcb_query_tree(comp.conn, w->id), NULL);
    if (!tree)
        return XCB_NONE;

    int n = xcb_query_tree_children_length(tree);
    if (n > 0) {
        xcb_window_t *children = xcb_query_tree_children(tree);

        /* The framed case: the client is the child the WM marked, and
         * only the first child when none of them is marked (a frame
         * whose client has not been given its WM_STATE yet). */
        w->client = children[0];
        for (int i = 0; i < n; i++) {
            if (wm_state_present(children[i])) {
                w->client = children[i];
                break;
            }
        }
        /* Whoever the client turns out to be, this is the moment its
         * identity can be read -- and every path that resolves a client
         * has to do it, not just the reparent. A window adopted at
         * startup (windows_scan) never gets reparented while kicomp is
         * watching, so reading it only there left every window that
         * predates the compositor with no group at all: which is why
         * VirtualBox's mini-toolbar dodged the machine window it belongs
         * to, on a session where the compositor was started last. */
        read_window_group(w);
    } else if (w->override_redirect) {
        w->client = w->id;   /* never framed: it speaks for itself */
        read_window_group(w);
    } else {
        /* A frame whose client hasn't been reparented into it yet -- which
         * is the normal state of affairs at CreateNotify time, since the
         * WM creates the frame first. Answer for now, but don't cache it:
         * caching the frame as its own client is how every EWMH property
         * ends up being read from the wrong window, which silently turns
         * every window into "type unknown, state none" -- a shade then
         * looks exactly like a resize. */
        free(tree);
        return w->id;
    }

    free(tree);

    /* The state properties live on the client, not on the frame, so the
     * changes have to be selected there -- event masks are per-client,
     * so this disturbs neither the WM nor the application. */
    if (w->client != w->id) {
        uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
        xcb_change_window_attributes(comp.conn, w->client, XCB_CW_EVENT_MASK, &mask);
    }

    return w->client;
}

/* _NET_WM_WINDOW_TYPE on the client window, collapsed to a CompWindowKind
 * (see comp.h). Read once the client is known; a window that never says
 * what it is stays UNKNOWN, which effects treat as "normal" or skip
 * depending on what they're for. */
static CompWindowType type_for_atom(xcb_atom_t t)
{
    if (t == comp.atoms.type_normal)        return COMP_WINDOW_NORMAL;
    if (t == comp.atoms.type_dialog)        return COMP_WINDOW_DIALOG;
    if (t == comp.atoms.type_utility)       return COMP_WINDOW_UTILITY;
    if (t == comp.atoms.type_toolbar)       return COMP_WINDOW_TOOLBAR;
    if (t == comp.atoms.type_splash)        return COMP_WINDOW_SPLASH;
    if (t == comp.atoms.type_menu)          return COMP_WINDOW_MENU;
    if (t == comp.atoms.type_dropdown_menu) return COMP_WINDOW_DROPDOWN_MENU;
    if (t == comp.atoms.type_popup_menu)    return COMP_WINDOW_POPUP_MENU;
    if (t == comp.atoms.type_combo)         return COMP_WINDOW_COMBO;
    if (t == comp.atoms.type_tooltip)       return COMP_WINDOW_TOOLTIP;
    if (t == comp.atoms.type_notification)  return COMP_WINDOW_NOTIFICATION;
    if (t == comp.atoms.type_dnd)           return COMP_WINDOW_DND;
    if (t == comp.atoms.type_dock)          return COMP_WINDOW_DOCK;
    if (t == comp.atoms.type_desktop)       return COMP_WINDOW_DESKTOP;
    return COMP_WINDOW_UNKNOWN;
}

static void read_window_kind(CompWindow *w)
{
    w->type = COMP_WINDOW_UNKNOWN;

    xcb_window_t client = resolve_client(w);
    if (client == XCB_NONE || comp.atoms.net_wm_window_type == XCB_NONE)
        return;

    xcb_get_property_reply_t *r = xcb_get_property_reply(comp.conn,
        xcb_get_property(comp.conn, 0, client, comp.atoms.net_wm_window_type,
                         XCB_ATOM_ATOM, 0, 8), NULL);
    if (!r)
        return;

    if (r->type == XCB_ATOM_ATOM && r->format == 32) {
        xcb_atom_t *types = xcb_get_property_value(r);
        int n = xcb_get_property_value_length(r) / 4;

        /* First recognized value wins, per EWMH: the list is in the
         * client's order of preference. A type nobody here knows leaves
         * the window UNKNOWN rather than pretending it is normal --
         * "unknown" is a type an effect can be pointed at like any
         * other. */
        for (int i = 0; i < n; i++) {
            CompWindowType t = type_for_atom(types[i]);
            if (t != COMP_WINDOW_UNKNOWN) {
                w->type = t;
                break;
            }
        }
    }
    free(r);
}

void window_refresh_kind(CompWindow *w)
{
    read_window_kind(w);
}

/* WM_CLIENT_LEADER, WM_TRANSIENT_FOR and _NET_WM_PID: who this window
 * belongs with (comp.h). Read once, when the client is known -- none of
 * the three changes over a window's life in any application that isn't
 * misbehaving, and an effect asking per frame would be three round trips
 * per window per frame. */
static void read_window_group(CompWindow *w)
{
    w->leader = XCB_NONE;
    w->transient_for = XCB_NONE;
    w->pid = 0;

    if (w->client == XCB_NONE)
        return;

    xcb_get_property_cookie_t lc = xcb_get_property(comp.conn, 0, w->client,
        comp.atoms.wm_client_leader, XCB_ATOM_WINDOW, 0, 1);
    xcb_get_property_cookie_t tc = xcb_get_property(comp.conn, 0, w->client,
        XCB_ATOM_WM_TRANSIENT_FOR, XCB_ATOM_WINDOW, 0, 1);
    xcb_get_property_cookie_t pc = xcb_get_property(comp.conn, 0, w->client,
        comp.atoms.net_wm_pid, XCB_ATOM_CARDINAL, 0, 1);

    xcb_get_property_reply_t *r = xcb_get_property_reply(comp.conn, lc, NULL);
    if (r) {
        if (r->type == XCB_ATOM_WINDOW && xcb_get_property_value_length(r) >= 4)
            w->leader = *(xcb_window_t *)xcb_get_property_value(r);
        free(r);
    }
    r = xcb_get_property_reply(comp.conn, tc, NULL);
    if (r) {
        if (r->type == XCB_ATOM_WINDOW && xcb_get_property_value_length(r) >= 4)
            w->transient_for = *(xcb_window_t *)xcb_get_property_value(r);
        free(r);
    }
    r = xcb_get_property_reply(comp.conn, pc, NULL);
    if (r) {
        if (r->type == XCB_ATOM_CARDINAL && xcb_get_property_value_length(r) >= 4)
            w->pid = *(uint32_t *)xcb_get_property_value(r);
        free(r);
    }
}

bool windows_same_group(const CompWindow *a, const CompWindow *b)
{
    if (a == b)
        return true;

    /* One is the other's dialog. Compared against both the frame and the
     * client, since WM_TRANSIENT_FOR names the client window and the
     * compositor tracks frames. */
    if (a->transient_for != XCB_NONE &&
        (a->transient_for == b->client || a->transient_for == b->id))
        return true;
    if (b->transient_for != XCB_NONE &&
        (b->transient_for == a->client || b->transient_for == a->id))
        return true;

    /* The ICCCM group hint, when the application sets one. */
    if (a->leader != XCB_NONE && a->leader == b->leader)
        return true;

    /* And the fallback that catches the ones that don't -- VirtualBox's
     * detached mini-toolbar is its machine window's sibling by nothing
     * but the process it came from. */
    if (a->pid != 0 && a->pid == b->pid)
        return true;

    return false;
}

void window_client_reparented(xcb_window_t frame_id, xcb_window_t client)
{
    CompWindow *w = window_find(frame_id);
    if (!w)
        return;

    /* The WM has just put a client inside a frame we track: that client
     * is where _NET_WM_WINDOW_TYPE and _NET_WM_STATE live, so this is the
     * moment the frame stops being anonymous. */
    w->client = client;
    read_window_group(w);

    uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
    xcb_change_window_attributes(comp.conn, client, XCB_CW_EVENT_MASK, &mask);

    read_window_kind(w);
    w->state = w->state_before = read_window_state(w);
    comp_log("window 0x%x framed client 0x%x (%s, state 0x%x)",
             w->id, client, comp_window_type_name(w->type), w->state);
}

/* ------------------------------------------------------------------ */
/* state, and the semantic events that come out of it                  */
/* ------------------------------------------------------------------ */

/* _NET_WM_STATE + ICCCM WM_STATE on the client window, as a
 * CompWindowState mask. This is what lets an unmap be told apart from a
 * minimize, and a resize from a maximize (see comp.h). */
static uint32_t read_window_state(CompWindow *w)
{
    uint32_t state = 0;

    xcb_window_t client = resolve_client(w);
    if (client == XCB_NONE)
        return 0;

    xcb_get_property_reply_t *r = xcb_get_property_reply(comp.conn,
        xcb_get_property(comp.conn, 0, client, comp.atoms.net_wm_state,
                         XCB_ATOM_ATOM, 0, 16), NULL);
    if (r) {
        if (r->type == XCB_ATOM_ATOM && r->format == 32) {
            xcb_atom_t *v = xcb_get_property_value(r);
            int n = xcb_get_property_value_length(r) / 4;
            for (int i = 0; i < n; i++) {
                if (v[i] == comp.atoms.state_maximized_horz ||
                    v[i] == comp.atoms.state_maximized_vert)
                    state |= COMP_STATE_MAXIMIZED;
                else if (v[i] == comp.atoms.state_shaded)
                    state |= COMP_STATE_SHADED;
                else if (v[i] == comp.atoms.state_fullscreen)
                    state |= COMP_STATE_FULLSCREEN;
                else if (v[i] == comp.atoms.state_above)
                    state |= COMP_STATE_ABOVE;
                else if (v[i] == comp.atoms.state_hidden)
                    state |= COMP_STATE_MINIMIZED;
            }
        }
        free(r);
    }

    /* ICCCM's WM_STATE is the older and more reliable statement of
     * "iconified", and kiwm sets it -- _NET_WM_STATE_HIDDEN is the EWMH
     * spelling of the same thing and not every WM sets both. */
    r = xcb_get_property_reply(comp.conn,
        xcb_get_property(comp.conn, 0, client, comp.atoms.wm_state,
                         comp.atoms.wm_state, 0, 2), NULL);
    if (r) {
        if (xcb_get_property_value_length(r) >= 4) {
            uint32_t *v = xcb_get_property_value(r);
            if (v[0] == 3 /* IconicState */)
                state |= COMP_STATE_MINIMIZED;
        }
        free(r);
    }

    return state;
}

/* Has the window manager taken this window on? ICCCM's WM_STATE is set
 * when a WM adopts a window and stays there while the window is put away
 * -- iconified, or sitting on a desktop that isn't being shown. Its
 * *presence* is the question here, not its value: it is what separates
 * "unmapped because the WM stowed it" from "created but never mapped
 * yet", which are otherwise the same thing seen from outside.
 *
 * Only asked when adopting a window that predates us, so the round trip
 * is paid once per window at startup. */
static bool window_is_managed(CompWindow *w)
{
    xcb_window_t client = resolve_client(w);
    return client != XCB_NONE && wm_state_present(client);
}

/* What this window certainly covers, in its own coordinates.
 *
 * A window that is its own client (override-redirect, or one no WM
 * framed) is opaque exactly when it has no alpha channel. A framed one
 * is opaque where its *client* is, if that client has none -- the frame
 * around it may well be translucent, and under kiwm it is.
 *
 * Anything uncertain is left empty rather than guessed at: a wrong
 * "opaque" is a window that vanishes behind another one, which is a far
 * worse bug than a missed optimisation. */
/* Does this window have a bounding shape of its own? */
static bool shape_present(xcb_window_t win)
{
    if (!comp.caps.shape || win == XCB_NONE)
        return false;

    xcb_shape_query_extents_reply_t *r = xcb_shape_query_extents_reply(comp.conn,
        xcb_shape_query_extents(comp.conn, win), NULL);
    if (!r)
        return false;

    bool shaped = r->bounding_shaped;
    free(r);
    return shaped;
}

static void read_opaque(CompWindow *w)
{
    w->opaque_known = true;
    w->opaque = (CompRect){ 0, 0, 0, 0 };

    if (w->input_only || w->zombie)
        return;

    xcb_window_t client = resolve_client(w);
    if (client == XCB_NONE)
        return;

    /* Shaped is not opaque, whatever the depth says. A window with no
     * alpha channel still paints only inside its silhouette, and the
     * rest of its rectangle shows what is behind it -- kiwm's move
     * outline is exactly that: a full-screen, 24-bit window cut down to
     * a wireframe, which by depth alone reads as "covers everything" and
     * by that reading hid every window on the desktop while it was up.
     *
     * Asked once, with the depth, and cached with it: a window's shape
     * can change, but a window that grows one has bigger changes going
     * on (a resize, a reshape) and both invalidate this. */
    if (shape_present(client))
        return;

    if (client == w->id) {
        if (!w->argb)
            w->opaque = (CompRect){ 0, 0, w->w + w->border * 2,
                                          w->h + w->border * 2 };
        return;
    }

    /* One round trip, ever. The rectangle changes whenever the client is
     * reconfigured and arrives in that event; the *depth* is fixed for
     * the life of the window, so it is the only thing worth asking the
     * server about -- and asking once is the difference between this
     * being free and being a round trip per frame of a resize drag. */
    xcb_get_geometry_reply_t *g = xcb_get_geometry_reply(comp.conn,
        xcb_get_geometry(comp.conn, client), NULL);
    if (!g)
        return;

    w->client_depth = g->depth;
    if (g->depth != 32) {
        /* Relative to the frame's own origin, which is where window_rect
         * starts -- the border the frame draws is part of that. */
        w->opaque = (CompRect){
            g->x + w->border, g->y + w->border,
            g->width + g->border_width * 2,
            g->height + g->border_width * 2
        };
    }
    free(g);
}

/* A window inside one of ours was reconfigured (main.c). The event
 * carries the new rectangle, so if it is the client we care about there
 * is nothing to ask anyone: the opaque area is that rectangle, and this
 * costs a comparison. */
void window_client_reconfigured(xcb_window_t frame, xcb_window_t child,
                                int x, int y, int width, int height, int border)
{
    CompWindow *w = window_find(frame);
    if (!w)
        return;

    /* Not the client: a toolkit's own subwindow, and none of our
     * business. */
    if (w->client != XCB_NONE && w->client != child)
        return;
    if (w->client == XCB_NONE) {
        w->opaque_known = false;   /* not resolved yet; work it out later */
        return;
    }

    if (w->client_depth == 0) {
        w->opaque_known = false;   /* depth still unknown: read it once */
        return;
    }

    w->opaque_known = true;
    if (w->client_depth == 32)
        w->opaque = (CompRect){ 0, 0, 0, 0 };
    else
        w->opaque = (CompRect){ x + w->border, y + w->border,
                                width + border * 2, height + border * 2 };
}

/* Where this window is certainly opaque, in root coordinates. An empty
 * rectangle means "nothing is known to be", which every caller has to
 * treat as "assume it covers nothing". */
CompRect window_opaque_rect(CompWindow *w)
{
    if (!w->opaque_known)
        read_opaque(w);

    if (w->opaque.w <= 0 || w->opaque.h <= 0)
        return (CompRect){ 0, 0, 0, 0 };

    CompRect r = window_rect(w);
    return (CompRect){ r.x + w->opaque.x, r.y + w->opaque.y,
                       w->opaque.w, w->opaque.h };
}

static void emit(CompWindow *w, CompEventKind kind)
{
    CompEvent ev = { .kind = kind };
    effects_window_event(w, &ev);
}

/* Same, for the events an appearance in the same batch changes the
 * meaning of -- focus, today (effect.h's with_appear). */
static void emit_with_appear(CompWindow *w, CompEventKind kind, bool appeared)
{
    CompEvent ev = { .kind = kind, .with_appear = appeared };
    effects_window_event(w, &ev);
}

static void emit_geometry(CompWindow *w, CompEventKind kind,
                          const CompRect *from, const CompRect *to, bool interactive)
{
    CompEvent ev = {
        .kind = kind,
        .from = *from,
        .to = *to,
        .interactive = interactive,
    };
    effects_window_event(w, &ev);
}

void window_state_changed(CompWindow *w)
{
    /* Read at flush time with everything else -- a state property and the
     * configure that goes with it belong to the same change. */
    w->pending_state = true;
}

void window_held_changed(CompWindow *w, bool held)
{
    w->held = held;
}

void window_focus_changed(xcb_window_t active)
{
    if (comp.active_window == active)
        return;

    xcb_window_t previous = comp.active_window;
    comp.active_window = active;

    for (CompWindow *w = comp.stack; w; w = w->next) {
        if (w->zombie)
            continue;
        bool now_focused = (w->id == active || (w->client != XCB_NONE && w->client == active));
        bool was_focused = (w->id == previous || (w->client != XCB_NONE && w->client == previous));
        /* The flag itself changes now -- the shadow and anything else
         * that draws differently for a focused window must not wait a
         * beat -- but the *event* is deferred to the flush, where the
         * restack that came with it has also been seen. */
        if (now_focused && !w->focused) {
            static uint64_t serial;
            w->focused = true;
            w->focus_serial = ++serial;
            w->pending_focus = true;
            w->pending_unfocus = false;
        } else if (was_focused && w->focused && !now_focused) {
            w->focused = false;
            w->pending_unfocus = true;
            w->pending_focus = false;
        } else {
            continue;
        }

        /* Focus changes how this window is *drawn*: a shadow has its own
         * radius, opacity and offset per focus state, and they can differ
         * by a lot. Nothing else marks that area dirty -- the flag simply
         * flips -- so without this the old shadow stays until something
         * else happens to repaint those pixels. In practice a decorating
         * WM hides it by repainting its titlebar on focus, which damages
         * the window anyway; an undecorated window has nothing to hide
         * behind. output_damage_window_rect() grows the rectangle by this
         * window's shadow reach on its own. */
        CompRect r = window_rect(w);
        output_damage_window_rect(w, &r);
    }
}

/* Which of the state bits changed, as the event that describes it.
 * Ordered by how much the change means: a fullscreen that is also a
 * maximize reads as a fullscreen. Returns false when nothing in the
 * state changed and the geometry change is just a move or a resize. */
static bool state_delta_event(uint32_t before, uint32_t now, CompEventKind *out)
{
    uint32_t changed = before ^ now;

    if (changed & COMP_STATE_SHADED) {
        *out = (now & COMP_STATE_SHADED) ? COMP_EVENT_SHADE : COMP_EVENT_UNSHADE;
        return true;
    }
    if (changed & COMP_STATE_FULLSCREEN) {
        *out = (now & COMP_STATE_FULLSCREEN) ? COMP_EVENT_FULLSCREEN : COMP_EVENT_UNFULLSCREEN;
        return true;
    }
    if (changed & COMP_STATE_MAXIMIZED) {
        *out = (now & COMP_STATE_MAXIMIZED) ? COMP_EVENT_MAXIMIZE : COMP_EVENT_UNMAXIMIZE;
        return true;
    }
    return false;
}

/* Deferred classification: everything that happened to a window during
 * this batch of events, decided now that the batch is over.
 *
 * The reason for the delay is that X's order is not the desktop's: an
 * unmap arrives before the property that says it was a minimize, and a
 * configure arrives before (or after) the property that says it was a
 * maximize. A beat later, with the whole batch drained, both are known.
 * That beat is a fraction of a millisecond -- the same drain the paint
 * already waits for. */
bool windows_have_pending(void)
{
    for (CompWindow *w = comp.stack; w; w = w->next)
        if (w->pending_appear || w->pending_disappear ||
            w->pending_geometry || w->pending_state ||
            w->pending_focus || w->pending_unfocus)
            return true;
    return false;
}

/* Snapshots the current stacking into z_before, so the *next* batch can
 * ask what was on top of what before it happened (comp.h). Taken at the
 * end of the flush, once this batch's restacks have been applied. */
static void stack_snapshot(void)
{
    int z = 0;
    for (CompWindow *w = comp.stack; w; w = w->next)
        w->z_before = z++;
}

bool window_was_above(const CompWindow *a, const CompWindow *b)
{
    return a->z_before > b->z_before;
}

void window_bell(xcb_window_t which)
{
    if (which == XCB_NONE)
        return;

    for (CompWindow *w = comp.stack; w; w = w->next) {
        if (w->zombie || !w->mapped)
            continue;
        if (w->id != which && w->client != which)
            continue;
        emit(w, COMP_EVENT_BELL);
        return;
    }
}

void windows_flush_events(void)
{
    CompWindow *w = comp.stack;
    while (w) {
        CompWindow *next = w->next;

        if (!w->pending_appear && !w->pending_disappear &&
            !w->pending_geometry && !w->pending_state &&
            !w->pending_focus && !w->pending_unfocus) {
            w = next;
            continue;
        }

        /* Re-read the state only for the events whose meaning depends on
         * it. A focus change is not one of them, and it is the most
         * frequent thing that happens on a desktop -- paying a round trip
         * per click to learn a window is still not maximized would be a
         * poor trade. */
        bool needs_state = w->pending_appear || w->pending_disappear ||
                           w->pending_geometry || w->pending_state;

        uint32_t before = w->state_before;
        uint32_t now = w->state;
        if (needs_state)
            now = w->state = read_window_state(w);

        if (w->pending_disappear) {
            w->pending_disappear = false;

            /* It was only up to be photographed (comp.h's held): it is
             * going back where it already was, so nothing happened to
             * the desktop and there is nothing to animate. Its picture
             * is still worth keeping, which is the whole reason it was
             * held. */
            if (w->held && !w->zombie) {
                if (comp.keep_stowed && !w->stowed) {
                    w->stowed = true;
                    window_retain(w);
                }
                w->state_before = now;
                w = next;
                continue;
            }

            CompEventKind kind;
            if (w->zombie)
                kind = COMP_EVENT_CLOSE;
            else if (now & COMP_STATE_MINIMIZED)
                kind = COMP_EVENT_MINIMIZE;
            else if (left_with_a_desktop(w))
                kind = COMP_EVENT_DESKTOP_LEAVE;
            else
                kind = COMP_EVENT_CLOSE;

            emit(w, kind);

            /* Put away rather than gone: keep the last picture of it, so
             * an effect that shows what is *not* on screen -- the expo
             * grid laying out every desktop, and eventually the
             * minimized half of show-windows -- has something to draw.
             * X frees an unmapped window's contents, so the pixmap we
             * already hold is the only copy there will ever be, and it
             * has to be claimed here, before the flush lets it go.
             *
             * The window stays out of the scene until an effect asks for
             * it (comp.show_stowed_output), so nothing about the screen
             * changes; what it costs is one pixmap per hidden window,
             * which is why it is a setting. */
            if (comp.keep_stowed && !w->zombie && !w->stowed &&
                (kind == COMP_EVENT_DESKTOP_LEAVE || kind == COMP_EVENT_MINIMIZE)) {
                w->stowed = true;
                window_retain(w);
            }

            /* Nobody kept it: let the contents go, and the entry with
             * them if the window itself is already gone. */
            if (w->retain_count == 0) {
                effects_window_gone(w);
                if (w->zombie) {
                    window_destroy(w);
                    w = next;
                    continue;
                }
                renderer_window_invalidate(w);
            }
        }

        bool appeared = w->pending_appear;

        if (w->held && w->pending_appear) {
            /* The other half: a window mapped for its picture has not
             * arrived anywhere. It keeps `has_been_mapped` as it was --
             * this map says nothing about whether the window has ever
             * been on screen for real. */
            w->pending_appear = false;
            density_update_window(w);

            /* Wherever an effect is drawing it, though, has to be
             * repainted whole: what it was showing until now is the
             * picture this window had when it was put away, and the
             * live one is a different picture. The application's own
             * repaint after being mapped may or may not have reached us
             * as damage -- it happens in the same breath as the map --
             * and half a window is worse than the old one. */
            CompRect r = window_rect(w);
            output_damage_window_rect(w, &r);
            effects_damage_window(w, &r);
        }

        if (w->pending_appear) {
            w->pending_appear = false;
            /* Newly on screen: ask its output for the density it wants
             * (density.h). Harmless and free at scale 1, where the answer
             * is "nothing to ask for". */
            density_update_window(w);

            CompEventKind kind;
            if ((before & COMP_STATE_MINIMIZED) && !(now & COMP_STATE_MINIMIZED))
                kind = COMP_EVENT_RESTORE;
            else if (w->has_been_mapped && left_with_a_desktop(w))
                kind = COMP_EVENT_DESKTOP_ENTER;
            else
                /* Anything else appearing is *opening*, whether or not it
                 * has been on screen before.
                 *
                 * "Has been mapped once" used to be reason enough to call
                 * this a restore, which conflated two unrelated things:
                 * coming back from being minimized -- which the state test
                 * above is what actually knows about -- and an application
                 * hiding one of its own windows and showing it again.
                 * Plasma's launcher, krunner and every applet popup work
                 * the second way: the window is kept alive and mapped and
                 * unmapped, so it opened exactly once and every appearance
                 * after that claimed to be a restore.
                 *
                 * Which handed each of them the minimize effect, running
                 * backwards. That effect travels between the window and
                 * the box a taskbar reserved for it, and with no taskbar
                 * saying anything it aims at the bottom edge of the output
                 * (effects/minimize.c) -- so the launcher came flying up
                 * from the bottom of the screen every time it was opened,
                 * while the fade and scale that answer `open` never ran at
                 * all.
                 *
                 * It also makes the two halves of this agree. A window its
                 * owner unmaps without minimizing it is a CLOSE above; the
                 * same window mapped again is the OPEN that matches it. */
                kind = COMP_EVENT_OPEN;

            w->has_been_mapped = true;
            emit(w, kind);
        }

        if (w->pending_geometry) {
            w->pending_geometry = false;
            w->pending_state = false;

            CompRect to = window_rect(w);
            CompEventKind kind;
            if (!state_delta_event(before, now, &kind))
                kind = COMP_EVENT_MOVE;

            /* It may have crossed onto a differently scaled monitor. */
            density_update_window(w);

            emit_geometry(w, kind, &w->pending_from, &to, w->pending_interactive);
        } else if (w->pending_state) {
            /* A state change with no geometry change of its own -- still
             * worth reporting as what it is, with the window's current
             * rectangle standing in for both ends. */
            w->pending_state = false;

            CompEventKind kind;
            if (state_delta_event(before, now, &kind)) {
                CompRect r = window_rect(w);
                emit_geometry(w, kind, &r, &r, false);
            }
        }

        /* Last, so an effect answering to focus sees the window where the
         * WM has just put it. What was *covering* it a moment ago is a
         * different question, and the stacking here can no longer answer
         * it -- the raise already happened -- which is what z_before is
         * for (comp.h, window_was_above). */
        if (w->pending_unfocus) {
            w->pending_unfocus = false;
            emit(w, COMP_EVENT_UNFOCUS);
        }
        if (w->pending_focus) {
            w->pending_focus = false;
            emit_with_appear(w, COMP_EVENT_FOCUS, appeared);
        }

        /* Nothing claimed the contents the last resize replaced. */
        renderer_stash_drop_unheld(w);

        w->state_before = now;
        w = next;
    }

    /* This batch is over: what the stacking looks like now is what the
     * next one will have to compare against. */
    stack_snapshot();
}

static void damage_create(CompWindow *w)
{
    if (!comp.caps.damage || w->damage != XCB_NONE || w->input_only)
        return;
    w->damage = xcb_generate_id(comp.conn);
    /* NON_EMPTY: one event per damage region until we subtract, which is
     * all a repaint-the-output prototype needs -- and the cheapest of the
     * report levels in event traffic. */
    xcb_damage_create(comp.conn, w->damage, w->id, XCB_DAMAGE_REPORT_LEVEL_NON_EMPTY);
}

static void damage_destroy(CompWindow *w)
{
    if (w->damage == XCB_NONE)
        return;
    xcb_damage_destroy(comp.conn, w->damage);
    w->damage = XCB_NONE;
}

static void window_add_at(xcb_window_t id, xcb_window_t above, bool on_top)
{
    if (id == XCB_NONE || id == comp.overlay || id == comp.cm_window)
        return;
    if (window_find(id))
        return;

    xcb_get_window_attributes_cookie_t ac =
        xcb_get_window_attributes(comp.conn, id);
    xcb_get_geometry_cookie_t gc = xcb_get_geometry(comp.conn, id);

    xcb_get_window_attributes_reply_t *attr =
        xcb_get_window_attributes_reply(comp.conn, ac, NULL);
    xcb_get_geometry_reply_t *geo =
        xcb_get_geometry_reply(comp.conn, gc, NULL);

    if (!attr || !geo) {
        /* Window died between the event and these round trips -- normal,
         * not an error. */
        free(attr);
        free(geo);
        return;
    }

    CompWindow *w = calloc(1, sizeof(*w));
    if (!w) {
        free(attr);
        free(geo);
        return;
    }

    w->id = id;
    w->x = geo->x;
    w->y = geo->y;
    w->w = geo->width;
    w->h = geo->height;
    w->border = geo->border_width;
    w->depth = geo->depth;
    w->visual = attr->visual;
    w->argb = (geo->depth == 32);
    w->input_only = (attr->_class == XCB_WINDOW_CLASS_INPUT_ONLY);
    w->override_redirect = attr->override_redirect;
    w->mapped = (attr->map_state == XCB_MAP_STATE_VIEWABLE);
    w->opacity = 1.0;
    w->damage = XCB_NONE;

    free(attr);
    free(geo);

    if (on_top)
        link_top(w);
    else
        link_above(w, above);

    /* PropertyChange so _NET_WM_WINDOW_OPACITY changes reach us. Event
     * masks are per-client, so this never disturbs the WM's or the app's
     * own selections on the same window. */
    uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
    xcb_change_window_attributes(comp.conn, id, XCB_CW_EVENT_MASK, &mask);

    /* ShapeNotify, so the cached bounding region can be dropped when the
     * window's silhouette changes. kiwm reshapes a frame on every resize
     * (rounded corners), so this is not a rare event. */
    if (comp.caps.shape)
        xcb_shape_select_input(comp.conn, id, 1);

    window_update_opacity(w);
    read_wm_layer(w);
    read_window_kind(w);
    w->state = w->state_before = read_window_state(w);

    /* Windows already on screen when kicomp started did not just appear.
     * Anything else that is already mapped by the time these round trips
     * come back *did*: a WM creates a frame and maps it in one go, so the
     * MapNotify is often already behind us -- and window_map() ignores a
     * window it considers mapped, which is exactly how an opening window
     * ended up with no open event and no animation at all. */
    w->has_been_mapped = adopting_existing && w->mapped;

    /* And a window that predates us and is *not* mapped has usually been
     * mapped all the same -- it is on a desktop that isn't showing, or
     * minimized -- we simply weren't here to watch it happen. Left as
     * "never mapped", its first appearance is read as the window opening:
     * start the compositor with a fullscreen window parked on another
     * desktop, switch to that desktop, and it plays the open animation
     * and sits out the desktop-wall slide it should have arrived with.
     *
     * WM_STATE is what tells the two apart. A window the WM has taken on
     * has it; one a client created and has not mapped yet does not, and
     * that one really is about to open. */
    if (adopting_existing && !w->mapped && window_is_managed(w))
        w->has_been_mapped = true;

    if (w->mapped && !adopting_existing)
        w->pending_appear = true;

    if (w->mapped) {
        damage_create(w);
        CompRect r = window_rect(w);
        output_damage_window_rect(w, &r);
    }
}

void window_add(xcb_window_t id, xcb_window_t above)
{
    window_add_at(id, above, false);
}

void window_add_top(xcb_window_t id)
{
    window_add_at(id, XCB_NONE, true);
}

/* Releases everything and drops the entry from the mirror. */
static void window_destroy(CompWindow *w)
{
    /* Drops the density pictures and, for a window that is merely going
     * away rather than dying, the requests we wrote on it (density.h). */
    density_forget(w);
    damage_destroy(w);
    renderer_window_free(w);
    unlink_window(w);
    free(w);
}

void window_retain(CompWindow *w)
{
    w->retain_count++;
}

void window_release(CompWindow *w)
{
    if (w->retain_count > 0)
        w->retain_count--;
    if (w->retain_count > 0)
        return;

    CompRect r = window_rect(w);
    output_damage_window_rect(w, &r);

    if (w->zombie) {
        /* Nothing left to be: the X window is gone and the last effect
         * that was still drawing it has let go. */
        window_destroy(w);
        return;
    }

    /* Still a real window, merely hidden (minimized, on another
     * desktop). Keep the mirror entry, drop the frozen contents -- a
     * remap names a fresh pixmap anyway. */
    renderer_window_invalidate(w);
}

void window_remove(xcb_window_t id)
{
    CompWindow *w = window_find(id);
    if (!w)
        return;

    if (w->mapped) {
        CompRect r = window_rect(w);
        output_damage_window_rect(w, &r);
    }

    damage_destroy(w);

    /* The X window is gone, but that is not the same as the *mirror*
     * entry being gone: a window that was on screen a moment ago still
     * owes the desktop a closing animation, and the contents pixmap we
     * named is ours until we free it. So anything that just disappeared
     * (or that an effect is already holding) stays as a zombie until the
     * flush has classified it and the last effect has let go. */
    bool was_visible = w->mapped || w->pending_disappear;
    w->mapped = false;

    if (was_visible) {
        /* It was still on screen (or still waiting to be classified):
         * this is the disappearance, and the flush will say what kind. */
        w->zombie = true;
        w->damage = XCB_NONE;
        w->pending_appear = false;
        w->pending_disappear = true;
        comp_log("window 0x%x destroyed, kept until the effects are done", w->id);
        return;
    }

    /* A window that is merely *stowed* is being kept for a desktop it
     * might come back to, and it is not coming back: the picture is of
     * something that no longer exists. Letting the stow's own retain go
     * here is what keeps a window closed while another desktop was
     * showing from leaving its pixmap behind for the rest of the
     * session. */
    if (w->stowed) {
        w->stowed = false;
        window_release(w);
    }

    if (w->retain_count > 0) {
        /* Already reported gone and already being animated -- the X
         * window catching up with that changes nothing, and must not be
         * announced a second time. */
        w->zombie = true;
        w->damage = XCB_NONE;
        return;
    }

    effects_window_gone(w);
    window_destroy(w);
}

void window_map(xcb_window_t id)
{
    CompWindow *w = window_find(id);
    if (!w || w->mapped)
        return;

    w->mapped = true;

    /* Back on screen: the kept picture is now the stale one, and the
     * window will name a fresh pixmap below. */
    if (w->stowed) {
        w->stowed = false;
        window_release(w);
    }

    /* It came back (a restore, a desktop switched to). Whatever was
     * animating its exit is now a lie -- drop it, which also releases the
     * window it was holding. */
    if (w->retain_count > 0)
        effects_window_gone(w);

    /* The contents pixmap of an unmapped window is meaningless, so a
     * fresh one is named on the next paint. */
    renderer_window_invalidate(w);
    damage_create(w);
    window_update_opacity(w);
    /* A frame created empty has its client by now -- kiwm reparents
     * before mapping, but the CreateNotify reached us first. */
    if (w->type == COMP_WINDOW_UNKNOWN)
        read_window_kind(w);

    /* What kind of appearance this is (opening, restoring, arriving with
     * a desktop) is decided at flush time, once the properties that say
     * so have also arrived. A window that was about to be reported as
     * disappearing and came straight back never disappeared at all. */
    w->pending_disappear = false;
    w->pending_appear = true;

    CompRect r = window_rect(w);
    output_damage_window_rect(w, &r);
}

void window_unmap(xcb_window_t id)
{
    CompWindow *w = window_find(id);
    if (!w || !w->mapped)
        return;

    w->mapped = false;
    damage_destroy(w);

    /* Deferred, not because it costs anything, but because right now
     * there is no way to know *why* it went away -- closed, minimized,
     * left behind by a desktop switch. The contents are kept until the
     * flush decides, which is also where an effect gets its chance to
     * retain them. */
    w->pending_appear = false;
    w->pending_disappear = true;

    CompRect r = window_rect(w);
    output_damage_window_rect(w, &r);
}

/* Two configures closer together than this are treated as part of one
 * interactive stream (a move/resize drag), which effects must stay out
 * of. Roughly "slower than a human can produce by dragging": a drag
 * generates them as fast as motion events arrive, a maximize generates
 * one (sometimes an immediate pair, hence the count below). */
#define INTERACTIVE_GAP_MS 120.0

void window_configure(xcb_window_t id, int x, int y, int w_, int h_, int border,
                      xcb_window_t above)
{
    CompWindow *w = window_find(id);
    if (!w) {
        /* A window created before we started listening, or one whose
         * CreateNotify we skipped. Adopt it now. */
        window_add(id, above);
        return;
    }

    CompRect old = window_rect(w);
    bool moved = (x != w->x || y != w->y);
    bool resized = (w_ != w->w || h_ != w->h || border != w->border);

    w->x = x;
    w->y = y;
    w->w = w_;
    w->h = h_;
    w->border = border;

    if (resized)
        w->opaque_known = false;

    if (resized) {
        /* A resize gives the window a brand new backing pixmap; the one
         * we hold is the old size -- but it is also the only record of
         * what the window looked like a moment ago, which an effect may
         * still need (shade rolls up a window whose real pixmap has
         * already collapsed to a titlebar). Set aside rather than freed;
         * the flush drops it if no effect claims it. */
        renderer_window_stash(w, &old);
    }

    window_restack(id, above);

    if (w->mapped) {
        CompRect now = window_rect(w);
        output_damage_window_rect(w, &old);
        output_damage_window_rect(w, &now);

        if (moved || resized) {
            /* Is this one jump, or one step of a drag? Without the
             * WM-to-compositor IPC of section 32 the timing is the only
             * evidence there is -- and a single fast pair still counts as
             * a jump, because a WM commonly reconfigures twice in a row
             * when adopting a new state. */
            double t = comp_now_ms();
            if (t - w->last_configure_ms < INTERACTIVE_GAP_MS)
                w->fast_configures++;
            else
                w->fast_configures = 0;
            w->last_configure_ms = t;

            /* Deferred like the rest: whether this resize *was* a
             * maximize, a shade or a fullscreen is written in a property
             * that may still be in flight. Several configures in one
             * batch collapse into one event, from where the window
             * started to where it ended up. */
            if (!w->pending_geometry) {
                w->pending_geometry = true;
                w->pending_from = old;
            }
            w->pending_interactive = (w->fast_configures >= 2);
        }
    }
}

void window_restack(xcb_window_t id, xcb_window_t above)
{
    CompWindow *w = window_find(id);
    if (!w)
        return;

    /* Already in the right place? Cheap check: the window directly below
     * us is `above`. */
    CompWindow *below = NULL;
    for (CompWindow *it = comp.stack; it; it = it->next) {
        if (it->next == w) {
            below = it;
            break;
        }
    }
    xcb_window_t cur_above = below ? below->id : XCB_NONE;
    if (cur_above == above)
        return;

    if (above != XCB_NONE && !window_find(above)) {
        /* The server says this window now sits above one the scene has
         * never heard of. That is not a window to guess around: it is
         * proof the scene's order has fallen out of step with the
         * server's -- an event that named a window created and
         * destroyed on the far side of windows_scan(), typically, which
         * is what a WM re-framing everything the moment a compositor
         * takes the selection produces. "Put it on top" was the old
         * answer, and it is how the focused window came to be drawn
         * under an unfocused one and every panel under both. The tree
         * is the only thing that knows; ask it, once, for everything. */
        comp_log("restack 0x%x above unknown 0x%x: resyncing from the tree", id, above);
        windows_resync_order();
        return;
    }

    unlink_window(w);
    link_above(w, above);

    if (w->mapped) {
        CompRect r = window_rect(w);
        output_damage_window_rect(w, &r);
    }
}

/* Put every window the scene knows where the server's tree has it. The
 * ordering half of windows_scan(), for when an event reveals the scene
 * has drifted (window_restack). Adds nothing and removes nothing: a
 * window in the tree the scene lacks is one whose CreateNotify is still
 * on its way, and one the scene has that the tree lacks has a
 * DestroyNotify coming. Only the order is taken from the tree. */
void windows_resync_order(void)
{
    xcb_query_tree_reply_t *tree =
        xcb_query_tree_reply(comp.conn, xcb_query_tree(comp.conn, comp.root), NULL);
    if (!tree)
        return;

    xcb_window_t *children = xcb_query_tree_children(tree);
    int n = xcb_query_tree_children_length(tree);

    xcb_window_t above = XCB_NONE;
    for (int i = 0; i < n; i++) {
        if (!window_find(children[i]))
            continue;
        window_restack(children[i], above);
        above = children[i];
    }

    free(tree);
    output_damage_all();
}

void windows_scan(void)
{
    adopting_existing = true;
    xcb_query_tree_reply_t *tree =
        xcb_query_tree_reply(comp.conn, xcb_query_tree(comp.conn, comp.root), NULL);
    if (!tree)
        return;

    xcb_window_t *children = xcb_query_tree_children(tree);
    int n = xcb_query_tree_children_length(tree);

    /* QueryTree returns bottom-most first, which is exactly the order our
     * list wants: each new window goes above the previous one.
     *
     * A window already known is *restacked* to where the tree has it,
     * not skipped. Events are selected before this scan runs, so a
     * window that was created or restacked in between arrived through
     * an event first and was placed by that event alone -- a create
     * puts it on top -- and the tree is the only thing that knows where
     * it really ended up. Skipping it left it wherever the event said,
     * which was wrong for the rest of the session. */
    xcb_window_t above = XCB_NONE;
    for (int i = 0; i < n; i++) {
        if (window_find(children[i]))
            window_restack(children[i], above);
        else
            window_add(children[i], above);
        if (window_find(children[i]))
            above = children[i];
    }

    free(tree);
    adopting_existing = false;
}

void windows_teardown(void)
{
    CompWindow *w = comp.stack;
    while (w) {
        CompWindow *next = w->next;
        damage_destroy(w);
        renderer_window_free(w);
        free(w);
        w = next;
    }
    comp.stack = NULL;
}
