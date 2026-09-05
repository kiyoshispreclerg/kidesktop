/* Per-output desktop tracking. See desktop.h -- the whole file exists to
 * answer one question for the effects: which way did this output's desktop
 * just move? */
#include "desktop.h"
#include "animation.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* How long after the property changed a window's disappearance still
 * counts as "it left with that desktop". The same window window.c uses to
 * classify the event in the first place -- an effect asking about a switch
 * that is already older than that is asking about the wrong one. */
#define SWITCH_RECENT_MS 250.0

typedef struct {
    char name[32];       /* output name, empty in the global fallback */
    int desktop;         /* current, -1 until first read */
    int dx, dy;          /* direction of the last switch, in grid cells */
    double changed_ms;
} Track;

static Track tracks[MAX_OUTPUTS];
static int track_count;

/* No _KIWM_OUTPUTS: a single global desktop (plain EWMH), so the one track
 * answers for every output. */
static bool global_only = true;

/* Columns in _NET_DESKTOP_LAYOUT, 0 when nothing published one. */
static int layout_columns;

/* A CARDINAL[] property from the root window. Caller frees. */
static uint32_t *read_cardinals(xcb_atom_t atom, int *count)
{
    *count = 0;
    if (atom == XCB_NONE)
        return NULL;

    xcb_get_property_reply_t *r = xcb_get_property_reply(comp.conn,
        xcb_get_property(comp.conn, 0, comp.root, atom, XCB_ATOM_CARDINAL, 0, 64),
        NULL);
    if (!r)
        return NULL;

    uint32_t *out = NULL;
    if (r->type == XCB_ATOM_CARDINAL && r->format == 32) {
        int n = xcb_get_property_value_length(r) / 4;
        if (n > 0) {
            out = malloc(sizeof(uint32_t) * (size_t)n);
            if (out) {
                memcpy(out, xcb_get_property_value(r), sizeof(uint32_t) * (size_t)n);
                *count = n;
            }
        }
    }
    free(r);
    return out;
}

/* _KIWM_OUTPUTS: NUL-separated names in index order. Fills `names` and
 * returns how many it found. */
static int read_output_names(char names[MAX_OUTPUTS][32])
{
    if (comp.atoms.kiwm_outputs == XCB_NONE)
        return 0;

    xcb_get_property_reply_t *r = xcb_get_property_reply(comp.conn,
        xcb_get_property(comp.conn, 0, comp.root, comp.atoms.kiwm_outputs,
                         XCB_GET_PROPERTY_TYPE_ANY, 0, 256), NULL);
    if (!r)
        return 0;

    int n = 0;
    int len = xcb_get_property_value_length(r);
    const char *p = xcb_get_property_value(r);

    for (int i = 0; i < len && n < MAX_OUTPUTS; ) {
        int start = i;
        while (i < len && p[i] != '\0')
            i++;
        int l = i - start;
        if (l > 0) {
            if (l > 31)
                l = 31;
            memcpy(names[n], p + start, (size_t)l);
            names[n][l] = '\0';
            n++;
        }
        i++;   /* past the NUL */
    }

    free(r);
    return n;
}

static void read_layout(void)
{
    layout_columns = 0;

    int n = 0;
    uint32_t *v = read_cardinals(comp.atoms.net_desktop_layout, &n);
    /* [orientation, columns, rows, starting_corner] */
    if (v && n >= 3)
        layout_columns = (int)v[1];
    free(v);
}

/* Desktop index -> grid cell. With no layout published (or a layout of one
 * column, which EWMH allows to mean "as many rows as it takes"), the
 * desktops are a single row: switching is left or right. */
static void desktop_cell(int index, int *col, int *row)
{
    if (layout_columns > 1) {
        *col = index % layout_columns;
        *row = index / layout_columns;
    } else {
        *col = index;
        *row = 0;
    }
}

static int sign(int v)
{
    return v > 0 ? 1 : (v < 0 ? -1 : 0);
}

