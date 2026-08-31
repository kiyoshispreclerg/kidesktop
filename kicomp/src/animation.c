/* Monotonic time and easing curves -- see animation.h. */
#define _POSIX_C_SOURCE 200809L

#include "animation.h"
#include "comp.h"

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

float comp_lerp(float a, float b, float p)
{
    return a + (b - a) * p;
}
