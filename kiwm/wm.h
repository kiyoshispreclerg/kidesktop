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

/* btns.png sprite sheet column order (see greenxp/btns.slice) -- fixed by
 * convention, not configurable (unlike the on-screen *order* of buttons,
 * see DecoElemKind/kiwm.conf's titlebar_layout=). Rows (not enumerated
 * here) are always normal=0/hover=1/clicked=2 top-to-bottom; "clicked"
 * isn't wired to anything yet (kiwm fires button actions on press, not
 * release, so there's no separate held-down moment to show it during). */
#define BTNCOL_CLOSE             0
#define BTNCOL_MAXIMIZE          1
#define BTNCOL_RESTORE           2
#define BTNCOL_MINIMIZE          3
#define BTNCOL_SHADE             4
#define BTNCOL_KEEP_ABOVE        5
#define BTNCOL_KEEP_ALL_DESKTOPS 6

/* One element of the titlebar layout (kiwm.conf's titlebar_layout=,
 * default "icon,title,shade,minimize,maximize,close") -- see decoration.h's
 * compute_deco_layout(). DECO_TITLE is the sole flexible element (absorbs
 * whatever width the fixed-width ones don't use); everything else is a
 * fixed BUTTON_W-wide slot, in whatever order the config lists them,
 * left to right. */
typedef enum {
    DECO_TITLE = 0,
    DECO_ICON,
    DECO_SHADE,
    DECO_MINIMIZE,
    DECO_MAXIMIZE,  /* also stands in for "restore" once maximized -- same slot, same position */
    DECO_CLOSE,
    DECO_KEEP_ABOVE,
    DECO_KEEP_ALL_DESKTOPS,
} DecoElemKind;

#define MAX_DECO_ELEMS 12

/* How close together (ms, comparing xcb_timestamp_t's, which are itself
 * server milliseconds) two titlebar clicks must land to count as a
 * double-click (see events.c's handle_button_press). Not configurable --
 * matches a typical desktop double-click speed closely enough. */
#define DOUBLE_CLICK_MS   400

/* Fallback cap (ms/frame) for how often a move/resize drag actually
 * reconfigures+redraws+reshapes the window (events.c's handle_motion()),
 * used only when the dragged client's output has no determinable refresh
 * rate (see output.c's compute_output_refresh_hz()) -- normally the
 * throttle paces to that output's *actual* Hz (1000.0 / refresh_hz)
 * instead of this fixed number, so a 144Hz panel gets a shorter interval
 * than a 60Hz one and neither wastes work redrawing faster than the
 * screen can even show it. Confirmed via KIWM_DEBUG_RESIZE
 * instrumentation that some input devices/drivers (touchpads with
 * pointer smoothing especially) report *far* more MotionNotify events
 * than are ever simultaneously queued -- coalescing (main.c's event
 * loop) only helps when events genuinely back up, and does nothing when
 * they arrive one at a time faster than the eye needs but not faster
 * than kiwm can physically drain them; the fix for that case is a
 * time-based cap, not a queue-based one. handle_button_release() forces
 * one final apply unconditionally so the window is never left showing a
 * stale, throttled size once the drag actually ends. */
#define DRAG_REDRAW_FALLBACK_MS 16.0

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
    double refresh_hz;      /* current mode's refresh rate, see output.c's compute_output_refresh_hz(); 60.0 if undeterminable */
} XisOutput;

/* Which screen edge (if any) a window is currently snapped to -- see
 * events.c's handle_motion(). SNAP_TOP behaves like maximize (and is
 * tracked through Client::maximized, not this enum); the enum only needs
 * to distinguish the two half-width snaps from "not snapped". */
typedef enum {
    SNAP_NONE = 0,
    SNAP_LEFT,
    SNAP_RIGHT,
    SNAP_TOP   /* only ever used transiently as KiWM::drag_snap_side -- a
                * client actually snapped to the top edge is represented
                * via Client::maximized instead, not Client::snap_side. */
} SnapSide;

/* A non-managed window (dock/panel, e.g. xispanel) that reserves screen
 * edge space via _NET_WM_STRUT(_PARTIAL). Tracked separately from Client
 * since dock/desktop/toolbar/menu window types are never framed or added
 * to wm.clients (see client.c's should_manage_decorated()). */
