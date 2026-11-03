/* RandR outputs, each with its own render target and its own dirty state.
 * See kiwm-kicomp-projeto.md sections 4, 18 and 39. */
#include "output.h"
#include "capture.h"
#include "animation.h"
#include "region.h"
#include "renderer.h"
#include "presenter.h"
#include "shadow.h"
#include "config.h"
#include "xscale.h"
#include "density.h"

#include <xcb/randr.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool rect_intersect(const CompRect *a, const CompRect *b, CompRect *out)
{
    int x1 = a->x > b->x ? a->x : b->x;
    int y1 = a->y > b->y ? a->y : b->y;
    int x2 = (a->x + a->w) < (b->x + b->w) ? (a->x + a->w) : (b->x + b->w);
    int y2 = (a->y + a->h) < (b->y + b->h) ? (a->y + a->h) : (b->y + b->h);

    if (x2 <= x1 || y2 <= y1)
        return false;

    out->x = x1;
    out->y = y1;
    out->w = x2 - x1;
    out->h = y2 - y1;
    return true;
}

static void get_atom_name_into(xcb_atom_t atom, char *out, size_t out_sz)
{
    out[0] = '\0';
    xcb_get_atom_name_reply_t *r =
        xcb_get_atom_name_reply(comp.conn, xcb_get_atom_name(comp.conn, atom), NULL);
    if (!r)
        return;
    int len = xcb_get_atom_name_name_length(r);
    char *name = xcb_get_atom_name_name(r);
    if (len > 0) {
        if ((size_t)len >= out_sz)
            len = (int)out_sz - 1;
        memcpy(out, name, (size_t)len);
        out[len] = '\0';
    }
    free(r);
}

/* Same computation kiwm's output.c does: the output's CRTC, that CRTC's
 * current mode, refresh = dot_clock / (htotal * vtotal). Kept here rather
 * than shared because kicomp must not link against the WM (section 14) --
 * and because the compositor is the side that actually needs it, for
 * per-output pacing (Fase 7).
 *
 * Also reports the CRTC itself, which the Present presenter needs: a
 * frame is timed against the vblank of the monitor it is for. */
static double compute_output_refresh_hz(xcb_randr_output_t output_id,
                                        xcb_randr_crtc_t *crtc_out)
{
    double hz = 60.0;
    if (crtc_out)
        *crtc_out = XCB_NONE;

    xcb_randr_get_screen_resources_current_reply_t *res =
        xcb_randr_get_screen_resources_current_reply(comp.conn,
            xcb_randr_get_screen_resources_current(comp.conn, comp.root), NULL);
    if (!res)
        return hz;

    xcb_randr_get_output_info_reply_t *oinfo = xcb_randr_get_output_info_reply(comp.conn,
        xcb_randr_get_output_info(comp.conn, output_id, res->config_timestamp), NULL);
    if (!oinfo || oinfo->crtc == XCB_NONE) {
        free(oinfo);
        free(res);
        return hz;
    }

    if (crtc_out)
        *crtc_out = oinfo->crtc;

    xcb_randr_get_crtc_info_reply_t *cinfo = xcb_randr_get_crtc_info_reply(comp.conn,
        xcb_randr_get_crtc_info(comp.conn, oinfo->crtc, res->config_timestamp), NULL);
    free(oinfo);
    if (!cinfo || cinfo->mode == XCB_NONE) {
        free(cinfo);
        free(res);
        return hz;
    }

    xcb_randr_mode_info_t *modes = xcb_randr_get_screen_resources_current_modes(res);
    int nmodes = xcb_randr_get_screen_resources_current_modes_length(res);
    for (int i = 0; i < nmodes; i++) {
        if (modes[i].id != cinfo->mode)
            continue;
        double vtotal = modes[i].vtotal;
        if (modes[i].mode_flags & XCB_RANDR_MODE_FLAG_DOUBLE_SCAN)
            vtotal *= 2;
        if (modes[i].mode_flags & XCB_RANDR_MODE_FLAG_INTERLACE)
            vtotal /= 2;
        if (modes[i].htotal > 0 && vtotal > 0)
            hz = (double)modes[i].dot_clock / ((double)modes[i].htotal * vtotal);
        break;
    }

    free(cinfo);
    free(res);
    if (hz < 1.0 || hz > 500.0)
        hz = 60.0;
    return hz;
}