/* One track's desktop changed from `was` to `now`. */
static void track_moved(Track *t, int was, int now)
{
    int c0, r0, c1, r1;
    desktop_cell(was, &c0, &r0);
    desktop_cell(now, &c1, &r1);

    /* One switch, one screen. The desktop three cells to the right is
     * still "to the right": sliding three screen widths in one animation
     * would be a tour of the desktops nobody asked for, and every wall
     * that has ever felt right moves exactly one screen per switch. */
    t->dx = sign(c1 - c0);
    t->dy = sign(r1 - r0);
    t->changed_ms = comp_now_ms();

    /* The core's own "a desktop switch just happened" stamp, set here
     * rather than where the PropertyNotify arrives: this function also
     * runs from the pre-classification round trip (main.c), so a switch
     * whose property event is still in flight is still seen in time for
     * the windows that vanished with it to be classified as having left
     * with a desktop rather than having been closed. */
    comp.desktop_changed_ms = t->changed_ms;

    comp_log("desktop %s%s%d -> %d (%+d,%+d)",
             t->name[0] ? t->name : "", t->name[0] ? ": " : "",
             was, now, t->dx, t->dy);
}

static Track *track_find(const char *name)
{
    for (int i = 0; i < track_count; i++)
        if (strcmp(tracks[i].name, name) == 0)
            return &tracks[i];
    return NULL;
}

static Track *track_for_name(const char *name)
{
    Track *found = track_find(name);
    if (found)
        return found;

    if (track_count >= MAX_OUTPUTS)
        return NULL;

    Track *t = &tracks[track_count++];
    memset(t, 0, sizeof(*t));
    size_t len = strlen(name);
    if (len >= sizeof(t->name))
        len = sizeof(t->name) - 1;
    memcpy(t->name, name, len);
    t->name[len] = '\0';
    t->desktop = -1;
    return t;
}

void desktop_refresh(void)
{
    read_layout();

    char names[MAX_OUTPUTS][32];
    int name_count = read_output_names(names);

    int desk_count = 0;
    uint32_t *desks = read_cardinals(comp.atoms.kiwm_output_desktop, &desk_count);

    if (name_count > 0 && desks && desk_count > 0) {
        /* Per-output desktops. A switch on one monitor says nothing about
         * the others, which is the entire reason for reading this rather
         * than _NET_CURRENT_DESKTOP. */
        global_only = false;

        int n = name_count < desk_count ? name_count : desk_count;
        for (int i = 0; i < n; i++) {
            Track *t = track_for_name(names[i]);
            if (!t)
                continue;
            int now = (int)desks[i];
            if (t->desktop >= 0 && t->desktop != now)
                track_moved(t, t->desktop, now);
            t->desktop = now;
        }
        free(desks);
        return;
    }
    free(desks);

    /* Plain EWMH: one desktop for the whole screen. */
    int n = 0;
    uint32_t *cur = read_cardinals(comp.atoms.net_current_desktop, &n);
    if (cur && n >= 1) {
        global_only = true;
        Track *t = track_for_name("");
        if (t) {
            int now = (int)cur[0];
            if (t->desktop >= 0 && t->desktop != now)
                track_moved(t, t->desktop, now);
            t->desktop = now;
        }
    }
    free(cur);
}

bool desktop_switch_for_rect(const CompRect *r, int *dx, int *dy)
{
    Track *t = NULL;

    if (global_only) {
        t = track_count > 0 ? &tracks[0] : NULL;
    } else {
        /* The output the window sits on: a window belongs to whichever
         * output holds its centre, the same rule the minimize effect uses
         * to find its fallback target. */
        CompRect centre = { r->x + r->w / 2, r->y + r->h / 2, 1, 1 };
        for (int i = 0; i < comp.output_count && !t; i++) {
            CompRect hit;
            if (rect_intersect(&centre, &comp.outputs[i].rect, &hit))
                t = track_find(comp.outputs[i].name);
        }
    }

    if (!t || t->changed_ms <= 0.0)
        return false;
    if (comp_now_ms() - t->changed_ms > SWITCH_RECENT_MS)
        return false;
    if (t->dx == 0 && t->dy == 0)
        return false;

    *dx = t->dx;
    *dy = t->dy;
    return true;
}

/* ------------------------------------------------------------------ */
/* what an effect that lays the desktops out needs (desktop.h)         */
/* ------------------------------------------------------------------ */

