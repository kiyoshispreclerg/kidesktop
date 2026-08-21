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

static void ewmh_set_workarea(void)
{
    int pi = primary_output_index();
    XisOutput ref = { .x = 0, .y = 0,
                       .width = wm.screen->width_in_pixels,
                       .height = wm.screen->height_in_pixels };
    if (pi >= 0)
        ref = wm.outputs[pi];

    uint32_t area[MAX_DESKTOPS * 4];
    for (int i = 0; i < wm.num_desktops; i++) {
        area[i * 4 + 0] = (uint32_t)ref.x;
        area[i * 4 + 1] = (uint32_t)ref.y;
        area[i * 4 + 2] = (uint32_t)ref.width;
        area[i * 4 + 3] = (uint32_t)ref.height;
    }
    xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE, wm.root,
                        wm.atoms.net_workarea, XCB_ATOM_CARDINAL, 32,
                        (uint32_t)(wm.num_desktops * 4), area);
}

void outputs_refresh(void)
{
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
        if (c->output != output_idx || c->minimized)
            continue;
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
