/* Per-output frame clocks -- see scheduler.h. */
#include "scheduler.h"
#include "effect.h"

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
    if (!effects_active())
        return;

    /* An animation changes what the scene looks like without anything
     * damaging a window, so the frames have to come from here. Only
     * outputs whose deadline arrived: a 60 Hz output does not get woken
     * at 144 Hz because it shares a screen with one. */
    for (int i = 0; i < comp.output_count; i++) {
        CompOutput *o = &comp.outputs[i];
        if (now >= o->next_frame_ms)
            o->dirty = true;
    }
}

int scheduler_timeout(double now)
{
    double earliest = -1.0;
    bool animating = effects_active();

    for (int i = 0; i < comp.output_count; i++) {
        CompOutput *o = &comp.outputs[i];

        /* Nothing owed: neither an animation nor a repaint waiting for
         * this output's next slot. */
        if (!animating && !o->dirty)
            continue;

        double due = o->next_frame_ms;
        if (due <= now)
            return 0;                /* already owed -- don't sleep at all */
        if (earliest < 0.0 || due < earliest)
            earliest = due;
    }

    if (earliest < 0.0)
        return -1;                   /* idle: sleep until an X event */

    int ms = (int)(earliest - now);
    return ms < 0 ? 0 : ms;
}
