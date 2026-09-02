/*
 * kicomp - shared types and global compositor state.
 *
 * Every other module includes this header. The global `comp` instance is
 * defined in main.c (same arrangement as kiwm's wm.h/main.c).
 *
 * Deliberately mirrors kiwm-kicomp-projeto.md's vocabulary: output is the
 * unit of presentation (section 4/18), the compositor keeps only a visual
 * mirror of the WM's logical state (section 33), and nothing here may be
 * required for kiwm to work on its own (section 31).
 */
#ifndef KICOMP_COMP_H
#define KICOMP_COMP_H

#include <xcb/xcb.h>
#include <xcb/composite.h>
#include <xcb/damage.h>
#include <xcb/xfixes.h>
#include <xcb/render.h>
#include <xcb/randr.h>

#include <stdbool.h>
#include <stdint.h>

#define MAX_OUTPUTS      16
#define MAX_SCENE_NODES 512

typedef struct CompRect {
    int x, y, w, h;
} CompRect;

/* What changed, as a handful of rectangles. The type lives here because
 * an output carries one; everything you can *do* with it is in region.h.
 * Deliberately client-side rather than an XFixes region -- see that
 * header for why. */
#define COMP_REGION_MAX 16
typedef struct CompRegion {
    CompRect rects[COMP_REGION_MAX];
    int count;
    bool full;     /* "everything": output_damage_all(), or an overflow */
} CompRegion;

/* Intersection in root coordinates. Returns false (and leaves *out
 * untouched) when the two rectangles don't overlap at all -- that's the
 * "is this window visible on this output" test of section 26. */
bool rect_intersect(const CompRect *a, const CompRect *b, CompRect *out);

/* Runtime capability detection (section 17/30). Never branch on "is this
 * XiS" -- branch on whether the specific capability answered. */
typedef struct CompCaps {
    bool composite;      /* Composite extension present */
    bool overlay;        /* Composite >= 0.3, i.e. the overlay window exists */
    bool damage;
    bool xfixes;
    bool render;
    bool randr;
    bool shape;          /* SHAPE: non-rectangular windows, rounded corners */
    bool present;        /* Present extension: vblank-timed presentation */
    /* X-INPUT-SCALE: per-CRTC cursor confinement (inputscale.h). This is
     * what per-output scaling depends on -- without it kicomp scales
     * nothing, because the pointer could then reach scanout the
     * compositor isn't drawing a desktop into. */
    bool input_scale;
    bool flip_per_crtc;  /* XiS per-CRTC FLIP -- not probed yet (Fase 8) */
} CompCaps;

/* One output = one scene, one drawable, one clock, one presentation
 * (section 18). This prototype already keeps the per-output target and
 * dirty flag; the per-output clock/pacing is Fase 7. */
typedef struct CompOutput {
    int id;
    char name[32];

    /* Two rectangles, and the difference between them is this project's
     * whole HiDPI story (section 56).
     *
     *   rect      the *logical* box: where windows live, in root
     *             coordinates. Everything above the renderer -- the
     *             scene, the effects, damage, the WM itself -- works in
     *             these coordinates and nothing else.
     *   physical  the CRTC's actual scanout box. Bigger than the logical
     *             one by `scale` when this output is being scaled.
     *
     * At scale 1.0 they are the same rectangle, which is the whole of
     * what every output was before scaling existed.
     *
     * The compositor draws the logical scene into a physical-sized
     * target, magnifying it. That is what makes a 4K monitor show a
     * 1920-wide desktop *sharply* -- with X-DENSITY (density.h) the
     * clients redraw their own contents at the same factor, so what gets
     * magnified is only the windows that didn't. */
    CompRect rect;       /* logical, root coordinates */
    CompRect physical;   /* what the CRTC actually scans out */
    float scale;         /* physical / logical; 1.0 = not scaled */

    double refresh_hz;   /* per-output, never a session-wide constant (section 46) */

    /* The RandR CRTC scanning this output out, when there is one. What
     * Present needs to be told so a frame is timed against *this*
     * monitor's vblank rather than against whichever one the server would
     * have picked -- and, later, what a per-CRTC flip is aimed at. */
    xcb_randr_crtc_t crtc;

    bool dirty;          /* section 39: never repaint an output "just in case" */

    /* *What* changed on it, in root coordinates, since it was last
     * painted. `dirty` says whether to paint at all; this says how much
     * of it to paint. A dirty output whose region is empty is repainted
     * whole -- so any path that marks an output dirty without saying
     * where errs towards a correct frame rather than a missing one. */
    CompRegion damage;

    /* This output's own frame clock (scheduler.c): when it may next be
     * painted, in monotonic ms. Zero means "immediately", which is what
     * a freshly created output wants. */
    double next_frame_ms;

    /* Render target for this output, owned by the renderer backend and
     * read by the presenter. A GL renderer would keep its FBO/context in
     * render_data and expose an equivalent handle here. */
    xcb_render_picture_t target;

    void *render_data;
    void *present_data;
} CompOutput;

