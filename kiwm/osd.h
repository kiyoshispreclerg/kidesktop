#ifndef KIWM_OSD_H
#define KIWM_OSD_H

#include "wm.h"

/* On-screen overlays for Alt+Tab-style window switching and Meta+Tab-style
 * per-output desktop switching -- see osd.c's file comment for the overall
 * design (hold-to-preview, release-to-commit, themed like the decoration,
 * no compositor needed). */

/* One entry kiwm's tabbox is currently offering -- just enough for a
 * TabBoxOps implementation to render a row/cell and, on commit, to know
 * which Client to actually focus. */
typedef struct {
    Client *items[MAX_CLIENTS];
    int count;
    int selected; /* index into items, the one that would be committed right now */
} TabBoxState;

/* kwin calls this a "tabbox" -- kiwm's own window-switcher OSD is built
 * behind this same kind of small vtable on purpose, so a different visual
 * presentation (a grid of thumbnails, cover-flow, ...) can be swapped in
 * later by writing a new TabBoxOps and pointing osd.c's active_tabbox_ops
 * at it, with zero changes to the hold/release key handling or to the
 * window-list eligibility rules below. Only one implementation exists
 * today: simple_list_tabbox_ops, a plain vertical list of icon+title. */
typedef struct {
    const char *name;

    /* Fills state->items[0..count) with every Client eligible to switch to
     * (same rule cycle_focus() already used: mapped, not minimized, on
     * output_idx and (sticky or on `desktop`)) and sets state->selected to
     * wherever the currently-focused client landed (0 if none). Called
     * once when the OSD opens; the list is then held fixed for the whole
     * hold (see osd.c's file comment) except for individual removals via
     * osd_client_destroyed(). */
    void (*build)(TabBoxState *state, int output_idx, int desktop);

    /* Natural content size (excluding osd.c's own chrome padding) for this
     * many entries, so osd.c can size+center the OSD window. */
    void (*measure)(const TabBoxState *state, int *out_w, int *out_h);

    /* Paints the content area (already translated so 0,0 is its top-left
     * corner) into a `w`x`h` box. */
    void (*paint)(cairo_t *cr, const TabBoxState *state, int w, int h);
} TabBoxOps;

extern const TabBoxOps simple_list_tabbox_ops;

/* Called from events.c's handle_key_press() for every mod_cycle+Tab /
 * mod_cycle+Shift+Tab press (direction +1/-1) -- opens the window-switcher
 * OSD on the first call of a hold (grabbing the keyboard so the modifier's
 * own release is guaranteed to reach osd_handle_key_release() regardless
 * of input focus), or just steps the selection if one is already open.
 * A no-op if wm.osd_enabled is off (falls back to the plain immediate
 * cycle_focus() kiwm always had). */
void osd_windows_step(int direction);

/* Same idea for mod_control+Tab / mod_control+Shift+Tab (desktop switch). */
void osd_desktops_step(int direction);

/* Escape while either OSD is open: close it without committing (focus/
 * desktop stay exactly as they were before the hold started). No-op if no
 * OSD is open. */
void osd_cancel(void);

/* Whether either OSD is currently open (keyboard actively grabbed) --
 * events.c checks this to route Escape here instead of wherever it'd
 * normally go. */
bool osd_active(void);

/* events.c's handle_event() forwards every KeyRelease here unconditionally
 * (cheap no-op when osd_active() is false) -- osd.c is the only thing that
 * ever needs KeyRelease, since it's how the driving modifier's own release
 * is detected (see wm.h's key_alt_l/r/key_super_l/r doc comment). */
void osd_handle_key_release(xcb_key_release_event_t *ev);

/* client.c's unmanage() calls this for every destroyed client so a window
 * that closes mid-hold (e.g. crashes) can't be committed to or drawn --
 * removes it from the open tabbox's list in place (selection re-clamped),
 * a no-op if no window-switcher OSD is open or `c` isn't in its list. */
void osd_client_destroyed(Client *c);

#endif /* KIWM_OSD_H */