int desktop_count(void)
{
    int n = 0;
    uint32_t *v = read_cardinals(comp.atoms.kiwm_num_desktops, &n);
    int count = (v && n >= 1) ? (int)v[0] : 0;
    free(v);
    if (count > 0)
        return count;

    v = read_cardinals(comp.atoms.net_number_of_desktops, &n);
    count = (v && n >= 1) ? (int)v[0] : 0;
    free(v);
    return count;
}

void desktop_grid(int *columns, int *rows)
{
    int count = desktop_count();
    if (count < 1)
        count = 1;

    int cols = layout_columns > 1 ? layout_columns : count;
    if (cols > count)
        cols = count;
    if (cols < 1)
        cols = 1;

    if (columns)
        *columns = cols;
    if (rows)
        *rows = (count + cols - 1) / cols;
}

int desktop_output_index(const CompOutput *o)
{
    if (!o || global_only)
        return -1;

    char names[MAX_OUTPUTS][32];
    int n = read_output_names(names);
    for (int i = 0; i < n; i++)
        if (strcmp(names[i], o->name) == 0)
            return i;
    return -1;
}

int desktop_current_for_output(const CompOutput *o)
{
    Track *t = track_find(global_only ? "" : (o ? o->name : ""));
    return t ? t->desktop : -1;
}

bool desktop_of_window(const CompWindow *w, int *desktop, int *output_index)
{
    if (!w)
        return false;

    xcb_window_t client = w->client != XCB_NONE ? w->client : w->id;

    int desk = -1, out = -1;
    xcb_get_property_reply_t *r = xcb_get_property_reply(comp.conn,
        xcb_get_property(comp.conn, 0, client, comp.atoms.net_wm_desktop,
                         XCB_ATOM_CARDINAL, 0, 1), NULL);
    if (r) {
        if (xcb_get_property_value_length(r) >= 4) {
            uint32_t v = *(uint32_t *)xcb_get_property_value(r);
            desk = (v == 0xffffffffu) ? COMP_DESKTOP_ALL : (int)v;
        }
        free(r);
    }

    /* Under kiwm a desktop number is only unique within an output, so the
     * pair is the answer and half of it is a wrong one
     * (kiwm/PROTOCOL.md). */
    r = xcb_get_property_reply(comp.conn,
        xcb_get_property(comp.conn, 0, client, comp.atoms.kiwm_wm_output,
                         XCB_ATOM_CARDINAL, 0, 1), NULL);
    if (r) {
        if (xcb_get_property_value_length(r) >= 4)
            out = (int)*(uint32_t *)xcb_get_property_value(r);
        free(r);
    }

    if (desktop)
        *desktop = desk;
    if (output_index)
        *output_index = out;
    return desk >= 0 || desk == COMP_DESKTOP_ALL;
}

bool desktop_request_switch(const CompOutput *o, int desktop)
{
    if (desktop < 0)
        return false;

    xcb_client_message_event_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.response_type = XCB_CLIENT_MESSAGE;
    msg.format = 32;
    msg.window = comp.root;

    int out = desktop_output_index(o);
    if (out >= 0 && comp.atoms.kiwm_set_output_desktop != XCB_NONE) {
        msg.type = comp.atoms.kiwm_set_output_desktop;
        msg.data.data32[0] = (uint32_t)out;
        msg.data.data32[1] = (uint32_t)desktop;
    } else if (comp.atoms.net_current_desktop != XCB_NONE) {
        msg.type = comp.atoms.net_current_desktop;
        msg.data.data32[0] = (uint32_t)desktop;
        msg.data.data32[1] = XCB_CURRENT_TIME;
    } else {
        return false;
    }

    xcb_send_event(comp.conn, 0, comp.root,
                   XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY |
                   XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT,
                   (const char *)&msg);
    xcb_flush(comp.conn);
    return true;
}

bool desktop_request_prime(void)
{
    if (comp.atoms.kiwm_prime_desktop_layers == XCB_NONE)
        return false;

    xcb_client_message_event_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.response_type = XCB_CLIENT_MESSAGE;
    msg.format = 32;
    msg.window = comp.root;
    msg.type = comp.atoms.kiwm_prime_desktop_layers;

    xcb_send_event(comp.conn, 0, comp.root,
                   XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY |
                   XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT,
                   (const char *)&msg);
    xcb_flush(comp.conn);
    return true;
}

void desktop_shutdown(void)
{
    track_count = 0;
}
