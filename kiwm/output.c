/* RandR outputs and the virtual desktop each one tracks independently
 * (kiwm-kicomp-projeto.md section 6). See wm.h's KiWM::outputs. */
#include "output.h"
#include "wm.h"
#include "client.h"
#include "ewmh.h"

#include <xcb/randr.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void ewmh_set_workarea(void);

int primary_output_index(void)
{
    for (int i = 0; i < wm.output_count; i++)
        if (wm.outputs[i].primary)
            return i;
    return wm.output_count > 0 ? 0 : -1;
}

int output_index_for_point(int x, int y)
{
    for (int i = 0; i < wm.output_count; i++) {
        XisOutput *o = &wm.outputs[i];
        if (x >= o->x && x < o->x + o->width &&
            y >= o->y && y < o->y + o->height)
            return i;
    }
    return primary_output_index();
}

int output_for_pointer(void)
{
    xcb_query_pointer_reply_t *r =
        xcb_query_pointer_reply(wm.conn, xcb_query_pointer(wm.conn, wm.root), NULL);
    if (!r)
        return primary_output_index();
    int idx = output_index_for_point(r->root_x, r->root_y);
    free(r);
    return idx;
}


/* Which output a screen-wide *effect* acts on: the window switcher and
 * desktop switcher overlays (osd.c), a direct desktop jump
 * (keybind.c's KB_DESKTOP_GOTO), and later the same two effects under
 * kicomp. wm.osd_output_follows_pointer=0 (default) keeps kiwm's original
 * behavior -- the currently focused window's output, falling back to the
 * pointer's output only when nothing is focused at all; =1 always uses the
 * pointer's output regardless of what's focused. Deliberately *not* the
 * same thing as focus_follows_mouse= (see wm.h): this only decides which
 * screen these effects act on, never what receives keyboard input.
 *
 * Polled fresh on each call, so a hold-style effect (osd.c) must call it
 * once when the hold starts and keep that answer for the whole hold rather
 * than re-asking mid-hold and migrating between screens. */
int output_for_effects(void)
{
    if (wm.osd_output_follows_pointer)
        return output_for_pointer();
    return wm.focused ? wm.focused->output : output_for_pointer();
}

static void get_atom_name_into(xcb_atom_t atom, char *out, size_t out_sz)
{
    out[0] = '\0';
    xcb_get_atom_name_reply_t *r =
        xcb_get_atom_name_reply(wm.conn, xcb_get_atom_name(wm.conn, atom), NULL);
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

static void ewmh_update_output_props(void)
{
    char buf[MAX_OUTPUTS * 64];
    size_t off = 0;

    for (int i = 0; i < wm.output_count; i++) {
        size_t len = strlen(wm.outputs[i].name);
        if (off + len + 1 > sizeof(buf))
            break;
        memcpy(buf + off, wm.outputs[i].name, len);
        off += len;
        buf[off++] = '\0';
    }

    xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE, wm.root,
                        wm.atoms.kiwm_outputs, wm.atoms.utf8_string, 8,
                        (uint32_t)off, buf);

    uint32_t desktops[MAX_OUTPUTS];
    for (int i = 0; i < wm.output_count; i++)
        desktops[i] = (uint32_t)wm.outputs[i].desktop;

    xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE, wm.root,
                        wm.atoms.kiwm_output_desktop, XCB_ATOM_CARDINAL, 32,
                        (uint32_t)wm.output_count, desktops);

    uint32_t numws = (uint32_t)wm.num_desktops;
    xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE, wm.root,
                        wm.atoms.kiwm_num_output_desktops, XCB_ATOM_CARDINAL, 32,
                        1, &numws);
}

void ewmh_set_current_desktop(int desktop)
{
    uint32_t v = (uint32_t)desktop;
    xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE, wm.root,
                        wm.atoms.net_current_desktop, XCB_ATOM_CARDINAL, 32, 1, &v);
}

/* ------------------------------------------------------------------ */
/* dock/panel struts (_NET_WM_STRUT / _NET_WM_STRUT_PARTIAL)           */
/* ------------------------------------------------------------------ */

/* Which output a dock's own on-screen rectangle sits on -- dock/panel
 * windows are never reparented by kiwm (see client.c's manage()), so a
 * plain xcb_get_geometry() on one gives root-relative coordinates
 * directly, no translate_coordinates needed. */
