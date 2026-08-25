#ifndef KIWM_DECORATION_H
#define KIWM_DECORATION_H

#include "wm.h"

void load_decoration(void);
bool client_deco_visible(Client *c);
void draw_decoration(Client *c);

/* Resolved position/width of one titlebar element (see wm.h's
 * DecoElemKind/wm.deco_layout). */
typedef struct {
    DecoElemKind kind;
    int x, width;
} DecoSlot;

/* Fills `out` (up to max_out) with each configured titlebar element's
 * on-screen [x, x+width) span for a frame of the given width, in
 * wm.deco_layout order -- the single shared layout math events.c's
 * hit-testing/hover and decoration.c's drawing both build on, so they can
 * never disagree about where an element actually is. Returns the number
 * of slots filled. */
int compute_deco_layout(int frame_width, DecoSlot *out, int max_out);

/* Loads (or reloads) c->icon from _NET_WM_ICON, picking whichever of the
 * property's multiple embedded sizes is closest to the titlebar icon
 * slot's size without being tiny. Safe to call with no icon present
 * (leaves c->icon NULL, drawn blank) or repeatedly (replaces any
 * previous surface). */
void load_client_icon(Client *c);

#endif /* KIWM_DECORATION_H */
