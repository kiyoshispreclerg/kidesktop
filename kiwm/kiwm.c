/*
 * kiwm - XiS Window Manager
 *
 * First real prototype, per kiwm-kicomp-projeto.md.
 *
 * Scope of this prototype (Fase 1 + slices of Fase 2/3/4):
 *   - XCB connection, root ownership, --replace (ICCCM manager selection).
 *   - RandR outputs (xcb_randr_get_monitors), hotplug-aware.
 *   - Virtual desktops tracked independently per output (not a single
 *     global workspace number): _NET_CURRENT_DESKTOP/_NET_NUMBER_OF_DESKTOPS
 *     mirror the *primary* output only, for compatibility with plain
 *     pagers/taskbars. Real per-output state lives in custom root
 *     properties: _KIWM_OUTPUTS, _KIWM_OUTPUT_DESKTOP, _KIWM_NUM_OUTPUT_DESKTOPS.
 *   - map / unmap / move / resize / maximize / minimize / raise+focus.
 *   - Cairo + Imlib2 decoration, hardcoded to greenxp/bg.png (like
 *     xispanel's theme background loader), cached at startup.
 *   - Decoration is hidden on maximized windows (frame == output rect).
 *   - Enough EWMH/ICCCM for a taskbar (xispanel's tasklist widget) to
 *     list/activate/close/minimize/maximize windows.
 *
 * cc -Wall -Wextra -O2 -o kiwm kiwm.c \
 *     $(pkg-config --cflags --libs xcb xcb-randr cairo cairo-xcb imlib2)
 */

#define _POSIX_C_SOURCE 200809L

#include <xcb/xcb.h>
#include <xcb/xproto.h>
#include <xcb/randr.h>
#include <cairo/cairo.h>
#include <cairo/cairo-xcb.h>
#include <Imlib2.h>

#include <X11/keysym.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define TITLEBAR_H        26
#define BUTTON_W          24
#define MIN_CLIENT_W     100
#define MIN_CLIENT_H      60
#define MAX_CLIENTS      256
#define MAX_OUTPUTS       16
#define NUM_WORKSPACES     4

#define MOD_ALT           XCB_MOD_MASK_1
#define MOD_ALT_SHIFT     (XCB_MOD_MASK_1 | XCB_MOD_MASK_SHIFT)
#define MOD_META          XCB_MOD_MASK_4
#define MOD_META_SHIFT    (XCB_MOD_MASK_4 | XCB_MOD_MASK_SHIFT)

/* ICCCM WM_STATE values */
#define WM_STATE_WITHDRAWN 0
#define WM_STATE_NORMAL    1
#define WM_STATE_ICONIC    3

typedef struct Client Client;

typedef enum {
    DRAG_NONE,
    DRAG_MOVE,
    DRAG_RESIZE
} DragMode;

typedef struct XisOutput {
    char name[64];
    int x, y, width, height;
    bool primary;
    int desktop;            /* current virtual desktop for this output, 0..NUM_WORKSPACES-1 */
} XisOutput;

struct Client {
    xcb_window_t window;    /* application window */
    xcb_window_t frame;     /* decorated frame */

    int x, y, width, height;                 /* content geometry, global coords */
    int saved_x, saved_y, saved_w, saved_h;   /* restore geometry before maximize */
    int frame_width, frame_height;

    bool mapped;
    bool maximized;
    bool minimized;
    int ignore_unmap;   /* absorbs the automatic UnmapNotify from reparenting an
                          * already-mapped pre-existing window at startup */

    int output;              /* index into wm.outputs */
    int desktop;              /* per-output virtual desktop this client belongs to */

    char title[256];

    Client *next;
};

typedef struct {
    xcb_atom_t wm_protocols;
    xcb_atom_t wm_delete_window;
    xcb_atom_t wm_state;
    xcb_atom_t wm_change_state;
    xcb_atom_t net_wm_name;
    xcb_atom_t utf8_string;
    xcb_atom_t manager;

    xcb_atom_t net_supported;
    xcb_atom_t net_supporting_wm_check;
    xcb_atom_t net_client_list;
    xcb_atom_t net_client_list_stacking;
    xcb_atom_t net_active_window;
    xcb_atom_t net_close_window;
    xcb_atom_t net_number_of_desktops;
    xcb_atom_t net_current_desktop;
    xcb_atom_t net_wm_desktop;
    xcb_atom_t net_workarea;

    xcb_atom_t net_wm_state;
    xcb_atom_t net_wm_state_hidden;
    xcb_atom_t net_wm_state_maximized_vert;
    xcb_atom_t net_wm_state_maximized_horz;
    xcb_atom_t net_wm_state_skip_taskbar;

    xcb_atom_t net_wm_window_type;
    xcb_atom_t net_wm_window_type_normal;
    xcb_atom_t net_wm_window_type_dock;
    xcb_atom_t net_wm_window_type_desktop;
    xcb_atom_t net_wm_window_type_toolbar;
    xcb_atom_t net_wm_window_type_menu;

    xcb_atom_t kiwm_outputs;
    xcb_atom_t kiwm_output_desktop;
    xcb_atom_t kiwm_num_output_desktops;
    xcb_atom_t kiwm_set_output_desktop;
} Atoms;

typedef struct {
    xcb_connection_t *conn;
    xcb_screen_t *screen;
    xcb_window_t root;
    xcb_visualtype_t *visual;
    int randr_event_base;

    Atoms atoms;
    xcb_window_t check_win;
    xcb_window_t sel_win;    /* WM_Sn selection owner window, watched for SelectionClear */
    xcb_atom_t sn_atom;

    XisOutput outputs[MAX_OUTPUTS];
    int output_count;

    Client *clients;
    Client *focused;

    cairo_surface_t *deco_bg;   /* cached greenxp/bg.png, ARGB32 */
    bool hide_deco_on_maximize;

    DragMode drag_mode;
    Client *drag_client;
    int drag_start_root_x, drag_start_root_y;
    int drag_start_x, drag_start_y, drag_start_w, drag_start_h;

    xcb_keycode_t key_tab, key_1, key_2, key_3, key_4, key_up;

    bool running;
} KiWM;

static KiWM wm;

static void die(const char *msg)
{
    fprintf(stderr, "kiwm: %s\n", msg);
    exit(EXIT_FAILURE);
}

static xcb_atom_t intern_atom(const char *name)
{
    xcb_intern_atom_cookie_t cookie =
        xcb_intern_atom(wm.conn, 0, (uint16_t)strlen(name), name);
    xcb_intern_atom_reply_t *reply =
        xcb_intern_atom_reply(wm.conn, cookie, NULL);
    if (!reply)
        return XCB_ATOM_NONE;
    xcb_atom_t atom = reply->atom;
    free(reply);
    return atom;
}

/* ------------------------------------------------------------------ */
/* outputs / per-output virtual desktops                               */
/* ------------------------------------------------------------------ */

static int primary_output_index(void)
{
    for (int i = 0; i < wm.output_count; i++)
        if (wm.outputs[i].primary)
            return i;
    return wm.output_count > 0 ? 0 : -1;
}

static int output_index_for_point(int x, int y)
{
    for (int i = 0; i < wm.output_count; i++) {
        XisOutput *o = &wm.outputs[i];
        if (x >= o->x && x < o->x + o->width &&
            y >= o->y && y < o->y + o->height)
            return i;
    }
    return primary_output_index();
}

static int output_for_pointer(void)
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

    uint32_t numws = NUM_WORKSPACES;
    xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE, wm.root,
                        wm.atoms.kiwm_num_output_desktops, XCB_ATOM_CARDINAL, 32,
                        1, &numws);
}

static void ewmh_set_current_desktop(int desktop)
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

    uint32_t area[NUM_WORKSPACES * 4];
    for (int i = 0; i < NUM_WORKSPACES; i++) {
        area[i * 4 + 0] = (uint32_t)ref.x;
        area[i * 4 + 1] = (uint32_t)ref.y;
        area[i * 4 + 2] = (uint32_t)ref.width;
        area[i * 4 + 3] = (uint32_t)ref.height;
    }
    xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE, wm.root,
                        wm.atoms.net_workarea, XCB_ATOM_CARDINAL, 32,
                        NUM_WORKSPACES * 4, area);
}

