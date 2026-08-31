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

/* What happened to a window, in the desktop's vocabulary rather than in
 * X's. This is what effects are configured against: an effect says which
 * of these it answers to (kicomp.conf's events=), and the core only calls
 * it for those -- so "roll up on shade, but don't slide on it" is a
 * config line, not a special case in an effect.
 *
 * Keep in sync with event_names[] in effect.c. */
typedef enum {
    COMP_EVENT_OPEN = 0,
    COMP_EVENT_CLOSE,
    COMP_EVENT_MINIMIZE,
    COMP_EVENT_RESTORE,          /* un-minimize */
    COMP_EVENT_MAXIMIZE,
    COMP_EVENT_UNMAXIMIZE,
    COMP_EVENT_SHADE,
    COMP_EVENT_UNSHADE,
    COMP_EVENT_FULLSCREEN,
    COMP_EVENT_UNFULLSCREEN,
    COMP_EVENT_FOCUS,
    COMP_EVENT_UNFOCUS,
    COMP_EVENT_MOVE,             /* a geometry change that is none of the above */
    COMP_EVENT_DESKTOP_LEAVE,    /* hidden because its desktop was left */
    COMP_EVENT_DESKTOP_ENTER,
    COMP_EVENT_COUNT
} CompEventKind;

typedef struct CompEvent {
    CompEventKind kind;

    /* Geometry events only: frame rects in root coordinates, and whether
     * this is one step of a drag rather than a single jump. */
    CompRect from;
    CompRect to;
    bool interactive;
} CompEvent;

/* Event names as they appear in kicomp.conf ("open", "unmaximize",
 * "desktop-leave", ...). */
const char *comp_event_name(CompEventKind kind);

/* Parses a comma-separated list of those names into a mask. "all" and
 * "none" are accepted as the whole list. Unknown names are reported and
 * skipped, so one typo doesn't quietly disable an effect. */
uint32_t comp_event_mask_parse(const char *list);

#define COMP_EVENT_BIT(kind) (1u << (kind))
#define COMP_EVENTS_ALL      0xffffffffu

/* What kicomp.conf's [effect:<name>] section can say about one effect.
 * `duration` is a multiple of the global animation unit, never a time:
 * "this one should feel twice as long as the desktop's normal
 * animation", not "320 ms". */
typedef struct CompEffectConfig {
    bool enabled;
    double duration;
    uint32_t events;    /* which CompEventKinds this effect answers to */
} CompEffectConfig;

/* Settings for one module by name, or NULL if no such effect exists (how
 * config.c reports a typo in a section header). */
CompEffectConfig *effect_config(const char *name);

/* What an effect asks at the moment it starts. effect_duration() is
 * already in milliseconds: the module's configured multiple of the
 * user's global animation_duration. */
bool effect_is_enabled(const char *name);
double effect_duration(const char *name);

/* Hands one [effect:<name>] key to that module's own parser. False when
 * the effect or the key is unknown. */
bool effect_config_key(const char *name, const char *key, const char *value);

/* A module is the part that watches the desktop and decides to start an
 * effect. Every callback is optional. */
typedef struct CompEffectModule {
    const char *name;

    /* Defaults for this effect, overridable per [effect:<name>] section.
     * default_duration is a multiple of the global unit, default_events a
     * mask of COMP_EVENT_BIT(...). */
    bool default_enabled;
    double default_duration;
    uint32_t default_events;

    /* Something happened to a window. The core only calls this for events
     * in the effect's configured mask, so a module never has to check
     * which event it got unless it treats several of them differently. */
    void (*window_event)(CompWindow *w, const CompEvent *ev);

    /* Keys from this effect's [effect:<name>] section beyond the two
     * universal ones (enabled, duration). Returns false for a key the
     * module doesn't know, so a typo in the config gets reported instead
     * of silently doing nothing. Optional: an effect with no settings of
     * its own leaves it NULL. */
    bool (*config_key)(const char *key, const char *value);
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

/* The single notification from the core (window.c). Dispatched only to
 * the effects whose configured mask contains this event; a no-op when
 * effects are off. */
void effects_window_event(CompWindow *w, const CompEvent *ev);

/* The window is going away: drop anything animating it, right now. */
void effects_window_gone(CompWindow *w);

void effects_shutdown(void);

/* ---- effects/ modules ---- */
extern const CompEffectModule effect_geometry;
extern const CompEffectModule effect_fade_in;
extern const CompEffectModule effect_fade_out;
extern const CompEffectModule effect_scale_in;
extern const CompEffectModule effect_scale_out;

#endif /* KICOMP_EFFECT_H */
