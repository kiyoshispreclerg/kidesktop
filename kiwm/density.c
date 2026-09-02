/*
 * X-DENSITY, client side, for kiwm's own decorations.
 *
 * A compositor doing per-monitor HiDPI scaling draws a logical desktop
 * magnified into a monitor's real pixels. Everything drawn at logical size
 * comes out soft, decoration included -- and the decoration's pixels are
 * *kiwm's*: the compositor can only magnify what is in the frame, and what
 * is in the frame is a titlebar drawn at, say, 26 px.
 *
 * So kiwm answers the same protocol its clients do (TESTS/X-DENSITY.md in
 * the xnsguard tree, and kicomp's density.c on the other side): the
 * compositor writes `_X_DENSITY_REQUESTED` on the *frame*, kiwm redraws
 * the decoration at that density into a pixmap of its own, and publishes
 * `_X_DENSITY_SCALE` and `_X_DENSITY_PIXMAP` for the compositor to sample
 * instead of the magnified frame.
 *
 * Three things make it fit rather than bolt on:
 *
 *   - It is the same painting. paint_deco() is handed a Cairo context with
 *     cairo_scale() applied, so text is re-shaped by Pango at the larger
 *     size and the shapes are re-rasterized -- not a magnified bitmap.
 *   - The pixmap is always ARGB, whatever the frame's depth, and cleared
 *     to transparent. kiwm paints the titlebar strip and the borders and
 *     nothing else, so the client's area stays a hole: the compositor
 *     composites this *over* the window it already drew, and the client's
 *     own contents (dense or not) show through untouched.
 *   - Nothing happens unless a compositor asks. No request, no pixmap, no
 *     properties, no cost -- and uncomposited kiwm never allocates any of
 *     it (project doc section 31: the WM must not depend on a compositor
 *     being there).
 */
#include "wm.h"
#include "atoms.h"
#include "client.h"
#include "decoration.h"
#include "density.h"

#include <cairo/cairo-xcb.h>
#include <stdio.h>
#include <stdlib.h>

/* A density is published as the fraction the protocol carries; kiwm only
 * ever echoes back what it was asked for, since it can honour any factor
 * exactly (it re-renders rather than resamples). */
static void publish_scale(Client *c)
{
    uint32_t value[2] = { c->deco_density_num, c->deco_density_den };
    xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE, c->frame,
                        wm.atoms.x_density_scale, XCB_ATOM_CARDINAL, 32,
                        2, value);
}

static void publish_pixmap(Client *c)
{
    uint32_t value = c->deco_density_pixmap;
    xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE, c->frame,
                        wm.atoms.x_density_pixmap, XCB_ATOM_CARDINAL, 32,
                        1, &value);
}

static void free_pixmap(Client *c)
{
    if (c->deco_density_pixmap != XCB_NONE) {
        xcb_free_pixmap(wm.conn, c->deco_density_pixmap);
        c->deco_density_pixmap = XCB_NONE;
    }
    c->deco_density_w = c->deco_density_h = 0;
}

void deco_density_forget(Client *c)
{
    if (c->deco_density_num == 1 && c->deco_density_den == 1 &&
        c->deco_density_pixmap == XCB_NONE)
        return;

    free_pixmap(c);
    c->deco_density_num = c->deco_density_den = 1;

    /* The frame may be about to be destroyed, in which case its
     * properties go with it -- but when the request was merely withdrawn,
     * saying so is what stops a compositor from sampling a pixmap that no
     * longer exists. */
    xcb_delete_property(wm.conn, c->frame, wm.atoms.x_density_pixmap);
    xcb_delete_property(wm.conn, c->frame, wm.atoms.x_density_scale);
}

void deco_density_request_changed(Client *c)
{
    uint32_t num = 1, den = 1;

    xcb_get_property_reply_t *r = xcb_get_property_reply(wm.conn,
        xcb_get_property(wm.conn, 0, c->frame, wm.atoms.x_density_requested,
                         XCB_ATOM_CARDINAL, 0, 2), NULL);
    if (r) {
        if (r->type == XCB_ATOM_CARDINAL && r->format == 32 &&
            xcb_get_property_value_length(r) >= 8) {
            uint32_t *v = xcb_get_property_value(r);
            if (v[1] != 0) {
                num = v[0];
                den = v[1];
            }
        }
        free(r);
    }

    if (num == c->deco_density_num && den == c->deco_density_den)
        return;

    if (num == den) {
        deco_density_forget(c);
        fprintf(stderr, "kiwm: decoration density back to 1/1 for 0x%x\n", c->frame);
        return;
    }

    c->deco_density_num = num;
    c->deco_density_den = den;
    fprintf(stderr, "kiwm: decoration density %u/%u for 0x%x\n", num, den, c->frame);

    /* Redraw immediately: the request is only useful once there is
     * something at that density to sample. */
    draw_decoration(c);
    xcb_flush(wm.conn);
}

void deco_density_publish(Client *c, int w, int h, bool focused)
{
    if (c->deco_density_num == c->deco_density_den)
        return;
    if (!wm.argb_visual) {
        /* No 32-bit visual to make a transparent pixmap with; publishing
         * an opaque one would paint over the client's own area. */
        return;
    }

    double density = (double)c->deco_density_num / (double)c->deco_density_den;
    int dw = (int)(w * density + 0.5);
    int dh = (int)(h * density + 0.5);
    if (dw <= 0 || dh <= 0)
        return;

    bool resized = (dw != c->deco_density_w || dh != c->deco_density_h);
    if (resized)
        free_pixmap(c);

    if (c->deco_density_pixmap == XCB_NONE) {
        c->deco_density_pixmap = xcb_generate_id(wm.conn);
        xcb_create_pixmap(wm.conn, 32, c->deco_density_pixmap, c->frame,
                          (uint16_t)dw, (uint16_t)dh);
        c->deco_density_w = dw;
        c->deco_density_h = dh;
    }

    cairo_surface_t *surface = cairo_xcb_surface_create(wm.conn,
        c->deco_density_pixmap, wm.argb_visual, dw, dh);
    cairo_t *cr = cairo_create(surface);

    /* The scale is the whole trick: everything below draws in the frame's
     * own coordinates and lands on `density` times as many pixels. */
    cairo_scale(cr, density, density);
    paint_deco(c, cr, w, h, focused, true);

    cairo_destroy(cr);
    cairo_surface_flush(surface);
    cairo_surface_destroy(surface);

    /* Contents first, announcement after (the protocol's own ordering
     * rule): a compositor that sampled on the property change would
     * otherwise catch a pixmap that is the wrong size, or still empty.
     *
     * And the pixmap property is rewritten with the same XID every time,
     * not only when it changed: a Pixmap raises no Damage of its own, so
     * this PropertyNotify *is* the "there is a new frame here" signal. */
    if (resized)
        publish_scale(c);
    publish_pixmap(c);
}