static void outputs_refresh(void)
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
    for (Client *c = wm.clients; c; c = c->next)
        c->output = output_index_for_point(c->x + c->width / 2, c->y + c->height / 2);

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

/* ------------------------------------------------------------------ */
/* misc X helpers                                                      */
/* ------------------------------------------------------------------ */

static xcb_visualtype_t *find_root_visual(xcb_screen_t *screen)
{
    xcb_depth_iterator_t depth_iter = xcb_screen_allowed_depths_iterator(screen);

    for (; depth_iter.rem; xcb_depth_next(&depth_iter)) {
        xcb_visualtype_iterator_t visual_iter =
            xcb_depth_visuals_iterator(depth_iter.data);

        for (; visual_iter.rem; xcb_visualtype_next(&visual_iter))
            if (visual_iter.data->visual_id == screen->root_visual)
                return visual_iter.data;
    }
    return NULL;
}

static xcb_keycode_t keysym_to_keycode(xcb_keysym_t keysym)
{
    const xcb_setup_t *setup = xcb_get_setup(wm.conn);
    xcb_keycode_t min_kc = setup->min_keycode;
    xcb_keycode_t max_kc = setup->max_keycode;

    xcb_get_keyboard_mapping_reply_t *reply = xcb_get_keyboard_mapping_reply(
        wm.conn, xcb_get_keyboard_mapping(wm.conn, min_kc, (uint8_t)(max_kc - min_kc + 1)), NULL);
    if (!reply)
        return 0;

    xcb_keysym_t *syms = xcb_get_keyboard_mapping_keysyms(reply);
    int per = reply->keysyms_per_keycode;

    for (xcb_keycode_t kc = min_kc; kc <= max_kc; kc++) {
        for (int i = 0; i < per; i++) {
            if (syms[(kc - min_kc) * per + i] == keysym) {
                free(reply);
                return kc;
            }
        }
    }
    free(reply);
    return 0;
}

static bool client_supports_protocol(xcb_window_t window, xcb_atom_t proto)
{
    xcb_get_property_reply_t *reply = xcb_get_property_reply(wm.conn,
        xcb_get_property(wm.conn, 0, window, wm.atoms.wm_protocols, XCB_ATOM_ATOM, 0, 64), NULL);
    if (!reply)
        return false;

    bool found = false;
    if (reply->type == XCB_ATOM_ATOM && reply->format == 32) {
        xcb_atom_t *atoms = xcb_get_property_value(reply);
        int n = xcb_get_property_value_length(reply) / (int)sizeof(xcb_atom_t);
        for (int i = 0; i < n; i++)
            if (atoms[i] == proto) { found = true; break; }
    }
    free(reply);
    return found;
}

static xcb_atom_t get_window_type(xcb_window_t window)
{
    xcb_get_property_reply_t *reply = xcb_get_property_reply(wm.conn,
        xcb_get_property(wm.conn, 0, window, wm.atoms.net_wm_window_type, XCB_ATOM_ATOM, 0, 32), NULL);
    if (!reply)
        return XCB_ATOM_NONE;

    xcb_atom_t type = XCB_ATOM_NONE;
    if (reply->type == XCB_ATOM_ATOM && reply->format == 32 &&
        xcb_get_property_value_length(reply) > 0) {
        xcb_atom_t *atoms = xcb_get_property_value(reply);
        type = atoms[0];
    }
    free(reply);
    return type;
}

static bool should_manage_decorated(xcb_window_t window)
{
    xcb_atom_t t = get_window_type(window);
    if (t == XCB_ATOM_NONE)
        return true;
    return !(t == wm.atoms.net_wm_window_type_dock ||
             t == wm.atoms.net_wm_window_type_desktop ||
             t == wm.atoms.net_wm_window_type_toolbar ||
             t == wm.atoms.net_wm_window_type_menu);
}

static Client *find_client_window(xcb_window_t window)
{
    for (Client *c = wm.clients; c; c = c->next)
        if (c->window == window || c->frame == window)
            return c;
    return NULL;
}

/* ------------------------------------------------------------------ */
/* EWMH/ICCCM state on clients                                        */
/* ------------------------------------------------------------------ */

static void ewmh_update_client_list(void)
{
    xcb_window_t list[MAX_CLIENTS];
    int n = 0;
    for (Client *c = wm.clients; c && n < MAX_CLIENTS; c = c->next)
        list[n++] = c->window;

    xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE, wm.root,
                        wm.atoms.net_client_list, XCB_ATOM_WINDOW, 32, (uint32_t)n, list);
    xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE, wm.root,
                        wm.atoms.net_client_list_stacking, XCB_ATOM_WINDOW, 32, (uint32_t)n, list);
}

static void ewmh_update_active_window(void)
{
    xcb_window_t w = wm.focused ? wm.focused->window : XCB_NONE;
    xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE, wm.root,
                        wm.atoms.net_active_window, XCB_ATOM_WINDOW, 32, 1, &w);
}

static void set_icccm_wm_state(Client *c, uint32_t state)
{
    uint32_t data[] = { state, XCB_ATOM_NONE };
    xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE, c->window,
                        wm.atoms.wm_state, wm.atoms.wm_state, 32, 2, data);
}

/* Rewrites _NET_WM_STATE preserving any atoms we don't manage ourselves
 * (e.g. an app's own _NET_WM_STATE_SKIP_TASKBAR), only replacing the
 * hidden/maximized bits we own. */
static void ewmh_update_wm_state(Client *c)
{
    xcb_atom_t keep[32];
    int nkeep = 0;

    xcb_get_property_reply_t *reply = xcb_get_property_reply(wm.conn,
        xcb_get_property(wm.conn, 0, c->window, wm.atoms.net_wm_state, XCB_ATOM_ATOM, 0, 32), NULL);
    if (reply) {
        if (reply->type == XCB_ATOM_ATOM && reply->format == 32) {
            xcb_atom_t *atoms = xcb_get_property_value(reply);
            int n = xcb_get_property_value_length(reply) / (int)sizeof(xcb_atom_t);
            for (int i = 0; i < n && nkeep < 32; i++) {
                xcb_atom_t a = atoms[i];
                if (a == wm.atoms.net_wm_state_hidden ||
                    a == wm.atoms.net_wm_state_maximized_vert ||
                    a == wm.atoms.net_wm_state_maximized_horz)
                    continue;
                keep[nkeep++] = a;
            }
        }
        free(reply);
    }

    xcb_atom_t out[40];
    int n = 0;
    for (int i = 0; i < nkeep; i++)
        out[n++] = keep[i];
    if (c->minimized)
        out[n++] = wm.atoms.net_wm_state_hidden;
    if (c->maximized) {
        out[n++] = wm.atoms.net_wm_state_maximized_vert;
        out[n++] = wm.atoms.net_wm_state_maximized_horz;
    }

    xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE, c->window,
                        wm.atoms.net_wm_state, XCB_ATOM_ATOM, 32, (uint32_t)n, out);

    set_icccm_wm_state(c, c->minimized ? WM_STATE_ICONIC : WM_STATE_NORMAL);
}

static void ewmh_update_wm_desktop(Client *c)
{
    uint32_t v = (uint32_t)c->desktop;
    xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE, c->window,
                        wm.atoms.net_wm_desktop, XCB_ATOM_CARDINAL, 32, 1, &v);
}

static void get_title(Client *c)
{
    memset(c->title, 0, sizeof(c->title));

    xcb_get_property_reply_t *reply = xcb_get_property_reply(wm.conn,
        xcb_get_property(wm.conn, 0, c->window, wm.atoms.net_wm_name, wm.atoms.utf8_string, 0, 256), NULL);

    int len = reply ? xcb_get_property_value_length(reply) : 0;
    if (reply && len > 0) {
        if ((size_t)len >= sizeof(c->title))
            len = (int)sizeof(c->title) - 1;
        memcpy(c->title, xcb_get_property_value(reply), (size_t)len);
        c->title[len] = '\0';
    }
    free(reply);

    if (!c->title[0]) {
        reply = xcb_get_property_reply(wm.conn,
            xcb_get_property(wm.conn, 0, c->window, XCB_ATOM_WM_NAME, XCB_GET_PROPERTY_TYPE_ANY, 0, 256), NULL);
        len = reply ? xcb_get_property_value_length(reply) : 0;
        if (reply && len > 0) {
            if ((size_t)len >= sizeof(c->title))
                len = (int)sizeof(c->title) - 1;
            memcpy(c->title, xcb_get_property_value(reply), (size_t)len);
            c->title[len] = '\0';
        }
        free(reply);
    }

    if (!c->title[0])
        snprintf(c->title, sizeof(c->title), "Untitled");
}

