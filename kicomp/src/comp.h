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

#include <stdbool.h>
#include <stdint.h>

#define MAX_OUTPUTS      16
#define MAX_SCENE_NODES 512

typedef struct CompRect {
    int x, y, w, h;
} CompRect;

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
    bool present;        /* Present extension -- not used yet (Fase 7) */
    bool flip_per_crtc;  /* XiS per-CRTC FLIP -- not probed yet (Fase 8) */
} CompCaps;

/* One output = one scene, one drawable, one clock, one presentation
 * (section 18). This prototype already keeps the per-output target and
 * dirty flag; the per-output clock/pacing is Fase 7. */
typedef struct CompOutput {
    int id;
    char name[32];

    CompRect rect;       /* root coordinates */
    double refresh_hz;   /* per-output, never a session-wide constant (section 46) */

    bool dirty;          /* section 39: never repaint an output "just in case" */

    /* Render target for this output, owned by the renderer backend and
     * read by the presenter. A GL renderer would keep its FBO/context in
     * render_data and expose an equivalent handle here. */
    xcb_render_picture_t target;

    void *render_data;
    void *present_data;
} CompOutput;

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

    uint8_t depth;
    xcb_visualid_t visual;
    bool argb;                 /* depth 32 visual -> its alpha channel is real */

    double opacity;            /* _NET_WM_WINDOW_OPACITY, 1.0 when unset */

    /* _KIWM_LAYER: kiwm marks its own overlay windows ("osd", "outline")
     * so the compositor can recognize them. Empty for everything else.
     * Whether they get composited at all is kicomp's call
     * (--skip-wm-layers) -- kiwm draws them regardless. */
    char wm_layer[16];

    xcb_damage_damage_t damage;

    /* XRender backend state (renderer-xrender.c). A second renderer would
     * add its own fields here or hang them off a void *render_data. */
    xcb_pixmap_t pixmap;
    xcb_render_picture_t picture;
    xcb_render_picture_t alpha;

    /* The window's SHAPE bounding region, in window-relative coordinates.
     * Without it a shaped window comes out square: the server clips a
     * window to its shape when *it* paints, but NameWindowPixmap hands
     * over the full rectangle and the clipping has to be redone here --
     * which is exactly what kiwm's rounded corners are. */
    xcb_xfixes_region_t shape;
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
    } atoms;

    bool running;
    bool verbose;

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
} KiComp;

extern KiComp comp;

/* comp_log: only with -v. comp_info: always, for the handful of lines
 * that should say out loud what the compositor decided to be. */
void comp_log(const char *fmt, ...);
void comp_info(const char *fmt, ...);

#endif /* KICOMP_COMP_H */
