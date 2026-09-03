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
#include "animation.h"

typedef struct CompEffect CompEffect;
typedef struct CompEffectInstance CompEffectInstance;
typedef struct CompEffectModule CompEffectModule;

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

    /* The instance that started it, and therefore the settings it runs
     * with -- two instances of one module animate the same window with
     * different numbers precisely because each running effect remembers
     * which one it came from. */
    const CompEffectInstance *instance;

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

    /* This window opened or came back in the same batch of events. Mostly
     * of interest to whatever answers `focus`: a window that has just
     * appeared takes focus as a matter of course, and it had no previous
     * position on screen for anything to have been covering. */
    bool with_appear;
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

/* Window type names as they appear in kicomp.conf ("normal", "tooltip",
 * "popup-menu", ...) and the mask an effect is filtered by. */
const char *comp_window_type_name(CompWindowType type);
uint32_t comp_window_type_mask_parse(const char *list);

#define COMP_WINDOW_BIT(type) (1u << (type))
#define COMP_WINDOWS_ALL      0xffffffffu

/* One *instance* of an effect: a module plus the settings it runs with.
 *
 * Every module has a base instance, configured by [effect:<module>], and
 * may have any number of named ones, [effect:<module>:<instance>], each
 * of which starts as a copy of the base and overrides only what it says.
 * That is what makes "scale-out on close, and a slower, deeper scale-out
 * on minimize" two lines of config rather than a second effect.
 *
 * The three universal settings are here; whatever else the module
 * understands lives in `config`, a block the module describes
 * (config_size/config_defaults) and parses (config_key), one per
 * instance -- which is precisely why two instances can differ. */
struct CompEffectInstance {
    const CompEffectModule *module;
    CompEffectInstance *next;

    char name[64];        /* "scale-out" or "scale-out:minimize" */

    bool enabled;
    double duration;      /* multiple of the global animation unit */
    uint32_t events;      /* CompEventKind mask */
    uint32_t windows;     /* CompWindowType mask */
    CompEasing easing;    /* how the movement is weighted between its ends */

    void *config;         /* module-private, module->config_size bytes */
};

/* This instance's duration in milliseconds: its multiple of the user's
 * global animation_duration. What an effect asks when it starts. */
double effect_instance_duration(const CompEffectInstance *inst);

/* Linear progress through this instance's curve. Every effect that moves
 * something between two points goes through here rather than picking a
 * curve of its own, so easing= in the config actually means something --
 * and so that two instances of one effect can be weighted differently. */
float effect_ease(const CompEffect *e, float p);

/* The base instance of a module by name (NULL if there is no such
 * effect), and a named instance of it, created on first mention as a copy
 * of the base. Both are how config.c turns a section header into
 * something to write into. */
CompEffectInstance *effect_base_instance(const char *module);
CompEffectInstance *effect_named_instance(const char *module, const char *instance);

/* Hands one key to an instance's own module parser. False when the key is
 * one the module doesn't know. */
bool effect_instance_config_key(CompEffectInstance *inst, const char *key,
                                const char *value);

/* A module is the part that watches the desktop and decides to start an
 * effect. Every callback is optional. */
struct CompEffectModule {
    const char *name;

    /* Defaults, overridable per section. default_duration is a multiple
     * of the global unit; the two masks are COMP_EVENT_BIT(...) and
     * COMP_WINDOW_BIT(...) respectively. */
    bool default_enabled;
    double default_duration;
    uint32_t default_events;
    uint32_t default_windows;
    CompEasing default_easing;

    /* Called once, after the config has been read and this instance
     * exists. For a module that needs to arm a trigger of its own -- a
     * hotkey, today -- rather than waiting for the WM to do something.
     * NULL for every effect that only answers to events, which is all of
     * them but one. */
    void (*init)(const CompEffectInstance *self);

    /* Something happened to a window. The core calls this only for events
     * in this instance's event mask, on a window in its type mask, so a
     * module never checks either -- `self` is the instance that matched,
     * and its `config` holds the settings to run with. */
    void (*window_event)(CompWindow *w, const CompEvent *ev,
                         const CompEffectInstance *self);

    /* The module's own settings: how big the block is, how to fill it
     * with defaults, and how to parse one key into it. All three NULL for
     * an effect with no settings beyond the universal ones. config_key
     * returns false for a key it doesn't know, so a typo gets reported
     * instead of silently doing nothing. */
    size_t config_size;
    void (*config_defaults)(void *config);
    bool (*config_key)(void *config, const char *key, const char *value);
};

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
extern const CompEffectModule effect_shade;
extern const CompEffectModule effect_minimize;
extern const CompEffectModule effect_desktop_wall;
extern const CompEffectModule effect_smooth_move;
extern const CompEffectModule effect_dodge;
extern const CompEffectModule effect_show_windows;

#endif /* KICOMP_EFFECT_H */
