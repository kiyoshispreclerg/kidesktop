/* See density.h. */
#include "density.h"
#include "output.h"
#include "window.h"
#include "renderer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static xcb_window_t manager_window;

/* The density to ask a window for: whatever its output is scaled to. A
 * window straddling two differently scaled outputs has to pick one, and
 * the one holding its centre is the same rule everything else here uses.
 * Returns 1.0 for a window on no output at all. */
static float wanted_density(const CompWindow *w)
{
    CompRect r = window_rect(w);
    CompRect centre = { r.x + r.w / 2, r.y + r.h / 2, 1, 1 };

    for (int i = 0; i < comp.output_count; i++) {
        CompRect hit;
        if (rect_intersect(&centre, &comp.outputs[i].rect, &hit))
            return comp.outputs[i].scale;
    }
    return 1.0f;
}

/* A float as the small fraction the protocol carries. Densities are
 * things like 2, 1.5 and 1.25, so eighths express every sane one exactly
 * and nothing has to negotiate over rounding. */
static void as_fraction(float density, uint32_t *num, uint32_t *den)
{
    uint32_t eighths = (uint32_t)(density * 8.0f + 0.5f);
    if (eighths < 8)
        eighths = 8;

    uint32_t n = eighths, d = 8;
    while (n % 2 == 0 && d % 2 == 0) {
        n /= 2;
        d /= 2;
    }
    *num = n;
    *den = d;
}

void density_init(void)
{
    if (comp.atoms.density_manager == XCB_NONE)
        return;

    /* The compositor's own window does for this, exactly as it does for
     * _NET_WM_CM_S<n>: what matters to a client is that *someone* holds
     * it, and that it is released when the compositor goes away -- which
     * closing the connection does by itself. */
    manager_window = comp.cm_window;
    if (manager_window == XCB_NONE)
        return;

    xcb_set_selection_owner(comp.conn, manager_window, comp.atoms.density_manager,
                            XCB_CURRENT_TIME);

    xcb_get_selection_owner_reply_t *own = xcb_get_selection_owner_reply(comp.conn,
        xcb_get_selection_owner(comp.conn, comp.atoms.density_manager), NULL);
    bool ours = own && own->owner == manager_window;
    free(own);

    if (!ours) {
        /* Somebody else is claiming to consume the protocol. Not fatal --
         * we simply never ask for a density, and clients keep drawing
         * themselves at 1/1 for us. */
        fprintf(stderr, "kicomp: _X_DENSITY_MANAGER_S%d is owned by someone "
                        "else; not requesting densities\n", comp.screen_num);
        manager_window = XCB_NONE;
        return;
    }

    comp_log("owning _X_DENSITY_MANAGER_S%d", comp.screen_num);
}

/* Writes (or withdraws) the request on one window. Returns whether it is
 * now requested. */
static bool request_on(xcb_window_t target, uint32_t num, uint32_t den,
                       bool currently_requested)
{
    if (target == XCB_NONE)
        return false;

    if (num == den) {
        /* Deleted rather than written as 1/1: the protocol's own way of
         * saying "back to normal", and it leaves nothing behind on a
         * window that outlives this compositor. */
        if (currently_requested)
            xcb_delete_property(comp.conn, target, comp.atoms.density_requested);
        return false;
    }

    uint32_t value[2] = { num, den };
    xcb_change_property(comp.conn, XCB_PROP_MODE_REPLACE, target,
                        comp.atoms.density_requested, XCB_ATOM_CARDINAL, 32,
                        2, value);
    return true;
}

void density_update_window(CompWindow *w)
{
    if (manager_window == XCB_NONE)
        return;
    /* The compositor's own layers and input-only windows have nothing to
     * redraw, and the desktop window is a wallpaper. */
    if (w->input_only || w->wm_layer[0] || w->type == COMP_WINDOW_DESKTOP)
        return;

    float density = wanted_density(w);

    uint32_t num, den;
    as_fraction(density, &num, &den);

    /* Two requests, because there are two sets of pixels: the app's
     * contents, and the decoration the WM drew around them. A window that
     * isn't framed (an override-redirect menu is its own client) gets one,
     * since both would be the same window. */
    bool was = w->density_requested;
    w->density_requested = request_on(w->client, num, den, w->density_requested);

    if (w->client != w->id) {
        w->deco_density_requested =
            request_on(w->id, num, den, w->deco_density_requested);
    }

    if (w->density_requested != was || (w->density_requested && num != den))
        comp_log("window 0x%x: asked for density %u/%u", w->id, num, den);
}

void density_update_all(void)
{
    for (CompWindow *w = comp.stack; w; w = w->next)
        density_update_window(w);
}

