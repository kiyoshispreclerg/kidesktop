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

void desktop_shutdown(void);

#endif /* KICOMP_DESKTOP_H */