/* EWMH/ICCCM states kicomp tracks, so that a bare X transition can be
 * told apart from what it *means*: an unmap is a close, a minimize or a
 * desktop leaving; a resize is a maximize, a shade, a fullscreen or just
 * a resize. Effects are configured in those terms, never in X's. */
typedef enum {
    COMP_STATE_MAXIMIZED  = 1 << 0,
    COMP_STATE_SHADED     = 1 << 1,
    COMP_STATE_FULLSCREEN = 1 << 2,
    COMP_STATE_MINIMIZED  = 1 << 3,   /* _NET_WM_STATE_HIDDEN or WM_STATE=Iconic */
    COMP_STATE_ABOVE      = 1 << 4,   /* _NET_WM_STATE_ABOVE: always on top */
} CompWindowState;

/* Window types, straight from _NET_WM_WINDOW_TYPE and one per EWMH
 * value, so an effect can be pointed at exactly what its author (or the
 * user) means -- "tooltips but not menus" is a real preference, and a
 * coarser classification would have to guess for them.
 *
 * Keep in sync with window_type_names[] in effect.c. */
typedef enum {
    COMP_WINDOW_UNKNOWN = 0,   /* no type set: an ordinary window, said quietly */
    COMP_WINDOW_NORMAL,
    COMP_WINDOW_DIALOG,
    COMP_WINDOW_UTILITY,
    COMP_WINDOW_TOOLBAR,
    COMP_WINDOW_SPLASH,
    COMP_WINDOW_MENU,
    COMP_WINDOW_DROPDOWN_MENU,
    COMP_WINDOW_POPUP_MENU,
    COMP_WINDOW_COMBO,
    COMP_WINDOW_TOOLTIP,
    COMP_WINDOW_NOTIFICATION,
    COMP_WINDOW_DND,
    COMP_WINDOW_DOCK,
    COMP_WINDOW_DESKTOP,
    COMP_WINDOW_TYPE_COUNT
} CompWindowType;

/* The compositor's mirror of one top-level (a direct child of root -- with
 * kiwm that means the frame window, whose backing pixmap already contains
 * the reparented client and the Cairo decoration). */
