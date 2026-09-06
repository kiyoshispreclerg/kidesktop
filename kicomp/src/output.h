/*
 * kicomp - RandR outputs, each one an independent presentation unit.
 * See kiwm-kicomp-projeto.md sections 4 and 18.
 */
#ifndef KICOMP_OUTPUT_H
#define KICOMP_OUTPUT_H

#include "comp.h"

/* (Re)reads the root geometry and the RandR monitor list, then recreates
 * every per-output render target. Called at startup, on RandR screen
 * changes and on a root ConfigureNotify. Falls back to a single output
 * covering the whole root window when RandR isn't there or reports
 * nothing -- a compositor with zero outputs would just render nowhere. */
void outputs_refresh(void);

void outputs_teardown(void);

/* Marks every output the rectangle touches as dirty (root coordinates)
 * and records *which part* of it changed. Outputs the rectangle doesn't
 * reach stay clean and are not repainted -- section 39.
 *
 * The rectangle is grown by the shadow margin on the way in: a window's
 * shadow is drawn outside the window, so the area its change dirties is
 * bigger than the window itself. Callers never have to know that. */
void output_damage_rect(const CompRect *r);

/* Same, but grown by *this window's* shadow reach rather than the worst
 * case over both styles -- zero for a window shadow_for_window() would
 * refuse a shadow to (a maximized or fullscreen window above all: its
 * edges are the screen's, and growing its damage rectangle at all just
 * spills it across whatever output happens to sit past that edge). Use
 * this whenever the rectangle being damaged really is one window's, and
 * output_damage_rect() for anything else (a whole output, an effect's
 * own on-screen item). */
void output_damage_window_rect(const CompWindow *w, const CompRect *r);

void output_damage_all(void);

/* What to repaint on this output, for the renderer and the presenter. A
 * dirty output that nobody said anything specific about comes back as a
 * full region -- being told to paint and not being told where means
 * paint everything, never paint nothing. */
void output_paint_region(CompOutput *o, CompRegion *out);

/* Clean again: called once the frame has been presented. */
void output_painted(CompOutput *o);

#endif /* KICOMP_OUTPUT_H */