/* The scale this output's [output:NAME] config section asks for, or 0
 * when it asks for none. The fork's per-output "DPI" property is not
 * read: it is information for clients, and the scale is the transform
 * someone set (kiconf, xrandr --scale) -- see TESTS/DPI-PER-OUTPUT.md.
 * Clamped to what sharp scaling means here: a scale below 1 would be
 * supersampling, which the server's own RandR scaling already does well. */
static float wanted_scale(const char *name)
{
    float scale = config_output_scale(name);   /* < 0: not configured */

    if (scale < 0.0f)
        return 0.0f;
    if (scale < 1.0f)
        scale = 1.0f;
    if (scale > 4.0f)
        scale = 4.0f;
    return scale;
}

typedef struct {
    xcb_randr_crtc_t crtc;
    xcb_randr_mode_t mode;
    uint16_t rotation;
    int noutputs;
    xcb_randr_output_t outputs[8];
    int x, y;              /* current position */
    int old_w, old_h;      /* current footprint (transformed size) */
    int new_w, new_h;      /* footprint at the wanted scale */
    int new_x, new_y;
    float scale;           /* wanted (or current, when nothing asks) */
    bool rescale;
} ScaleCrtc;

/* A crtc's current scale, from its transform: crtc -> framebuffer, so the
 * matrix's diagonal is logical/physical. 1 when there is none. */
static float crtc_current_scale(xcb_randr_crtc_t crtc)
{
    xcb_randr_get_crtc_transform_reply_t *t =
        xcb_randr_get_crtc_transform_reply(comp.conn,
            xcb_randr_get_crtc_transform(comp.conn, crtc), NULL);
    float scale = 1.0f;

    if (t) {
        int32_t m11 = t->current_transform.matrix11;
        if (m11 > 0)
            scale = 65536.0f / (float)m11;
        free(t);
    }
    return scale;
}

/* Queues the transform (it takes effect with the CRTC's next config).
 * False when the server refuses it -- one without transform support, like
 * Xvfb -- and then nothing about the layout may change for its sake. */
static bool set_crtc_scale(xcb_randr_crtc_t crtc, float scale)
{
    xcb_render_transform_t t = { 0 };
    int32_t d = (int32_t)(65536.0f / scale + 0.5f);
    const char *filter = scale > 1.001f ? "bilinear" : "nearest";

    t.matrix11 = d;
    t.matrix22 = d;
    t.matrix33 = 65536;
    xcb_generic_error_t *err = xcb_request_check(comp.conn,
        xcb_randr_set_crtc_transform_checked(comp.conn, crtc, t,
                                             (uint16_t)strlen(filter), filter,
                                             0, NULL));
    if (err) {
        free(err);
        return false;
    }
    return true;
}

static void set_screen_size(int w, int h)
{
    /* millimetres for 96 DPI, which is what the server would make up */
    xcb_randr_set_screen_size(comp.conn, comp.root, (uint16_t)w, (uint16_t)h,
                              (uint32_t)(w * 25.4 / 96.0 + 0.5),
                              (uint32_t)(h * 25.4 / 96.0 + 0.5));
}

