/* RandR outputs, each with its own render target and its own dirty state.
 * See kiwm-kicomp-projeto.md sections 4, 18 and 39. */
#include "output.h"
#include "renderer.h"
#include "presenter.h"

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
 * per-output pacing (Fase 7). */
static double compute_output_refresh_hz(xcb_randr_output_t output_id)
{
    double hz = 60.0;

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

                o->rect.x = m->x;
                o->rect.y = m->y;
                o->rect.w = m->width;
                o->rect.h = m->height;
                o->refresh_hz = 60.0;

                int noutputs = xcb_randr_monitor_info_outputs_length(m);
                if (noutputs > 0) {
                    xcb_randr_output_t *backing = xcb_randr_monitor_info_outputs(m);
                    o->refresh_hz = compute_output_refresh_hz(backing[0]);
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
        o->rect.x = 0;
        o->rect.y = 0;
        o->rect.w = comp.root_w;
        o->rect.h = comp.root_h;
        o->refresh_hz = 60.0;
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
    comp_info("%d drawable%s (%s)", drawables, drawables == 1 ? "" : "s",
              comp.single_drawable ? "legacy single-screen mode"
                                   : "one per output");
    for (int i = 0; i < comp.output_count; i++) {
        CompOutput *o = &comp.outputs[i];
        comp_info("  [%d] %-12s %dx%d+%d+%d @ %.2f Hz%s",
                  o->id, o->name, o->rect.w, o->rect.h, o->rect.x, o->rect.y,
                  o->refresh_hz, o->target ? "" : "  (no target!)");
    }
}

void output_damage_rect(const CompRect *r)
{
    CompRect ignored;
    for (int i = 0; i < comp.output_count; i++) {
        CompOutput *o = &comp.outputs[i];
        if (rect_intersect(r, &o->rect, &ignored))
            o->dirty = true;
    }
}

void output_damage_all(void)
{
    for (int i = 0; i < comp.output_count; i++)
        comp.outputs[i].dirty = true;
}
