/*
 * kicomp - effects (sections 23, 25, 42).
 *
 * The core knows two things and no more: a running effect
 * (CompEffect/CompEffectOps) and a module that decides when to start one
 * (CompEffectModule). It never knows what an effect *is* -- fade, zoom,
 * geometry, wobbly are all the same shape from here.
 *
 * The rule from section 42 (a new effect = one .c file, no core changes)
 * holds with one line of bookkeeping: the module's descriptor goes into
 * the table in effect.c. Nothing else in the compositor changes.
 *
 * Two things an effect must respect:
 *
 *   - Time, not frames (section 20). `update` is handed a monotonic
 *     timestamp; progress comes from that alone.
 *   - The output (section 25). `apply` runs once per output being
 *     painted, with that output's scene, so the same effect can be
 *     mid-flight on one monitor and finished on another.
 *
 * And one thing it must never do: touch the WM's logical state (section
 * 27). An effect changes CompSceneNode fields -- transform, opacity --
 * and nothing else. The window really is where the WM says it is; it
 * merely looks like it isn't yet.
 */
#ifndef KICOMP_EFFECT_H
#define KICOMP_EFFECT_H

#include "comp.h"
#include "scene.h"

typedef struct CompEffect CompEffect;

typedef struct CompEffectOps {
    const char *name;

    /* Advance to `now` (ms, monotonic). Also where an effect marks the
     * outputs it covers dirty -- it is the only one that knows what area
     * it is about to change. */
    void (*update)(CompEffect *e, double now);

    /* Modify `s` (already built for `o`) in place. */
    void (*apply)(CompEffect *e, CompScene *s, CompOutput *o);

    bool (*finished)(const CompEffect *e, double now);

    /* Free whatever `data` holds. The CompEffect itself is freed by the
     * core. */
    void (*destroy)(CompEffect *e);
} CompEffectOps;

struct CompEffect {
    const CompEffectOps *ops;
    CompEffect *next;

    double start_time;   /* ms, monotonic */
    double duration;     /* ms */

    /* The window this effect animates, or NULL for one that isn't bound
     * to a single window. The core uses it to drop the effect when the
     * window goes away -- an effect must never outlive its subject. */
    CompWindow *window;

    void *data;
};

/* What kicomp.conf's [effect:<name>] section can say about one effect.
 * `duration` is a multiple of the global animation unit, never a time:
 * "this one should feel twice as long as the desktop's normal
 * animation", not "320 ms". */
typedef struct CompEffectConfig {
    bool enabled;
    double duration;
} CompEffectConfig;

/* Settings for one module by name, or NULL if no such effect exists (how
 * config.c reports a typo in a section header). */
CompEffectConfig *effect_config(const char *name);

/* What an effect asks at the moment it starts. effect_duration() is
 * already in milliseconds: the module's configured multiple of the
 * user's global animation_duration. */
bool effect_is_enabled(const char *name);
double effect_duration(const char *name);

/* A module is the part that watches the desktop and decides to start an
 * effect. Every callback is optional. */
typedef struct CompEffectModule {
    const char *name;

    /* Defaults for this effect, overridable per [effect:<name>] section.
     * default_duration is a multiple of the global unit. */
    bool default_enabled;
    double default_duration;

    /* A window's geometry changed. `from`/`to` are frame rects in root
     * coordinates, `interactive` is true when this configure is part of a
     * stream of them (a drag), which is exactly when an animation must
     * stay out of the way. */
    void (*window_configured)(CompWindow *w, const CompRect *from,
                              const CompRect *to, bool interactive);

    void (*window_mapped)(CompWindow *w);
    void (*window_unmapped)(CompWindow *w);
} CompEffectModule;

/* Effects are off until effects_init() runs (kicomp --effects). */
void effects_init(void);
bool effects_enabled(void);

/* Started by a module; the core takes ownership. */
void effects_add(CompEffect *e);

/* Whether any effect is running -- what makes the main loop switch from
 * "sleep until something happens" to "keep frames coming". */
bool effects_active(void);

/* Advance every running effect and retire the finished ones. */
void effects_update(double now);

/* Apply every running effect to one output's freshly built scene. */
void effects_apply(CompScene *s, CompOutput *o);

/* Notifications from the core (window.c). No-ops when effects are off. */
void effects_window_configured(CompWindow *w, const CompRect *from,
                               const CompRect *to, bool interactive);
void effects_window_mapped(CompWindow *w);
void effects_window_unmapped(CompWindow *w);

/* The window is going away: drop anything animating it, right now. */
void effects_window_gone(CompWindow *w);

void effects_shutdown(void);

/* ---- effects/ modules ---- */
extern const CompEffectModule effect_geometry;

#endif /* KICOMP_EFFECT_H */
