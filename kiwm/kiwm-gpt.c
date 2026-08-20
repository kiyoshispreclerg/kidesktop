/*
 * kiwm - KiDesktop Window Manager
 *
 * Prototype 0.1
 *
 * A deliberately small X11 stacking WM with Cairo decorations.
 *
 * Features:
 *   - XCB connection / root ownership
 *   - client reparenting into decorated frame windows
 *   - Cairo titlebar decoration
 *   - focus (click-to-focus + enter) and stacking
 *   - titlebar drag = move
 *   - Alt+Left drag = move
 *   - Alt+Right drag = resize
 *   - titlebar buttons: minimize / maximize / close
 *   - no decoration for dock / desktop / toolbar / menu windows
 *   - simple global workspaces (Alt+1..4)
 *   - Alt+Tab / Alt+Shift+Tab cycle focus
 *
 * This is a prototype: no full EWMH / RandR per-output / compositor yet.

 cc -Wall -Wextra -O2 -o kiwm kiwm.c $(pkg-config --cflags --libs xcb cairo cairo-xcb)


 */

#define _POSIX_C_SOURCE 200809L

#include <xcb/xcb.h>
#include <xcb/xproto.h>
#include <cairo/cairo.h>
#include <cairo/cairo-xcb.h>

#include <X11/keysym.h>

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TITLEBAR_H       30
#define BORDER_W          1
#define BUTTON_W          28
#define MIN_CLIENT_W     120
#define MIN_CLIENT_H      80
#define MAX_CLIENTS      256
#define NUM_WORKSPACES    4

#define MOD_ALT           XCB_MOD_MASK_1
#define MOD_ALT_SHIFT     (XCB_MOD_MASK_1 | XCB_MOD_MASK_SHIFT)

typedef struct Client Client;

typedef enum {
    DRAG_NONE,
    DRAG_MOVE,
    DRAG_RESIZE
} DragMode;

struct Client {
    xcb_window_t window;       /* application window */
    xcb_window_t frame;        /* decorated frame */
    int x, y;
    int width, height;         /* client dimensions */
    int frame_width;
    int frame_height;
    bool mapped;
    bool maximized;
    int workspace;             /* 0 .. NUM_WORKSPACES-1 */

    char title[256];

    Client *next;
};