typedef struct DockWindow {
    xcb_window_t window;
    int left, right, top, bottom;
    /* Which output this dock's own on-screen rectangle sits on (index into
     * wm.outputs, or -1 if undetermined) -- see output.c's
     * assign_dock_output(). _NET_WM_STRUT(_PARTIAL)'s l/r/t/b values are
     * defined relative to the *whole* screen with no notion of "which
     * monitor", so without this a dock living on one output would still
     * eat into a different output's workarea whenever both outputs share
     * the same absolute row/column range (e.g. both start at x=0). */
    int output;
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
    bool shaded;    /* content window unmapped, only the titlebar shows --
                     * see client.c's toggle_shade(). Orthogonal to
                     * maximized/snap_side (tiling always unshades first,
                     * see client.c's unshade_now()). */
    bool keep_above;  /* stacked above every non-keep_above client, see
                       * client.c's raise_above_clients(). */
    bool sticky;      /* visible regardless of its output's current
                       * desktop -- see client.c's toggle_sticky() and
                       * every "wm.outputs[c->output].desktop == c->desktop"
                       * visibility check across client.c/events.c/output.c,
                       * all of which now also accept c->sticky. */

    cairo_surface_t *icon;  /* _NET_WM_ICON, scaled down once when loaded;
                             * NULL if the client has none (drawn blank). */
    int ignore_unmap;   /* absorbs the automatic UnmapNotify from reparenting an
                          * already-mapped pre-existing window at startup */

    int output;              /* index into wm.outputs */
    int desktop;              /* per-output virtual desktop this client belongs to */

    /* Windows7/kwin-style edge snap (see events.c's handle_motion). SNAP_NONE
     * unless the window is currently filling exactly one half of its
     * output's workarea; top-edge snapping reuses `maximized` instead, since
     * it's the exact same state a titlebar maximize-click produces. */
    SnapSide snap_side;

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
    xcb_atom_t net_wm_state_shaded;
    xcb_atom_t net_wm_state_above;
    xcb_atom_t net_wm_state_sticky;
    xcb_atom_t net_wm_icon;

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
    bool shape_ext_present;  /* XCB SHAPE extension, for rounded corners (see radius_tl etc). */
    xcb_gcontext_t deco_gc;  /* reused across every draw_decoration() call -- see decoration.c. */
    bool debug_resize;       /* KIWM_DEBUG_RESIZE=1 -- see main.c's monotonic_ms(). */

    /* Cursor-font glyphs (core X "cursor" font, no Xcursor lib needed),
     * loaded once in main.c's setup_wm() and swapped in for the duration
     * of a drag via xcb_grab_pointer()'s cursor argument (events.c's
     * begin_drag()) -- ungrabbing at drag end reverts to whatever cursor
     * would normally show, no explicit restore needed. */
    xcb_font_t cursor_font;
    xcb_cursor_t cursor_move;
    xcb_cursor_t cursor_resize_nw, cursor_resize_ne, cursor_resize_sw, cursor_resize_se;

    /* Live root window size, refreshed by output.c's outputs_refresh()
     * (via a fresh xcb_get_geometry() on wm.root) every time RandR reports
     * a screen change. wm.screen->width_in_pixels/height_in_pixels is a
     * snapshot taken once at connection setup and is NEVER updated by xcb
     * afterward -- using it directly in workarea/maximize math silently
     * goes stale the moment a monitor is plugged in, unplugged, or
     * resized after startup (e.g. a maximize on a since-added bigger
     * output getting clamped to the *old*, smaller virtual screen size). */
    int screen_w, screen_h;

    Atoms atoms;
    xcb_window_t check_win;
    xcb_window_t sel_win;    /* WM_Sn selection owner window, watched for SelectionClear */
    xcb_atom_t sn_atom;

    XisOutput outputs[MAX_OUTPUTS];
    int output_count;

    DockWindow docks[MAX_DOCKS];
    int dock_count;

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

    /* Theme folder path (kiwm.conf's theme=, default "greenxp") -- resolved
     * relative to the same 3 candidate locations the old hardcoded
     * greenxp/ search used (../<theme>, ./<theme>, plain <theme>), see
     * decoration.c's find_theme_file(). */
    char theme_path[256];

    /* Theme (see decoration.c's load_decoration()). Every
     * piece loads independently and falls back on its own if missing --
     * there's no all-or-nothing theme requirement. */
    cairo_surface_t *deco_bg;   /* greenxp/bg.png, ARGB32, or NULL */
    int bg_slice_l, bg_slice_t, bg_slice_r, bg_slice_b;  /* greenxp/slice, 9-slice insets */

    cairo_surface_t *deco_btns; /* greenxp/btns.png button sprite sheet, ARGB32, or NULL */
    int btn_cell_w, btn_cell_h; /* greenxp/btns.slice: cell_width=/cell_height= */

    /* greenxp/colors: per-focus titlebar/border colors. have_theme_colors
     * is set as soon as the file is found at all (even if some keys are
     * missing -- those individual colors just fall back to the plain
     * deco_bg_/deco_fg_/border_ fields below instead of a whole-file
     * fallback). */
    /* Per-corner frame rounding in pixels (theme's colors file,
     * border_radius= -- 1 number = all 4 corners, 2 = top/bottom, 4 =
     * top-left/top-right/bottom-right/bottom-left, CSS order). 0
     * (default, no colors file or no border_radius= key) means square
     * corners, same as before this existed. Applied via the XCB SHAPE
     * extension (see decoration.c's apply_rounded_shape()), since kiwm
     * has no compositor to do real alpha-blended rounding -- it clips
     * the frame's bounding shape instead, a real (if slightly stair-
     * stepped at small sizes) rounded corner with no compositor needed. */
    int radius_tl, radius_tr, radius_br, radius_bl;
    /* Whether a maximized window still gets those corners rounded (theme's
     * colors file, round_maximized=, default 0/no -- a maximized window
     * touching the screen edges with rounded corners looks wrong, so this
     * defaults to squaring it off, unlike border_radius itself which
     * defaults to square everywhere until a theme opts in). A window that
     * exactly fills its output's full rectangle (frame == output, which
     * is also what a future real fullscreen state would look like) is
     * NEVER rounded regardless of this setting -- see decoration.c's
     * client_fills_output(), not configurable on purpose. */
    bool round_maximized;

    /* Title text (theme's colors file, font=/font_size=/title_center=).
     * font names a Fontconfig family ("sans-serif" default, same as with
     * no theme at all); font_size is in pixels (12.5 default, the old
     * hardcoded cairo_set_font_size() value); title_center draws the title
     * centered within its slot instead of left-aligned with an 8px pad.
     * The title element is always the greedy one anyway (compute_deco_
     * layout gives it whatever width is left over), so centering it needs
     * no separate spacer element -- just where pango_show_text_boxed()
     * places the text inside that width. */
    char title_font[128];
    double title_font_size;
    bool title_center;

    bool have_theme_colors;
    double bg_active_r, bg_active_g, bg_active_b;
    double bg_inactive_r, bg_inactive_g, bg_inactive_b;
    double fg_active_r, fg_active_g, fg_active_b;
    double fg_inactive_r, fg_inactive_g, fg_inactive_b;
    double border_active_r, border_active_g, border_active_b;
    double border_inactive_r, border_inactive_g, border_inactive_b;

    /* Which client/element the pointer currently hovers, for the sprite
     * theme's hover row (see decoration.c's draw_button()) -- meaningless
     * without deco_btns loaded, so plain-fallback decoration never
     * bothers tracking or repainting for this. hover_btn is an index into
     * deco_layout (below), or -1 for none/not-a-button element. */
    Client *hover_client;
    int hover_btn;

    /* Titlebar element order, kiwm.conf's titlebar_layout= -- see
     * DecoElemKind and decoration.h's compute_deco_layout(). Defaults (see
     * config.c) to icon, title, shade, minimize, maximize, close, matching
     * kiwm's original fixed layout except for the newly-added icon slot. */
    DecoElemKind deco_layout[MAX_DECO_ELEMS];
    int deco_layout_count;

    bool hide_deco_on_maximize;
    double deco_bg_r, deco_bg_g, deco_bg_b;   /* fallback titlebar background when no theme */
    double deco_fg_r, deco_fg_g, deco_fg_b;   /* fallback title text color */

    /* Left/right/bottom decoration border: a flat-colored strip (no PNG
     * theming yet, see decoration.c) of this thickness on the three sides
     * the titlebar doesn't already cover, from kiwm.conf's
     * border_thickness=/border_color= (default: 0, i.e. no side/bottom
     * border, preserving the old titlebar-only look). */
    int border_thickness;
    double border_r, border_g, border_b;

    int num_desktops;   /* virtual desktops per output, from kiwm.conf's num_desktops= (default 4) */

    /* Whether merely moving the pointer into a window raises+focuses it
     * (classic "sloppy"/focus-follows-mouse), vs. requiring a click --
     * kiwm.conf's focus_follows_mouse= (default 0/off: click-to-focus). */
    bool focus_follows_mouse;

    /* Distance in pixels from an output's workarea edge, while dragging a
     * window by its titlebar/mod-drag, that engages Windows7/kwin-style
     * edge snapping (see events.c's handle_motion). kiwm.conf's
     * snap_threshold= (default 20). 0 disables snapping entirely. */
    int snap_threshold;
    /* SNAP_NONE/current snap side engaged by the drag in progress, and the
     * output it was computed against -- reset at the start of every drag
     * in handle_button_press. Separate from Client::snap_side because a
     * window not yet released still has its *pending* snap tracked here,
     * so handle_motion can tell when the pointer has moved out of the edge
     * zone again and needs to restore the pre-drag floating geometry. */
    SnapSide drag_snap_side;

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
    /* Which edges grow during a DRAG_RESIZE, decided once at the press
     * that started it (events.c's handle_button_press) from whichever
     * corner was nearest the click, kwin/compiz-style -- the opposite
     * corner then stays fixed for the whole drag (handle_motion). */
    bool resize_right, resize_bottom;
    double last_drag_apply_ms;  /* see DRAG_REDRAW_FALLBACK_MS / events.c's handle_motion() */

    /* Manual double-click detection for the plain (non-button) titlebar
     * area -- X has no double-click event of its own, just consecutive
     * ButtonPress'es (see events.c's handle_button_press). */
    xcb_timestamp_t last_titlebar_click_time;
    xcb_window_t last_titlebar_click_frame;

    xcb_keycode_t key_tab, key_1, key_2, key_3, key_4, key_up;

    bool running;
} KiWM;

extern KiWM wm;

/* CLOCK_MONOTONIC in milliseconds, defined in main.c -- used by
 * wm.debug_resize instrumentation wherever it needs fine-grained timing
 * (events.c's handle_motion(), decoration.c's draw_decoration()). */
double monotonic_ms(void);

#endif /* KIWM_WM_H */
