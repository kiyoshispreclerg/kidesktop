/*
 * kicomp - drop shadows.
 *
 * Not an effect: nothing about a shadow animates, it is simply part of
 * how a window is drawn. So it lives beside the renderer rather than in
 * effects/, and the renderer draws it under each window it applies to.
 *
 * Cost, since it is the usual objection to shadows on XRender: low, and
 * independent of window size. A Gaussian blur of a rectangle is
 * separable, and the blur of a rectangle's edge is the same profile
 * everywhere along it -- so a shadow is four small corner tiles, four
 * one-pixel edge strips repeated along the sides, and a solid middle.
 * The tiles depend only on the radius, so they are built once and reused
 * by every window; per frame a shadow costs nine composites of a solid
 * colour through a mask. Nothing is recomputed when a window moves or
 * resizes, which is exactly what makes this cheap enough to leave on --
 * a full-size blurred bitmap per window, rebuilt on every resize step of
 * a drag, is the version of this that is too slow, and this isn't it.
 *
 * kicomp.conf:
 *
 *   [shadow]
 *   enabled  = 1
 *   windows  = windows,menus     # which window types get one
 *   radius   = 12                # blur radius in pixels
 *   opacity  = 0.45
 *   offset_x = 0
 *   offset_y = 6
 *   color    = #000000
 *
 *   # the same five for windows that don't have focus; each one that is
 *   # left out keeps the focused value
 *   radius_inactive  = 10
 *   opacity_inactive = 0.28
 *   offset_y_inactive = 4
 */
#ifndef KICOMP_SHADOW_H
#define KICOMP_SHADOW_H

#include "comp.h"

/* One shadow's appearance, already resolved for a particular window. */
typedef struct CompShadowStyle {
    int radius;
    float opacity;
    int offset_x, offset_y;
    float r, g, b;
} CompShadowStyle;

typedef struct CompShadowConfig {
    bool enabled;
    uint32_t windows;              /* CompWindowType mask */
    CompShadowStyle active;
    CompShadowStyle inactive;

    /* Which of the inactive values the config actually named; the rest
     * follow the active ones, so a config that only wants a dimmer
     * shadow when unfocused says only opacity_inactive. */
    bool has_inactive_radius;
    bool has_inactive_opacity;
    bool has_inactive_offset_x;
    bool has_inactive_offset_y;
    bool has_inactive_color;
} CompShadowConfig;

extern CompShadowConfig comp_shadow;

void shadow_config_defaults(void);
/* One key from the [shadow] section. False for a key that isn't one. */
bool shadow_config_key(const char *key, const char *value);
/* Fills in the inactive values that weren't named. Called once, after the
 * config file has been read. */
void shadow_config_finish(void);

/* Whether this window gets a shadow at all, and what it looks like right
 * now (focused windows and unfocused ones may differ in every value). */
bool shadow_for_window(const CompWindow *w, CompShadowStyle *out);

#endif /* KICOMP_SHADOW_H */
