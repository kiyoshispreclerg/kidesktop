/* Monotonic time and easing curves -- see animation.h. */
#define _POSIX_C_SOURCE 200809L

#include "animation.h"
#include "comp.h"

#include <math.h>
#include <string.h>
#include <time.h>

double comp_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

double comp_anim_duration(double factor)
{
    double d = comp.anim_duration_ms * factor;
    return d > 0.0 ? d : 0.0;
}

float comp_progress(double now, double start, double duration)
{
    if (duration <= 0.0)
        return 1.0f;
    double p = (now - start) / duration;
    if (p < 0.0)
        return 0.0f;
    if (p > 1.0)
        return 1.0f;
    return (float)p;
}

float comp_ease_linear(float p)
{
    return p;
}

float comp_ease_in(float p)
{
    return p * p;
}

float comp_ease_out(float p)
{
    float q = 1.0f - p;
    return 1.0f - q * q;
}

float comp_ease_in_out(float p)
{
    return p < 0.5f ? 2.0f * p * p
                    : 1.0f - 2.0f * (1.0f - p) * (1.0f - p);
}

float comp_ease_spring(float p)
{
    /* A damped oscillation: an exponential envelope closing on 1, times a
     * cosine that carries it slightly past and back. Tuned so the
     * overshoot is a few percent and the wobble is gone well before the
     * end -- enough to read as weight, not as a bounce. Pinned at both
     * ends so the animation still starts exactly where it started and
     * lands exactly where it belongs. */
    if (p <= 0.0f)
        return 0.0f;
    if (p >= 1.0f)
        return 1.0f;
    return 1.0f - expf(-6.0f * p) * cosf(6.5f * p);
}

static const char *const easing_names[COMP_EASE_COUNT] = {
    "linear", "in", "out", "in-out", "spring",
};

const char *comp_easing_name(CompEasing e)
{
    if (e < 0 || e >= COMP_EASE_COUNT)
        return "?";
    return easing_names[e];
}

int comp_easing_parse(const char *name)
{
    for (int i = 0; i < COMP_EASE_COUNT; i++)
        if (strcmp(name, easing_names[i]) == 0)
            return i;

    /* The names people reach for first, from CSS and every other
     * animation vocabulary. */
    if (strcmp(name, "ease-in") == 0)     return COMP_EASE_IN;
    if (strcmp(name, "ease-out") == 0)    return COMP_EASE_OUT;
    if (strcmp(name, "ease-in-out") == 0) return COMP_EASE_IN_OUT;
    if (strcmp(name, "ease") == 0)        return COMP_EASE_OUT;

    return -1;
}

float comp_ease(CompEasing curve, float p)
{
    switch (curve) {
    case COMP_EASE_LINEAR: return comp_ease_linear(p);
    case COMP_EASE_IN:     return comp_ease_in(p);
    case COMP_EASE_IN_OUT: return comp_ease_in_out(p);
    case COMP_EASE_SPRING: return comp_ease_spring(p);
    case COMP_EASE_OUT:
    default:               return comp_ease_out(p);
    }
}

float comp_lerp(float a, float b, float p)
{
    return a + (b - a) * p;
}
