/* RandR outputs, each with its own render target and its own dirty state.
 * See kiwm-kicomp-projeto.md sections 4, 18 and 39. */
#include "output.h"
#include "animation.h"
#include "region.h"
#include "renderer.h"
#include "presenter.h"
#include "shadow.h"
#include "config.h"
#include "inputscale.h"
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

/* The XLibre fork publishes a per-output RandR property literally called
 * "DPI", where 96 means 1x -- see TESTS/DPI-PER-OUTPUT.md. It is a
 * convention of this environment rather than part of any standard, so it
 * is asked for with only_if_exists: a server that doesn't have it simply
 * answers nothing and every output stays at scale 1. */
static float read_output_dpi_scale(xcb_randr_output_t output_id)
{
    if (output_id == XCB_NONE)
        return 0.0f;

    xcb_intern_atom_reply_t *a = xcb_intern_atom_reply(comp.conn,
        xcb_intern_atom(comp.conn, 1, 3, "DPI"), NULL);
    if (!a)
        return 0.0f;

    xcb_atom_t dpi_atom = a->atom;
    free(a);
    if (dpi_atom == XCB_NONE)
        return 0.0f;

    xcb_randr_get_output_property_reply_t *r =
        xcb_randr_get_output_property_reply(comp.conn,
            xcb_randr_get_output_property(comp.conn, output_id, dpi_atom,
                                          XCB_ATOM_ANY, 0, 1, 0, 0), NULL);
    if (!r)
        return 0.0f;

    float scale = 0.0f;
    if (r->format == 32 && xcb_randr_get_output_property_data_length(r) >= 4) {
        int32_t dpi = *(int32_t *)xcb_randr_get_output_property_data(r);
        if (dpi > 0)
            scale = (float)dpi / 96.0f;
    }
    free(r);
    return scale;
}

/* The scale this output ends up at: what the config says, or what the
 * server's DPI property implies, or 1. Clamped to something a compositor
 * can honestly draw -- a scale below 1 would mean *growing* the desktop
 * past the panel, which is RandR's own --scale to do, not this. */
static float resolve_output_scale(const char *name, xcb_randr_output_t output_id)
{
    /* No X-INPUT-SCALE, no scaling -- whatever the config or the DPI
     * property say. A scaled output whose pointer isn't confined has a
     * margin of scanout the cursor can enter and nothing draws into, which
     * is worse than not scaling at all. The capability decides (section
     * 17/30); see inputscale.h. */
    if (!comp.caps.input_scale)
        return 1.0f;

    float scale = config_output_scale(name);   /* < 0: not configured */

    if (scale < 0.0f)
        scale = read_output_dpi_scale(output_id);

    if (scale < 1.0f)
        scale = 1.0f;
    if (scale > 4.0f)
        scale = 4.0f;
    return scale;
}

/* The logical box: the top-left part of the scanout the compositor
 * actually draws a desktop into. Shrink-only, which is the same rule
 * X-INPUT-SCALE enforces on the confinement that mirrors it. */
static CompRect logical_box(const CompRect *physical, float scale)
{
    if (scale <= 1.0f)
        return *physical;

    CompRect r = *physical;
    r.w = (int)((float)physical->w / scale + 0.5f);
    r.h = (int)((float)physical->h / scale + 0.5f);
    if (r.w < 1) r.w = 1;
    if (r.h < 1) r.h = 1;
    return r;
}

void outputs_teardown(void)
{
    for (int i = 0; i < comp.output_count; i++) {
        if (presenter && presenter->destroy)
            presenter->destroy(&comp.outputs[i]);
        if (renderer)
            renderer->destroy(&comp.outputs[i]);
    }
    comp.output_count = 0;
}

void outputs_refresh(void)
{
    /* Before anything is read: while a CRTC is confined, the server
     * reports the *confined* box as that output's geometry -- on purpose,
     * so the WM and every toolkit lay out inside the logical desktop
     * without knowing X-INPUT-SCALE exists (see inputscale.h). Reading
     * RandR with our own confinement still in force would therefore
     * mistake the logical box for the physical one and scale it down
     * again. */
    inputscale_release_all();

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

                o->scale = resolve_output_scale(o->name, backing_output);
                o->rect = logical_box(&o->physical, o->scale);

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
        o->scale = resolve_output_scale(o->name, XCB_NONE);
        o->rect = logical_box(&o->physical, o->scale);
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
        if (presenter && presenter->init && !presenter->init(o))
            fprintf(stderr, "kicomp: no presenter for output %s\n", o->name);
        o->dirty = true;
        drawables++;
    }

    /* Said out loud, not behind -v: how many drawables there are and why
     * is the single most useful thing to know about a running compositor
     * -- it's the difference between the per-output pipeline and the
     * legacy one, and it's what changes on every hotplug. */
    inputscale_apply();

    /* Every window may now be on a differently scaled output. */
    density_update_all();

    comp_info("%d drawable%s (%s)", drawables, drawables == 1 ? "" : "s",
              comp.single_drawable ? "legacy single-screen mode"
                                   : "one per output");
    for (int i = 0; i < comp.output_count; i++) {
        CompOutput *o = &comp.outputs[i];
        if (o->scale > 1.0f)
            comp_info("  [%d] %-12s %dx%d+%d+%d @ %.2f Hz  scale %.2fx "
                      "(logical %dx%d)%s",
                      o->id, o->name, o->physical.w, o->physical.h,
                      o->physical.x, o->physical.y, o->refresh_hz, o->scale,
                      o->rect.w, o->rect.h,
                      o->render_data ? "" : "  (no target!)");
        else
            comp_info("  [%d] %-12s %dx%d+%d+%d @ %.2f Hz%s",
                      o->id, o->name, o->rect.w, o->rect.h, o->rect.x, o->rect.y,
                      o->refresh_hz, o->render_data ? "" : "  (no target!)");
    }
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
        region_add(&o->damage, &hit);
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
        region_set_full(&comp.outputs[i].damage);
    }
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
