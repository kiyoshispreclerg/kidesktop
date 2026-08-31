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

#include <xcb/shape.h>

#include <stdlib.h>
#include <string.h>

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

CompWindow *window_find(xcb_window_t id)
{
    for (CompWindow *w = comp.stack; w; w = w->next)
        if (w->id == id)
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
    renderer_window_free(w);
    unlink_window(w);
    free(w);
}

void window_map(xcb_window_t id)
{
    CompWindow *w = window_find(id);
    if (!w || w->mapped)
        return;

    w->mapped = true;
    /* The contents pixmap of an unmapped window is meaningless, so a
     * fresh one is named on the next paint. */
    renderer_window_invalidate(w);
    damage_create(w);
    window_update_opacity(w);

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
    renderer_window_invalidate(w);

    CompRect r = window_rect(w);
    output_damage_rect(&r);
}

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