typedef struct CompWindow {
    struct CompWindow *next;   /* stacking order, bottom-most first */

    xcb_window_t id;

    int x, y, w, h;
    int border;

    bool mapped;
    bool input_only;           /* InputOnly windows have no contents to draw */
    bool override_redirect;    /* not managed by the WM: menus, tooltips, docks
                                * that place themselves -- and never framed, so
                                * such a window is its own client */

    uint8_t depth;
    xcb_visualid_t visual;
    bool argb;                 /* depth 32 visual -> its alpha channel is real */

    double opacity;            /* _NET_WM_WINDOW_OPACITY, 1.0 when unset */

    /* _KIWM_LAYER: kiwm marks its own overlay windows ("osd", "outline")
     * so the compositor can recognize them. Empty for everything else.
     * Whether they get composited at all is kicomp's call
     * (--skip-wm-layers) -- kiwm draws them regardless. */
    char wm_layer[16];

    /* What kind of thing this is, and the client window inside the frame
     * that says so. A WM-framed window is a frame whose child carries all
     * the EWMH properties; an override-redirect menu is its own client.
     * Effects need this to be told apart -- "fade menus but not docks" is
     * a normal thing to want. */
    CompWindowType type;
    xcb_window_t client;       /* XCB_NONE until resolved */

    /* Who this window belongs with. An application is often several
     * windows -- a main window, its dialogs, a tool palette, VirtualBox's
     * detached mini-toolbar -- and an effect that moves windows out of
     * each other's way has no business making one of them dodge another.
     * Read once, when the client is resolved:
     *
     *   leader  WM_CLIENT_LEADER, the ICCCM group hint
     *   pid     _NET_WM_PID, the fallback for the many windows that
     *           publish no leader at all
     *   transient_for  a dialog's parent
     */
    xcb_window_t leader;
    xcb_window_t transient_for;
    uint32_t pid;

    /* When this window was last reconfigured, and how many configures
     * arrived back to back -- how window.c tells a drag (a stream) from
     * a maximize (one jump), which is the difference between an effect
     * that helps and one that lags behind the pointer. */
    double last_configure_ms;
    int fast_configures;

    /* An effect can ask for a window to stay in the scene after X has
     * taken it away -- closing, minimizing and leaving a desktop are all
     * "the window is gone before the animation is". The contents pixmap
     * we named stays valid for as long as we hold it, unmapped or
     * destroyed, so there is something real left to draw.
     *
     * retain_count > 0: at least one effect is still drawing it.
     * zombie: the X window itself is gone; only our pixmap remains, and
     * the mirror entry disappears when the last effect lets go. */
    int retain_count;
    bool zombie;

    /* EWMH state (CompWindowState), and what it was before the batch of
     * events being processed -- the difference is what says "this resize
     * was a maximize". */
    uint32_t state;
    uint32_t state_before;

    bool focused;
    bool has_been_mapped;      /* distinguishes opening from coming back */

    /* Where this window sat in the stack at the end of the *previous*
     * batch of events -- 0 at the bottom, upwards. Not the current order,
     * which is in the list itself: this is the order from before whatever
     * just happened, and the difference is the only way to answer "was
     * this window covering that one a moment ago?". A raise and the focus
     * that comes with it arrive together, so by the time anything is
     * classified the raise has already happened and the current stacking
     * can no longer tell you what was on top of what. */
    int z_before;

    /* Deferred until the event queue is drained: at unmap time we don't
     * yet know whether the window was closed, minimized or left behind on
     * another desktop, and at configure time the state property saying
     * "maximized" may still be in flight. Classifying a beat later, with
     * the whole batch in hand, is what makes the semantic events honest.
     * See window.c's windows_flush_events(). */
    bool pending_appear;
    bool pending_disappear;
    /* Focus is deferred for a reason of its own: the WM publishes
     * _NET_ACTIVE_WINDOW and restacks the window as one gesture, in
     * either order, so an effect that wants to know what the newly
     * focused window is *in front of* has to be told once both have
     * arrived. */
    bool pending_focus;
    bool pending_unfocus;
    bool pending_geometry;
    bool pending_state;
    bool pending_interactive;
    CompRect pending_from;

    xcb_damage_damage_t damage;

    /* The server has damage waiting for this window, not yet collected.
     * Nothing is asked of the server when the event arrives: the Damage
     * object accumulates on its own, and one subtract per frame both
     * fetches everything since the last one and re-arms the reporting.
     * See damage.c. */
    bool damage_pending;

    /* XRender backend state (renderer-xrender.c). A second renderer would
     * add its own fields here or hang them off a void *render_data. */
    xcb_pixmap_t pixmap;
    xcb_render_picture_t picture;
    xcb_render_picture_t alpha;
    float alpha_value;         /* what `alpha` currently holds */

    /* The contents the window had *before* the resize that just replaced
     * them. A window being rolled up has already collapsed to its
     * titlebar by the time the compositor is told it was a shade, and the
     * pixels that have to roll up are gone from the live pixmap -- but
     * not from this one, because a named pixmap stays ours until we free
     * it. Held by whichever effect is drawing it, freed as soon as
     * nothing is (renderer.h's stash calls). */
    xcb_pixmap_t prev_pixmap;
    xcb_render_picture_t prev_picture;
    CompRect prev_rect;
    int prev_holds;

    /* X-DENSITY (density.h): the client redrawing its own contents at a
     * multiple of its logical size, into a pixmap of its own. The window
     * keeps its geometry; only these pixels are denser.
     *
     *   density_requested  we asked (and must un-ask when we stop)
     *   density_num/den    what the client says it is *actually* drawing
     *                      at -- believed over what we asked for
     *   density_pixmap     the auxiliary pixmap it published
     *   density_picture    the renderer's cached Picture for it
     *   client_rect        where the client sits inside its frame, since
     *                      that pixmap holds the client's content without
     *                      the decoration around it
     */
    bool density_requested;
    uint32_t density_num, density_den;
    xcb_pixmap_t density_pixmap;
    xcb_render_picture_t density_picture;
    CompRect client_rect;

    /* And the same again for the *frame*, which is a client of the
     * protocol too: the decoration's pixels are the WM's, drawn at
     * logical size into the frame, so the only way to have a sharp
     * titlebar on a scaled output is to ask the WM for one (kiwm's
     * density.c answers). Its pixmap covers the whole frame with the
     * client's area left transparent, so it composites straight over the
     * window and the client's own contents show through. */
    bool deco_density_requested;
    uint32_t deco_density_num, deco_density_den;
    xcb_pixmap_t deco_density_pixmap;
    xcb_render_picture_t deco_density_picture;

    /* The window's SHAPE bounding region, in window-relative coordinates.
     * Without it a shaped window comes out square: the server clips a
     * window to its shape when *it* paints, but NameWindowPixmap hands
     * over the full rectangle and the clipping has to be redone here --
     * which is exactly what kiwm's rounded corners are. */
    xcb_xfixes_region_t shape;

    /* The shape's own extents, in window coordinates, and whether the
     * window is shaped at all. A shaped window's *rectangle* can be far
     * bigger than the window you can see -- VirtualBox's mini-toolbar is
     * a full-screen window with a small bar carved out of it -- so
     * anything that reasons about where a window visually is (the shadow)
     * has to ask the shape, not the geometry. */
    bool shaped;
    CompRect shape_extents;
} CompWindow;