static void assign_dock_output(DockWindow *d)
{
    xcb_get_geometry_reply_t *geo =
        xcb_get_geometry_reply(wm.conn, xcb_get_geometry(wm.conn, d->window), NULL);
    if (!geo) {
        d->output = -1;
        d->x = d->y = d->width = d->height = 0;
        return;
    }
    d->output = output_index_for_point(geo->x + geo->width / 2, geo->y + geo->height / 2);
    d->x = geo->x;
    d->y = geo->y;
    d->width = geo->width;
    d->height = geo->height;
    free(geo);
}

/* Maxes the struts of just the docks attributed to a single output (see
 * DockWindow::output) -- _NET_WM_STRUT(_PARTIAL)'s l/r/t/b values are
 * screen-relative by spec with no notion of "which monitor", so applying
 * one dock's strut to every output that happens to share its absolute
 * row/column range (a real risk whenever two outputs start at the same x
 * or y) would wrongly eat into a workarea the dock isn't even on. */
static void struts_for_output(int output_idx, int *l, int *r, int *t, int *b)
{
    *l = *r = *t = *b = 0;
    for (int i = 0; i < wm.dock_count; i++) {
        DockWindow *d = &wm.docks[i];
        if (d->output != output_idx)
            continue;
        if (d->left > *l) *l = d->left;
        if (d->right > *r) *r = d->right;
        if (d->top > *t) *t = d->top;
        if (d->bottom > *b) *b = d->bottom;
    }
}

/* Reads the first 4 CARDINALs (left, right, top, bottom) out of whichever
 * of _NET_WM_STRUT_PARTIAL (preferred) or the older _NET_WM_STRUT is set.
 * _PARTIAL's remaining 8 start/end-range values are ignored -- kiwm treats
 * a strut as reserving its full screen edge, not a sub-range of it (see
 * kiwm-kicomp-projeto.md's "don't chase full conformance" guidance). */
static bool read_strut(xcb_window_t window, int *l, int *r, int *t, int *b)
{
    xcb_get_property_reply_t *reply = xcb_get_property_reply(wm.conn,
        xcb_get_property(wm.conn, 0, window, wm.atoms.net_wm_strut_partial,
                         XCB_ATOM_CARDINAL, 0, 12), NULL);
    if (!reply || xcb_get_property_value_length(reply) < 16) {
        free(reply);
        reply = xcb_get_property_reply(wm.conn,
            xcb_get_property(wm.conn, 0, window, wm.atoms.net_wm_strut,
                             XCB_ATOM_CARDINAL, 0, 4), NULL);
    }
    if (!reply || xcb_get_property_value_length(reply) < 16) {
        free(reply);
        return false;
    }

    uint32_t *v = xcb_get_property_value(reply);
    *l = (int)v[0];
    *r = (int)v[1];
    *t = (int)v[2];
    *b = (int)v[3];
    free(reply);
    return true;
}

static DockWindow *dock_find(xcb_window_t window)
{
    for (int i = 0; i < wm.dock_count; i++)
        if (wm.docks[i].window == window)
            return &wm.docks[i];
    return NULL;
}

bool dock_is_tracked(xcb_window_t window)
{
    return dock_find(window) != NULL;
}

void dock_track(xcb_window_t window)
{
    if (dock_find(window) || wm.dock_count >= MAX_DOCKS)
        return;
    DockWindow *d = &wm.docks[wm.dock_count++];
    d->window = window;
    d->left = d->right = d->top = d->bottom = 0;
    read_strut(window, &d->left, &d->right, &d->top, &d->bottom);
    assign_dock_output(d);
    ewmh_set_workarea();
}

bool dock_refresh_strut(xcb_window_t window)
{
    DockWindow *d = dock_find(window);
    if (!d)
        return false;
    d->left = d->right = d->top = d->bottom = 0;
    read_strut(window, &d->left, &d->right, &d->top, &d->bottom);
    ewmh_set_workarea();
    return true;
}

bool dock_forget(xcb_window_t window)
{
    for (int i = 0; i < wm.dock_count; i++) {
        if (wm.docks[i].window != window)
            continue;
        wm.docks[i] = wm.docks[--wm.dock_count];
        ewmh_set_workarea();
        return true;
    }
    return false;
}

