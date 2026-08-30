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

/* Upper bound on kiwm.conf's outline_width= (outline.c's wireframe band).
 * Not a technical limit, just the point past which the "outline" stops
 * being one and starts being a slab covering the window it's pointing at. */
#define MAX_OUTLINE_WIDTH 64

/* Upper bound on kiwm.conf's resize_grip= -- past this the grip is eating
 * a serious amount of the application's own window. */
#define MAX_RESIZE_GRIP 32

/* How far (pixels, either axis) the pointer must travel from where a
 * move-drag started before a maximized or half-tiled window actually
 * leaves that state and starts following the cursor -- see events.c's
 * handle_motion() and KiWM::drag_detile_pending. Clicking a maximized
 * window's titlebar (or mod-dragging it) shouldn't restore it on contact:
 * the click is usually aimed at a button, or at nothing. The titlebar's
 * own height is the threshold simply because it's the one "how big is a
 * deliberate drag" number kiwm already has. */
#define DRAG_DETILE_THRESHOLD TITLEBAR_H

/* Upper bound used only to size fixed stack arrays (e.g. _NET_WORKAREA);
 * the actual per-output desktop count is wm.num_desktops, read from
 * kiwm.conf's num_desktops= key at startup (see config.c), default 4. */
#define MAX_DESKTOPS      32
#define DEFAULT_NUM_DESKTOPS 4

/* btns.png sprite sheet column order (see greenxp/btns.slice) -- fixed by
 * convention, not configurable (unlike the on-screen *order* of buttons,
 * see DecoElemKind/kiwm.conf's titlebar_layout=). Rows (not enumerated
 * here) are always normal=0/hover=1/clicked=2 top-to-bottom -- "clicked"
 * is drawn while a button is held down, between the press that arms it
 * and the release that fires it (see KiWM::pressed_client). A sheet with
 * fewer rows just reuses its last one. */
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

/* Stacking layer a client belongs to, derived from its state (see client.c's
 * client_layer()) -- never set directly. Ordered bottom to top; client.c's
 * restack_all() rebuilds the real X stacking order from this every time
 * something that could change it happens (focus, keep_above/keep_below
 * toggling, a window (un)managed), replacing the old ad-hoc "just re-raise
 * keep_above clients on top of whatever's there" approach -- that never
 * gave keep_below a defined position relative to keep_above, and had no
 * way to keep two keep_above clients in a sane relative order either.
 * The order follows EWMH's suggested stacking (below < normal < dock <
 * above), with one refinement every mainstream WM makes: a fullscreen
 * window goes above *everything*, docks included, but only while it's the
 * focused one (LAYER_ACTIVE_FULLSCREEN -- kwin calls this its "active
 * layer", mutter/metacity do the same). That's what lets a fullscreen
 * video or VM cover the panel while you're using it, and lets Alt+Tab
 * bring any other window -- or the panel -- back over it the instant it
 * stops being focused, instead of a fullscreen window being unconditionally
 * pinned on top forever (kiwm's original behavior) or the panel floating
 * over a fullscreen VM (what came after it).
 *
 * LAYER_DOCK isn't a client layer: dock/panel windows are never framed
 * into a Client at all (see client.c's should_manage_decorated()), but
 * restack_all() *does* place them, since where a panel sits relative to
 * normal, keep-above and active-fullscreen windows is exactly the kind of
 * question only the WM can answer. Desktop-type windows are still outside
 * this ordering entirely -- see manage()'s own last_desktop_window
 * chaining, which keeps them clustered at the very bottom.
 *
 * LAYER_OUTLINE and LAYER_OSD, at the top, are the WM's *own* surfaces --
 * nothing a client can ask for ever reaches either, which is the point.
 * The outline (outline.c's wireframe preview) goes above every client,
 * including an active fullscreen one, but stays below the overlay that's
 * usually driving it, so the two never fight over which is on top.
 *
 * LAYER_OSD, at the very top, is the WM's own surfaces: the switcher
 * overlays today (osd.c), whatever kicomp puts on screen later. Nothing a
 * client can ask for ever reaches it, which is the point -- it's the one
 * layer guaranteed to be above even an active fullscreen window, so kiwm's
 * own UI can never end up drawing behind the window it's offering to
 * switch away from, and no client repaint (or missing one) can leave stale
 * pixels over it. */
