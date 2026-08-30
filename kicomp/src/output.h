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

/* Marks every output the rectangle touches as dirty (root coordinates).
 * Outputs the rectangle doesn't reach stay clean and are not repainted --
 * section 39. */
void output_damage_rect(const CompRect *r);

void output_damage_all(void);

#endif /* KICOMP_OUTPUT_H */
