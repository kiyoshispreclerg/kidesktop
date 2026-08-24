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
#define MAX_DOCKS         16

/* Upper bound used only to size fixed stack arrays (e.g. _NET_WORKAREA);
 * the actual per-output desktop count is wm.num_desktops, read from
 * kiwm.conf's num_desktops= key at startup (see config.c), default 4. */
#define MAX_DESKTOPS      32
#define DEFAULT_NUM_DESKTOPS 4

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
    int desktop;            /* current virtual desktop for this output, 0..wm.num_desktops-1 */
} XisOutput;

/* A non-managed window (dock/panel, e.g. xispanel) that reserves screen
 * edge space via _NET_WM_STRUT(_PARTIAL). Tracked separately from Client
 * since dock/desktop/toolbar/menu window types are never framed or added
 * to wm.clients (see client.c's should_manage_decorated()). */
typedef struct DockWindow {
    xcb_window_t window;
    int left, right, top, bottom;
} DockWindow;

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
    xcb_atom_t net_frame_extents;
    xcb_atom_t net_wm_strut;
    xcb_atom_t net_wm_strut_partial;

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
    xcb_atom_t kiwm_wm_output;
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

    DockWindow docks[MAX_DOCKS];
    int dock_count;
    /* Aggregated (max across all tracked docks) screen-edge reservation,
     * recomputed by output.c's recompute_struts() whenever a dock's strut
     * changes or a dock is destroyed. Screen-absolute pixels, same as
     * _NET_WM_STRUT itself. */
    int strut_left, strut_right, strut_top, strut_bottom;

    /* Most recently mapped _NET_WM_WINDOW_TYPE_DESKTOP window (e.g.
     * xisback's wallpaper/fade windows) -- see client.c's manage(). Each
     * new one is explicitly stacked directly above this one (not just
     * left at X's default "new window goes on top of everything"
     * placement), so the whole desktop-type group stays clustered at the
     * bottom of the stack, below every normal window, in creation order --
     * matching kiwm-kicomp-projeto.md section 13's stacking model, and
     * what xisback's create_fade_window() comment already assumes a
     * "compliant WM" does. XCB_NONE (0) when no desktop-type window has
     * been mapped yet, or the tracked one was destroyed. */
    xcb_window_t last_desktop_window;

    Client *clients;
    Client *focused;

    cairo_surface_t *deco_bg;   /* cached greenxp/bg.png, ARGB32 */
    bool hide_deco_on_maximize;
    double deco_bg_r, deco_bg_g, deco_bg_b;   /* fallback titlebar background when no PNG loads */
    double deco_fg_r, deco_fg_g, deco_fg_b;   /* fallback title text color */

    int num_desktops;   /* virtual desktops per output, from kiwm.conf's num_desktops= (default 4) */

    /* Which modifier drives Alt+Tab-style window cycling vs. Meta-style
     * window control (move/maximize/desktop-cycle) -- configurable via
     * kiwm.conf's mod_cycle=/mod_control= (values "alt" or "meta"),
     * defaulting to MOD_ALT/MOD_META respectively (see config.c). */
    uint16_t mod_cycle;
    uint16_t mod_control;

    DragMode drag_mode;
    Client *drag_client;
    int drag_start_root_x, drag_start_root_y;
    int drag_start_x, drag_start_y, drag_start_w, drag_start_h;

    xcb_keycode_t key_tab, key_1, key_2, key_3, key_4, key_up;

    bool running;
} KiWM;

extern KiWM wm;

#endif /* KIWM_WM_H */