typedef enum {
    LAYER_BELOW = 0,
    LAYER_NORMAL,
    LAYER_DOCK,
    LAYER_ABOVE,
    LAYER_ACTIVE_FULLSCREEN,
    LAYER_OUTLINE,
    LAYER_OSD,
    LAYER_COUNT
} WmLayer;

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

    /* The dock's own on-screen rectangle, cached from the xcb_get_geometry()
     * assign_dock_output() already does -- used by events.c's magnet_snap()
     * to snap a dragged window's edge against a panel/taskbar's edge too,
     * not just other Clients and the screen edge. Like `output` above, only
     * refreshed when assign_dock_output() re-runs (dock_track() and RandR
     * screen-change re-assignment); a dock moving/resizing itself after
     * that with no output change goes unnoticed, same limitation `output`
     * already has and for the same reason (panels don't, in practice). 0
     * width/height (assign_dock_output()'s xcb_get_geometry() failing)
     * means "don't use this as a magnet candidate at all". */
    int x, y, width, height;
} DockWindow;

/* One window found touching a resize drag's moving edge, captured at the
 * exact moment the drag started -- see wm.h's KiWM::resize_neighbors_x/y
 * and events.c's begin_drag()/handle_motion(). Resized oppositely in
 * lockstep so it stays touching: shrinking the dragged window along that
 * edge grows the neighbor by the same amount (its far edge stays
 * anchored), and vice versa. orig_x/y/w/h is the neighbor's own geometry
 * at the moment it was detected, so every motion event recomputes its new
 * geometry from the *total* displacement since drag start (not
 * incrementally from the previous event) -- the same anti-drift approach
 * KiWM::drag_start_x/y/w/h already uses for the dragged window itself. */
#define MAX_RESIZE_NEIGHBORS 8
/* How close (pixels) a gap between two frame edges still counts as
 * "touching" for resize-neighbor detection -- deliberately tiny and fixed
 * (not kiwm.conf's magnet_threshold=, which is about visually pulling
 * distant edges together; this is about recognizing two edges that are
 * *already* essentially flush, e.g. from a previous magnet snap or a
 * tiling/snap operation). */
#define RESIZE_NEIGHBOR_EPSILON_PX 1
typedef struct {
    Client *client;
    int orig_x, orig_y, orig_w, orig_h;
} ResizeNeighbor;

struct Client {
    xcb_window_t window;    /* application window */
    xcb_window_t frame;     /* decorated frame */

    int x, y, width, height;                 /* content geometry, global coords */
    int saved_x, saved_y, saved_w, saved_h;   /* restore geometry before maximize */
    int frame_width, frame_height;

    bool mapped;
    /* Maximization, tracked per axis: EWMH has always had
     * _NET_WM_STATE_MAXIMIZED_VERT and _HORZ as two independent states,
     * and both single-axis ones are real things a user asks for (kwin
     * binds them to the maximize button's right and middle click, which
     * is where kiwm's own come from -- see events.c's run_deco_button()).
     * "Maximized" with no qualifier means both, which is what
     * client_maximized() below asks; nothing outside the maximize code
     * should test the two flags directly. */
    bool max_horz;
    bool max_vert;
    bool minimized;
    bool shaded;    /* content window unmapped, only the titlebar shows --
                     * see client.c's toggle_shade(). Orthogonal to
                     * maximized/snap_side (tiling always unshades first,
                     * see client.c's unshade_now()). */
    bool keep_above;  /* _NET_WM_STATE_ABOVE -- see client.c's client_layer()/
                       * restack_all(). Mutually exclusive with keep_below. */
    bool keep_below;  /* _NET_WM_STATE_BELOW, ditto. */
    bool sticky;      /* visible regardless of its output's current
                       * desktop -- see client.c's toggle_sticky() and
                       * every "wm.outputs[c->output].desktop == c->desktop"
                       * visibility check across client.c/events.c/output.c,
                       * all of which now also accept c->sticky. */
    bool fullscreen;  /* _NET_WM_STATE_FULLSCREEN -- see client.c's
                       * toggle_fullscreen(). Unlike maximize, fills the
                       * output's whole rectangle (not just its workarea --
                       * a fullscreen window covers docks/panels too) and
                       * unconditionally hides the decoration regardless of
                       * wm.hide_deco_on_maximize (see decoration.c's
                       * client_deco_visible()). */
    /* The client asked for no decoration at all -- either _MOTIF_WM_HINTS
     * with decorations=0 (what Qt/GTK/SDL frameless windows set) or KDE's
     * _KDE_NET_WM_WINDOW_TYPE_OVERRIDE. Still a fully managed client
     * (framed, focusable, in the taskbar, movable, tiles and maximizes
     * normally); its frame just has no titlebar or border, so the frame
     * ends up exactly the size of the content. Read once in manage() --
     * apps set this before mapping and effectively never change it. */
    bool undecorated;