/* Usable rect for output_idx after clipping out reserved dock/panel edges.
 * Struts are screen-absolute (not per-output), so an output only loses
 * space where its own rect actually overlaps the reserved margin -- an
 * output far from the panel's screen edge is unaffected. */
void compute_output_workarea(int output_idx, int *x, int *y, int *w, int *h)
{
    XisOutput *o = &wm.outputs[output_idx];
    int screen_w = wm.screen_w;
    int screen_h = wm.screen_h;

    int sl, sr, st, sb;
    struts_for_output(output_idx, &sl, &sr, &st, &sb);

    int left_edge = sl;
    int top_edge = st;
    int right_edge = screen_w - sr;
    int bottom_edge = screen_h - sb;

    int ux = o->x > left_edge ? o->x : left_edge;
    int uy = o->y > top_edge ? o->y : top_edge;
    int orx = o->x + o->width < right_edge ? o->x + o->width : right_edge;
    int ory = o->y + o->height < bottom_edge ? o->y + o->height : bottom_edge;

    *x = ux;
    *y = uy;
    *w = orx > ux ? orx - ux : 0;
    *h = ory > uy ? ory - uy : 0;
}

/* _NET_WORKAREA, plus _NET_DESKTOP_GEOMETRY/_NET_DESKTOP_VIEWPORT.
 *
 * EWMH has room for exactly one work-area rectangle per desktop covering
 * the whole (possibly multi-monitor) screen -- a limitation of the spec,
 * and one every multi-head WM has to pick a lie for. kiwm used to publish
 * the *primary output's* usable area (which is what KWin does too), and
 * that turns out to be an actively harmful lie: a client that dutifully
 * constrains one of its own popups to this rectangle drags every popup
 * belonging to a window on any *other* output onto the primary one -- the
 * "panel popups from the small screen appear in the big screen's corner"
 * symptom exactly.
 *
 * So publish the bounding box of every output's usable area instead. On a
 * single monitor that's identical to before; on several it at least
 * *contains* every real work area, so a client clamping to it leaves
 * windows where they are instead of yanking them to another monitor. The
 * exact usable area per output is still what maximize actually uses
 * (compute_output_workarea()), and is still exposed losslessly through the
 * _KIWM_* protocol (PROTOCOL.md), which has no one-rectangle problem. */
static void ewmh_set_workarea(void)
{
    int x = 0, y = 0, right = wm.screen_w, bottom = wm.screen_h;
    bool first = true;

    for (int i = 0; i < wm.output_count; i++) {
        int ox, oy, ow, oh;
        compute_output_workarea(i, &ox, &oy, &ow, &oh);
        if (ow <= 0 || oh <= 0)
            continue;
        if (first) {
            x = ox; y = oy; right = ox + ow; bottom = oy + oh;
            first = false;
        } else {
            if (ox < x) x = ox;
            if (oy < y) y = oy;
            if (ox + ow > right) right = ox + ow;
            if (oy + oh > bottom) bottom = oy + oh;
        }
    }

    int w = right - x, h = bottom - y;
    if (w <= 0) { x = 0; w = wm.screen_w; }
    if (h <= 0) { y = 0; h = wm.screen_h; }

    uint32_t area[MAX_DESKTOPS * 4];
    for (int i = 0; i < wm.num_desktops; i++) {
        area[i * 4 + 0] = (uint32_t)x;
        area[i * 4 + 1] = (uint32_t)y;
        area[i * 4 + 2] = (uint32_t)w;
        area[i * 4 + 3] = (uint32_t)h;
    }
    xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE, wm.root,
                        wm.atoms.net_workarea, XCB_ATOM_CARDINAL, 32,
                        (uint32_t)(wm.num_desktops * 4), area);

    /* The usable area just changed, so every window whose size kiwm (not
     * the user) decided has to be recomputed against it -- a maximized
     * window has to give a newly-arrived panel its space back, and get it
     * back when that panel goes away. See client.c. */
    refit_tiled_clients();
}