/* One CARDINAL[] property from the client window. */
static bool read_cardinals(xcb_window_t win, xcb_atom_t atom,
                           uint32_t *out, int want)
{
    if (atom == XCB_NONE || win == XCB_NONE)
        return false;

    xcb_get_property_reply_t *r = xcb_get_property_reply(comp.conn,
        xcb_get_property(comp.conn, 0, win, atom, XCB_ATOM_CARDINAL, 0,
                         (uint32_t)want), NULL);
    if (!r)
        return false;

    bool ok = false;
    if (r->type == XCB_ATOM_CARDINAL && r->format == 32 &&
        xcb_get_property_value_length(r) >= want * 4) {
        memcpy(out, xcb_get_property_value(r), sizeof(uint32_t) * (size_t)want);
        ok = true;
    }
    free(r);
    return ok;
}

void density_property_changed(CompWindow *w, xcb_window_t on)
{
    /* The frame answers for the decoration, the client for its contents.
     * A window that is its own client has only the one answer. */
    bool is_frame = (on == w->id && w->client != w->id);

    if (on == XCB_NONE)
        return;

    uint32_t scale[2] = { 1, 1 };
    uint32_t pixmap = 0;

    bool has_scale = read_cardinals(on, comp.atoms.density_scale, scale, 2);
    bool has_pixmap = read_cardinals(on, comp.atoms.density_pixmap, &pixmap, 1);

    /* Believe the client, not the request: it may have rounded, or hit a
     * size limit, or refused entirely. */
    if (!has_scale || scale[1] == 0) {
        scale[0] = scale[1] = 1;
    }
    if (!has_pixmap)
        pixmap = 0;

    uint32_t *num = is_frame ? &w->deco_density_num : &w->density_num;
    uint32_t *den = is_frame ? &w->deco_density_den : &w->density_den;
    xcb_pixmap_t *pix = is_frame ? &w->deco_density_pixmap : &w->density_pixmap;

    bool changed = (*num != scale[0] || *den != scale[1] || *pix != pixmap);

    *num = scale[0];
    *den = scale[1];

    if (*pix != pixmap) {
        /* The picture cached for the old pixmap describes a drawable that
         * may not even exist any more. */
        renderer_window_density_invalidate(w, is_frame);
        *pix = pixmap;
    }

    if (changed)
        comp_log("window 0x%x: %s density %u/%u pixmap 0x%x", w->id,
                 is_frame ? "decoration" : "content", scale[0], scale[1], pixmap);

    /* Every property change is also "the contents changed": a pixmap
     * raises no Damage of its own, so the client rewriting the same XID is
     * how it says it drew a new frame (TESTS/X-DENSITY.md section 4). */
    CompRect r = window_rect(w);
    output_damage_rect(&r);
}

void density_forget(CompWindow *w)
{
    if (w->density_requested && w->client != XCB_NONE) {
        xcb_delete_property(comp.conn, w->client, comp.atoms.density_requested);
        w->density_requested = false;
    }
    if (w->deco_density_requested) {
        xcb_delete_property(comp.conn, w->id, comp.atoms.density_requested);
        w->deco_density_requested = false;
    }

    renderer_window_density_invalidate(w, false);
    renderer_window_density_invalidate(w, true);
    w->density_pixmap = w->deco_density_pixmap = 0;
    w->density_num = w->density_den = 1;
    w->deco_density_num = w->deco_density_den = 1;
}

static bool active(uint32_t num, uint32_t den, xcb_pixmap_t pixmap, float *factor)
{
    if (pixmap == 0 || den == 0 || num == den)
        return false;
    if (factor)
        *factor = (float)num / (float)den;
    return true;
}

bool density_active(const CompWindow *w, float *factor)
{
    return active(w->density_num, w->density_den, w->density_pixmap, factor);
}

bool deco_density_active(const CompWindow *w, float *factor)
{
    return active(w->deco_density_num, w->deco_density_den,
                  w->deco_density_pixmap, factor);
}

void density_shutdown(void)
{
    if (manager_window == XCB_NONE)
        return;

    /* Let go of the selection *and* of every request: a client that
     * outlives this compositor must not be left redrawing itself densely
     * for a pixmap nobody is sampling. */
    for (CompWindow *w = comp.stack; w; w = w->next) {
        if (w->density_requested && w->client != XCB_NONE)
            xcb_delete_property(comp.conn, w->client, comp.atoms.density_requested);
        if (w->deco_density_requested)
            xcb_delete_property(comp.conn, w->id, comp.atoms.density_requested);
    }

    xcb_set_selection_owner(comp.conn, XCB_NONE, comp.atoms.density_manager,
                            XCB_CURRENT_TIME);
    xcb_flush(comp.conn);
    manager_window = XCB_NONE;
}