bool outputs_apply_scale_policy(void)
{
    if (!comp.caps.randr || !comp.apply_scale || comp.single_drawable)
        return false;

    xcb_randr_get_screen_resources_current_reply_t *res =
        xcb_randr_get_screen_resources_current_reply(comp.conn,
            xcb_randr_get_screen_resources_current(comp.conn, comp.root), NULL);
    if (!res)
        return false;

    ScaleCrtc crtcs[MAX_OUTPUTS];
    int n = 0;
    bool any = false;

    xcb_randr_crtc_t *ids = xcb_randr_get_screen_resources_current_crtcs(res);
    int nids = xcb_randr_get_screen_resources_current_crtcs_length(res);
    xcb_randr_mode_info_t *modes = xcb_randr_get_screen_resources_current_modes(res);
    int nmodes = xcb_randr_get_screen_resources_current_modes_length(res);

    for (int i = 0; i < nids && n < MAX_OUTPUTS; i++) {
        xcb_randr_get_crtc_info_reply_t *ci = xcb_randr_get_crtc_info_reply(comp.conn,
            xcb_randr_get_crtc_info(comp.conn, ids[i], res->config_timestamp), NULL);
        if (!ci)
            continue;
        int nout = xcb_randr_get_crtc_info_outputs_length(ci);
        if (ci->mode == XCB_NONE || nout == 0) {
            free(ci);
            continue;
        }

        ScaleCrtc *c = &crtcs[n];
        memset(c, 0, sizeof(*c));
        c->crtc = ids[i];
        c->mode = ci->mode;
        c->rotation = ci->rotation;
        c->noutputs = nout < 8 ? nout : 8;
        memcpy(c->outputs, xcb_randr_get_crtc_info_outputs(ci),
               sizeof(xcb_randr_output_t) * (size_t)c->noutputs);
        c->x = ci->x;
        c->y = ci->y;
        c->old_w = c->new_w = ci->width;   /* the transformed footprint */
        c->old_h = c->new_h = ci->height;
        free(ci);

        int mw = 0, mh = 0;
        for (int k = 0; k < nmodes; k++)
            if (modes[k].id == c->mode) {
                mw = modes[k].width;
                mh = modes[k].height;
            }

        /* Only plain landscape outputs: a rotation already has a
         * transform of its own, and scaling on top of it is xrandr's
         * business, not a policy's. */
        if (mw > 0 && mh > 0 && c->rotation == XCB_RANDR_ROTATION_ROTATE_0) {
            xcb_randr_get_output_info_reply_t *oi = xcb_randr_get_output_info_reply(comp.conn,
                xcb_randr_get_output_info(comp.conn, c->outputs[0],
                                          res->config_timestamp), NULL);
            char name[32] = "";
            if (oi) {
                int len = xcb_randr_get_output_info_name_length(oi);
                if (len > (int)sizeof(name) - 1)
                    len = (int)sizeof(name) - 1;
                memcpy(name, xcb_randr_get_output_info_name(oi), (size_t)len);
                name[len] = '\0';
                free(oi);
            }

            float cur = crtc_current_scale(c->crtc);
            float want = wanted_scale(name);
            c->scale = want > 0.0f ? want : cur;
            if (want > 0.0f && (want - cur > 0.01f || cur - want > 0.01f)) {
                c->rescale = true;
                c->new_w = (int)((float)mw / want + 0.5f);
                c->new_h = (int)((float)mh / want + 0.5f);
                any = true;
                comp_info("%s: scale %.2fx -> %.2fx (logical %dx%d)", name,
                          cur, want, c->new_w, c->new_h);
            }
        }
        n++;
    }

    if (!any) {
        free(res);
        return false;
    }

    /* Queue the transforms before anything moves, so an output the server
     * can't scale keeps its size and its neighbours stay where they are. */
    xcb_grab_server(comp.conn);
    any = false;
    for (int i = 0; i < n; i++) {
        ScaleCrtc *c = &crtcs[i];
        if (!c->rescale)
            continue;
        if (set_crtc_scale(c->crtc, c->scale)) {
            any = true;
            continue;
        }
        fprintf(stderr, "kicomp: the server can't scale this output (no CRTC "
                "transform support); leaving the layout as it is\n");
        c->rescale = false;
        c->new_w = c->old_w;
        c->new_h = c->old_h;
    }
    if (!any) {
        xcb_ungrab_server(comp.conn);
        xcb_flush(comp.conn);
        free(res);
        return false;
    }

    /* Keep the arrangement: every output that started at or beyond the
     * far edge of another moves in by however much that one shrank (or
     * out by however much it grew). Rows and columns stay edge to edge,
     * which is all the policy promises. */
    int screen_w = 0, screen_h = 0;
    for (int i = 0; i < n; i++) {
        ScaleCrtc *p = &crtcs[i];
        p->new_x = p->x;
        p->new_y = p->y;
        for (int j = 0; j < n; j++) {
            const ScaleCrtc *o = &crtcs[j];
            if (j == i)
                continue;
            if (p->x >= o->x + o->old_w)
                p->new_x -= o->old_w - o->new_w;
            if (p->y >= o->y + o->old_h)
                p->new_y -= o->old_h - o->new_h;
        }
        if (p->new_x + p->new_w > screen_w)
            screen_w = p->new_x + p->new_w;
        if (p->new_y + p->new_h > screen_h)
            screen_h = p->new_y + p->new_h;
    }

    /* Grow first, so no CRTC is ever placed outside the screen, then set
     * every CRTC, then shrink to fit -- the order xrandr uses. Still under
     * the grab taken above, so nobody sees the layout half done. */
    int root_w = comp.root_w, root_h = comp.root_h;
    xcb_get_geometry_reply_t *geo =
        xcb_get_geometry_reply(comp.conn, xcb_get_geometry(comp.conn, comp.root), NULL);
    if (geo) {
        root_w = geo->width;
        root_h = geo->height;
        free(geo);
    }
    int grow_w = screen_w > root_w ? screen_w : root_w;
    int grow_h = screen_h > root_h ? screen_h : root_h;

    if (grow_w != root_w || grow_h != root_h)
        set_screen_size(grow_w, grow_h);

    for (int i = 0; i < n; i++) {
        ScaleCrtc *c = &crtcs[i];
        if (!c->rescale && c->new_x == c->x && c->new_y == c->y)
            continue;
        xcb_randr_set_crtc_config_reply_t *r = xcb_randr_set_crtc_config_reply(comp.conn,
            xcb_randr_set_crtc_config(comp.conn, c->crtc, XCB_CURRENT_TIME,
                                      res->config_timestamp,
                                      (int16_t)c->new_x, (int16_t)c->new_y,
                                      c->mode, c->rotation,
                                      (uint32_t)c->noutputs, c->outputs), NULL);
        if (!r || r->status != XCB_RANDR_SET_CONFIG_SUCCESS)
            fprintf(stderr, "kicomp: RandR refused to rescale/move a CRTC "
                    "(status %d)\n", r ? r->status : -1);
        free(r);
    }

    if (screen_w != grow_w || screen_h != grow_h)
        set_screen_size(screen_w, screen_h);
    xcb_ungrab_server(comp.conn);
    xcb_flush(comp.conn);

    free(res);
    return true;
}