/* The virtual screen's size and origin. kiwm has no viewport scrolling, so
 * the viewport is always 0,0 and the geometry is just the root window's
 * current size -- but leaving them unset, as kiwm did, means clients read
 * whatever the *previous* WM wrote, or nothing at all on a fresh session.
 * Refreshed alongside the outputs, since a RandR change is exactly when
 * the root window's size changes. */
void ewmh_set_desktop_geometry(void)
{
    uint32_t geom[] = { (uint32_t)wm.screen_w, (uint32_t)wm.screen_h };
    xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE, wm.root,
                        wm.atoms.net_desktop_geometry, XCB_ATOM_CARDINAL, 32, 2, geom);

    uint32_t viewport[MAX_DESKTOPS * 2] = { 0 };
    xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE, wm.root,
                        wm.atoms.net_desktop_viewport, XCB_ATOM_CARDINAL, 32,
                        (uint32_t)(wm.num_desktops * 2), viewport);
}

/* This output's current mode's refresh rate in Hz, for pacing move/resize
 * drag redraws to the actual display instead of an arbitrary fixed rate
 * (see events.c's handle_motion(), DRAG_REDRAW_INTERVAL_MS's replacement).
 * `output_id` is one of an RandR monitor's backing xcb_randr_output_t's
 * (see outputs_refresh() below) -- picks that output's CRTC, then that
 * CRTC's current mode, then computes refresh = dot_clock / (htotal *
 * vtotal), adjusted for interlace/doublescan same as every other RandR
 * refresh-rate reader (xrandr itself included). Falls back to 60.0 if
 * anything along the way is missing or looks like garbage (variable-
 * refresh/adaptive-sync panels can report a "current" mode that doesn't
 * mean much as a single fixed number; a 1..500Hz sanity clamp guards
 * against reading that as some absurd throttle interval). */
