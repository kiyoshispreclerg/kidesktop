/*
 * kicomp - per-output frame pacing (section 19).
 *
 * Each output has its own clock, running at its own refresh rate: a
 * 144 Hz monitor never waits for a 60 Hz one, and an animation's *state*
 * comes from the shared monotonic clock while each output merely samples
 * it at its own rhythm (section 49's critical test).
 *
 * This is the software half of Fase 7. There is no MSC/UST yet -- the
 * period comes from RandR's reported refresh rate, not from presentation
 * feedback -- so it paces and coalesces but does not yet lock to vblank.
 * When the Present/XiS presenter can report a real MSC, only
 * scheduler_tick() changes.
 *
 * Two jobs, both cheap:
 *
 *   - coalescing: a burst of damage from one application is one repaint,
 *     not one per event;
 *   - pacing: while an animation runs, each output is asked to repaint at
 *     its own period rather than as fast as the CPU allows.
 *
 * An output whose deadline has already passed repaints immediately, so an
 * isolated event (a keystroke's cursor blink) is never delayed by this.
 */
#ifndef KICOMP_SCHEDULER_H
#define KICOMP_SCHEDULER_H

#include "comp.h"

/* Whether `o` may be painted now. Marks the next deadline when it says
 * yes, so callers must actually paint when it does. */
bool scheduler_may_paint(CompOutput *o, double now);

/* While effects run, every output needs frames whether or not anything
 * damaged them -- this marks the ones whose deadline has arrived. */
void scheduler_tick(double now);

/* Whether this output is one the next paint would actually paint: dirty,
 * with backend state, not handed to a window drawing itself, and not
 * already waiting on a frame in flight.
 *
 * Shared by the paint and by scheduler_timeout() on purpose. The two used
 * to decide it separately, disagree, and between them spin the main loop
 * at tens of thousands of iterations a second for the whole length of any
 * animation -- see scheduler.c. */
bool scheduler_wants_frame(CompOutput *o);

/* poll() timeout in ms until the earliest output that still owes a frame,
 * or the next animation frame when an effect is running with nothing
 * dirty; -1 when nothing is pending and the compositor can sleep until an
 * X event. */
int scheduler_timeout(double now);

#endif /* KICOMP_SCHEDULER_H */
