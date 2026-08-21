/*
 * kiwm - shared types, constants and global WM state.
 *
 * Every other module includes this header. It has no matching wm.c --
 * the global `wm` instance is defined in main.c.
 */
#ifndef KIWM_WM_H
#define KIWM_WM_H

#include <xcb/xcb.h>
#include <cairo/cairo.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

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

extern KiWM wm;

#endif /* KIWM_WM_H */