/* ------------------------------------------------------------------ */
/* decoration: Cairo + Imlib2, hardcoded greenxp/bg.png                */
/* ------------------------------------------------------------------ */

static cairo_surface_t *load_png_argb(const char *path)
{
    Imlib_Image img = imlib_load_image(path);
    if (!img)
        return NULL;

    imlib_context_set_image(img);
    int iw = imlib_image_get_width();
    int ih = imlib_image_get_height();
    if (iw <= 0 || ih <= 0 || iw > 4096 || ih > 4096) {
        imlib_free_image();
        return NULL;
    }

    DATA32 *src = imlib_image_get_data_for_reading_only();
    if (!src) {
        imlib_free_image();
        return NULL;
    }

    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, iw, ih);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        imlib_free_image();
        return NULL;
    }

    unsigned char *dst = cairo_image_surface_get_data(surf);
    int stride = cairo_image_surface_get_stride(surf);
    for (int y = 0; y < ih; y++) {
        uint32_t *row = (uint32_t *)(void *)(dst + y * stride);
        for (int x = 0; x < iw; x++) {
            uint32_t argb = src[y * iw + x];
            uint8_t a = (uint8_t)((argb >> 24) & 0xff);
            uint8_t r = (uint8_t)((argb >> 16) & 0xff);
            uint8_t g = (uint8_t)((argb >> 8) & 0xff);
            uint8_t b = (uint8_t)(argb & 0xff);
            r = (uint8_t)((r * a) / 255);
            g = (uint8_t)((g * a) / 255);
            b = (uint8_t)((b * a) / 255);
            row[x] = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        }
    }
    cairo_surface_mark_dirty(surf);

    imlib_free_image();
    return surf;
}

/* Hardcoded for this first prototype: no theme.conf yet (see section 10
 * of kiwm-kicomp-projeto.md), just the same PNG xispanel themes use. */
static void load_decoration(void)
{
    const char *env = getenv("KIWM_DECO_BG");
    const char *candidates[] = {
        "../greenxp/bg.png",
        "./greenxp/bg.png",
        "greenxp/bg.png",
    };

    if (env && env[0]) {
        wm.deco_bg = load_png_argb(env);
        if (wm.deco_bg) {
            fprintf(stderr, "kiwm: decoration background loaded from '%s'\n", env);
            return;
        }
    }

    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        wm.deco_bg = load_png_argb(candidates[i]);
        if (wm.deco_bg) {
            fprintf(stderr, "kiwm: decoration background loaded from '%s'\n", candidates[i]);
            return;
        }
    }

    fprintf(stderr, "kiwm: could not load greenxp/bg.png, falling back to flat color decoration\n");
}

static bool client_deco_visible(Client *c)
{
    return !(c->maximized && wm.hide_deco_on_maximize);
}

static void draw_button(cairo_t *cr, double x, char glyph)
{
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.30);
    cairo_rectangle(cr, x, 0, BUTTON_W, TITLEBAR_H);
    cairo_fill(cr);

    cairo_set_source_rgba(cr, 0.92, 0.92, 0.95, 1.0);
    cairo_set_line_width(cr, 1.5);

    double cx = x + BUTTON_W / 2.0;
    double cy = TITLEBAR_H / 2.0;

    if (glyph == '-') {
        cairo_move_to(cr, cx - 5, cy);
        cairo_line_to(cr, cx + 5, cy);
        cairo_stroke(cr);
    } else if (glyph == '+') {
        cairo_rectangle(cr, cx - 5, cy - 5, 10, 10);
        cairo_stroke(cr);
    } else if (glyph == 'x') {
        cairo_move_to(cr, cx - 4, cy - 4);
        cairo_line_to(cr, cx + 4, cy + 4);
        cairo_move_to(cr, cx + 4, cy - 4);
        cairo_line_to(cr, cx - 4, cy + 4);
        cairo_stroke(cr);
    }
}

static void draw_decoration(Client *c)
{
    if (!client_deco_visible(c))
        return;
    if (!wm.visual)
        return;

    int w = c->frame_width;
    int h = TITLEBAR_H;
    if (w <= 0 || h <= 0)
        return;

    cairo_surface_t *surface = cairo_xcb_surface_create(wm.conn, c->frame, wm.visual, w, h);
    cairo_t *cr = cairo_create(surface);

    if (wm.deco_bg) {
        int iw = cairo_image_surface_get_width(wm.deco_bg);
        int ih = cairo_image_surface_get_height(wm.deco_bg);
        cairo_save(cr);
        cairo_scale(cr, (double)w / (double)iw, (double)h / (double)ih);
        cairo_set_source_surface(cr, wm.deco_bg, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
        cairo_paint(cr);
        cairo_restore(cr);
    } else {
        /* No theme PNG (missing file, or no theme configured yet):
         * plain black background, white text below. */
        cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
        cairo_paint(cr);
    }

    /* Focus tint on top of the theme image. */
    if (c == wm.focused)
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.10);
    else
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.35);
    cairo_paint(cr);

    cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 12.5);
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);

    cairo_text_extents_t ext;
    cairo_text_extents(cr, c->title, &ext);
    double title_x = 8.0;
    double title_y = (TITLEBAR_H - ext.height) / 2.0 - ext.y_bearing;
    cairo_move_to(cr, title_x, title_y);
    cairo_show_text(cr, c->title);

    draw_button(cr, w - BUTTON_W * 3, '-');
    draw_button(cr, w - BUTTON_W * 2, '+');
    draw_button(cr, w - BUTTON_W,     'x');

    cairo_destroy(cr);
    cairo_surface_flush(surface);
    cairo_surface_destroy(surface);
}

static void configure_frame(Client *c)
{
    bool deco = client_deco_visible(c);
    int th = deco ? TITLEBAR_H : 0;

    c->frame_width = c->width;
    c->frame_height = c->height + th;

    uint32_t fv[] = {
        (uint32_t)c->x, (uint32_t)c->y,
        (uint32_t)c->frame_width, (uint32_t)c->frame_height
    };
    xcb_configure_window(wm.conn, c->frame,
                         XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                         XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, fv);

    uint32_t cv[] = { 0, (uint32_t)th, (uint32_t)c->width, (uint32_t)c->height };
    xcb_configure_window(wm.conn, c->window,
                         XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                         XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, cv);

    draw_decoration(c);
}

/* ------------------------------------------------------------------ */
/* focus / stacking / workspace switching                              */
/* ------------------------------------------------------------------ */

static void focus_client(Client *c)
{
    if (!c)
        return;

    Client *old = wm.focused;

    if (old != c) {
        wm.focused = c;
        xcb_set_input_focus(wm.conn, XCB_INPUT_FOCUS_POINTER_ROOT,
                            c->window, XCB_CURRENT_TIME);
        if (old)
            draw_decoration(old);
        ewmh_update_active_window();
    }

    xcb_configure_window(wm.conn, c->frame, XCB_CONFIG_WINDOW_STACK_MODE,
                         (uint32_t[]){ XCB_STACK_MODE_ABOVE });

    draw_decoration(c);
    ewmh_update_client_list();
    xcb_flush(wm.conn);
}

