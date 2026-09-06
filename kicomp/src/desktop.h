/*
 * kicomp - which virtual desktop each output is showing, and which way it
 * just moved.
 *
 * The compositor doesn't manage desktops and never will (section 33): it
 * reads what the WM publishes on the root window and nothing else. What it
 * needs out of that is one question an effect can't answer for itself --
 * "the desktop just changed: in which direction?" -- because a wall that
 * always slides the same way is not a wall, it's a wipe.
 *
 * Two sources, in order of preference:
 *
 *   _KIWM_OUTPUTS + _KIWM_OUTPUT_DESKTOP   one current desktop per output
 *                                          (kiwm/PROTOCOL.md), which is the
 *                                          real model here: switching a
 *                                          desktop on one monitor must not
 *                                          animate the windows on another.
 *   _NET_CURRENT_DESKTOP                   the single global desktop every
 *                                          other WM has; applied to every
 *                                          output at once.
 *
 * Direction comes from _NET_DESKTOP_LAYOUT (columns x rows, row-major from
 * the top-left, which is what kiwm publishes): desktop index -> grid cell,
 * and the difference between the old cell and the new one is the direction.
 * With no layout published the desktops are treated as one row, so a switch
 * is left or right -- the same thing every pager assumes.
 */
#ifndef KICOMP_DESKTOP_H
#define KICOMP_DESKTOP_H

#include "comp.h"

/* Re-reads the properties above. Called for the root PropertyNotify on any
 * of them, and once at startup so the first switch has something to
 * compare against. */
void desktop_refresh(void);

/* The direction of the switch the output under `r`'s centre has just made,
 * as a grid step: (+1,0) for the desktop to the right, (0,-1) for the one
 * above, and so on. False when nothing switched recently, when that output
 * isn't one the WM tells us about, or when the two desktops sit in the same
 * cell -- in each of those cases an effect has nothing honest to animate
 * and should stay out of the way.
 *
 * `r` is a window's rectangle in root coordinates: the caller is asking on
 * behalf of a window, and a window belongs to the output it sits on. */
bool desktop_switch_for_rect(const CompRect *r, int *dx, int *dy);

/* What the WM publishes about its desktops, for an effect that lays them
 * out rather than merely animating a switch between them (the expo
 * grid). All of it is read from the root window; none of it is kicomp's
 * to decide (section 33).
 *
 * `desktop_grid()` fills the columns and rows the desktops sit in, from
 * _NET_DESKTOP_LAYOUT and the desktop count -- a count with no layout is
 * arranged as one row, which is what a pager assumes.
 *
 * `desktop_current_for_output()` is the desktop that output is showing;
 * `desktop_of_window()` the one a window belongs to, which under kiwm is
 * only meaningful together with its output (kiwm/PROTOCOL.md), so both
 * come back at once. -1 for anything the WM doesn't say.
 *
 * `desktop_request_switch()` asks the WM to show a desktop on an output:
 * _KIWM_SET_OUTPUT_DESKTOP where that exists, _NET_CURRENT_DESKTOP
 * otherwise. It asks; the WM decides, and the answer arrives as the
 * property changing like any other. */
/* What _NET_WM_DESKTOP's 0xFFFFFFFF means: the window is on every
 * desktop -- a panel, a wallpaper, anything the WM keeps everywhere. An
 * effect laying the desktops out has to draw it in every one of them. */
#define COMP_DESKTOP_ALL (-2)

int  desktop_count(void);
void desktop_grid(int *columns, int *rows);
int  desktop_current_for_output(const CompOutput *o);
int  desktop_output_index(const CompOutput *o);
bool desktop_of_window(const CompWindow *w, int *desktop, int *output_index);
bool desktop_request_switch(const CompOutput *o, int desktop);

/* Asks the WM to put every wallpaper it is hiding on screen for a
 * moment, underneath the one you can see, so that there is a picture of
 * it at all (kiwm/PROTOCOL.md's _KIWM_PRIME_DESKTOP_LAYERS).
 *
 * X frees an unmapped window's contents: a desktop the user has never
 * visited has never been mapped, so the expo grid would open with the
 * current desktop in one cell and nothing in the others. Only the WM can
 * put a window on screen, so the compositor asks -- and, as with every
 * other request here, what comes back is windows mapping and unmapping
 * like any other. False where the WM doesn't answer to this. */
bool desktop_request_prime(void);

/* Asks the WM to put one window that belongs to a desktop nobody is
 * showing back on screen for `ms`, so that there is a *live* picture of
 * it rather than the one it was left with (kiwm/PROTOCOL.md's
 * _KIWM_HOLD_WINDOW).
 *
 * Nothing reaches the application and nothing about the window's state
 * changes; the WM marks it _KIWM_HELD for as long as it lasts, which is
 * how window.c knows the map is not an arrival. There is no way to hand
 * it back early on purpose: the WM is the one that undoes it, so a
 * compositor that dies mid-picture cannot leave the session wrong. An
 * effect that wants to keep it asks again. False where the WM doesn't
 * answer to this. */
bool desktop_request_hold(const CompWindow *w, int ms);

void desktop_shutdown(void);

#endif /* KICOMP_DESKTOP_H */
