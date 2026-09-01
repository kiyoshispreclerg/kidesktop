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

/* How an animation is weighted between its two ends. The names are the
 * ones kicomp.conf uses (easing=), and the shapes are the usual four
 * plus a spring:
 *
 *   linear   even the whole way -- a straight interpolation
 *   in       slow at the start, fastest as it arrives (weight at the
 *            origin: it accelerates away from where it came from)
 *   out      fastest at the start, easing into the destination (weight
 *            at the destination -- the default, and what most desktops
 *            use, because arriving gently is what reads as "settled")
 *   in-out   slow at both ends, quick through the middle
 *   spring   overshoots slightly and settles back, the way a real object
 *            with mass would
 *
 * Keep in sync with easing_names[] in animation.c. */
typedef enum {
    COMP_EASE_LINEAR = 0,
    COMP_EASE_IN,
    COMP_EASE_OUT,
    COMP_EASE_IN_OUT,
    COMP_EASE_SPRING,
    COMP_EASE_COUNT
} CompEasing;

const char *comp_easing_name(CompEasing e);
/* The easing by name, or -1 for one that doesn't exist (how config.c
 * reports a typo). */
int comp_easing_parse(const char *name);

/* Applies a curve to linear progress. */
float comp_ease(CompEasing curve, float p);

float comp_ease_linear(float p);
float comp_ease_in(float p);
float comp_ease_out(float p);
float comp_ease_in_out(float p);
float comp_ease_spring(float p);

/* Straight-line interpolation, the only mixing an effect should need to
 * do by hand. */
float comp_lerp(float a, float b, float p);

#endif /* KICOMP_ANIMATION_H */