static void switch_workspace(int output_idx, int desktop)
{
    if (output_idx < 0 || output_idx >= wm.output_count)
        return;
    if (desktop < 0 || desktop >= NUM_WORKSPACES)
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

static void cycle_focus(int direction)
{
    int output_idx = wm.focused ? wm.focused->output : output_for_pointer();
    if (output_idx < 0)
        return;
    int desktop = wm.outputs[output_idx].desktop;

    Client *eligible[MAX_CLIENTS];
    int n = 0;
    int current_idx = -1;

    for (Client *c = wm.clients; c && n < MAX_CLIENTS; c = c->next) {
        if (c->output == output_idx && c->desktop == desktop && c->mapped && !c->minimized) {
            if (c == wm.focused)
                current_idx = n;
            eligible[n++] = c;
        }
    }
    if (n == 0)
        return;

    int next = (current_idx < 0) ? 0 : (current_idx + direction + n) % n;
    focus_client(eligible[next]);
}

/* Cycles the virtual desktop of the "focused output" -- the output that
 * has the currently active window, falling back to whatever output the
 * pointer is on when nothing is focused. */
static void cycle_output_desktop(int direction)
{
    int output_idx = wm.focused ? wm.focused->output : output_for_pointer();
    if (output_idx < 0 || wm.output_count == 0)
        return;

    int cur = wm.outputs[output_idx].desktop;
    int next = (cur + direction + NUM_WORKSPACES) % NUM_WORKSPACES;
    switch_workspace(output_idx, next);
}

/* ------------------------------------------------------------------ */
/* window state transitions                                            */
/* ------------------------------------------------------------------ */

static void send_delete(Client *c)
{
    if (wm.atoms.wm_protocols == XCB_ATOM_NONE ||
        wm.atoms.wm_delete_window == XCB_ATOM_NONE)
        return;

    xcb_client_message_event_t ev = {
        .response_type = XCB_CLIENT_MESSAGE,
        .format = 32,
        .window = c->window,
        .type = wm.atoms.wm_protocols
    };
    ev.data.data32[0] = wm.atoms.wm_delete_window;
    ev.data.data32[1] = XCB_CURRENT_TIME;

    xcb_send_event(wm.conn, 0, c->window, XCB_EVENT_MASK_NO_EVENT, (const char *)&ev);
}

static void close_client(Client *c)
{
    if (client_supports_protocol(c->window, wm.atoms.wm_delete_window))
        send_delete(c);
    else
        xcb_kill_client(wm.conn, c->window);
    xcb_flush(wm.conn);
}

static void toggle_maximize(Client *c, int want /* -1=toggle 0=unmax 1=max */)
{
    bool target = (want == -1) ? !c->maximized : (want == 1);
    if (target == c->maximized)
        return;

    if (target) {
        c->saved_x = c->x;
        c->saved_y = c->y;
        c->saved_w = c->width;
        c->saved_h = c->height;

        XisOutput *o = &wm.outputs[c->output >= 0 ? c->output : 0];
        c->maximized = true;
        c->x = o->x;
        c->y = o->y;
        c->width = o->width;
        c->height = o->height - (wm.hide_deco_on_maximize ? 0 : TITLEBAR_H);
    } else {
        c->maximized = false;
        c->x = c->saved_x;
        c->y = c->saved_y;
        c->width = c->saved_w;
        c->height = c->saved_h;
    }

    configure_frame(c);
    ewmh_update_wm_state(c);
    xcb_flush(wm.conn);
}

static void minimize_client(Client *c)
{
    if (c->minimized)
        return;

    c->minimized = true;
    if (c->mapped) {
        xcb_unmap_window(wm.conn, c->frame);
        c->mapped = false;
    }
    if (wm.focused == c)
        wm.focused = NULL;

    ewmh_update_wm_state(c);
    ewmh_update_active_window();
    xcb_flush(wm.conn);
}

static void restore_client(Client *c)
{
    if (!c->minimized)
        return;
    c->minimized = false;

    if (wm.outputs[c->output].desktop == c->desktop) {
        xcb_map_window(wm.conn, c->frame);
        c->mapped = true;
    }
    ewmh_update_wm_state(c);
    xcb_flush(wm.conn);
}

static void activate_client(Client *c)
{
    if (c->minimized)
        restore_client(c);
    if (c->output >= 0 && wm.outputs[c->output].desktop != c->desktop)
        switch_workspace(c->output, c->desktop);
    if (c->mapped)
        focus_client(c);
}

static void set_client_desktop(Client *c, int desktop)
{
    if (desktop < 0)
        desktop = 0;
    if (desktop >= NUM_WORKSPACES)
        desktop = NUM_WORKSPACES - 1;
    if (desktop == c->desktop)
        return;

    bool was_visible = (c->output >= 0 && wm.outputs[c->output].desktop == c->desktop);
    bool now_visible = (c->output >= 0 && wm.outputs[c->output].desktop == desktop);

    c->desktop = desktop;

    if (was_visible && !now_visible && c->mapped)
        xcb_unmap_window(wm.conn, c->frame);
    else if (!was_visible && now_visible && c->mapped)
        xcb_map_window(wm.conn, c->frame);

    ewmh_update_wm_desktop(c);
    xcb_flush(wm.conn);
}

/* ------------------------------------------------------------------ */
/* manage / unmanage                                                    */
/* ------------------------------------------------------------------ */

static void remove_client(Client *client)
{
    Client **pp = &wm.clients;
    while (*pp) {
        if (*pp == client) {
            *pp = client->next;
            if (wm.focused == client)
                wm.focused = NULL;
            free(client);
            return;
        }
        pp = &(*pp)->next;
    }
}

static void unmanage(Client *c)
{
    int fx = c->x, fy = c->y, fw = c->frame_width, fh = c->frame_height;

    if (wm.focused == c)
        wm.focused = NULL;
    if (wm.drag_client == c) {
        wm.drag_client = NULL;
        wm.drag_mode = DRAG_NONE;
    }

    xcb_unmap_window(wm.conn, c->frame);

    xcb_void_cookie_t reparent_cookie =
        xcb_reparent_window_checked(wm.conn, c->window, wm.root, c->x, c->y);
    xcb_generic_error_t *err = xcb_request_check(wm.conn, reparent_cookie);
    if (err)
        free(err);

    xcb_destroy_window(wm.conn, c->frame);

    if (fw > 0 && fh > 0)
        xcb_clear_area(wm.conn, 0, wm.root, fx, fy, (uint16_t)fw, (uint16_t)fh);

    remove_client(c);
    ewmh_update_client_list();
    ewmh_update_active_window();
    xcb_flush(wm.conn);
}

static void manage(xcb_window_t window)
{
    if (find_client_window(window))
        return;

    xcb_get_window_attributes_reply_t *attr = xcb_get_window_attributes_reply(
        wm.conn, xcb_get_window_attributes(wm.conn, window), NULL);
    if (!attr)
        return;
    if (attr->override_redirect) {
        free(attr);
        return;
    }
    bool was_viewable = attr->map_state == XCB_MAP_STATE_VIEWABLE;
    free(attr);

    if (!should_manage_decorated(window)) {
        /* Panels, docks, desktops: managed just enough to be mapped and
         * to show up wherever _NET_WM_WINDOW_TYPE says they belong,
         * never framed or added to the taskbar client list. */
        xcb_map_window(wm.conn, window);
        xcb_flush(wm.conn);
        return;
    }

    xcb_get_geometry_reply_t *geo =
        xcb_get_geometry_reply(wm.conn, xcb_get_geometry(wm.conn, window), NULL);
    if (!geo)
        return;

    Client *c = calloc(1, sizeof(*c));
    if (!c) {
        free(geo);
        return;
    }

    c->window = window;
    c->x = geo->x;
    c->y = geo->y;
    c->width = geo->width < MIN_CLIENT_W ? MIN_CLIENT_W : geo->width;
    c->height = geo->height < MIN_CLIENT_H ? MIN_CLIENT_H : geo->height;
    free(geo);

    c->output = output_index_for_point(c->x + c->width / 2, c->y + c->height / 2);
    if (c->output < 0)
        c->output = 0;
    c->desktop = wm.output_count > 0 ? wm.outputs[c->output].desktop : 0;

    get_title(c);

    c->frame = xcb_generate_id(wm.conn);

    uint32_t values[] = {
        XCB_EVENT_MASK_EXPOSURE |
        XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE |
        XCB_EVENT_MASK_POINTER_MOTION |
        XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_LEAVE_WINDOW,
        0
    };
    xcb_create_window(wm.conn, wm.screen->root_depth, c->frame, wm.root,
                      c->x, c->y, c->width, c->height + TITLEBAR_H, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, wm.screen->root_visual,
                      XCB_CW_EVENT_MASK | XCB_CW_BORDER_PIXEL, values);

    uint32_t client_mask = XCB_EVENT_MASK_PROPERTY_CHANGE |
                           XCB_EVENT_MASK_STRUCTURE_NOTIFY |
                           XCB_EVENT_MASK_FOCUS_CHANGE;
    xcb_change_window_attributes(wm.conn, window, XCB_CW_EVENT_MASK, &client_mask);

    xcb_grab_button(wm.conn, 0, window, XCB_EVENT_MASK_BUTTON_PRESS,
                    XCB_GRAB_MODE_SYNC, XCB_GRAB_MODE_ASYNC,
                    XCB_NONE, XCB_NONE, XCB_BUTTON_INDEX_1, XCB_MOD_MASK_ANY);

    xcb_reparent_window(wm.conn, window, c->frame, 0, TITLEBAR_H);

    xcb_map_window(wm.conn, window);
    xcb_map_window(wm.conn, c->frame);

    c->mapped = true;
    c->ignore_unmap = was_viewable ? 1 : 0;
    set_icccm_wm_state(c, WM_STATE_NORMAL);

    c->next = wm.clients;
    wm.clients = c;

    configure_frame(c);
    ewmh_update_wm_desktop(c);
    ewmh_update_wm_state(c);
    ewmh_update_client_list();
    focus_client(c);
}

/* Called once at startup so windows already open before kiwm starts (or
 * left over from a --replace'd WM) get framed too, not just windows
 * mapped afterward. A window qualifies if it's currently viewable, or if
 * it carries an ICCCM WM_STATE property (an app that was managed before
 * and expects to be picked back up, even if briefly unmapped). */
static void manage_existing_windows(void)
{
    xcb_query_tree_reply_t *tree =
        xcb_query_tree_reply(wm.conn, xcb_query_tree(wm.conn, wm.root), NULL);
    if (!tree)
        return;

    xcb_window_t *children = xcb_query_tree_children(tree);
    int n = xcb_query_tree_children_length(tree);

    for (int i = 0; i < n; i++) {
        xcb_window_t w = children[i];
        if (w == wm.check_win || w == wm.sel_win || find_client_window(w))
            continue;

        xcb_get_window_attributes_reply_t *attr = xcb_get_window_attributes_reply(
            wm.conn, xcb_get_window_attributes(wm.conn, w), NULL);
        if (!attr)
            continue;
        bool viewable = attr->map_state == XCB_MAP_STATE_VIEWABLE;
        bool override = attr->override_redirect;
        free(attr);
        if (override)
            continue;

        bool has_wm_state = false;
        xcb_get_property_reply_t *st = xcb_get_property_reply(wm.conn,
            xcb_get_property(wm.conn, 0, w, wm.atoms.wm_state, wm.atoms.wm_state, 0, 2), NULL);
        if (st) {
            if (st->type == wm.atoms.wm_state)
                has_wm_state = true;
            free(st);
        }

        if (viewable || has_wm_state)
            manage(w);
    }

    free(tree);
    xcb_flush(wm.conn);
}

/* ------------------------------------------------------------------ */
/* event handlers                                                       */
/* ------------------------------------------------------------------ */

static void handle_map_request(xcb_map_request_event_t *ev)
{
    Client *c = find_client_window(ev->window);
    if (!c) {
        manage(ev->window);
    } else {
        if (wm.outputs[c->output].desktop == c->desktop) {
            xcb_map_window(wm.conn, c->frame);
            c->mapped = true;
        }
        c->minimized = false;
        set_icccm_wm_state(c, WM_STATE_NORMAL);
        ewmh_update_wm_state(c);
    }
    xcb_flush(wm.conn);
}

static void handle_configure_request(xcb_configure_request_event_t *ev)
{
    Client *c = find_client_window(ev->window);

    if (!c) {
        uint16_t mask = ev->value_mask;
        uint32_t values[7];
        int n = 0;
        if (mask & XCB_CONFIG_WINDOW_X)            values[n++] = (uint32_t)ev->x;
        if (mask & XCB_CONFIG_WINDOW_Y)            values[n++] = (uint32_t)ev->y;
        if (mask & XCB_CONFIG_WINDOW_WIDTH)        values[n++] = ev->width;
        if (mask & XCB_CONFIG_WINDOW_HEIGHT)       values[n++] = ev->height;
        if (mask & XCB_CONFIG_WINDOW_BORDER_WIDTH) values[n++] = ev->border_width;
        if (mask & XCB_CONFIG_WINDOW_SIBLING)      values[n++] = ev->sibling;
        if (mask & XCB_CONFIG_WINDOW_STACK_MODE)   values[n++] = ev->stack_mode;
        xcb_configure_window(wm.conn, ev->window, mask, values);
        xcb_flush(wm.conn);
        return;
    }

    if (c->maximized) {
        /* Ignore geometry requests while maximized; just re-affirm current state. */
        configure_frame(c);
        xcb_flush(wm.conn);
        return;
    }

    if (ev->value_mask & XCB_CONFIG_WINDOW_X)      c->x = ev->x;
    if (ev->value_mask & XCB_CONFIG_WINDOW_Y)      c->y = ev->y;
    if (ev->value_mask & XCB_CONFIG_WINDOW_WIDTH)  c->width = ev->width < MIN_CLIENT_W ? MIN_CLIENT_W : ev->width;
    if (ev->value_mask & XCB_CONFIG_WINDOW_HEIGHT) c->height = ev->height < MIN_CLIENT_H ? MIN_CLIENT_H : ev->height;

    configure_frame(c);
    xcb_flush(wm.conn);
}

static void handle_button_press(xcb_button_press_event_t *ev)
{
    Client *c = find_client_window(ev->event);
    if (!c)
        c = find_client_window(ev->child);
    if (!c)
        return;

    focus_client(c);

    int rel_x = ev->root_x - c->x;
    int rel_y = ev->root_y - c->y;

    if (client_deco_visible(c) && ev->detail == 1 && rel_y >= 0 && rel_y < TITLEBAR_H) {
        int w = c->frame_width;

        if (rel_x >= w - BUTTON_W) {
            close_client(c);
            return;
        }
        if (rel_x >= w - BUTTON_W * 2) {
            toggle_maximize(c, -1);
            return;
        }
        if (rel_x >= w - BUTTON_W * 3) {
            minimize_client(c);
            return;
        }

        wm.drag_mode = DRAG_MOVE;
        wm.drag_client = c;
        wm.drag_start_root_x = ev->root_x;
        wm.drag_start_root_y = ev->root_y;
        wm.drag_start_x = c->x;
        wm.drag_start_y = c->y;
        wm.drag_start_w = c->width;
        wm.drag_start_h = c->height;

        xcb_grab_pointer(wm.conn, 0, wm.root,
                         XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_POINTER_MOTION,
                         XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
                         XCB_NONE, XCB_NONE, XCB_CURRENT_TIME);
        xcb_flush(wm.conn);
        return;
    }

    if (ev->state & (MOD_ALT | MOD_META)) {
        if (ev->detail == 1)
            wm.drag_mode = DRAG_MOVE;
        else if (ev->detail == 3 && (ev->state & MOD_ALT))
            wm.drag_mode = DRAG_RESIZE;
        else
            return;

        wm.drag_client = c;
        wm.drag_start_root_x = ev->root_x;
        wm.drag_start_root_y = ev->root_y;
        wm.drag_start_x = c->x;
        wm.drag_start_y = c->y;
        wm.drag_start_w = c->width;
        wm.drag_start_h = c->height;

        xcb_grab_pointer(wm.conn, 0, wm.root,
                         XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_POINTER_MOTION,
                         XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
                         XCB_NONE, XCB_NONE, XCB_CURRENT_TIME);
        xcb_flush(wm.conn);
        return;
    }

    if (rel_y >= (client_deco_visible(c) ? TITLEBAR_H : 0)) {
        xcb_allow_events(wm.conn, XCB_ALLOW_REPLAY_POINTER, ev->time);
        xcb_flush(wm.conn);
    }
}

static void handle_motion(xcb_motion_notify_event_t *ev)
{
    if (!wm.drag_client || wm.drag_mode == DRAG_NONE)
        return;

    Client *c = wm.drag_client;
    int dx = ev->root_x - wm.drag_start_root_x;
    int dy = ev->root_y - wm.drag_start_root_y;

    if (c->maximized)
        toggle_maximize(c, 0);

    if (wm.drag_mode == DRAG_MOVE) {
        c->x = wm.drag_start_x + dx;
        c->y = wm.drag_start_y + dy;
    } else {
        c->width = wm.drag_start_w + dx;
        c->height = wm.drag_start_h + dy;
        if (c->width < MIN_CLIENT_W) c->width = MIN_CLIENT_W;
        if (c->height < MIN_CLIENT_H) c->height = MIN_CLIENT_H;
    }

    configure_frame(c);
    xcb_flush(wm.conn);
}

static void handle_button_release(xcb_button_release_event_t *ev)
{
    (void)ev;
    if (wm.drag_client) {
        Client *c = wm.drag_client;
        int new_output = output_index_for_point(c->x + c->width / 2, c->y + c->height / 2);
        if (new_output >= 0 && new_output != c->output) {
            c->output = new_output;
            c->desktop = wm.outputs[new_output].desktop;
            ewmh_update_wm_desktop(c);
        }
        xcb_ungrab_pointer(wm.conn, XCB_CURRENT_TIME);
        wm.drag_client = NULL;
        wm.drag_mode = DRAG_NONE;
        xcb_flush(wm.conn);
    }
}

static void handle_property_notify(xcb_property_notify_event_t *ev)
{
    Client *c = find_client_window(ev->window);
    if (!c)
        return;

    if (ev->atom == wm.atoms.net_wm_name || ev->atom == XCB_ATOM_WM_NAME) {
        get_title(c);
        draw_decoration(c);
        xcb_flush(wm.conn);
    }
}

static void handle_enter_notify(xcb_enter_notify_event_t *ev)
{
    Client *c = find_client_window(ev->event);
    if (!c)
        c = find_client_window(ev->child);
    if (c && c->mapped && !c->minimized && wm.outputs[c->output].desktop == c->desktop)
        focus_client(c);
}

static void handle_key_press(xcb_key_press_event_t *ev)
{
    uint16_t mods = ev->state & (XCB_MOD_MASK_SHIFT | XCB_MOD_MASK_LOCK |
                                 XCB_MOD_MASK_CONTROL | XCB_MOD_MASK_1 |
                                 XCB_MOD_MASK_2 | XCB_MOD_MASK_3 |
                                 XCB_MOD_MASK_4 | XCB_MOD_MASK_5);
    uint16_t clean_alt = mods & (MOD_ALT | XCB_MOD_MASK_SHIFT);
    uint16_t clean_meta = mods & (MOD_META | XCB_MOD_MASK_SHIFT);

    if (ev->detail == wm.key_tab) {
        if (clean_alt == MOD_ALT_SHIFT)        { cycle_focus(-1); return; }
        if (clean_alt == MOD_ALT)              { cycle_focus(+1); return; }
        if (clean_meta == MOD_META_SHIFT)      { cycle_output_desktop(-1); return; }
        if (clean_meta == MOD_META)            { cycle_output_desktop(+1); return; }
        return;
    }

    if (ev->detail == wm.key_up && clean_meta == MOD_META) {
        if (wm.focused)
            toggle_maximize(wm.focused, -1);
        return;
    }

    if (clean_alt == MOD_ALT) {
        int output_idx = wm.focused ? wm.focused->output : output_for_pointer();
        if (output_idx < 0)
            return;
        if (ev->detail == wm.key_1) { switch_workspace(output_idx, 0); return; }
        if (ev->detail == wm.key_2) { switch_workspace(output_idx, 1); return; }
        if (ev->detail == wm.key_3) { switch_workspace(output_idx, 2); return; }
        if (ev->detail == wm.key_4) { switch_workspace(output_idx, 3); return; }
    }
}

static void handle_net_wm_state(Client *c, uint32_t action, xcb_atom_t a1, xcb_atom_t a2)
{
    bool is_max = (a1 == wm.atoms.net_wm_state_maximized_vert || a1 == wm.atoms.net_wm_state_maximized_horz ||
                   a2 == wm.atoms.net_wm_state_maximized_vert || a2 == wm.atoms.net_wm_state_maximized_horz);
    bool is_hidden = (a1 == wm.atoms.net_wm_state_hidden || a2 == wm.atoms.net_wm_state_hidden);

    /* action: 0=remove, 1=add, 2=toggle (_NET_WM_STATE_TOGGLE) */
    if (is_max) {
        int want = (action == 2) ? -1 : (action == 1 ? 1 : 0);
        toggle_maximize(c, want);
    }
    if (is_hidden) {
        bool want_hidden = (action == 2) ? !c->minimized : (action == 1);
        if (want_hidden)
            minimize_client(c);
        else
            restore_client(c);
    }
}

static void handle_client_message(xcb_client_message_event_t *ev)
{
    if (ev->type == wm.atoms.kiwm_set_output_desktop) {
        switch_workspace((int)ev->data.data32[0], (int)ev->data.data32[1]);
        return;
    }

    Client *c = find_client_window(ev->window);
    if (!c)
        return;

    if (ev->type == wm.atoms.net_active_window) {
        activate_client(c);
    } else if (ev->type == wm.atoms.net_close_window) {
        close_client(c);
    } else if (ev->type == wm.atoms.wm_change_state) {
        if (ev->data.data32[0] == WM_STATE_ICONIC)
            minimize_client(c);
    } else if (ev->type == wm.atoms.net_wm_state) {
        handle_net_wm_state(c, ev->data.data32[0],
                            (xcb_atom_t)ev->data.data32[1], (xcb_atom_t)ev->data.data32[2]);
    } else if (ev->type == wm.atoms.net_wm_desktop) {
        set_client_desktop(c, (int)ev->data.data32[0]);
    }
}

static void handle_event(xcb_generic_event_t *event)
{
    uint8_t type = event->response_type & ~0x80;

    if (wm.randr_event_base && type == wm.randr_event_base + XCB_RANDR_SCREEN_CHANGE_NOTIFY) {
        outputs_refresh();
        return;
    }

    switch (type) {
    case XCB_MAP_REQUEST:
        handle_map_request((xcb_map_request_event_t *)event);
        break;
    case XCB_CONFIGURE_REQUEST:
        handle_configure_request((xcb_configure_request_event_t *)event);
        break;
    case XCB_DESTROY_NOTIFY: {
        xcb_destroy_notify_event_t *ev = (xcb_destroy_notify_event_t *)event;
        Client *c = find_client_window(ev->window);
        if (c)
            unmanage(c);
        break;
    }
    case XCB_UNMAP_NOTIFY: {
        xcb_unmap_notify_event_t *ev = (xcb_unmap_notify_event_t *)event;
        Client *c = find_client_window(ev->window);
        if (c && ev->window == c->window) {
            if (c->ignore_unmap > 0) {
                /* Spurious auto-unmap from reparenting an already-mapped
                 * pre-existing window at startup -- not a real withdrawal. */
                c->ignore_unmap--;
                break;
            }
            if (c->mapped && wm.outputs[c->output].desktop == c->desktop && !c->minimized) {
                c->mapped = false;
                xcb_unmap_window(wm.conn, c->frame);
            }
        }
        break;
    }
    case XCB_BUTTON_PRESS:
        handle_button_press((xcb_button_press_event_t *)event);
        break;
    case XCB_BUTTON_RELEASE:
        handle_button_release((xcb_button_release_event_t *)event);
        break;
    case XCB_MOTION_NOTIFY:
        handle_motion((xcb_motion_notify_event_t *)event);
        break;
    case XCB_PROPERTY_NOTIFY:
        handle_property_notify((xcb_property_notify_event_t *)event);
        break;
    case XCB_ENTER_NOTIFY:
        handle_enter_notify((xcb_enter_notify_event_t *)event);
        break;
    case XCB_KEY_PRESS:
        handle_key_press((xcb_key_press_event_t *)event);
        break;
    case XCB_CLIENT_MESSAGE:
        handle_client_message((xcb_client_message_event_t *)event);
        break;
    case XCB_SELECTION_CLEAR: {
        xcb_selection_clear_event_t *ev = (xcb_selection_clear_event_t *)event;
        if (ev->owner == wm.sel_win && ev->selection == wm.sn_atom) {
            /* Replaced by --replace: let go of SubstructureRedirect right
             * away so the new WM's acquire_wm_selection() probe succeeds,
             * then unwind normally through cleanup(). */
            fprintf(stderr, "kiwm: replaced by another window manager, exiting\n");
            uint32_t mask = XCB_EVENT_MASK_NO_EVENT;
            xcb_change_window_attributes(wm.conn, wm.root, XCB_CW_EVENT_MASK, &mask);
            xcb_flush(wm.conn);
            wm.running = false;
        }
        break;
    }
    case XCB_EXPOSE: {
        xcb_expose_event_t *ev = (xcb_expose_event_t *)event;
        Client *c = find_client_window(ev->window);
        if (c) {
            draw_decoration(c);
            xcb_flush(wm.conn);
        }
        break;
    }
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* --replace: ICCCM manager selection                                   */
/* ------------------------------------------------------------------ */

static bool acquire_wm_selection(int screen_nbr, bool replace)
{
    char selname[32];
    snprintf(selname, sizeof(selname), "WM_S%d", screen_nbr);
    wm.sn_atom = intern_atom(selname);
    wm.atoms.manager = intern_atom("MANAGER");

    xcb_get_selection_owner_reply_t *owner_reply = xcb_get_selection_owner_reply(
        wm.conn, xcb_get_selection_owner(wm.conn, wm.sn_atom), NULL);
    xcb_window_t old_owner = owner_reply ? owner_reply->owner : XCB_NONE;
    free(owner_reply);

    if (old_owner != XCB_NONE && !replace) {
        fprintf(stderr, "kiwm: another window manager is already running "
                        "(pass --replace to take over)\n");
        return false;
    }

    wm.sel_win = xcb_generate_id(wm.conn);
    xcb_create_window(wm.conn, XCB_COPY_FROM_PARENT, wm.sel_win, wm.root,
                      -1, -1, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      wm.screen->root_visual, 0, NULL);

    if (old_owner != XCB_NONE) {
        /* A well-behaved previous kiwm (see the SELECTION_CLEAR handler in
         * handle_event) watches its own selection window and releases
         * SubstructureRedirect + exits as soon as it loses WM_Sn -- no
         * StructureNotify polling on old_owner needed for that case. This
         * DestroyNotify wait is only a fallback for WMs that just quit
         * outright without the SelectionClear protocol. */
        uint32_t mask = XCB_EVENT_MASK_STRUCTURE_NOTIFY;
        xcb_change_window_attributes(wm.conn, old_owner, XCB_CW_EVENT_MASK, &mask);
    }

    xcb_set_selection_owner(wm.conn, wm.sel_win, wm.sn_atom, XCB_CURRENT_TIME);
    xcb_flush(wm.conn);

    if (old_owner != XCB_NONE) {
        fprintf(stderr, "kiwm: waiting for previous window manager to release control...\n");
        time_t start = time(NULL);
        for (;;) {
            xcb_generic_error_t *err = xcb_request_check(wm.conn,
                xcb_change_window_attributes_checked(wm.conn, wm.root,
                    XCB_CW_EVENT_MASK, (uint32_t[]){ XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT }));
            if (!err) {
                /* Old WM already let go: undo the probe grab, real setup
                 * below re-selects the full event mask anyway. */
                break;
            }
            free(err);

            xcb_generic_event_t *ev = xcb_poll_for_event(wm.conn);
            if (ev) {
                uint8_t type = ev->response_type & ~0x80;
                if (type == XCB_DESTROY_NOTIFY &&
                    ((xcb_destroy_notify_event_t *)ev)->window == old_owner) {
                    free(ev);
                    break;
                }
                free(ev);
            } else {
                if (time(NULL) - start > 3) {
                    fprintf(stderr, "kiwm: timed out waiting for previous WM, continuing anyway\n");
                    break;
                }
                struct timespec ts = { 0, 20000000L };
                nanosleep(&ts, NULL);
            }
        }
    }

    xcb_client_message_event_t ev = { 0 };
    ev.response_type = XCB_CLIENT_MESSAGE;
    ev.format = 32;
    ev.window = wm.root;
    ev.type = wm.atoms.manager;
    ev.data.data32[0] = XCB_CURRENT_TIME;
    ev.data.data32[1] = wm.sn_atom;
    ev.data.data32[2] = wm.sel_win;
    xcb_send_event(wm.conn, 0, wm.root, XCB_EVENT_MASK_STRUCTURE_NOTIFY, (const char *)&ev);
    xcb_flush(wm.conn);

    return true;
}

/* ------------------------------------------------------------------ */
/* startup                                                              */
/* ------------------------------------------------------------------ */

static void ewmh_init_atoms(void)
{
    wm.atoms.wm_protocols = intern_atom("WM_PROTOCOLS");
    wm.atoms.wm_delete_window = intern_atom("WM_DELETE_WINDOW");
    wm.atoms.wm_state = intern_atom("WM_STATE");
    wm.atoms.wm_change_state = intern_atom("WM_CHANGE_STATE");
    wm.atoms.net_wm_name = intern_atom("_NET_WM_NAME");
    wm.atoms.utf8_string = intern_atom("UTF8_STRING");

    wm.atoms.net_supported = intern_atom("_NET_SUPPORTED");
    wm.atoms.net_supporting_wm_check = intern_atom("_NET_SUPPORTING_WM_CHECK");
    wm.atoms.net_client_list = intern_atom("_NET_CLIENT_LIST");
    wm.atoms.net_client_list_stacking = intern_atom("_NET_CLIENT_LIST_STACKING");
    wm.atoms.net_active_window = intern_atom("_NET_ACTIVE_WINDOW");
    wm.atoms.net_close_window = intern_atom("_NET_CLOSE_WINDOW");
    wm.atoms.net_number_of_desktops = intern_atom("_NET_NUMBER_OF_DESKTOPS");
    wm.atoms.net_current_desktop = intern_atom("_NET_CURRENT_DESKTOP");
    wm.atoms.net_wm_desktop = intern_atom("_NET_WM_DESKTOP");
    wm.atoms.net_workarea = intern_atom("_NET_WORKAREA");

    wm.atoms.net_wm_state = intern_atom("_NET_WM_STATE");
    wm.atoms.net_wm_state_hidden = intern_atom("_NET_WM_STATE_HIDDEN");
    wm.atoms.net_wm_state_maximized_vert = intern_atom("_NET_WM_STATE_MAXIMIZED_VERT");
    wm.atoms.net_wm_state_maximized_horz = intern_atom("_NET_WM_STATE_MAXIMIZED_HORZ");
    wm.atoms.net_wm_state_skip_taskbar = intern_atom("_NET_WM_STATE_SKIP_TASKBAR");

    wm.atoms.net_wm_window_type = intern_atom("_NET_WM_WINDOW_TYPE");
    wm.atoms.net_wm_window_type_normal = intern_atom("_NET_WM_WINDOW_TYPE_NORMAL");
    wm.atoms.net_wm_window_type_dock = intern_atom("_NET_WM_WINDOW_TYPE_DOCK");
    wm.atoms.net_wm_window_type_desktop = intern_atom("_NET_WM_WINDOW_TYPE_DESKTOP");
    wm.atoms.net_wm_window_type_toolbar = intern_atom("_NET_WM_WINDOW_TYPE_TOOLBAR");
    wm.atoms.net_wm_window_type_menu = intern_atom("_NET_WM_WINDOW_TYPE_MENU");

    /* Custom, per kiwm-kicomp-projeto.md section 6: independent virtual
     * desktops per output, since EWMH itself has no such concept. */
    wm.atoms.kiwm_outputs = intern_atom("_KIWM_OUTPUTS");
    wm.atoms.kiwm_output_desktop = intern_atom("_KIWM_OUTPUT_DESKTOP");
    wm.atoms.kiwm_num_output_desktops = intern_atom("_KIWM_NUM_OUTPUT_DESKTOPS");
    wm.atoms.kiwm_set_output_desktop = intern_atom("_KIWM_SET_OUTPUT_DESKTOP");
}

static void ewmh_init_supported(void)
{
    xcb_atom_t supported[] = {
        wm.atoms.net_supported, wm.atoms.net_supporting_wm_check,
        wm.atoms.net_client_list, wm.atoms.net_client_list_stacking,
        wm.atoms.net_active_window, wm.atoms.net_close_window,
        wm.atoms.net_number_of_desktops, wm.atoms.net_current_desktop,
        wm.atoms.net_wm_desktop, wm.atoms.net_workarea,
        wm.atoms.net_wm_state, wm.atoms.net_wm_state_hidden,
        wm.atoms.net_wm_state_maximized_vert, wm.atoms.net_wm_state_maximized_horz,
        wm.atoms.net_wm_state_skip_taskbar,
        wm.atoms.net_wm_window_type, wm.atoms.net_wm_window_type_normal,
        wm.atoms.net_wm_window_type_dock, wm.atoms.net_wm_window_type_desktop,
        wm.atoms.net_wm_window_type_toolbar, wm.atoms.net_wm_window_type_menu,
        wm.atoms.net_wm_name,
    };
    xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE, wm.root,
                        wm.atoms.net_supported, XCB_ATOM_ATOM, 32,
                        sizeof(supported) / sizeof(supported[0]), supported);

    uint32_t numws = NUM_WORKSPACES;
    xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE, wm.root,
                        wm.atoms.net_number_of_desktops, XCB_ATOM_CARDINAL, 32, 1, &numws);
    ewmh_set_current_desktop(0);
}

static void ewmh_init_supporting_wm_check(void)
{
    wm.check_win = xcb_generate_id(wm.conn);
    xcb_create_window(wm.conn, XCB_COPY_FROM_PARENT, wm.check_win, wm.root,
                      -1, -1, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      wm.screen->root_visual, 0, NULL);

    xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE, wm.check_win,
                        wm.atoms.net_supporting_wm_check, XCB_ATOM_WINDOW, 32, 1, &wm.check_win);
    xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE, wm.root,
                        wm.atoms.net_supporting_wm_check, XCB_ATOM_WINDOW, 32, 1, &wm.check_win);

    xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE, wm.check_win,
                        wm.atoms.net_wm_name, wm.atoms.utf8_string, 8, 4, "kiwm");
}

