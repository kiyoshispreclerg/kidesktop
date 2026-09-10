/* Per-output frame clocks -- see scheduler.h. */
#include "scheduler.h"
#include "effect.h"
#include "presenter.h"
#include "unredirect.h"

static double period_ms(const CompOutput *o)
{
    double hz = o->refresh_hz > 1.0 ? o->refresh_hz : 60.0;
    return 1000.0 / hz;
}

bool scheduler_may_paint(CompOutput *o, double now)
{
    if (now < o->next_frame_ms)
        return false;

    double p = period_ms(o);

    /* Advance from the deadline, not from `now`, so a steady animation
     * keeps a steady cadence instead of drifting a little later every
     * frame. Falling more than one period behind (the machine was busy,
     * the session was suspended) resets to now rather than trying to
     * catch up with a burst of frames nobody will see. */
    o->next_frame_ms += p;
    if (o->next_frame_ms < now)
        o->next_frame_ms = now + p;

    return true;
}

void scheduler_tick(double now)
{
    (void)now;

    /* Nothing to do, and that is the point.
     *
     * This used to mark every output dirty on every frame for as long as
     * any effect was running, on the reasoning that an animation changes
     * the picture without damaging a window. Every effect in effects/
     * does damage what it changes, in its own update(), because it is
     * the only thing that knows what area that is -- so the blanket
     * marking was never what made animations work. What it did instead
     * was repaint every screen, whole, at the refresh rate, for as long
     * as an effect existed.
     *
     * Which is harmless for something that lasts 180 ms and expensive
     * for something that does not: the zoom lens stays until it is wound
     * back out, the stats panel until the key is pressed again, and both
     * held the whole desktop at a full repaint per frame while showing a
     * picture that was not changing. The stats panel is what caught it:
     * it read 60 fps on an idle screen.
     *
     * What still comes from the effects being active is the *waking*
     * (scheduler_timeout below): the loop keeps a frame cadence so every
     * update() runs on time. Only the dirtying is gone. */
}

bool scheduler_wants_frame(CompOutput *o)
{
    if (!o->dirty || !o->render_data)
        return false;

    /* Not ours to paint: a window is filling this output and drawing
     * itself (unredirect.h). */
    if (unredirect_holds(o))
        return false;

    /* Blocked behind a frame in flight. Nothing to *wait* for either: the
     * Present completion arrives as an X event, and the loop is already
     * sleeping on that fd. */
    if (presenter && presenter->busy && presenter->busy(o))
        return false;

    return true;
}

int scheduler_timeout(double now)
{
    double earliest = -1.0;

    for (int i = 0; i < comp.output_count; i++) {
        CompOutput *o = &comp.outputs[i];

        /* Exactly the outputs the paint would paint, asked with exactly
         * the same question (scheduler_wants_frame).
         *
         * Asking a *different* question here is what made this loop spin.
         * It used to count "an effect is running" as reason enough for an
         * output to owe a frame, while the paint itself skips any output
         * that is not dirty -- and since next_frame_ms only advances when
         * a frame is actually painted, an output that is clean, or held
         * behind a frame in flight, keeps a deadline somewhere in the
         * past forever. So this returned 0, poll() did not sleep, the
         * paint painted nothing, and round it went: measured at ~95,000
         * iterations per second for as long as any animation lasted, each
         * one running every effect's update() and posting its damage
         * again. */
        if (!scheduler_wants_frame(o))
            continue;

        double due = o->next_frame_ms;
        if (due <= now)
            return 0;                /* already owed -- don't sleep at all */
        if (earliest < 0.0 || due < earliest)
            earliest = due;
    }

    /* An animation is running with nothing dirty this instant. Something
     * is still owed -- the next update(), which is where each effect
     * damages what it is about to change -- so this must not sleep until
     * the next X event, or the animation stalls mid-way.
     *
     * A deadline already in the past is read as "no deadline standing"
     * rather than "a frame is late": that output has not painted in a
     * while, so its clock says nothing about when its next frame is due.
     * Answering `now + one period` is what turns this from a zero timeout
     * into an actual sleep. A dirty output whose slot has genuinely passed
     * is unaffected -- it returned 0 above and scheduler_may_paint() lets
     * it paint at once. */
    if (earliest < 0.0 && effects_active()) {
        for (int i = 0; i < comp.output_count; i++) {
            CompOutput *o = &comp.outputs[i];
            double due = o->next_frame_ms > now ? o->next_frame_ms
                                                : now + period_ms(o);
            if (earliest < 0.0 || due < earliest)
                earliest = due;
        }
    }

    if (earliest < 0.0)
        return -1;                   /* idle: sleep until an X event */

    int ms = (int)(earliest - now);
    return ms < 0 ? 0 : ms;
}