void outputs_teardown(void)
{
    for (int i = 0; i < comp.output_count; i++) {
        xscale_detach(&comp.outputs[i]);
        if (presenter && presenter->destroy)
            presenter->destroy(&comp.outputs[i]);
        if (renderer)
            renderer->destroy(&comp.outputs[i]);
    }
    comp.output_count = 0;
}

void outputs_refresh(void)
{
    xcb_get_geometry_reply_t *root_geo =
        xcb_get_geometry_reply(comp.conn, xcb_get_geometry(comp.conn, comp.root), NULL);
    if (root_geo) {
        comp.root_w = root_geo->width;
        comp.root_h = root_geo->height;
        free(root_geo);
    } else if (comp.root_w == 0 || comp.root_h == 0) {
        comp.root_w = comp.screen->width_in_pixels;
        comp.root_h = comp.screen->height_in_pixels;
    }

    /* Targets are sized to their output, so any layout change means every
     * target is potentially the wrong size: tear all of them down and
     * rebuild. Output changes are rare (hotplug, mode set); optimizing
     * this would only add ways to keep a stale drawable around. */
    outputs_teardown();

    CompOutput fresh[MAX_OUTPUTS];
    int n = 0;

    if (comp.caps.randr && !comp.single_drawable) {
        xcb_randr_get_monitors_reply_t *r =
            xcb_randr_get_monitors_reply(comp.conn,
                xcb_randr_get_monitors(comp.conn, comp.root, 1), NULL);
        if (r) {
            xcb_randr_monitor_info_iterator_t it =
                xcb_randr_get_monitors_monitors_iterator(r);

            for (; it.rem && n < MAX_OUTPUTS; xcb_randr_monitor_info_next(&it)) {
                xcb_randr_monitor_info_t *m = it.data;
                CompOutput *o = &fresh[n];
                memset(o, 0, sizeof(*o));

                o->id = n;
                get_atom_name_into(m->name, o->name, sizeof(o->name));
                if (!o->name[0])
                    snprintf(o->name, sizeof(o->name), "output-%d", n);

                o->physical.x = m->x;
                o->physical.y = m->y;
                o->physical.w = m->width;
                o->physical.h = m->height;
                o->refresh_hz = 60.0;

                xcb_randr_output_t backing_output = XCB_NONE;
                int noutputs = xcb_randr_monitor_info_outputs_length(m);
                if (noutputs > 0) {
                    xcb_randr_output_t *backing = xcb_randr_monitor_info_outputs(m);
                    backing_output = backing[0];
                    o->refresh_hz = compute_output_refresh_hz(backing[0], &o->crtc);
                }

                /* The monitor's box is the CRTC's transformed footprint:
                 * the logical box, whatever scaling is in place. */
                o->rect = o->physical;
                o->scale = 1.0f;
                (void)backing_output;

                /* Drawn at physical size only when the server can take
                 * the picture (xscale.h); a scaled output it can't is
                 * drawn at logical size for the server to magnify. */
                int sw = 0, sh = 0;
                bool presents = renderer && renderer->scanout_present &&
                                comp.caps.xscale_present &&
                                presenter == presenter_present();
                if (renderer && (renderer->scanout_source || presents) &&
                    xscale_crtc_capable(o->crtc, &sw, &sh) &&
                    sw > o->rect.w && sh > o->rect.h) {
                    float sx = (float)sw / (float)o->rect.w;
                    float sy = (float)sh / (float)o->rect.h;
                    /* one factor for both axes is all the renderers do */
                    if (sx - sy < 0.02f && sy - sx < 0.02f) {
                        o->sourced = true;
                        o->source_presented = presents;
                        o->scale = sx;
                        o->physical.w = sw;
                        o->physical.h = sh;
                    }
                }

                n++;
            }
            free(r);
        }
    }

    /* No RandR, a server that reports no monitors, or --single-drawable:
     * the root window is the only output there is. Section 45 -- nothing
     * here may require an extension to be present -- and, for the legacy
     * mode, the one supported way to opt out of per-output presentation:
     * everything downstream (scene, renderer, presenter, dirty state) is
     * unchanged, it just has a single, screen-sized output to work on. */
    if (n == 0) {
        CompOutput *o = &fresh[0];
        memset(o, 0, sizeof(*o));
        o->id = 0;
        snprintf(o->name, sizeof(o->name), comp.single_drawable ? "screen" : "root");
        o->physical.x = 0;
        o->physical.y = 0;
        o->physical.w = comp.root_w;
        o->physical.h = comp.root_h;
        o->refresh_hz = 60.0;
        o->scale = 1.0f;
        o->rect = o->physical;
        n = 1;
    }

    memcpy(comp.outputs, fresh, sizeof(CompOutput) * (size_t)n);
    comp.output_count = n;

    int drawables = 0;
    for (int i = 0; i < comp.output_count; i++) {
        CompOutput *o = &comp.outputs[i];
        if (!renderer->init(o)) {
            fprintf(stderr, "kicomp: no render target for output %s\n", o->name);
            continue;
        }

        /* A sourced output that can't get its source ready is drawn at
         * logical size like any other, and left to the server. */
        if (o->sourced && !xscale_attach(o)) {
            fprintf(stderr, "kicomp: %s: no scanout source, drawing it at "
                    "logical size\n", o->name);
            xscale_detach(o);
            renderer->destroy(o);
            o->sourced = false;
            o->source_presented = false;
            o->scale = 1.0f;
            o->physical = o->rect;
            if (!renderer->init(o)) {
                fprintf(stderr, "kicomp: no render target for output %s\n", o->name);
                continue;
            }
        }

        /* The XRender path's sourced output is presented into its source
         * pixmap (xscale_present), never onto the overlay: a presenter
         * window for it would cover the next output. */
        bool own_present = o->sourced && o->source_picture;
        if (!own_present && presenter && presenter->init && !presenter->init(o))
            fprintf(stderr, "kicomp: no presenter for output %s\n", o->name);
        o->dirty = true;
        drawables++;
    }

    /* Every window may now be on a differently scaled output. */
    density_update_all();

    comp_info("%d drawable%s (%s)", drawables, drawables == 1 ? "" : "s",
              comp.single_drawable ? "legacy single-screen mode"
                                   : "one per output");
    for (int i = 0; i < comp.output_count; i++) {
        CompOutput *o = &comp.outputs[i];
        if (o->sourced)
            comp_info("  [%d] %-12s %dx%d+%d+%d @ %.2f Hz  scale %.2fx, "
                      "drawn at %dx%d (scanout source)%s",
                      o->id, o->name, o->rect.w, o->rect.h, o->rect.x, o->rect.y,
                      o->refresh_hz, o->scale, o->physical.w, o->physical.h,
                      o->render_data ? "" : "  (no target!)");
        else
            comp_info("  [%d] %-12s %dx%d+%d+%d @ %.2f Hz%s",
                      o->id, o->name, o->rect.w, o->rect.h, o->rect.x, o->rect.y,
                      o->refresh_hz, o->render_data ? "" : "  (no target!)");
    }

    /* Open capture sessions describe outputs by index into this list. */
    capture_outputs_changed();
}

