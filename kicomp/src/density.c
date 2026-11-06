/* See density.h. */
#include "density.h"
#include "output.h"
#include "window.h"
#include "renderer.h"
#include "effect.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static xcb_window_t manager_window;

/* The largest texture a GPU is trusted to hold (renderer-gl.c's own
 * shape_mask_texture assumes the same number), and how much of it a
 * dense window's auxiliary pixmap is allowed to ask for. Left as
 * headroom rather than spent in full: this is one window on a desktop
 * that may have several asking at once, and a wide monitor's own
 * framebuffer wants some of the same budget. */
#define DENSITY_MAX_TEXTURE 8192
#define DENSITY_TEXTURE_HEADROOM 0.70f

/* The density to ask a window for: whatever its output is scaled to,
 * times whatever the zoom lens on that output currently wants on top of
 * it (effect.h's zoom_density_level -- 1 where there is no lens, or the
 * window is on a different output than the one that has it). A window
 * straddling two differently scaled outputs has to pick one, and the one
 * holding its centre is the same rule everything else here uses. Returns
 * 1.0 for a window on no output at all. */
static float wanted_density(const CompWindow *w)
{
    CompRect r = window_rect(w);
    CompRect centre = { r.x + r.w / 2, r.y + r.h / 2, 1, 1 };

    float density = 1.0f;
    bool on_an_output = false;

    for (int i = 0; i < comp.output_count; i++) {
        CompRect hit;
        if (rect_intersect(&centre, &comp.outputs[i].rect, &hit)) {
            density = comp.outputs[i].scale *
                      (float)zoom_density_level(comp.outputs[i].id);
            on_an_output = true;
            break;
        }
    }
    if (!on_an_output)
        return 1.0f;

    /* Capped rather than refused: a window whose logical size would
     * blow its dense pixmap past the budget above is asked for as much
     * density as fits instead of the 1/1 it would fall back to on its
     * own -- sharper is still better than not, for as much of it as the
     * texture can actually hold. */
    float budget = (float)DENSITY_MAX_TEXTURE * DENSITY_TEXTURE_HEADROOM;
    if (r.w > 0 && budget / (float)r.w < density)
        density = budget / (float)r.w;
    if (r.h > 0 && budget / (float)r.h < density)
        density = budget / (float)r.h;
    if (density < 1.0f)
        density = 1.0f;

    return density;
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
    if (!comp.density_enabled) {
        /* Never claims the selection: a client checks for the manager
         * with XGetSelectionOwner()/XFixesSelectSelectionInput() before
         * ever asking for a density, so leaving it unclaimed is enough on
         * its own for every client to fall back to the plain, magnified
         * path -- nothing else in this file needs to know density is
         * off. */
        comp_log("X-DENSITY disabled (kicomp.conf's density=0)");
        return;
    }
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

/* Watches a newly published dense pixmap with Damage (comp.h's
 * density_damage), letting go of the one before. The destroy is checked
 * and discarded: freeing the pixmap takes its Damage with it, so it may
 * find nothing.
 *
 * The create waits for the server, once per pixmap, never per frame: the
 * client announces a pixmap only once and Damage is all that says it drew
 * again, so whatever it draws before the Damage exists must already be in
 * the pixmap when the repaint that follows this samples it. */
static void watch_pixmap(xcb_damage_damage_t *damage, bool *pending,
                         bool *works, xcb_pixmap_t pixmap)
{
    if (*damage != XCB_NONE)
        xcb_discard_reply(comp.conn,
            xcb_damage_destroy_checked(comp.conn, *damage).sequence);
    *damage = XCB_NONE;
    *pending = false;
    *works = false;

    if (!comp.caps.damage || pixmap == 0)
        return;

    *damage = xcb_generate_id(comp.conn);
    xcb_generic_error_t *err = xcb_request_check(comp.conn,
        xcb_damage_create_checked(comp.conn, *damage, pixmap,
                                  XCB_DAMAGE_REPORT_LEVEL_NON_EMPTY));
    if (err) {
        /* Freed already: the next pixmap it publishes is watched instead. */
        free(err);
        *damage = XCB_NONE;
    }
}

bool density_pixmap_damaged(xcb_drawable_t drawable)
{
    bool ours = false;

    if (drawable == XCB_NONE)
        return false;
    for (CompWindow *w = comp.stack; w; w = w->next) {
        if (w->density_pixmap == drawable && w->density_damage != XCB_NONE) {
            w->density_damage_pending = w->density_damage_works = true;
            ours = true;
        }
        if (w->deco_density_pixmap == drawable &&
            w->deco_density_damage != XCB_NONE) {
            w->deco_density_damage_pending = w->deco_density_damage_works = true;
            ours = true;
        }
    }
    return ours;
}

void density_property_changed(CompWindow *w, xcb_window_t on, xcb_atom_t atom)
{
    /* The frame answers for the decoration, the client for its contents.
     * A window that is its own client has only the one answer. */
    bool is_frame = (on == w->id && w->client != w->id);

    if (on == XCB_NONE)
        return;

    uint32_t scale[2] = { 1, 1 };
    uint32_t pixmap = 0;

    xcb_pixmap_t *pix = is_frame ? &w->deco_density_pixmap : &w->density_pixmap;
    bool works = is_frame ? w->deco_density_damage_works
                          : w->density_damage_works;

    bool has_pixmap = read_cardinals(on, comp.atoms.density_pixmap, &pixmap, 1);
    if (!has_pixmap)
        pixmap = 0;

    /* The same XID rewritten: an older client saying it drew a frame
     * (X-DENSITY.md section 4 asked for it before Damage did the job). One
     * read, not two -- the scale only ever changes along with the pixmap,
     * or on its own PropertyNotify. And when the pixmap's own Damage has
     * been reporting, nothing to repaint here at all: what changed is
     * already on its way through damage.c, piece by piece. */
    if (atom == comp.atoms.density_pixmap && pixmap != 0 && pixmap == *pix) {
        if (!works) {
            /* Drawn some way Damage doesn't see (straight from the GPU,
             * with no DamageAdd): the rewrite is the only word there is,
             * and it covers all of it. */
            CompRect r = window_rect(w);
            output_damage_window_rect(w, &r);
        }
        return;
    }

    bool has_scale = read_cardinals(on, comp.atoms.density_scale, scale, 2);

    /* Believe the client, not the request: it may have rounded, or hit a
     * size limit, or refused entirely. */
    if (!has_scale || scale[1] == 0) {
        scale[0] = scale[1] = 1;
    }

    uint32_t *num = is_frame ? &w->deco_density_num : &w->density_num;
    uint32_t *den = is_frame ? &w->deco_density_den : &w->density_den;

    bool changed = (*num != scale[0] || *den != scale[1] || *pix != pixmap);

    *num = scale[0];
    *den = scale[1];

    bool appeared = (*pix == 0 && pixmap != 0);
    if (*pix != pixmap) {
        /* The picture cached for the old pixmap describes a drawable that
         * may not even exist any more. */
        *pix = pixmap;
        renderer_window_density_invalidate(w, is_frame);
        if (is_frame)
            watch_pixmap(&w->deco_density_damage, &w->deco_density_damage_pending,
                         &w->deco_density_damage_works, pixmap);
        else
            watch_pixmap(&w->density_damage, &w->density_damage_pending,
                         &w->density_damage_works, pixmap);
    }

    /* Its holes may well have been published before we were looking
     * (a compositor restarted under a dense window): read once, as the
     * pixmap appears, and followed by PropertyNotify from then on. */
    if (appeared && !is_frame) {
        density_holes_changed(w, comp.atoms.density_passthrough);
        density_holes_changed(w, comp.atoms.density_sources);
    }

    if (changed)
        comp_log("window 0x%x: %s density %u/%u pixmap 0x%x", w->id,
                 is_frame ? "decoration" : "content", scale[0], scale[1], pixmap);

    /* A new pixmap or a new scale: all of the window is drawn differently.
     * (The same XID rewritten, the per-frame case, returned above.) */
    CompRect r = window_rect(w);
    output_damage_rect(&r);
}

/* ---- the holes: _X_DENSITY_PASSTHROUGH, _X_DENSITY_SOURCES ---- */

/* Up to `max` CARDINALs of a list property; how many there were. */
static int read_list(xcb_window_t win, xcb_atom_t atom, uint32_t *out, int max)
{
    if (atom == XCB_NONE || win == XCB_NONE)
        return 0;

    xcb_get_property_reply_t *r = xcb_get_property_reply(comp.conn,
        xcb_get_property(comp.conn, 0, win, atom, XCB_ATOM_CARDINAL, 0,
                         (uint32_t)max), NULL);
    if (!r)
        return 0;

    int n = 0;
    if (r->type == XCB_ATOM_CARDINAL && r->format == 32) {
        n = xcb_get_property_value_length(r) / 4;
        if (n > max)
            n = max;
        memcpy(out, xcb_get_property_value(r), sizeof(uint32_t) * (size_t)n);
    }
    free(r);
    return n;
}

/* A rectangle as the protocol carries it: CARDINALs, so a negative
 * offset (a child scrolled partly out) arrives wrapped. */
static CompRect list_rect(const uint32_t *v)
{
    return (CompRect){ (int)(int32_t)v[0], (int)(int32_t)v[1],
                       (int)v[2], (int)v[3] };
}

/* Names the source window's contents, which only works while it is
 * redirected (the client's job) and viewable; otherwise it simply stays
 * without a pixmap and its rectangle shows the window's own pixels. */
static void source_name(CompDensitySource *s)
{
    if (s->window == XCB_NONE || s->pixmap)
        return;

    xcb_pixmap_t p = xcb_generate_id(comp.conn);
    xcb_void_cookie_t nc =
        xcb_composite_name_window_pixmap_checked(comp.conn, s->window, p);
    xcb_get_geometry_cookie_t gc = xcb_get_geometry(comp.conn, p);

    xcb_generic_error_t *nerr = xcb_request_check(comp.conn, nc);
    xcb_generic_error_t *gerr = NULL;
    xcb_get_geometry_reply_t *g = xcb_get_geometry_reply(comp.conn, gc, &gerr);

    if (!nerr && g) {
        s->pixmap = p;
        s->width = g->width;
        s->height = g->height;
        s->depth = g->depth;
    }
    free(nerr);
    free(gerr);
    free(g);
}

/* Lets go of every source's pixmap: the renderer first, since its imports
 * read them. */
static void sources_drop_pixmaps(CompWindow *w)
{
    renderer_window_density_sources_invalidate(w);
    for (int i = 0; i < w->density_source_count; i++) {
        CompDensitySource *s = &w->density_sources[i];
        if (s->pixmap)
            xcb_free_pixmap(comp.conn, s->pixmap);
        s->pixmap = 0;
    }
}

static void source_watch(CompDensitySource *s)
{
    /* StructureNotify for its resizes (a new pixmap to name) and its
     * unmap and destruction; Damage for its frames. Checked and
     * discarded: the client may have destroyed it already. */
    uint32_t mask = XCB_EVENT_MASK_STRUCTURE_NOTIFY;
    xcb_discard_reply(comp.conn,
        xcb_change_window_attributes_checked(comp.conn, s->window,
                                             XCB_CW_EVENT_MASK, &mask).sequence);
    if (comp.caps.damage) {
        s->damage = xcb_generate_id(comp.conn);
        xcb_discard_reply(comp.conn,
            xcb_damage_create_checked(comp.conn, s->damage, s->window,
                                      XCB_DAMAGE_REPORT_LEVEL_NON_EMPTY).sequence);
    }
    source_name(s);
}

static void source_unwatch(CompDensitySource *s)
{
    if (s->window == XCB_NONE)
        return;
    uint32_t mask = 0;
    xcb_discard_reply(comp.conn,
        xcb_change_window_attributes_checked(comp.conn, s->window,
                                             XCB_CW_EVENT_MASK, &mask).sequence);
    if (s->damage)
        xcb_discard_reply(comp.conn,
            xcb_damage_destroy_checked(comp.conn, s->damage).sequence);
    s->damage = 0;
}

static void sources_clear(CompWindow *w)
{
    sources_drop_pixmaps(w);
    for (int i = 0; i < w->density_source_count; i++)
        source_unwatch(&w->density_sources[i]);
    w->density_source_count = 0;
}

void density_holes_changed(CompWindow *w, xcb_atom_t atom)
{
    uint32_t v[5 * DENSITY_MAX_AREAS] = { 0 };

    if (atom == comp.atoms.density_passthrough) {
        int n = read_list(w->client, atom, v, 4 * DENSITY_MAX_AREAS) / 4;
        for (int i = 0; i < n; i++)
            w->density_through[i] = list_rect(&v[4 * i]);
        w->density_through_count = n;
        comp_log("window 0x%x: %d passthrough area(s)", w->id, n);
    } else if (atom == comp.atoms.density_sources) {
        /* Rewritten when the layout changes, not per frame: starting the
         * list over is a handful of requests, and always right. */
        sources_clear(w);
        int n = read_list(w->client, atom, v, 5 * DENSITY_MAX_AREAS) / 5;
        for (int i = 0; i < n; i++) {
            CompDensitySource *s = &w->density_sources[i];
            memset(s, 0, sizeof(*s));
            s->rect = list_rect(&v[5 * i]);
            s->window = v[5 * i + 4];
            source_watch(s);
        }
        w->density_source_count = n;
        comp_log("window 0x%x: %d dense source(s)", w->id, n);
    } else {
        return;
    }

    CompRect r = window_rect(w);
    output_damage_window_rect(w, &r);
}

/* Every source shown as `window`, across every window that has any. */
#define FOR_EACH_SOURCE(w, s, xid)                                          \
    for (CompWindow *w = comp.stack; w; w = w->next)                        \
        for (int i_ = 0; i_ < w->density_source_count; i_++)                \
            for (CompDensitySource *s = &w->density_sources[i_];            \
                 s && s->window == (xid); s = NULL)

void density_source_damaged(xcb_drawable_t drawable)
{
    FOR_EACH_SOURCE(w, s, drawable)
        s->damage_pending = true;
}

void density_source_configured(xcb_window_t window)
{
    if (window == XCB_NONE)
        return;
    FOR_EACH_SOURCE(w, s, window) {
        /* Resized: what was named is the old size's contents. All of the
         * window's go, as the renderer drops its imports per window. */
        sources_drop_pixmaps(w);
        for (int i = 0; i < w->density_source_count; i++)
            source_name(&w->density_sources[i]);
        CompRect r = window_rect(w);
        output_damage_window_rect(w, &r);
    }
}

void density_source_gone(xcb_window_t window, bool destroyed)
{
    if (window == XCB_NONE)
        return;
    FOR_EACH_SOURCE(w, s, window) {
        if (destroyed) {
            /* Its Damage went with it. */
            s->window = XCB_NONE;
            s->damage = 0;
        }
        sources_drop_pixmaps(w);
        for (int i = 0; i < w->density_source_count; i++)
            source_name(&w->density_sources[i]);   /* the others */
        CompRect r = window_rect(w);
        output_damage_window_rect(w, &r);
    }
}

bool density_source_root_rect(const CompWindow *w, const CompDensitySource *s,
                              CompRect *out)
{
    if (!s->pixmap)
        return false;
    CompRect wr = window_rect(w);
    *out = (CompRect){ wr.x + w->client_rect.x + s->rect.x,
                       wr.y + w->client_rect.y + s->rect.y,
                       s->rect.w, s->rect.h };
    return true;
}

static bool contains(const CompRect *outer, const CompRect *r)
{
    return r->x >= outer->x && r->y >= outer->y &&
           r->x + r->w <= outer->x + outer->w && r->y + r->h <= outer->y + outer->h;
}

bool density_through_filled(const CompWindow *w, const CompRect *through)
{
    for (int i = 0; i < w->density_source_count; i++) {
        const CompDensitySource *s = &w->density_sources[i];
        if (s->pixmap && contains(&s->rect, through))
            return true;
    }
    return false;
}

bool density_source_hides(const CompWindow *w, const CompRect *r)
{
    if (w->density_source_count == 0 || !density_active(w, NULL))
        return false;
    for (int i = 0; i < w->density_source_count; i++) {
        CompRect at;
        if (density_source_root_rect(w, &w->density_sources[i], &at) &&
            contains(&at, r))
            return true;
    }
    return false;
}

void density_forget(CompWindow *w)
{
    sources_clear(w);
    w->density_through_count = 0;

    if (w->density_requested && w->client != XCB_NONE) {
        xcb_delete_property(comp.conn, w->client, comp.atoms.density_requested);
        w->density_requested = false;
    }
    if (w->deco_density_requested) {
        xcb_delete_property(comp.conn, w->id, comp.atoms.density_requested);
        w->deco_density_requested = false;
    }

    w->density_pixmap = w->deco_density_pixmap = 0;
    watch_pixmap(&w->density_damage, &w->density_damage_pending,
                 &w->density_damage_works, 0);
    watch_pixmap(&w->deco_density_damage, &w->deco_density_damage_pending,
                 &w->deco_density_damage_works, 0);
    renderer_window_density_invalidate(w, false);
    renderer_window_density_invalidate(w, true);
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