    /* The client listed WM_TAKE_FOCUS in WM_PROTOCOLS: besides the plain
     * SetInputFocus, it wants to be *told* when the WM gives it the
     * keyboard, so it can route the focus internally (Qt/KDE apps all do
     * this -- and krunner in particular only lights up its input field
     * once it gets the message; a bare SetInputFocus leaves it visibly
     * unfocused). See client.c's focus_client(). Read once in manage() --
     * WM_PROTOCOLS is set before mapping. */
    bool takes_focus;

    /* ICCCM WM_TRANSIENT_FOR: the window this one is a transient of (a
     * dialog's main window, or -- the case that made kiwm need this --
     * VirtualBox's fullscreen mini-toolbar, which is transient for the VM
     * window it floats over). XCB_NONE for a plain top-level. Purely a
     * stacking input: client.c's restack_all() keeps a transient above its
     * parent within their shared layer, so focusing the parent can't bury
     * its own dialog/toolbar behind it. Read once in manage(). */
    xcb_window_t transient_for;

    /* ICCCM window group (WM_HINTS' window_group, falling back to
     * WM_CLIENT_LEADER): the "these top-levels are one application unit"
     * relationship, for the windows that need to travel together but
     * aren't in a transient-for relationship at all. VirtualBox's
     * fullscreen mini-toolbar is exactly that -- same group leader as the
     * VM window, no WM_TRANSIENT_FOR whatsoever -- so a WM that only knows
     * about transients leaves it behind the VM the moment the VM takes
     * focus and claims the layer above everything. XCB_NONE if the client
     * declares no group. Only ever consulted for stacking; kiwm has no
     * other notion of application grouping. */
    xcb_window_t group_leader;

    /* _NET_WM_STATE_SKIP_TASKBAR: the client saying "I'm not a window the
     * user switches to". kiwm reads it for two things -- keeping such a
     * window above the rest of its own group (it's auxiliary chrome
     * floating over a real window, e.g. VirtualBox's mini-toolbar, and the
     * app never restacks it itself), and leaving it out of the window
     * switcher, where an entry you can't meaningfully "switch to" is just
     * noise. Read once in manage(); apps set it before mapping. */
    bool skip_taskbar;

    /* ICCCM WM_NORMAL_HINTS' win_gravity: how the client wants to be
     * positioned once the WM wraps decoration around it. Only two answers
     * matter in practice and kiwm implements exactly those: StaticGravity
     * ("my *content* goes where I asked, put your titlebar above it") and
     * everything else, which defaults to NorthWest ("put my *frame* where I
     * asked; the content lands below the titlebar"). Getting this wrong is
     * invisible on a freshly mapped window but very visible on adoption:
     * every Qt/GTK window asks for Static, so every WM switch used to walk
     * them all a titlebar's height down the screen. */
    uint8_t gravity;

    /* What this client permits being done to it, from WM_NORMAL_HINTS (a
     * window whose min size equals its max size can't be resized, so it
     * can't be maximized or tiled either) and _MOTIF_WM_HINTS' `functions`
     * field, which is how toolkits express "this dialog has no maximize
     * button". kiwm both *obeys* these (the corresponding operation is
     * refused, however it's invoked) and *shows* them: a disallowed action's
     * titlebar button isn't drawn at all, and the whole set is republished
     * as _NET_WM_ALLOWED_ACTIONS so taskbars and pagers grey out the same
     * entries. See client.c's update_client_actions(). */
    bool allow_move, allow_resize, allow_minimize, allow_maximize, allow_close;
    /* WM_NORMAL_HINTS says min size == max size, i.e. the client declared
     * itself unresizable -- one of the two inputs to allow_* above, kept
     * separately because it's re-read on its own (a toolkit can fix its
     * size long after mapping). */
    bool hints_fixed_size;

