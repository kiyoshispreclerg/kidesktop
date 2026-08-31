#ifndef KIWM_OSD_H
#define KIWM_OSD_H

#include "wm.h"
#include "output.h"   /* DesktopAxis -- which way osd_desktops_step() moves */

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

/* Called from keybind.c's run_action() for every window-switcher shortcut
 * (direction +1/-1) -- opens the window-switcher OSD on the first call of
 * a hold (grabbing the keyboard so the modifier's own release is
 * guaranteed to reach osd_handle_key_release() regardless of input focus),
 * or just steps the selection if one is already open. A no-op if
 * wm.osd_enabled is off (falls back to the plain immediate cycle_focus()
 * kiwm always had).
 *
 * `mods` is the pressed binding's own modifier mask: whichever modifier it
 * names is the one this hold is driven by, and therefore the one whose
 * release commits the selection. Shift is ignored in it (a "previous"
 * binding is the same hold as its "next" one, just shifted -- letting go
 * of Shift alone must not commit), and 0 -- nothing but a bare key -- falls
 * back to the kiwm.conf mod_cycle=/mod_control= default the shortcut would
 * have used before it was rebindable. */
void osd_windows_step(int direction, uint16_t mods);

/* Same idea for the desktop switcher. `axis` picks what a step means:
 * DESKTOP_AXIS_LINEAR walks the desktops in index order (what
 * key_desktop_next/prev do, unchanged), the other two move one column or
 * one row through the desktop_columns x desktop_rows grid. All of them
 * drive the same overlay, so an open hold can be stepped by any mix of
 * them. */
void osd_desktops_step(int direction, DesktopAxis axis, uint16_t mods);

/* Escape while either OSD is open: close it without committing (focus/
 * desktop stay exactly as they were before the hold started). No-op if no
 * OSD is open. */
void osd_cancel(void);

/* Whether `window` is the overlay's own window, currently up. The overlay
 * is override-redirect and never a Client, so this is how client.c's
 * restack_all() recognizes it while walking the root's children and puts
 * it in LAYER_OSD (see wm.h) -- the top layer, above even an active
 * fullscreen window. */
bool osd_owns_window(xcb_window_t window);

/* One explicit raise when the overlay is first mapped (mapping alone
 * doesn't restack). No-op when no overlay is up. */
void osd_raise_above_all(void);

/* Repaints the overlay after an Expose on its window -- a no-op for any
 * other window, or when no overlay is open. */
void osd_handle_expose(xcb_window_t window);

/* Whether either OSD is currently open (keyboard actively grabbed) --
 * events.c checks this to route Escape here instead of wherever it'd
 * normally go. */
bool osd_active(void);

/* events.c's handle_event() forwards every KeyRelease here unconditionally
 * (cheap no-op when osd_active() is false) -- osd.c is the only thing that
 * ever needs KeyRelease, since it's how the driving modifier's own release
 * is detected (see wm.h's key_alt_l/r/key_super_l/r doc comment). */
void osd_handle_key_release(xcb_key_release_event_t *ev);

/* Same check osd_handle_key_release() does -- "is the driving modifier
 * still down? if not, commit and close" -- but driven by main.c's event
 * loop on a timer instead of by an event, and a no-op when no OSD is open.
 *
 * The keyboard grab osd.c takes for a hold is not a guarantee that the
 * modifier's release will ever be delivered to kiwm: a client that grabs
 * the devices for itself while the overlay is up (VirtualBox capturing
 * input for its guest is the real-world case) can swallow it, and then no
 * further event of any kind arrives to notice it with -- the overlay just
 * stays on screen forever, keyboard still grabbed. Polling the live
 * modifier state is what makes that unwedge by itself; the click handling
 * in osd_handle_button_press() is the same safety net for input kiwm does
 * still receive. */
void osd_poll_release(void);

/* A mouse button was pressed while an overlay is open: ends the hold as if
 * the modifier had been released (committing the current selection) and
 * replays the click to whoever would normally have gotten it. Returns
 * whether it consumed the event -- false (and does nothing) when no
 * overlay is open, which is the normal case. */
bool osd_handle_button_press(xcb_button_press_event_t *ev);

/* client.c's unmanage() calls this for every destroyed client so a window
 * that closes mid-hold (e.g. crashes) can't be committed to or drawn --
 * removes it from the open tabbox's list in place (selection re-clamped),
 * a no-op if no window-switcher OSD is open or `c` isn't in its list. */
void osd_client_destroyed(Client *c);

#endif /* KIWM_OSD_H */
