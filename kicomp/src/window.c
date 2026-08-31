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

#include <xcb/shape.h>

#include <stdlib.h>
#include <string.h>

/* How long after a desktop switch a window vanishing (or appearing) is
 * credited to that switch rather than to being closed (or opened). The
 * WM does both in one go, so this only has to cover the gap between the
 * root property and the maps/unmaps that follow it. */
#define DESKTOP_SWITCH_WINDOW_MS 250.0

/* True only while windows_scan() adopts what was already on screen at
 * startup: those windows did not just appear, and must not be animated
 * as if they had. */
static bool adopting_existing;

static void window_destroy(CompWindow *w);
static uint32_t read_window_state(CompWindow *w);

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
        output_damage_rect(&r2);
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
static xcb_window_t resolve_client(CompWindow *w)
{
    if (w->client != XCB_NONE)
        return w->client;

    xcb_query_tree_reply_t *tree =
        xcb_query_tree_reply(comp.conn, xcb_query_tree(comp.conn, w->id), NULL);
    if (!tree)
        return XCB_NONE;

    int n = xcb_query_tree_children_length(tree);
    if (n > 0) {
        xcb_window_t *children = xcb_query_tree_children(tree);
        w->client = children[0];
    } else if (w->override_redirect) {
        w->client = w->id;   /* never framed: it speaks for itself */
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

void window_client_reparented(xcb_window_t frame_id, xcb_window_t client)
{
    CompWindow *w = window_find(frame_id);
    if (!w)
        return;

    /* The WM has just put a client inside a frame we track: that client
     * is where _NET_WM_WINDOW_TYPE and _NET_WM_STATE live, so this is the
     * moment the frame stops being anonymous. */
    w->client = client;

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

static void emit(CompWindow *w, CompEventKind kind)
{
    CompEvent ev = { .kind = kind };
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
        if (now_focused && !w->focused) {
            w->focused = true;
            emit(w, COMP_EVENT_FOCUS);
        } else if (was_focused && w->focused && !now_focused) {
            w->focused = false;
            emit(w, COMP_EVENT_UNFOCUS);
        }
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
            w->pending_geometry || w->pending_state)
            return true;
    return false;
}

void windows_flush_events(void)
{
    CompWindow *w = comp.stack;
    while (w) {
        CompWindow *next = w->next;

        if (!w->pending_appear && !w->pending_disappear &&
            !w->pending_geometry && !w->pending_state) {
            w = next;
            continue;
        }

        uint32_t before = w->state_before;
        uint32_t now = w->state = read_window_state(w);

        if (w->pending_disappear) {
            w->pending_disappear = false;

            CompEventKind kind;
            if (w->zombie)
                kind = COMP_EVENT_CLOSE;
            else if (now & COMP_STATE_MINIMIZED)
                kind = COMP_EVENT_MINIMIZE;
            else if (comp_now_ms() - comp.desktop_changed_ms < DESKTOP_SWITCH_WINDOW_MS)
                kind = COMP_EVENT_DESKTOP_LEAVE;
            else
                kind = COMP_EVENT_CLOSE;

            emit(w, kind);

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

        if (w->pending_appear) {
            w->pending_appear = false;

            CompEventKind kind;
            if ((before & COMP_STATE_MINIMIZED) && !(now & COMP_STATE_MINIMIZED))
                kind = COMP_EVENT_RESTORE;
            else if (w->has_been_mapped &&
                     comp_now_ms() - comp.desktop_changed_ms < DESKTOP_SWITCH_WINDOW_MS)
                kind = COMP_EVENT_DESKTOP_ENTER;
            else if (w->has_been_mapped)
                kind = COMP_EVENT_RESTORE;
            else
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

        w->state_before = now;
        w = next;
    }
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
    if (w->mapped && !adopting_existing)
        w->pending_appear = true;

    if (w->mapped) {
        damage_create(w);
        CompRect r = window_rect(w);
        output_damage_rect(&r);
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
    output_damage_rect(&r);

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
        output_damage_rect(&r);
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
    output_damage_rect(&r);
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
    output_damage_rect(&r);
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

    if (resized) {
        /* A resize gives the window a brand new backing pixmap; the one
         * we hold is the old size. */
        renderer_window_invalidate(w);
    }

    window_restack(id, above);

    if (w->mapped) {
        CompRect now = window_rect(w);
        output_damage_rect(&old);
        output_damage_rect(&now);

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

    unlink_window(w);
    link_above(w, above);

    if (w->mapped) {
        CompRect r = window_rect(w);
        output_damage_rect(&r);
    }
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
     * list wants: each new window goes above the previous one. */
    xcb_window_t above = XCB_NONE;
    for (int i = 0; i < n; i++) {
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