    int fs_saved_x, fs_saved_y, fs_saved_w, fs_saved_h; /* restore geometry before fullscreen */
    bool fs_was_max_horz;    /* which maximization to restore (vs. just float) on leaving fullscreen */
    bool fs_was_max_vert;
    SnapSide fs_saved_snap_side; /* ditto, for half-snapped windows */

    /* ICCCM WM_NORMAL_HINTS' minimum size (see ewmh.c's get_size_hints()),
     * floored to MIN_CLIENT_W/H so a client that sets a tiny or no hint at
     * all can never be resized down to something unusably small. Used
     * everywhere MIN_CLIENT_W/H used to be a flat constant: client.c's
     * manage()/toggle_maximize()/snap_client_to_side(), events.c's
     * handle_configure_request()/handle_motion(). */
    int min_w, min_h;

    cairo_surface_t *icon;  /* _NET_WM_ICON, scaled down once when loaded;
                             * NULL if the client has none (drawn blank). */
    int ignore_unmap;   /* absorbs the automatic UnmapNotify from reparenting an
                          * already-mapped pre-existing window at startup */
    /* Absorbs the automatic MapNotify from that same reparent. Reparenting
     * a mapped window makes the server unmap it, move it, and then map it
     * again -- and that last map arrives as an ordinary MapNotify, which
     * events.c reasonably reads as "the app is showing this window" and
     * answers by mapping the frame. For a window kiwm is adopting while
     * it's meant to stay hidden (a minimized window, an app's stashed-away
     * popup) that silently undoes the decision not to show it, which is
     * how a WM switch ended up revealing every hidden window on screen.
     * Only ever nonzero for that one adoption case. */
    int ignore_map;

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

/* "Maximized", unqualified: both axes at once. Everything that just wants
 * to know whether a window is filling its output asks this rather than
 * looking at Client::max_horz/max_vert, which only the maximize code
 * itself has any business telling apart. */
static inline bool client_maximized(const Client *c)
{
    return c->max_horz && c->max_vert;
}

typedef struct {
    xcb_atom_t wm_protocols;
    xcb_atom_t wm_delete_window;
    xcb_atom_t wm_take_focus;   /* see Client::takes_focus */
    xcb_atom_t wm_state;
    xcb_atom_t wm_change_state;
    xcb_atom_t net_wm_name;
    xcb_atom_t net_wm_pid;
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
    /* Virtual-screen size and origin. kiwm has no large-desktop/viewport
     * scrolling, so the viewport is always 0,0 and the geometry is just the
     * root window's size -- but publishing both matters anyway: toolkits
     * read them to know how big "the desktop" is, and a WM that leaves them
     * unset (kiwm did) leaves whatever the previous WM wrote, or nothing at
     * all on a fresh session. */
    xcb_atom_t net_desktop_geometry;
    xcb_atom_t net_desktop_viewport;
    /* Advertised in _NET_SUPPORTED to tell clients that window placement is
     * the WM's job -- which per EWMH means they should stop constraining
     * their own popups/menus to a screen. That self-clamping is the
     * suspected cause of a Plasma panel popup on a second monitor landing
     * in the *first* monitor's corner: _NET_WORKAREA has room for only one
     * rectangle for the whole (multi-monitor) desktop, so a client applying
     * it to a window on any other output pulls it onto the primary one.
     * kiwm honors requested positions verbatim for exactly these windows
     * (they're never framed or moved, see should_manage_decorated()), which
     * is what this hint promises. */
    xcb_atom_t net_wm_full_placement;
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
    xcb_atom_t net_wm_state_fullscreen;
    xcb_atom_t net_wm_state_below;
    xcb_atom_t net_wm_icon;

    /* _NET_WM_ALLOWED_ACTIONS and its members -- published per client from
     * Client::allow_* (see client.c's update_client_actions()), so a
     * taskbar's window menu offers the same operations kiwm's own titlebar
     * does. Purely output: kiwm derives what's allowed from the client's
     * ICCCM/Motif hints, never from this property. */
    xcb_atom_t net_wm_allowed_actions;
    /* _NET_WM_MOVERESIZE: a client asking kiwm to take over a move/resize
     * *it* decided the user started -- how a window gets dragged by empty
     * space inside it (Qt's Breeze/Oxygen toolbars, GTK headerbars,
     * Steam's own chrome). See events.c's handle_moveresize(). */
    xcb_atom_t net_wm_moveresize;
    xcb_atom_t net_wm_action_move;
    xcb_atom_t net_wm_action_resize;
    xcb_atom_t net_wm_action_minimize;
    xcb_atom_t net_wm_action_shade;
    xcb_atom_t net_wm_action_maximize_horz;
    xcb_atom_t net_wm_action_maximize_vert;
    xcb_atom_t net_wm_action_fullscreen;
    xcb_atom_t net_wm_action_change_desktop;
    xcb_atom_t net_wm_action_close;

    xcb_atom_t net_wm_window_type;
    xcb_atom_t net_wm_window_type_normal;
    xcb_atom_t net_wm_window_type_dock;
    xcb_atom_t net_wm_window_type_desktop;
    xcb_atom_t net_wm_window_type_toolbar;
    xcb_atom_t net_wm_window_type_menu;
    /* Transient popup/tooltip-ish types -- like dock/desktop/toolbar/menu
     * above, client.c's should_manage_decorated() excludes these from
     * framing entirely (mapped as-is, geometry never touched, no
     * titlebar): the app already knows exactly where it wants these and
     * kiwm reparenting/repositioning them would only get in the way (this
     * is also what was silently misplacing a second output's panel
     * tooltips onto the first output before this existed -- an
     * unrecognized window type fell through to full client management
     * instead of the untouched passthrough these need). Common on a KDE
     * Plasma session's own popups (application launcher, applet popups,
     * panel tooltips) once Plasma is talking to a WM that isn't KWin. */
    xcb_atom_t net_wm_window_type_popup_menu;
    xcb_atom_t net_wm_window_type_dropdown_menu;
    xcb_atom_t net_wm_window_type_tooltip;
    xcb_atom_t net_wm_window_type_notification;
    xcb_atom_t net_wm_window_type_combo;
    xcb_atom_t net_wm_window_type_dnd;
    xcb_atom_t net_wm_window_type_splash;

    /* KDE's own window type for a Plasma applet's popup (the notification
     * popup, the clipboard/battery/volume applets, ...). Not part of EWMH,
     * and Plasma sets it *instead of* -- not alongside -- a standard type,
     * so without recognizing it by name those popups fall through to full
     * client management and get a titlebar drawn around them, which is
     * exactly what they must never have. Excluded from framing along with
     * the standard popup types above. */
    xcb_atom_t kde_net_wm_window_type_applet_popup;
    /* KDE's "manage me normally but draw no decoration" type (kwin calls
     * it noBorder) -- unlike the popup types this one is a real, framed,
     * focusable, taskbar-listed client, it just supplies its own chrome.
     * VirtualBox's VM window sets it, as do Plasma's own dock windows.
     * Feeds Client::undecorated, not the exclusion list. */
    xcb_atom_t kde_net_wm_window_type_override;
    /* _MOTIF_WM_HINTS: prehistoric, never standardized, and still the way
     * every toolkit (Qt's FramelessWindowHint, GTK's gtk_window_set_
     * decorated(false), SDL, ...) actually asks for an undecorated window.
     * Only its `decorations` field is read -- see client.c's
     * window_wants_no_decoration(). */
    xcb_atom_t motif_wm_hints;
    /* ICCCM's session-management "these windows are one app" pointer, read
     * only as a fallback for WM_HINTS' window_group -- see client.c's
     * window_group_leader() and Client::group_leader. */
    xcb_atom_t wm_client_leader;

    xcb_atom_t kiwm_outputs;
    xcb_atom_t kiwm_output_desktop;
    xcb_atom_t kiwm_num_output_desktops;
    xcb_atom_t kiwm_set_output_desktop;
    xcb_atom_t kiwm_wm_output;
    /* _KIWM_MINIMIZED_GEOMETRY: where a minimized window's frame was when
     * it went away, kept on the window for as long as it stays minimized
     * -- see PROTOCOL.md and client.c's minimize_client(). */
    xcb_atom_t kiwm_minimized_geometry;
} Atoms;

typedef struct {
    xcb_connection_t *conn;
    xcb_screen_t *screen;
    xcb_window_t root;
    xcb_visualtype_t *visual;
    int randr_event_base;
    bool shape_ext_present;  /* XCB SHAPE extension: rounded corners (see radius_tl etc) and
                              * forwarding a client's own shape onto its frame (shape.c). */
    int shape_event_base;    /* SHAPE's runtime event number -- see shape.c's shape_init(). */
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
    /* Single-axis resize cursors, for a drag started on an *edge* grip
     * rather than a corner (see KiWM::resize_axis_x/resize_axis_y). */
    xcb_cursor_t cursor_resize_n, cursor_resize_s, cursor_resize_e, cursor_resize_w;

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

    /* An unframed popup (a Plasma applet popup/menu -- see client.c's
     * should_manage_decorated()) that kiwm handed the keyboard to. These
     * windows are never Clients, so nothing else here tracks them, but
     * they're not override-redirect either: they're ordinary top-levels
     * the app expects the WM to focus, and one that never gets focus can
     * be clicked but never typed into (a Plasma launcher whose search
     * field silently swallows every keystroke). Remembered only so the
     * keyboard can be handed back to wm.focused when the popup goes away
     * -- otherwise focus stays on a destroyed window and the whole
     * session goes deaf. XCB_NONE when no such popup holds focus. */
    xcb_window_t focused_popup;

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

    /* Which titlebar button is currently held down, if any. Button
     * actions fire on *release*, over the same button the press landed
     * on -- pressing arms the button and releasing elsewhere cancels it,
     * which is how every other toolkit's buttons behave and the only way
     * a misplaced click can be taken back. It's also what btns.png's
     * third row (the "clicked" look) has always been for; nothing drew it
     * while actions fired on press, since there was no held-down moment
     * to show. Same encoding as hover_btn: an index into deco_layout, or
     * -1 for none. */
    Client *pressed_client;
    int pressed_btn;
    /* Which mouse button armed it: the maximize button does something
     * different for each (full / horizontal / vertical -- see events.c's
     * run_deco_button()), so the release has to fire the action the
     * *press* was for, not whichever button happens to come up. */
    uint8_t pressed_button;

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

    /* Whether an edge snap resizes the window *during* the drag
     * (kiwm.conf's live_snap_resize=, default 0/off) or only shows where
     * it's going to land -- outline.c's wireframe rectangle -- and applies
     * the real geometry when the button is released. Off by default
     * because the live version means a window that jumps to half the
     * screen and back as the pointer crosses the edge zone, repeatedly,
     * while the user is still deciding; the outline is what xfwm and
     * kwin's own "electric borders" show instead. */
    bool live_snap_resize;

    /* How thick the outline's band is, in pixels (kiwm.conf's
     * outline_width=, default 16) -- outline.c's wireframe preview, used
     * by the switcher and by snap previews. Straddles the outlined
     * window's edge, half of it outside and half in (an odd value puts the
     * extra pixel inside). */
    int outline_width;

    /* How wide the invisible resize grip along a window's edges is, in
     * pixels (kiwm.conf's resize_grip=, default 12; 0 disables it). A plain
     * left-click landing within this far of a frame edge starts a
     * resize -- from that corner if it's within the grip of two edges at
     * once, otherwise along that one axis -- instead of going to the
     * client. It works regardless of decoration: kiwm already takes every
     * button press on a client window through a synchronous grab and
     * replays the ones it doesn't want (see events.c's
     * handle_button_press()), so the grip needs no visible border to live
     * in, which is the whole point for windows that have none. Keep it
     * small: every pixel of it is a pixel the application doesn't get. */
    int resize_grip;

    /* Which grip zone the pointer is currently hovering, if any, and the
     * pointer grab held while it is -- purely to show a resize cursor
     * there, since the grip is invisible and otherwise undiscoverable.
     * The grab is taken with owner_events set, so the application still
     * gets its own pointer events; only the cursor image changes. See
     * events.c's update_resize_grip_cursor(). */
    bool grip_hover_active;
    int grip_hover_zone;   /* GripZone, -1 when none */

    /* Whether a resize changes the window as the pointer moves
     * (kiwm.conf's live_resize=, default 1/on) or only draws an outline of
     * the size it's heading for, applying it once the button is released.
     * Covers every resize the same way -- the edge/corner grip, a
     * modifier-drag, a client's own _NET_WM_MOVERESIZE request -- since
     * they're one gesture as far as the user is concerned. The outline is
     * the same one the switcher and snap previews use (outline.c). */
    bool live_resize;

    /* Where the deferred (live_resize=0) resize has got to: the frame rect
     * the outline is currently showing, applied to the client by
     * handle_button_release(). Only meaningful while
     * resize_preview_active. */
    bool resize_preview_active;
    int resize_preview_x, resize_preview_y;     /* frame position */
    int resize_preview_w, resize_preview_h;     /* *content* size, what Client::width/height hold */
    /* SNAP_NONE/current snap side engaged by the drag in progress, and the
     * output it was computed against -- reset at the start of every drag
     * in handle_button_press. Separate from Client::snap_side because a
     * window not yet released still has its *pending* snap tracked here,
     * so handle_motion can tell when the pointer has moved out of the edge
     * zone again and needs to restore the pre-drag floating geometry. */
    SnapSide drag_snap_side;

    /* How close (in pixels) a dragged window's frame edge must get to
     * another window's frame edge (any client, decoration included --
     * frame rects, not content rects) or to its own output's screen edge
     * before it snaps flush against it -- kiwm.conf's magnet_threshold=
     * (default 10). 0 disables it. Distinct from snap_threshold/
     * drag_snap_side above: that's screen-edge *tiling* (maximize/half-
     * width, a whole different geometry); this is just a few pixels of
     * position nudging so two windows (or a window and the screen edge)
     * end up touching with no gap instead of a near-miss. See events.c's
     * magnet_snap_move()/magnet_snap_resize(). */
    int magnet_threshold;

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
    /* Which axes a resize actually changes. Both true for a corner drag
     * (kiwm's only kind until edge grips existed); a drag started on a
     * left/right edge leaves the height alone and vice versa, which is
     * what makes an edge grip feel like one instead of a corner in
     * disguise. See events.c's handle_motion(). */
    bool resize_axis_x, resize_axis_y;
    double last_drag_apply_ms;  /* see DRAG_REDRAW_FALLBACK_MS / events.c's handle_motion() */

    /* Set for the whole drag by events.c's begin_drag() when a DRAG_RESIZE
     * starts on the shared edge of two half-snapped windows (Client::
     * snap_side LEFT/RIGHT) that are still touching, with
     * link_resize_neighbors= on -- resizing them should just resize both
     * in place, still half-snapped, not detile back to whatever floating
     * size they had before being snapped (the normal behavior every other
     * resize/move on a snapped window still gets, via detile_for_drag()).
     * Only ever true for DRAG_RESIZE; a DRAG_MOVE always detiles a snapped
     * window regardless of this setting. */
    bool drag_preserve_snap;

    /* A DRAG_MOVE started on a maximized/half-tiled window, and that state
     * hasn't been given up yet: the window stays where it is until the
     * pointer has travelled DRAG_DETILE_THRESHOLD from the press point,
     * at which point handle_motion() detiles it under the cursor and
     * re-anchors the drag there. Clicking a maximized window's titlebar
     * (to press a button, or just to focus it) or mod-dragging it by a few
     * pixels used to restore it immediately, which made every such click a
     * gamble. */
    bool drag_detile_pending;

    /* Whether a resize drags along whatever's touching the edge being
     * resized (see ResizeNeighbor above) -- kiwm.conf's
     * link_resize_neighbors= (default 0/off). Off by default since it's a
     * surprising-until-you-expect-it behavior change to plain resizing;
     * events.c's begin_drag() skips detect_resize_neighbors() entirely
     * when this is false, so the feature has zero cost (not even the
     * detection scan) unless explicitly opted into. */
    bool link_resize_neighbors;

    /* Neighbors touching the moving *vertical* edge (left or right,
     * whichever wm.resize_right picks) and the moving *horizontal* edge
     * (top/bottom, wm.resize_bottom) respectively -- independent lists
     * since a corner resize drags both at once, and a window could
     * plausibly be a neighbor on one axis only, the other axis only, or
     * (cornered against the dragged window) both. Capped at
     * MAX_RESIZE_NEIGHBORS; a resize touching more windows than that along
     * one edge just leaves the extras alone -- same "don't chase full
     * conformance" spirit as everywhere else in kiwm. Reset to count 0 in
     * handle_button_release(); entries removed individually (compacted
     * down) by client.c's unmanage() if one of them gets destroyed
     * mid-drag. */
    ResizeNeighbor resize_neighbors_x[MAX_RESIZE_NEIGHBORS];
    int resize_neighbors_x_count;
    ResizeNeighbor resize_neighbors_y[MAX_RESIZE_NEIGHBORS];
    int resize_neighbors_y_count;
    /* The moving edge's own absolute (frame-space) coordinate at the exact
     * moment the drag started -- every motion event's neighbor update
     * compares *this* against the moving edge's current position to get
     * the total displacement to replay onto each neighbor. */
    int resize_edge_x_start, resize_edge_y_start;

    /* Manual double-click detection for the plain (non-button) titlebar
     * area -- X has no double-click event of its own, just consecutive
     * ButtonPress'es (see events.c's handle_button_press). */
    xcb_timestamp_t last_titlebar_click_time;
    xcb_window_t last_titlebar_click_frame;

    /* The newest server timestamp kiwm has seen on any event that carries
     * one (events.c's handle_event() records it). ICCCM requires a real
     * timestamp -- never CurrentTime -- in the WM_TAKE_FOCUS message a WM
     * sends when it hands a client the keyboard (see Client::takes_focus),
     * and a WM has no clock of its own: the only timestamps it can quote
     * are the ones the server handed it. */
    xcb_timestamp_t last_event_time;

    /* The one keycode kiwm resolves by hand (main.c's setup_wm()): the
     * fixed cancel key for a modal hold in progress (osd.c's overlays),
     * never grabbed and never configurable. Every actual shortcut lives in
     * keybind.c's table instead (kiwm.conf's key_*). */
    xcb_keycode_t key_escape;

    /* Whether Alt+Tab/Meta+Tab show a themed on-screen overlay while held
     * (window list / desktop grid, see osd.c) instead of switching
     * immediately on every Tab press -- kiwm.conf's osd_enabled= (default
     * 1/on). Off reverts Tab-cycling to the original immediate-switch
     * behavior (osd.c's osd_windows_step()/osd_desktops_step() just call
     * cycle_focus()/cycle_output_desktop() directly, no grab, no window). */
    bool osd_enabled;

    /* Whether osd.c's overlays apply each Tab step live (raising/focusing
     * the highlighted window, or switching to the highlighted desktop) as
     * you step through them, reverting back to whatever was active before
     * if the hold is cancelled with Escape -- vs. only applying once on
     * release, leaving everything untouched until then (the default,
     * matching a plain "browse, then decide" Alt+Tab). kiwm.conf's
     * osd_live_preview= (default 0/off). Meaningless (never read) when
     * osd_enabled is off. */
    bool osd_live_preview;

    /* Which output an overlay opens on (and lists/cycles windows or
     * desktops of) -- kiwm.conf's osd_output_follows_pointer= (default
     * 0/off: the output of the currently focused window, falling back to
     * the pointer's output only when nothing is focused -- unchanged from
     * before this existed). 1 always uses whichever output the pointer is
     * on at the moment the hold starts, polled once via output_for_pointer()
     * (osd.c's osd_pick_output()) -- deliberately *not* the same thing as
     * focus_follows_mouse=: this only decides which screen Alt+Tab/Meta+Tab
     * itself act on, never what receives actual keyboard input. Fixed for
     * the whole hold once picked, same as everything else about which
     * output an open overlay belongs to. */
    bool osd_output_follows_pointer;

    bool running;
} KiWM;

extern KiWM wm;

/* CLOCK_MONOTONIC in milliseconds, defined in main.c -- used by
 * wm.debug_resize instrumentation wherever it needs fine-grained timing
 * (events.c's handle_motion(), decoration.c's draw_decoration()). */
double monotonic_ms(void);

#endif /* KIWM_WM_H */