typedef struct {
    xcb_connection_t *conn;
    xcb_screen_t *screen;
    xcb_window_t root;
    xcb_visualtype_t *visual;

    xcb_atom_t wm_protocols;
    xcb_atom_t wm_delete_window;
    xcb_atom_t wm_name;
    xcb_atom_t utf8_string;

    xcb_atom_t net_wm_window_type;
    xcb_atom_t net_wm_window_type_dock;
    xcb_atom_t net_wm_window_type_desktop;
    xcb_atom_t net_wm_window_type_toolbar;
    xcb_atom_t net_wm_window_type_menu;

    Client *clients;
    Client *focused;

    int current_workspace;

    DragMode drag_mode;
    Client *drag_client;
    int drag_start_root_x;
    int drag_start_root_y;
    int drag_start_x;
    int drag_start_y;
    int drag_start_w;
    int drag_start_h;

    /* cached keycodes for bindings */
    xcb_keycode_t key_tab;
    xcb_keycode_t key_1;
    xcb_keycode_t key_2;
    xcb_keycode_t key_3;
    xcb_keycode_t key_4;

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

static xcb_visualtype_t *find_root_visual(xcb_screen_t *screen)
{
    xcb_depth_iterator_t depth_iter =
        xcb_screen_allowed_depths_iterator(screen);

    for (; depth_iter.rem; xcb_depth_next(&depth_iter)) {
        xcb_visualtype_iterator_t visual_iter =
            xcb_depth_visuals_iterator(depth_iter.data);

        for (; visual_iter.rem; xcb_visualtype_next(&visual_iter)) {
            if (visual_iter.data->visual_id == screen->root_visual)
                return visual_iter.data;
        }
    }

    return NULL;
}

static xcb_keycode_t keysym_to_keycode(xcb_keysym_t keysym)
{
    const xcb_setup_t *setup = xcb_get_setup(wm.conn);
    xcb_keycode_t min_kc = setup->min_keycode;
    xcb_keycode_t max_kc = setup->max_keycode;

    xcb_get_keyboard_mapping_cookie_t cookie =
        xcb_get_keyboard_mapping(wm.conn, min_kc, max_kc - min_kc + 1);

    xcb_get_keyboard_mapping_reply_t *reply =
        xcb_get_keyboard_mapping_reply(wm.conn, cookie, NULL);

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

static bool should_decorate(xcb_window_t window)
{
    if (wm.net_wm_window_type == XCB_ATOM_NONE)
        return true;

    xcb_get_property_cookie_t cookie =
        xcb_get_property(wm.conn, 0, window,
                         wm.net_wm_window_type, XCB_ATOM_ATOM,
                         0, 32);

    xcb_get_property_reply_t *reply =
        xcb_get_property_reply(wm.conn, cookie, NULL);

    if (!reply || reply->type != XCB_ATOM_ATOM || reply->format != 32) {
        free(reply);
        return true;
    }

    xcb_atom_t *atoms = xcb_get_property_value(reply);
    int n = xcb_get_property_value_length(reply) / (int)sizeof(xcb_atom_t);

    for (int i = 0; i < n; i++) {
        if (atoms[i] == wm.net_wm_window_type_dock ||
            atoms[i] == wm.net_wm_window_type_desktop ||
            atoms[i] == wm.net_wm_window_type_toolbar ||
            atoms[i] == wm.net_wm_window_type_menu) {
            free(reply);
            return false;
        }
    }

    free(reply);
    return true;
}

static Client *find_client_window(xcb_window_t window)
{
    for (Client *c = wm.clients; c; c = c->next) {
        if (c->window == window || c->frame == window)
            return c;
    }
    return NULL;
}

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

static void set_wm_state_normal(Client *c)
{
    xcb_atom_t wm_state = intern_atom("WM_STATE");
    if (wm_state == XCB_ATOM_NONE)
        return;

    uint32_t state[] = { 1, XCB_ATOM_NONE };
    xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE,
                        c->window, wm_state, wm_state, 32, 2, state);
}

static void set_wm_state_iconic(Client *c)
{
    xcb_atom_t wm_state = intern_atom("WM_STATE");
    if (wm_state == XCB_ATOM_NONE)
        return;

    uint32_t state[] = { 3, XCB_ATOM_NONE };
    xcb_change_property(wm.conn, XCB_PROP_MODE_REPLACE,
                        c->window, wm_state, wm_state, 32, 2, state);
}

static void send_delete(Client *c)
{
    if (wm.wm_protocols == XCB_ATOM_NONE ||
        wm.wm_delete_window == XCB_ATOM_NONE)
        return;

    xcb_client_message_event_t ev = {
        .response_type = XCB_CLIENT_MESSAGE,
        .format = 32,
        .window = c->window,
        .type = wm.wm_protocols
    };

    ev.data.data32[0] = wm.wm_delete_window;
    ev.data.data32[1] = XCB_CURRENT_TIME;

    xcb_send_event(wm.conn, 0, c->window,
                   XCB_EVENT_MASK_NO_EVENT,
                   (const char *)&ev);
}

static void get_title(Client *c)
{
    memset(c->title, 0, sizeof(c->title));

    xcb_get_property_cookie_t cookie =
        xcb_get_property(wm.conn, 0, c->window,
                         wm.wm_name, XCB_GET_PROPERTY_TYPE_ANY,
                         0, 256);

    xcb_get_property_reply_t *reply =
        xcb_get_property_reply(wm.conn, cookie, NULL);

    if (!reply)
        return;

    int len = xcb_get_property_value_length(reply);
    if (len > 0) {
        if (len >= (int)sizeof(c->title))
            len = sizeof(c->title) - 1;
        memcpy(c->title, xcb_get_property_value(reply), len);
        c->title[len] = '\0';
    }

    free(reply);

    if (!c->title[0])
        snprintf(c->title, sizeof(c->title), "Untitled");
}

static void draw_button(cairo_t *cr, double x, char glyph)
{
    cairo_set_source_rgba(cr, 0.15, 0.15, 0.18, 1.0);
    cairo_rectangle(cr, x, 0, BUTTON_W, TITLEBAR_H);
    cairo_fill(cr);

    cairo_set_source_rgba(cr, 0.80, 0.80, 0.84, 1.0);
    cairo_set_line_width(cr, 1.5);

    double cx = x + BUTTON_W / 2.0;
    double cy = TITLEBAR_H / 2.0;

    if (glyph == '-') {
        cairo_move_to(cr, cx - 6, cy);
        cairo_line_to(cr, cx + 6, cy);
        cairo_stroke(cr);
    } else if (glyph == '+') {
        cairo_rectangle(cr, cx - 6, cy - 6, 12, 12);
        cairo_stroke(cr);
    } else if (glyph == 'x') {
        cairo_move_to(cr, cx - 5, cy - 5);
        cairo_line_to(cr, cx + 5, cy + 5);
        cairo_move_to(cr, cx + 5, cy - 5);
        cairo_line_to(cr, cx - 5, cy + 5);
        cairo_stroke(cr);
    }
}

static void draw_decoration(Client *c)
{
    int w = c->frame_width;
    int h = c->frame_height;

    if (!wm.visual)
        return;

    cairo_surface_t *surface =
        cairo_xcb_surface_create(wm.conn, c->frame,
                                 wm.visual, w, h);

    cairo_t *cr = cairo_create(surface);

    /* Frame background */
    cairo_set_source_rgb(cr, 0.10, 0.10, 0.12);
    cairo_paint(cr);

    /* Titlebar */
    if (c == wm.focused)
        cairo_set_source_rgb(cr, 0.18, 0.27, 0.42);
    else
        cairo_set_source_rgb(cr, 0.14, 0.14, 0.17);

    cairo_rectangle(cr, 0, 0, w, TITLEBAR_H);
    cairo_fill(cr);

    /* Bottom border */
    cairo_set_source_rgb(cr, 0.07, 0.07, 0.08);
    cairo_rectangle(cr, 0, TITLEBAR_H, w, BORDER_W);
    cairo_fill(cr);

    /* Title */
    cairo_select_font_face(cr, "sans",
                           CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 13.0);
    cairo_set_source_rgb(cr, 0.92, 0.92, 0.95);

    cairo_text_extents_t ext;
    cairo_text_extents(cr, c->title, &ext);

    double title_x = 10.0;
    double title_y = (TITLEBAR_H - ext.height) / 2.0 - ext.y_bearing;

    cairo_move_to(cr, title_x, title_y);
    cairo_show_text(cr, c->title);

    /* Buttons from right to left. */
    draw_button(cr, w - BUTTON_W * 3, '-');
    draw_button(cr, w - BUTTON_W * 2, '+');
    draw_button(cr, w - BUTTON_W,     'x');

    cairo_destroy(cr);
    cairo_surface_flush(surface);
    cairo_surface_destroy(surface);
}

static void configure_frame(Client *c)
{
    c->frame_width = c->width;
    c->frame_height = c->height + TITLEBAR_H;

    uint32_t values[] = {
        (uint32_t)c->x,
        (uint32_t)c->y,
        (uint32_t)c->frame_width,
        (uint32_t)c->frame_height
    };

    xcb_configure_window(wm.conn, c->frame,
                         XCB_CONFIG_WINDOW_X |
                         XCB_CONFIG_WINDOW_Y |
                         XCB_CONFIG_WINDOW_WIDTH |
                         XCB_CONFIG_WINDOW_HEIGHT,
                         values);

    uint32_t client_values[] = {
        0,
        TITLEBAR_H,
        (uint32_t)c->width,
        (uint32_t)c->height
    };

    xcb_configure_window(wm.conn, c->window,
                         XCB_CONFIG_WINDOW_X |
                         XCB_CONFIG_WINDOW_Y |
                         XCB_CONFIG_WINDOW_WIDTH |
                         XCB_CONFIG_WINDOW_HEIGHT,
                         client_values);

    draw_decoration(c);
}

static void focus_client(Client *c)
{
    if (!c)
        return;

    Client *old = wm.focused;

    if (old != c) {
        wm.focused = c;

        xcb_set_input_focus(wm.conn,
                            XCB_INPUT_FOCUS_POINTER_ROOT,
                            c->window,
                            XCB_CURRENT_TIME);

        if (old)
            draw_decoration(old);
    }

    xcb_configure_window(wm.conn, c->frame,
                         XCB_CONFIG_WINDOW_STACK_MODE,
                         (uint32_t[]){ XCB_STACK_MODE_ABOVE });

    draw_decoration(c);

    xcb_flush(wm.conn);
}

static void switch_workspace(int ws)
{
    if (ws < 0 || ws >= NUM_WORKSPACES || ws == wm.current_workspace)
        return;

    int old = wm.current_workspace;
    wm.current_workspace = ws;

    Client *to_focus = NULL;

    for (Client *c = wm.clients; c; c = c->next) {
        if (c->workspace == old) {
            if (c->mapped)
                xcb_unmap_window(wm.conn, c->frame);
        } else if (c->workspace == ws) {
            if (c->mapped) {
                xcb_map_window(wm.conn, c->frame);
                if (!to_focus)
                    to_focus = c;
            }
        }
    }

    if (to_focus)
        focus_client(to_focus);
    else
        wm.focused = NULL;

    xcb_flush(wm.conn);

    fprintf(stderr, "kiwm: workspace %d\n", ws + 1);
}

static void cycle_focus(int direction)
{
    Client *eligible[MAX_CLIENTS];
    int n = 0;
    int current_idx = -1;

    for (Client *c = wm.clients; c && n < MAX_CLIENTS; c = c->next) {
        if (c->workspace == wm.current_workspace && c->mapped) {
            if (c == wm.focused)
                current_idx = n;
            eligible[n++] = c;
        }
    }

    if (n == 0)
        return;

    int next;
    if (current_idx < 0)
        next = 0;
    else
        next = (current_idx + direction + n) % n;

    focus_client(eligible[next]);
}

static void unmanage(Client *c)
{
    if (!c)
        return;

    int fx = c->x;
    int fy = c->y;
    int fw = c->frame_width;
    int fh = c->frame_height;

    if (c == wm.focused)
        wm.focused = NULL;

    xcb_unmap_window(wm.conn, c->frame);

    /*
     * Reparent may fail with BadWindow if the client already destroyed
     * itself (DestroyNotify path). Ignore the error.
     */
    xcb_void_cookie_t reparent_cookie =
        xcb_reparent_window_checked(wm.conn, c->window, wm.root, c->x, c->y);
    xcb_generic_error_t *err = xcb_request_check(wm.conn, reparent_cookie);
    if (err)
        free(err);

    xcb_destroy_window(wm.conn, c->frame);

    /*
     * Clear the old frame area on the root so the decoration does not
     * leave a "ghost" until the next expose/refresh.
     */
    if (fw > 0 && fh > 0)
        xcb_clear_area(wm.conn, 0, wm.root, fx, fy, (uint16_t)fw, (uint16_t)fh);

    remove_client(c);
    xcb_flush(wm.conn);
}

static void manage(xcb_window_t window)
{
    if (find_client_window(window))
        return;

    xcb_get_window_attributes_cookie_t cookie =
        xcb_get_window_attributes(wm.conn, window);

    xcb_get_window_attributes_reply_t *attr =
        xcb_get_window_attributes_reply(wm.conn, cookie, NULL);

    if (!attr)
        return;

    if (attr->override_redirect || attr->map_state == XCB_MAP_STATE_VIEWABLE) {
        free(attr);
        return;
    }

    free(attr);

    /* Panels, docks, desktops, etc. — map without decoration */
    if (!should_decorate(window)) {
        xcb_map_window(wm.conn, window);
        xcb_flush(wm.conn);
        return;
    }

    xcb_get_geometry_cookie_t gc =
        xcb_get_geometry(wm.conn, window);

    xcb_get_geometry_reply_t *geo =
        xcb_get_geometry_reply(wm.conn, gc, NULL);

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

    get_title(c);

    c->frame = xcb_generate_id(wm.conn);

    uint32_t values[] = {
        XCB_EVENT_MASK_EXPOSURE |
        XCB_EVENT_MASK_BUTTON_PRESS |
        XCB_EVENT_MASK_BUTTON_RELEASE |
        XCB_EVENT_MASK_POINTER_MOTION |
        XCB_EVENT_MASK_ENTER_WINDOW |
        XCB_EVENT_MASK_LEAVE_WINDOW,
        0
    };

    uint32_t mask =
        XCB_CW_EVENT_MASK |
        XCB_CW_BORDER_PIXEL;

    xcb_create_window(wm.conn,
                      wm.screen->root_depth,
                      c->frame,
                      wm.root,
                      c->x,
                      c->y,
                      c->width,
                      c->height + TITLEBAR_H,
                      0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      wm.screen->root_visual,
                      mask,
                      values);

    /*
     * Select structure/property events on the client before reparenting.
     * PropertyNotify lets us redraw the title if WM_NAME changes.
     */
    uint32_t client_mask =
        XCB_EVENT_MASK_PROPERTY_CHANGE |
        XCB_EVENT_MASK_STRUCTURE_NOTIFY |
        XCB_EVENT_MASK_FOCUS_CHANGE;

    xcb_change_window_attributes(wm.conn, window,
                                 XCB_CW_EVENT_MASK,
                                 &client_mask);

    /*
     * Passive grab so we receive ButtonPress on the client area
     * (click-to-focus). We replay the event afterwards so the
     * application still gets the click.
     */
    xcb_grab_button(wm.conn, 0, window,
                    XCB_EVENT_MASK_BUTTON_PRESS,
                    XCB_GRAB_MODE_SYNC,
                    XCB_GRAB_MODE_ASYNC,
                    XCB_NONE, XCB_NONE,
                    XCB_BUTTON_INDEX_1,
                    XCB_MOD_MASK_ANY);

    xcb_reparent_window(wm.conn, window, c->frame, 0, TITLEBAR_H);

    xcb_map_window(wm.conn, window);
    xcb_map_window(wm.conn, c->frame);

    c->mapped = true;
    c->workspace = wm.current_workspace;
    set_wm_state_normal(c);

    c->next = wm.clients;
    wm.clients = c;

    configure_frame(c);
    focus_client(c);
}

static void handle_map_request(xcb_map_request_event_t *ev)
{
    Client *c = find_client_window(ev->window);

    if (!c) {
        manage(ev->window);
    } else {
        xcb_map_window(wm.conn, c->frame);
        c->mapped = true;
        set_wm_state_normal(c);
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

        if (mask & XCB_CONFIG_WINDOW_X)      values[n++] = ev->x;
        if (mask & XCB_CONFIG_WINDOW_Y)      values[n++] = ev->y;
        if (mask & XCB_CONFIG_WINDOW_WIDTH)  values[n++] = ev->width;
        if (mask & XCB_CONFIG_WINDOW_HEIGHT) values[n++] = ev->height;
        if (mask & XCB_CONFIG_WINDOW_BORDER_WIDTH) values[n++] = ev->border_width;
        if (mask & XCB_CONFIG_WINDOW_SIBLING) values[n++] = ev->sibling;
        if (mask & XCB_CONFIG_WINDOW_STACK_MODE) values[n++] = ev->stack_mode;

        xcb_configure_window(wm.conn, ev->window, mask, values);
        return;
    }

    if (ev->value_mask & XCB_CONFIG_WINDOW_X)
        c->x = ev->x;

    if (ev->value_mask & XCB_CONFIG_WINDOW_Y)
        c->y = ev->y;

    if (ev->value_mask & XCB_CONFIG_WINDOW_WIDTH)
        c->width = ev->width < MIN_CLIENT_W ? MIN_CLIENT_W : ev->width;

    if (ev->value_mask & XCB_CONFIG_WINDOW_HEIGHT)
        c->height = ev->height < MIN_CLIENT_H ? MIN_CLIENT_H : ev->height;

    configure_frame(c);
}

static void handle_button_press(xcb_button_press_event_t *ev)
{
    /*
     * Events may be delivered to the frame itself or to the root
     * (with child = frame). Always resolve the client and compute
     * coordinates relative to the frame using root coordinates.
     */
    Client *c = find_client_window(ev->event);
    if (!c)
        c = find_client_window(ev->child);
    if (!c)
        return;

    focus_client(c);

    int rel_x = ev->root_x - c->x;
    int rel_y = ev->root_y - c->y;

    /* Left click on the titlebar */
    if (ev->detail == 1 && rel_y >= 0 && rel_y < TITLEBAR_H) {
        int w = c->frame_width;

        /* Close button */
        if (rel_x >= w - BUTTON_W) {
            send_delete(c);
            xcb_flush(wm.conn);
            return;
        }

        /* Maximize / restore */
        if (rel_x >= w - BUTTON_W * 2) {
            if (!c->maximized) {
                c->maximized = true;
                c->x = 0;
                c->y = 0;
                c->width = wm.screen->width_in_pixels;
                c->height = wm.screen->height_in_pixels - TITLEBAR_H;
            } else {
                c->maximized = false;
                c->width = 800;
                c->height = 600;
                c->x = 40;
                c->y = 40;
            }
            configure_frame(c);
            xcb_flush(wm.conn);
            return;
        }

        /* Minimize */
        if (rel_x >= w - BUTTON_W * 3) {
            xcb_unmap_window(wm.conn, c->frame);
            set_wm_state_iconic(c);
            c->mapped = false;
            xcb_flush(wm.conn);
            return;
        }

        /* Empty titlebar area → start move (no modifier needed) */
        wm.drag_mode = DRAG_MOVE;
        wm.drag_client = c;
        wm.drag_start_root_x = ev->root_x;
        wm.drag_start_root_y = ev->root_y;
        wm.drag_start_x = c->x;
        wm.drag_start_y = c->y;
        wm.drag_start_w = c->width;
        wm.drag_start_h = c->height;

        xcb_grab_pointer(wm.conn, 0, wm.root,
                         XCB_EVENT_MASK_BUTTON_RELEASE |
                         XCB_EVENT_MASK_POINTER_MOTION,
                         XCB_GRAB_MODE_ASYNC,
                         XCB_GRAB_MODE_ASYNC,
                         XCB_NONE, XCB_NONE, XCB_CURRENT_TIME);
        xcb_flush(wm.conn);
        return;
    }

    /* Alt + button from anywhere → move / resize */
    if (ev->state & MOD_ALT) {
        if (ev->detail == 1)
            wm.drag_mode = DRAG_MOVE;
        else if (ev->detail == 3)
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
                         XCB_EVENT_MASK_BUTTON_RELEASE |
                         XCB_EVENT_MASK_POINTER_MOTION,
                         XCB_GRAB_MODE_ASYNC,
                         XCB_GRAB_MODE_ASYNC,
                         XCB_NONE, XCB_NONE, XCB_CURRENT_TIME);
        xcb_flush(wm.conn);
        return;
    }

    /*
     * Normal click on the client area: we already focused above.
     * Replay the event so the application still receives the click
     * (the passive grab on the client used GrabModeSync).
     */
    if (rel_y >= TITLEBAR_H) {
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

    if (wm.drag_mode == DRAG_MOVE) {
        c->x = wm.drag_start_x + dx;
        c->y = wm.drag_start_y + dy;
    } else {
        c->width = wm.drag_start_w + dx;
        c->height = wm.drag_start_h + dy;

        if (c->width < MIN_CLIENT_W)
            c->width = MIN_CLIENT_W;
        if (c->height < MIN_CLIENT_H)
            c->height = MIN_CLIENT_H;
    }

    c->maximized = false;
    configure_frame(c);
    xcb_flush(wm.conn);
}

static void handle_button_release(xcb_button_release_event_t *ev)
{
    (void)ev;

    if (wm.drag_client) {
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

    if (ev->atom == wm.wm_name) {
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
    if (c && c->workspace == wm.current_workspace && c->mapped)
        focus_client(c);
}

static void handle_key_press(xcb_key_press_event_t *ev)
{
    uint16_t mods = ev->state & (XCB_MOD_MASK_SHIFT | XCB_MOD_MASK_LOCK |
                                 XCB_MOD_MASK_CONTROL | XCB_MOD_MASK_1 |
                                 XCB_MOD_MASK_2 | XCB_MOD_MASK_3 |
                                 XCB_MOD_MASK_4 | XCB_MOD_MASK_5);

    /* clean Alt / Alt+Shift (ignore CapsLock etc. for matching) */
    uint16_t clean = mods & (MOD_ALT | XCB_MOD_MASK_SHIFT);

    if (ev->detail == wm.key_tab) {
        if (clean == MOD_ALT_SHIFT)
            cycle_focus(-1);
        else if (clean == MOD_ALT)
            cycle_focus(+1);
        return;
    }

    if (clean == MOD_ALT) {
        if (ev->detail == wm.key_1) { switch_workspace(0); return; }
        if (ev->detail == wm.key_2) { switch_workspace(1); return; }
        if (ev->detail == wm.key_3) { switch_workspace(2); return; }
        if (ev->detail == wm.key_4) { switch_workspace(3); return; }
    }
}

static void setup_wm(void)
{
    wm.conn = xcb_connect(NULL, NULL);

    if (xcb_connection_has_error(wm.conn))
        die("could not connect to X server");

    const xcb_setup_t *setup = xcb_get_setup(wm.conn);
    xcb_screen_iterator_t it = xcb_setup_roots_iterator(setup);

    wm.screen = it.data;
    wm.root = wm.screen->root;
    wm.visual = find_root_visual(wm.screen);

    if (!wm.visual)
        die("could not find root visual");

    wm.wm_protocols = intern_atom("WM_PROTOCOLS");
    wm.wm_delete_window = intern_atom("WM_DELETE_WINDOW");
    wm.wm_name = intern_atom("WM_NAME");
    wm.utf8_string = intern_atom("UTF8_STRING");

    wm.net_wm_window_type = intern_atom("_NET_WM_WINDOW_TYPE");
    wm.net_wm_window_type_dock = intern_atom("_NET_WM_WINDOW_TYPE_DOCK");
    wm.net_wm_window_type_desktop = intern_atom("_NET_WM_WINDOW_TYPE_DESKTOP");
    wm.net_wm_window_type_toolbar = intern_atom("_NET_WM_WINDOW_TYPE_TOOLBAR");
    wm.net_wm_window_type_menu = intern_atom("_NET_WM_WINDOW_TYPE_MENU");

    wm.current_workspace = 0;

    wm.key_tab = keysym_to_keycode(XK_Tab);
    wm.key_1   = keysym_to_keycode(XK_1);
    wm.key_2   = keysym_to_keycode(XK_2);
    wm.key_3   = keysym_to_keycode(XK_3);
    wm.key_4   = keysym_to_keycode(XK_4);

    /*
     * SubstructureRedirectMask is the actual WM ownership lock.
     * Only one WM can select it on a root window.
     */
    uint32_t mask =
        XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT |
        XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY |
        XCB_EVENT_MASK_STRUCTURE_NOTIFY |
        XCB_EVENT_MASK_PROPERTY_CHANGE |
        XCB_EVENT_MASK_BUTTON_PRESS |
        XCB_EVENT_MASK_BUTTON_RELEASE |
        XCB_EVENT_MASK_POINTER_MOTION |
        XCB_EVENT_MASK_ENTER_WINDOW |
        XCB_EVENT_MASK_LEAVE_WINDOW |
        XCB_EVENT_MASK_KEY_PRESS;

    xcb_void_cookie_t cookie =
        xcb_change_window_attributes_checked(wm.conn, wm.root,
                                             XCB_CW_EVENT_MASK, &mask);

    xcb_generic_error_t *error =
        xcb_request_check(wm.conn, cookie);

    if (error) {
        free(error);
        die("another window manager is already running");
    }

    /* Keybindings: Alt+Tab, Alt+Shift+Tab, Alt+1..4 */
    if (wm.key_tab) {
        xcb_grab_key(wm.conn, 1, wm.root, MOD_ALT,
                     wm.key_tab, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
        xcb_grab_key(wm.conn, 1, wm.root, MOD_ALT_SHIFT,
                     wm.key_tab, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
    }
    if (wm.key_1)
        xcb_grab_key(wm.conn, 1, wm.root, MOD_ALT,
                     wm.key_1, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
    if (wm.key_2)
        xcb_grab_key(wm.conn, 1, wm.root, MOD_ALT,
                     wm.key_2, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
    if (wm.key_3)
        xcb_grab_key(wm.conn, 1, wm.root, MOD_ALT,
                     wm.key_3, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
    if (wm.key_4)
        xcb_grab_key(wm.conn, 1, wm.root, MOD_ALT,
                     wm.key_4, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);

    xcb_flush(wm.conn);

    fprintf(stderr, "kiwm: started on screen %dx%d (workspaces 1-%d)\n",
            wm.screen->width_in_pixels,
            wm.screen->height_in_pixels,
            NUM_WORKSPACES);
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

    if (wm.conn)
        xcb_disconnect(wm.conn);
}

static void handle_event(xcb_generic_event_t *event)
{
    uint8_t type = event->response_type & ~0x80;

    switch (type) {
    case XCB_MAP_REQUEST:
        handle_map_request((xcb_map_request_event_t *)event);
        break;

    case XCB_CONFIGURE_REQUEST:
        handle_configure_request((xcb_configure_request_event_t *)event);
        break;

    case XCB_DESTROY_NOTIFY: {
        xcb_destroy_notify_event_t *ev =
            (xcb_destroy_notify_event_t *)event;
        Client *c = find_client_window(ev->window);
        if (c)
            unmanage(c);
        break;
    }

    case XCB_UNMAP_NOTIFY: {
        xcb_unmap_notify_event_t *ev =
            (xcb_unmap_notify_event_t *)event;
        Client *c = find_client_window(ev->window);
        /*
         * Only treat as client-initiated unmap when the client
         * belongs to the current workspace. Unmaps caused by
         * workspace switching are ignored so mapped state is kept.
         */
        if (c && ev->window == c->window && c->mapped &&
            c->workspace == wm.current_workspace) {
            c->mapped = false;
            xcb_unmap_window(wm.conn, c->frame);
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

int main(void)
{
    memset(&wm, 0, sizeof(wm));
    wm.running = true;

    setup_wm();

    /*
     * Existing windows are intentionally not managed in this first
     * prototype. Start kiwm before applications (as kisession does).
     */

    while (wm.running) {
        xcb_generic_event_t *event =
            xcb_wait_for_event(wm.conn);

        if (!event)
            break;

        handle_event(event);
        free(event);
    }

    cleanup();
    return 0;
}