static void damage_rect_grown(const CompRect *r, int margin)
{
    CompRect grown = *r;
    if (margin > 0) {
        grown.x -= margin;
        grown.y -= margin;
        grown.w += margin * 2;
        grown.h += margin * 2;
    }

    for (int i = 0; i < comp.output_count; i++) {
        CompOutput *o = &comp.outputs[i];

        /* Clipped to the output: a region in root coordinates that ran
         * past the output's edge would make every renderer clamp it
         * again, and the part outside is not this output's to paint. */
        CompRect hit;
        if (!rect_intersect(&grown, &o->rect, &hit))
            continue;

        o->dirty = true;
        o->damage_serial++;
        region_add(&o->damage, &hit);
        capture_damage(o, &hit);
    }
}

void output_damage_rect(const CompRect *r)
{
    /* A window's shadow is painted *outside* the window, so the area a
     * window's change dirties is bigger than the window: damaging its
     * rectangle alone would leave a band of stale shadow behind it every
     * time it moves. Grown here, once, rather than at each of the thirty
     * call sites -- none of which has any business knowing shadows
     * exist.
     *
     * The worst case over both styles, because this entry point has no
     * particular window to ask: an output-wide rectangle, or one that is
     * already the whole of an effect item, is not going to be pushed
     * outside its output by a shadow's reach the way a single window's
     * exact edge can be -- see output_damage_window_rect() for that
     * case. */
    damage_rect_grown(r, shadow_margin());
}