static double compute_output_refresh_hz(xcb_randr_output_t output_id)
{
    double hz = 60.0;

    xcb_randr_get_screen_resources_current_reply_t *res = xcb_randr_get_screen_resources_current_reply(
        wm.conn, xcb_randr_get_screen_resources_current(wm.conn, wm.root), NULL);
    if (!res)
        return hz;

    xcb_randr_get_output_info_reply_t *oinfo = xcb_randr_get_output_info_reply(wm.conn,
        xcb_randr_get_output_info(wm.conn, output_id, res->config_timestamp), NULL);
    if (!oinfo || oinfo->crtc == XCB_NONE) {
        free(oinfo);
        free(res);
        return hz;
    }

    xcb_randr_get_crtc_info_reply_t *cinfo = xcb_randr_get_crtc_info_reply(wm.conn,
        xcb_randr_get_crtc_info(wm.conn, oinfo->crtc, res->config_timestamp), NULL);
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

/* `_XIS_CONFINED_AREA`, published by a compositor that is scaling an
 * output: CARDINAL[4*N], groups of x, y, width, height in root
 * coordinates, naming the part of each affected monitor that is really
 * desktop. Everything outside it on that monitor is scanout the
 * compositor magnifies the logical desktop into -- there is nothing there
 * for a window to be placed in, and the pointer can't even go there.
 *
 * So kiwm shrinks the output to it. Nothing else in kiwm has to know
 * about HiDPI, scaling or densities: maximize, snapping, placement,
 * _NET_WORKAREA and the switcher all work from wm.outputs[] and follow
 * automatically.
 *
 * Matched by geometry rather than by name or index: a rectangle inside an
 * output is that output's, which needs no agreement with the compositor
 * about what anything is called. Absent property -- the normal case, and
 * every machine without the extension -- changes nothing. */
static void apply_confined_areas(XisOutput *outs, int count)
{
    if (wm.atoms.xis_confined_area == XCB_NONE)
        return;

    xcb_get_property_reply_t *r = xcb_get_property_reply(wm.conn,
        xcb_get_property(wm.conn, 0, wm.root, wm.atoms.xis_confined_area,
                         XCB_ATOM_CARDINAL, 0, MAX_OUTPUTS * 4), NULL);
    if (!r)
        return;

    int n = xcb_get_property_value_length(r) / 4;
    uint32_t *v = xcb_get_property_value(r);

    for (int i = 0; i + 3 < n; i += 4) {
        int cx = (int)v[i], cy = (int)v[i + 1];
        int cw = (int)v[i + 2], ch = (int)v[i + 3];
        if (cw <= 0 || ch <= 0)
            continue;

        for (int o = 0; o < count; o++) {
            XisOutput *out = &outs[o];
            /* The confined box lies within this output's box. */
            if (cx < out->x || cy < out->y ||
                cx + cw > out->x + out->width ||
                cy + ch > out->y + out->height)
                continue;

            fprintf(stderr, "kiwm: output '%s' confined to %dx%d+%d+%d "
                            "(scanout %dx%d)\n",
                    out->name, cw, ch, cx, cy, out->width, out->height);
            out->x = cx;
            out->y = cy;
            out->width = cw;
            out->height = ch;
            break;
        }
    }

    free(r);
}


/* When an output's usable area changes -- a compositor confining it for
 * HiDPI scaling, a mode set, a monitor moving in the layout -- the windows
 * that were placed inside the old one have to be brought into the new one.
 * Otherwise the ones past its new edge are simply unreachable: not
 * clipped, not clamped, just outside the part of the screen that is
 * desktop, with no way to click them back.
 *
 * Only *floating* windows are moved here, and only moved: a maximized,
 * fullscreen or half-snapped window is defined by its output rather than
 * placed on it, and re-fitting those is the maximize/snap code's own job.
 * Nothing is ever resized -- a window that was 900 px wide stays 900 px
 * wide, because the user chose that size and a smaller desktop is not a
 * request to change it.
 *
 * The window's *centre* is what scales, not its top-left corner. With no
 * resize those two are different questions: keeping the top-left
 * proportional leaves a window that sat near the right edge hanging off
 * the new one by most of its width, while keeping the centre proportional
 * puts it visually where it was and leaves the clamp below to catch the
 * rest. And the clamp is the part that actually answers "can I still
 * click it": inside the area if it fits, top-left aligned if it is bigger
 * than the area at all. */
static void reposition_floating(int output_idx, const XisOutput *old_box,
                                const XisOutput *new_box)
{
    if (old_box->width <= 0 || old_box->height <= 0)
        return;
    if (old_box->x == new_box->x && old_box->y == new_box->y &&
        old_box->width == new_box->width && old_box->height == new_box->height)
        return;

    for (Client *c = wm.clients; c; c = c->next) {
        if (c->output != output_idx)
            continue;
        if (client_maximized(c) || c->max_horz || c->max_vert ||
            c->fullscreen || c->snap_side != SNAP_NONE)
            continue;

        int fw = c->frame_width > 0 ? c->frame_width : c->width;
        int fh = c->frame_height > 0 ? c->frame_height : c->height;

        /* Where its centre sat in the old area, as a fraction, put back
         * into the new one. */
        double rx = (double)(c->x + fw / 2 - old_box->x) / (double)old_box->width;
        double ry = (double)(c->y + fh / 2 - old_box->y) / (double)old_box->height;

        int x = new_box->x + (int)(rx * new_box->width) - fw / 2;
        int y = new_box->y + (int)(ry * new_box->height) - fh / 2;

        /* On screen, whatever the arithmetic said. */
        if (fw <= new_box->width) {
            if (x < new_box->x)
                x = new_box->x;
            if (x + fw > new_box->x + new_box->width)
                x = new_box->x + new_box->width - fw;
        } else {
            x = new_box->x;
        }
        if (fh <= new_box->height) {
            if (y < new_box->y)
                y = new_box->y;
            if (y + fh > new_box->y + new_box->height)
                y = new_box->y + new_box->height - fh;
        } else {
            y = new_box->y;
        }

        if (x == c->x && y == c->y)
            continue;

        c->x = x;
        c->y = y;
        apply_frame_geometry(c);
    }
}

void outputs_refresh(void)
{
    /* wm.screen (cached at xcb_connect time) never reflects RandR changes
     * after startup -- a fresh xcb_get_geometry() on the root window is
     * the only way to see the *current* virtual screen size, which
     * compute_output_workarea()/ewmh_set_workarea() need to not clamp a
     * bigger/newly-added output's workarea down to whatever the screen
     * size happened to be when kiwm started. */
    xcb_get_geometry_reply_t *root_geo =
        xcb_get_geometry_reply(wm.conn, xcb_get_geometry(wm.conn, wm.root), NULL);
    if (root_geo) {
        wm.screen_w = root_geo->width;
        wm.screen_h = root_geo->height;
        free(root_geo);
    } else if (wm.screen_w == 0 || wm.screen_h == 0) {
        wm.screen_w = wm.screen->width_in_pixels;
        wm.screen_h = wm.screen->height_in_pixels;
    }

    xcb_randr_get_monitors_reply_t *r =
        xcb_randr_get_monitors_reply(wm.conn,
            xcb_randr_get_monitors(wm.conn, wm.root, 1), NULL);

    XisOutput fresh[MAX_OUTPUTS];
    int n = 0;

    if (r) {
        xcb_randr_monitor_info_iterator_t it =
            xcb_randr_get_monitors_monitors_iterator(r);

        for (; it.rem && n < MAX_OUTPUTS; xcb_randr_monitor_info_next(&it)) {
            xcb_randr_monitor_info_t *m = it.data;
            XisOutput *o = &fresh[n];
            memset(o, 0, sizeof(*o));

            get_atom_name_into(m->name, o->name, sizeof(o->name));
            if (!o->name[0])
                snprintf(o->name, sizeof(o->name), "output-%d", n);

            o->x = m->x;
            o->y = m->y;
            o->width = m->width;
            o->height = m->height;
            o->primary = m->primary;
            o->desktop = 0;
            o->refresh_hz = 60.0;

            int noutputs = xcb_randr_monitor_info_outputs_length(m);
            if (noutputs > 0) {
                xcb_randr_output_t *backing = xcb_randr_monitor_info_outputs(m);
                o->refresh_hz = compute_output_refresh_hz(backing[0]);
            }

            for (int i = 0; i < wm.output_count; i++) {
                if (strcmp(wm.outputs[i].name, o->name) == 0) {
                    o->desktop = wm.outputs[i].desktop;
                    break;
                }
            }
            n++;
        }
        free(r);
    }

    if (n == 0) {
        n = 1;
        memset(&fresh[0], 0, sizeof(fresh[0]));
        snprintf(fresh[0].name, sizeof(fresh[0].name), "default");
        fresh[0].width = wm.screen->width_in_pixels;
        fresh[0].height = wm.screen->height_in_pixels;
        fresh[0].primary = true;
        fresh[0].desktop = wm.output_count > 0 ? wm.outputs[0].desktop : 0;
        fresh[0].refresh_hz = 60.0;
    }

    /* Before the list is committed, so every consumer of wm.outputs[]
     * sees the usable box rather than the scanout box. */
    apply_confined_areas(fresh, n);

    /* Kept before the list is replaced: bringing windows into a changed
     * area needs the area they were placed in (reposition_floating). */
    XisOutput previous[MAX_OUTPUTS];
    int previous_count = wm.output_count;
    memcpy(previous, wm.outputs, sizeof(XisOutput) * (size_t)previous_count);

    memcpy(wm.outputs, fresh, sizeof(XisOutput) * (size_t)n);
    wm.output_count = n;

    /* Before the reassignment below, which decides a client's output from
     * where its centre *is*: a window left outside the shrunken area would
     * be handed to whichever output happens to contain that point, or to
     * the primary one, before anything had a chance to bring it back. Its
     * c->output is still the old index here, and the outputs are matched
     * by name so this survives a hotplug reordering them. */
    for (int i = 0; i < previous_count; i++) {
        for (int j = 0; j < n; j++) {
            if (strcmp(previous[i].name, wm.outputs[j].name) != 0)
                continue;
            reposition_floating(i, &previous[i], &wm.outputs[j]);
            break;
        }
    }

    /* Reassign clients to whatever output now covers their center point --
     * desktop included, since a desktop number belongs to an output and a
     * client keeping the old one across this would be left on a desktop
     * the new output may not be showing (see client_reassign_output()). */
    for (Client *c = wm.clients; c; c = c->next)
        client_reassign_output(c, output_index_for_point(c->x + c->width / 2, c->y + c->height / 2));

    /* Same for tracked docks (see DockWindow::output) -- a screen layout
     * change could plausibly move which output a panel's fixed rectangle
     * now falls on. */
    for (int i = 0; i < wm.dock_count; i++)
        assign_dock_output(&wm.docks[i]);

    ewmh_update_output_props();
    ewmh_set_desktop_geometry();
    ewmh_set_workarea();
    xcb_flush(wm.conn);

    fprintf(stderr, "kiwm: %d output(s):\n", wm.output_count);
    for (int i = 0; i < wm.output_count; i++)
        fprintf(stderr, "  [%d] %s %dx%d+%d+%d%s desktop=%d\n", i,
                wm.outputs[i].name, wm.outputs[i].width, wm.outputs[i].height,
                wm.outputs[i].x, wm.outputs[i].y,
                wm.outputs[i].primary ? " (primary)" : "", wm.outputs[i].desktop);
}

void switch_workspace(int output_idx, int desktop)
{
    if (output_idx < 0 || output_idx >= wm.output_count)
        return;
    if (desktop < 0 || desktop >= wm.num_desktops)
        return;
    if (wm.outputs[output_idx].desktop == desktop)
        return;

    int old = wm.outputs[output_idx].desktop;

    /* A window being dragged comes along to the new desktop -- switch
     * desktops with the mouse button still held and the window travels
     * with the pointer instead of being left behind (and yanked out from
     * under the drag), the same "carry the grabbed window across" kwin and
     * compiz have. Any way of switching does it, since they're all the
     * same gesture from the user's side: the cycle shortcut, the switcher
     * OSD's live preview or its commit on release, a direct
     * key_desktop_N jump, or a pager click. Reassigning the desktop
     * *before* the map/unmap loop below is what keeps it continuously
     * visible: the loop then sees a window that already belongs to the
     * desktop being switched to, so it's never unmapped mid-drag. Sticky
     * windows are already on every desktop and have nothing to carry. */
    Client *carry = NULL;
    if (wm.drag_mode == DRAG_MOVE && wm.drag_client &&
        wm.drag_client->output == output_idx && !wm.drag_client->sticky &&
        wm.drag_client->desktop == old)
        carry = wm.drag_client;

    wm.outputs[output_idx].desktop = desktop;

    if (carry) {
        carry->desktop = desktop;
        ewmh_update_wm_desktop(carry);
    }

    /* Published *before* the maps and unmaps that carry it out, for the
     * same reason _NET_WM_STATE is published before the geometry that
     * carries a shade or a maximize out (client.c): anything watching from
     * outside sees the unmaps first, and has to be able to tell "this
     * window went away with its desktop" from "this window was closed".
     * With the property written afterwards that answer arrives one request
     * too late -- a compositor classifies the departure as a close and
     * runs a closing animation on a desktop switch. Requests are processed
     * in order, so writing it here makes the property change reach the
     * server before the first UnmapNotify it explains. */
    ewmh_update_output_props();
    if (output_idx == primary_output_index())
        ewmh_set_current_desktop(desktop);

    Client *to_focus = NULL;
    for (Client *c = wm.clients; c; c = c->next) {
        if (c->output != output_idx || c->minimized || c->sticky)
            continue; /* sticky clients stay mapped through every desktop switch */
        if (c->desktop == old) {
            if (c->mapped)
                xcb_unmap_window(wm.conn, c->frame);
        } else if (c->desktop == desktop) {
            /* Not `if (c->mapped)`: that flag means "should be on screen",
             * and toggle_sticky() clears it for a window whose desktop is
             * not the current one -- which is correct there and fatal
             * here, because it then gates the *only* path that would ever
             * map the window again. A window that had sticky toggled off
             * while its desktop was elsewhere could never come back:
             * still managed, still focusable, still listed, never drawn.
             *
             * What decides whether a window belongs on screen when its
             * desktop arrives is whether it is minimized, which is
             * already the loop's own precondition above. */
            {
                xcb_map_window(wm.conn, c->frame);
                c->mapped = true;
                /* Whichever of the desktop's windows was focused most
                 * recently -- which, since that's the last thing that
                 * happened before leaving this desktop, is the window the
                 * user left focused here. Taking the first match in
                 * wm.clients order instead (what this used to do) meant
                 * arriving on a desktop with some arbitrary window
                 * focused, unrelated to what was in use there. kiwm
                 * already keeps the focus history the switcher's
                 * osd_order=mru needs (Client::last_focus_serial), and
                 * this is the same question asked of it. */
                if (!to_focus || c->last_focus_serial > to_focus->last_focus_serial)
                    to_focus = c;
            }
        }
    }

    if (to_focus) {
        focus_client(to_focus);
    } else {
        if (wm.focused && wm.focused->output == output_idx)
            wm.focused = NULL;
        ewmh_update_active_window();
    }

    xcb_flush(wm.conn);

    fprintf(stderr, "kiwm: output '%s' -> desktop %d\n",
            wm.outputs[output_idx].name, desktop + 1);
}

/* ------------------------------------------------------------------ */
/* the desktop grid (kiwm.conf's desktop_columns=/desktop_rows=)        */
/* ------------------------------------------------------------------ */

void desktop_grid(int *out_cols, int *out_rows)
{
    int cols = wm.desktop_columns, rows = wm.desktop_rows;

    /* EWMH's own rule for _NET_DESKTOP_LAYOUT: one of the two may be 0,
     * meaning "as many as this desktop count needs"; both 0 (or neither
     * configured) falls back to a single row, which is the arrangement
     * kiwm had before any of this was configurable. */
    if (cols <= 0 && rows <= 0) {
        cols = wm.num_desktops;
        rows = 1;
    } else if (cols <= 0) {
        cols = (wm.num_desktops + rows - 1) / rows;
    } else if (rows <= 0) {
        rows = (wm.num_desktops + cols - 1) / cols;
    }

    *out_cols = cols < 1 ? 1 : cols;
    *out_rows = rows < 1 ? 1 : rows;
}

int desktop_at_cell(int row, int col)
{
    int cols, rows;
    desktop_grid(&cols, &rows);
    if (row < 0 || row >= rows || col < 0 || col >= cols)
        return -1;
    int d = row * cols + col;
    return d < wm.num_desktops ? d : -1;
}

void desktop_cell(int desktop, int *out_row, int *out_col)
{
    int cols, rows;
    desktop_grid(&cols, &rows);
    (void)rows;
    if (desktop < 0)
        desktop = 0;
    *out_row = desktop / cols;
    *out_col = desktop % cols;
}

int desktop_step(int from, int direction, DesktopAxis axis)
{
    if (wm.num_desktops < 1)
        return from;
    if (axis == DESKTOP_AXIS_LINEAR)
        return (from + direction + wm.num_desktops) % wm.num_desktops;

    int cols, rows;
    desktop_grid(&cols, &rows);
    int r, c;
    desktop_cell(from, &r, &c);

    /* Walks the row (or column) one cell at a time rather than jumping
     * straight to the neighbor, so a grid whose last row is short -- 5
     * desktops in a 3x2 -- steps over the empty cells instead of getting
     * stuck on one. At most one full lap, then give up and stay put. */
    int span = (axis == DESKTOP_AXIS_HORZ) ? cols : rows;
    for (int i = 0; i < span; i++) {
        if (axis == DESKTOP_AXIS_HORZ)
            c = (c + direction + cols) % cols;
        else
            r = (r + direction + rows) % rows;
        int d = desktop_at_cell(r, c);
        if (d >= 0)
            return d;
    }
    return from;
}

void ewmh_set_desktop_layout(void)
{
    int cols, rows;
    desktop_grid(&cols, &rows);
    /* _NET_WM_ORIENTATION_HORZ (row-major) from _NET_WM_TOPLEFT -- the
     * only arrangement kiwm's own grid and shortcuts describe, so it
     * publishes exactly that rather than an option nothing here reads. */
    uint32_t layout[] = { 0, (uint32_t)cols, (uint32_t)rows, 0 };
    xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE, wm.root,
                        wm.atoms.net_desktop_layout, XCB_ATOM_CARDINAL, 32, 4, layout);
}

/* Cycles the virtual desktop of the "focused output" -- the output that
 * has the currently active window, falling back to whatever output the
 * pointer is on when nothing is focused. */
void cycle_output_desktop(int direction, DesktopAxis axis)
{
    int output_idx = wm.focused ? wm.focused->output : output_for_pointer();
    if (output_idx < 0 || wm.output_count == 0)
        return;

    switch_workspace(output_idx, desktop_step(wm.outputs[output_idx].desktop, direction, axis));
}