static void setup_wm(bool replace)
{
    int preferred_screen = 0;
    wm.conn = xcb_connect(NULL, &preferred_screen);
    if (xcb_connection_has_error(wm.conn))
        die("could not connect to X server");

    const xcb_setup_t *setup = xcb_get_setup(wm.conn);
    xcb_screen_iterator_t it = xcb_setup_roots_iterator(setup);
    for (int i = 0; i < preferred_screen && it.rem; i++)
        xcb_screen_next(&it);

    wm.screen = it.data;
    wm.root = wm.screen->root;
    wm.visual = find_root_visual(wm.screen);
    if (!wm.visual)
        die("could not find root visual");

    ewmh_init_atoms();

    if (!acquire_wm_selection(preferred_screen, replace)) {
        xcb_disconnect(wm.conn);
        exit(EXIT_FAILURE);
    }

    wm.hide_deco_on_maximize = false;
    const char *hide_deco_env = getenv("KIWM_HIDE_DECO_ON_MAXIMIZE");
    if (hide_deco_env)
        wm.hide_deco_on_maximize = !(strcmp(hide_deco_env, "0") == 0 || strcmp(hide_deco_env, "no") == 0);

    load_decoration();

    /* RandR: outputs are the unit of presentation (section 4) even in a
     * WM without a compositor -- we still need it for per-output desktops. */
    const xcb_query_extension_reply_t *randr_ext = xcb_get_extension_data(wm.conn, &xcb_randr_id);
    if (randr_ext && randr_ext->present) {
        wm.randr_event_base = randr_ext->first_event;
        xcb_randr_select_input(wm.conn, wm.root, XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE);
    }
    outputs_refresh();

    wm.key_tab = keysym_to_keycode(XK_Tab);
    wm.key_1   = keysym_to_keycode(XK_1);
    wm.key_2   = keysym_to_keycode(XK_2);
    wm.key_3   = keysym_to_keycode(XK_3);
    wm.key_4   = keysym_to_keycode(XK_4);
    wm.key_up  = keysym_to_keycode(XK_Up);

    /* SubstructureRedirectMask is the actual WM ownership lock; only one
     * client can select it on the root window at a time. */
    uint32_t mask = XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT |
                    XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY |
                    XCB_EVENT_MASK_STRUCTURE_NOTIFY |
                    XCB_EVENT_MASK_PROPERTY_CHANGE |
                    XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE |
                    XCB_EVENT_MASK_POINTER_MOTION |
                    XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_LEAVE_WINDOW |
                    XCB_EVENT_MASK_KEY_PRESS;

    xcb_generic_error_t *error = xcb_request_check(wm.conn,
        xcb_change_window_attributes_checked(wm.conn, wm.root, XCB_CW_EVENT_MASK, &mask));
    if (error) {
        free(error);
        die("another window manager is already running (this should not "
            "happen after acquiring the manager selection)");
    }

    if (wm.key_tab) {
        xcb_grab_key(wm.conn, 1, wm.root, MOD_ALT, wm.key_tab, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
        xcb_grab_key(wm.conn, 1, wm.root, MOD_ALT_SHIFT, wm.key_tab, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
        xcb_grab_key(wm.conn, 1, wm.root, MOD_META, wm.key_tab, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
        xcb_grab_key(wm.conn, 1, wm.root, MOD_META_SHIFT, wm.key_tab, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
    }
    if (wm.key_1) xcb_grab_key(wm.conn, 1, wm.root, MOD_ALT, wm.key_1, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
    if (wm.key_2) xcb_grab_key(wm.conn, 1, wm.root, MOD_ALT, wm.key_2, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
    if (wm.key_3) xcb_grab_key(wm.conn, 1, wm.root, MOD_ALT, wm.key_3, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
    if (wm.key_4) xcb_grab_key(wm.conn, 1, wm.root, MOD_ALT, wm.key_4, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
    if (wm.key_up) xcb_grab_key(wm.conn, 1, wm.root, MOD_META, wm.key_up, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);

    ewmh_init_supported();
    ewmh_init_supporting_wm_check();
    manage_existing_windows();
    ewmh_update_client_list();
    ewmh_update_active_window();

    xcb_flush(wm.conn);

    fprintf(stderr, "kiwm: started on screen %dx%d (workspaces 1-%d per output)\n",
            wm.screen->width_in_pixels, wm.screen->height_in_pixels, NUM_WORKSPACES);
}

static void cleanup(void)
{
    Client *c = wm.clients;
    while (c) {
        Client *next = c->next;
        xcb_unmap_window(wm.conn, c->frame);
        xcb_reparent_window(wm.conn, c->window, wm.root, c->x, c->y);
        xcb_destroy_window(wm.conn, c->frame);
        free(c);
        c = next;
    }
    wm.clients = NULL;

    if (wm.deco_bg)
        cairo_surface_destroy(wm.deco_bg);

    if (wm.conn)
        xcb_disconnect(wm.conn);
}

int main(int argc, char **argv)
{
    bool replace = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--replace") == 0)
            replace = true;
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("usage: kiwm [--replace]\n");
            return EXIT_SUCCESS;
        }
    }

    memset(&wm, 0, sizeof(wm));
    wm.running = true;

    setup_wm(replace);

    while (wm.running) {
        xcb_generic_event_t *event = xcb_wait_for_event(wm.conn);
        if (!event)
            break;
        handle_event(event);
        free(event);
    }

    cleanup();
    return 0;
}
