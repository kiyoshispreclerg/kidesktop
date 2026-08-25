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
        return;
    }
    d->output = output_index_for_point(geo->x + geo->width / 2, geo->y + geo->height / 2);
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

static void ewmh_set_workarea(void)
{
    int pi = primary_output_index();
    int x = 0, y = 0, w = wm.screen_w, h = wm.screen_h;
    if (pi >= 0)
        compute_output_workarea(pi, &x, &y, &w, &h);

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
    }

    memcpy(wm.outputs, fresh, sizeof(XisOutput) * (size_t)n);
    wm.output_count = n;

    /* Reassign clients to whatever output now covers their center point. */
    for (Client *c = wm.clients; c; c = c->next) {
        int new_output = output_index_for_point(c->x + c->width / 2, c->y + c->height / 2);
        if (new_output != c->output) {
            c->output = new_output;
            ewmh_update_wm_output(c);
        }
    }

    /* Same for tracked docks (see DockWindow::output) -- a screen layout
     * change could plausibly move which output a panel's fixed rectangle
     * now falls on. */
    for (int i = 0; i < wm.dock_count; i++)
        assign_dock_output(&wm.docks[i]);

    ewmh_update_output_props();
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
    wm.outputs[output_idx].desktop = desktop;

    Client *to_focus = NULL;
    for (Client *c = wm.clients; c; c = c->next) {
        if (c->output != output_idx || c->minimized || c->sticky)
            continue; /* sticky clients stay mapped through every desktop switch */
        if (c->desktop == old) {
            if (c->mapped)
                xcb_unmap_window(wm.conn, c->frame);
        } else if (c->desktop == desktop) {
            if (c->mapped) {
                xcb_map_window(wm.conn, c->frame);
                if (!to_focus)
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

    ewmh_update_output_props();
    if (output_idx == primary_output_index())
        ewmh_set_current_desktop(desktop);

    xcb_flush(wm.conn);

    fprintf(stderr, "kiwm: output '%s' -> desktop %d\n",
            wm.outputs[output_idx].name, desktop + 1);
}

/* Cycles the virtual desktop of the "focused output" -- the output that
 * has the currently active window, falling back to whatever output the
 * pointer is on when nothing is focused. */
void cycle_output_desktop(int direction)
{
    int output_idx = wm.focused ? wm.focused->output : output_for_pointer();
    if (output_idx < 0 || wm.output_count == 0)
        return;

    int cur = wm.outputs[output_idx].desktop;
    int next = (cur + direction + wm.num_desktops) % wm.num_desktops;
    switch_workspace(output_idx, next);
}
