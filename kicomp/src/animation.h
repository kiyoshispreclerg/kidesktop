/*
 * kicomp - time and easing (section 20/40).
 *
 * The one rule this header exists to enforce: animation progress is
 * computed from a monotonic clock, never from a frame counter. The same
 * effect then takes the same wall-clock time on a 60 Hz output and on a
 * 144 Hz one, and an output that drops frames falls behind in smoothness
 * only, never in duration.
 */
#ifndef KICOMP_ANIMATION_H
#define KICOMP_ANIMATION_H

double comp_now_ms(void);

/* An effect's duration, as a multiple of the user's global
 * animation_duration (kicomp.conf). This is the only way an effect
 * should decide how long it lasts:
 *
 *     e->duration = comp_anim_duration(1.0);   the standard unit
 *     e->duration = comp_anim_duration(0.5);   should feel immediate
 *     e->duration = comp_anim_duration(2.0);   a big transition
 *
 * so one key in the config retimes every animation together, and a
 * desktop set to "fast" is fast everywhere. Returns 0 when the user
 * asked for no animation at all, which every effect must treat as
 * "finish immediately" rather than as a special case. */
double comp_anim_duration(double factor);

/* Clamped 0..1 progress of an animation started at `start` (ms). */
float comp_progress(double now, double start, double duration);

float comp_ease_linear(float p);
float comp_ease_in(float p);
float comp_ease_out(float p);
float comp_ease_in_out(float p);

/* Straight-line interpolation, the only mixing an effect should need to
 * do by hand. */
float comp_lerp(float a, float b, float p);

#endif /* KICOMP_ANIMATION_H */