typedef struct KiComp {
    xcb_connection_t *conn;
    xcb_screen_t *screen;
    int screen_num;

    xcb_window_t root;
    xcb_window_t overlay;      /* Composite overlay window (the COW) */
    xcb_window_t cm_window;    /* owns _NET_WM_CM_Sn */

    int root_w, root_h;

    CompCaps caps;

    uint8_t damage_event;
    uint8_t xfixes_event;
    uint8_t present_opcode;    /* Present events arrive as XGE generic events */
    uint8_t randr_event;
    uint8_t shape_event;

    CompOutput outputs[MAX_OUTPUTS];
    int output_count;

    CompWindow *stack;         /* bottom-most first */

    struct {
        xcb_atom_t net_wm_cm;
        xcb_atom_t net_wm_window_opacity;
        xcb_atom_t xrootpmap_id;
        xcb_atom_t esetroot_pmap_id;
        xcb_atom_t kiwm_layer;
        xcb_atom_t kicomp_quit;

        /* _NET_WM_WINDOW_TYPE and the handful of values that decide a
         * CompWindowKind. */
        xcb_atom_t net_wm_window_type;
        xcb_atom_t type_normal;
        xcb_atom_t type_dialog;
        xcb_atom_t type_utility;
        xcb_atom_t type_toolbar;
        xcb_atom_t type_splash;
        xcb_atom_t type_dock;
        xcb_atom_t type_desktop;
        xcb_atom_t type_menu;
        xcb_atom_t type_dropdown_menu;
        xcb_atom_t type_popup_menu;
        xcb_atom_t type_combo;
        xcb_atom_t type_tooltip;
        xcb_atom_t type_notification;
        xcb_atom_t type_dnd;

        /* State, so an unmap or a resize can be classified (see
         * CompWindowState). */
        xcb_atom_t wm_state;               /* ICCCM WM_STATE: Normal vs Iconic */
        xcb_atom_t net_wm_state;
        xcb_atom_t state_maximized_horz;
        xcb_atom_t state_maximized_vert;
        xcb_atom_t state_shaded;
        xcb_atom_t state_fullscreen;
        xcb_atom_t state_hidden;
        xcb_atom_t state_above;
        xcb_atom_t wm_client_leader;
        xcb_atom_t net_wm_pid;
        xcb_atom_t net_wm_icon_geometry;   /* where the taskbar keeps this window */
        xcb_atom_t net_active_window;
        xcb_atom_t net_current_desktop;
        xcb_atom_t net_desktop_layout;     /* the grid the desktops sit in */
        /* X-DENSITY (density.h) */
        xcb_atom_t density_manager;
        xcb_atom_t density_requested;
        xcb_atom_t density_scale;
        xcb_atom_t density_pixmap;

        xcb_atom_t randr_dpi;              /* the fork's per-output "DPI" */
        xcb_atom_t xis_confined_area;      /* published: the logical boxes */
        xcb_atom_t kiwm_outputs;           /* output names, in index order */
        xcb_atom_t kiwm_output_desktop;    /* one current desktop per output */
    } atoms;

    bool running;
    bool verbose;

    /* When a desktop switch was last seen on the root window. A window
     * disappearing right after one left with the desktop rather than
     * being closed -- the only evidence available without the IPC of
     * section 32. */
    double desktop_changed_ms;

    xcb_window_t active_window;   /* _NET_ACTIVE_WINDOW, for focus events */

    /* --single-drawable: one drawable for the whole screen instead of one
     * per output. The per-output pipeline is the real one (section 18);
     * this exists as a fallback/comparison for servers or drivers where
     * per-output presentation misbehaves, and it is the one place where
     * that architectural rule is deliberately switched off. */
    bool single_drawable;

    /* --skip-wm-layers: leave kiwm's own overlay windows (_KIWM_LAYER)
     * out of the scene, for when the compositor draws its own switcher/
     * preview effects instead of showing the WM's. */
    bool skip_wm_layers;

    /* Effects, and the single number they are all written in terms of
     * (kicomp.conf: effects=, animation_duration=). No effect states a
     * duration in milliseconds of its own -- each asks for a multiple of
     * this one (animation.h's comp_anim_duration), so this one key
     * retimes the whole desktop coherently instead of leaving a
     * collection of independently-tuned animations. */
    bool effects;
    double anim_duration_ms;

    /* Backend choice from kicomp.conf (renderer=, presenter=). "auto"
     * lets capability detection decide, which is the answer for anyone
     * not deliberately comparing one backend against another. */
    char renderer_name[16];
    char presenter_name[16];
} KiComp;

extern KiComp comp;

/* comp_log: only with -v. comp_info: always, for the handful of lines
 * that should say out loud what the compositor decided to be. */
void comp_log(const char *fmt, ...);
void comp_info(const char *fmt, ...);

#endif /* KICOMP_COMP_H */