void output_damage_window_rect(const CompWindow *w, const CompRect *r)
{
    damage_rect_grown(r, shadow_margin_for_window(w));
}

void output_damage_all(void)
{
    for (int i = 0; i < comp.output_count; i++) {
        comp.outputs[i].dirty = true;
        comp.outputs[i].damage_serial++;
        region_set_full(&comp.outputs[i].damage);
    }
    capture_damage_all();
}

void output_request_frame(CompOutput *o)
{
    if (o->dirty)
        return;
    /* One pixel, not nothing: a dirty output with no region is repainted
     * whole (output_paint_region), and the point is the opposite. */
    CompRect px = { o->rect.x, o->rect.y, 1, 1 };
    o->dirty = true;
    region_add(&o->damage, &px);
}

void output_paint_region(CompOutput *o, CompRegion *out)
{
    *out = o->damage;

    /* Dirty with nothing said about where: repaint the whole output. Any
     * path that marks an output dirty without posting a rectangle --
     * today the frame clock, tomorrow whatever else -- gets the old
     * behaviour rather than a frame that quietly paints nothing. */
    if (region_is_empty(out))
        region_set_full(out);
}

void output_painted(CompOutput *o)
{
    o->dirty = false;
    region_clear(&o->damage);

    /* One frame, counted where it happened. The window is a plain second
     * rather than a rolling average: the number is meant to be read by a
     * person watching it, and an average that never settles is harder to
     * read than one that steps once a second. */
    double now = comp_now_ms();
    if (o->fps_since <= 0.0)
        o->fps_since = now;

    o->frames_counted++;
    if (now - o->fps_since >= 1000.0) {
        o->fps = (int)((double)o->frames_counted * 1000.0 /
                       (now - o->fps_since) + 0.5);
        o->frames_counted = 0;
        o->fps_since = now;
    }
}
