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

    /* Where this window's own damage lands, while this effect is drawing
     * it somewhere other than where it is.
     *
     * A window redraws itself in its own coordinates -- a video frame, a
     * blinking cursor -- and the compositor repaints that part of the
     * screen. Under an effect that has moved and shrunk it, that part of
     * the screen is not where the window is being *shown*: the thumbnail
     * in an expo cell keeps the frame it was opened with while the real
     * window plays on somewhere nobody is looking. So the effect, which
     * is the only thing that knows where it put the window, maps the
     * rectangle.
     *
     * `in` and `out` are root coordinates. False for a window this
     * effect isn't moving, which is the answer for almost every window
     * of almost every frame. */
    bool (*damage_map)(const CompEffect *e, const CompWindow *w,
                       const CompRect *in, CompRect *out);

    /* The output this effect has taken over, or COMP_NO_OUTPUT.
     *
     * What "taken over" means: the effect is showing the user the whole
     * desktop rather than one window on it -- a cube, an expo grid, a
     * row of covers, a wall sliding one desktop out and another in.
     * Two of those at once is not a picture of anything, so a mode asks
     * effects_mode_running() before it opens and simply does not, and
     * the answer to a hotkey is that nothing happens.
     *
     * The desktop wall is one such thing made of many effects -- one per
     * window -- which is why the question is asked about *other kinds*
     * of effect rather than about any effect at all. */
    int (*owns_output)(const CompEffect *e);

    /* And this one draws it alone: it replaces the scene's node list
     * rather than adjusting it, so nothing else may apply to that
     * output. Effects are applied newest first, so an older one still
     * running would write its transforms over a list whose indices it no
     * longer understands, and the picture comes apart -- which is what
     * starting the cube during a wall used to do.
     *
     * Not every mode needs this. The wall moves windows about and is
     * perfectly happy for a window closing in the middle of it to fade
     * out as it goes. */
    bool rebuilds_scene;

    /* This window is being forgotten: drop every reference to it now.
     *
     * For the effects that are *modes* -- a grid, a row of covers --
     * which hold a list of windows rather than animating one. The core
     * already ends an effect whose own `window` has gone, and that is
     * the whole story for an animation; a mode outlives any one window
     * in it and would be left holding a pointer to freed memory.
     *
     * Called for every running effect, before the window is freed. */
    void (*window_gone)(CompEffect *e, CompWindow *w);

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
    COMP_EVENT_BELL,             /* the window rang the terminal bell */
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

/* An effect that has already shown the user what a change means claims
 * it, and whatever would otherwise animate that change stands down.
 *
 * What this is for: leaving the cube, the expo grid or the cover
 * switcher changes the desktop, and the desktop wall would then slide
 * that desktop in -- a second animation, of a change the user has just
 * watched happen, played over the top of the first. The windows arrived
 * with the mode that was showing them; they must not also come sliding
 * in from the side.
 *
 * Claimed for a stretch of time rather than for an instant, because the
 * event does not exist yet when the claim is made: the compositor asks
 * the window manager, the WM acts, and the unmaps and property changes
 * that become a desktop-leave come back a round trip later. The caller
 * says how long, because only it knows how long its own animation runs
 * and how far behind the events will trail.
 *
 * Per output, since a desktop is per output here -- a cube turning on
 * one monitor has nothing to say about a desktop changing on the other.
 * COMP_NO_OUTPUT claims the change everywhere.
 *
 * This is the general form of something the effects were already doing
 * by hand: dodge asks input_mode_ended_ms() so it does not shove windows
 * aside for a focus the user chose out of a grid. */
/* Is some *other* kind of desktop-scale effect already running on this
 * output (effect.h's owns_output)? `self` is the caller's own ops, so
 * that the desktop wall -- which is one animation made of one effect per
 * window -- does not refuse itself. */
bool effects_mode_running(int output_id, const CompEffectOps *self);

/* How long a claim needs to last.
 *
 * Long enough for the change to come back: the compositor asks the
 * window manager, the WM acts, and the unmaps and property changes that
 * become a desktop-leave arrive a round trip later -- tens of
 * milliseconds. Not scaled by the claiming effect's own duration, which
 * was the mistake this replaces: an animation's length says nothing
 * about when the events arrive, and with a slow animation setting it
 * produced a claim lasting over two seconds, which swallowed the next
 * desktop switch the user made by hand. */
#define COMP_CLAIM_MS 500.0

void effects_claim(CompEventKind kind, int output_id, double ms);

/* The same, for one window rather than a whole output.
 *
 * Because sometimes the change *should* be animated and one window's
 * part of it should not: choosing a window on another desktop from the
 * cover switcher is a desktop change, and the wall sliding that desktop
 * in is exactly right -- except for the window that was chosen, which
 * the user has just watched swing round to the front and which must
 * arrive where it already is rather than come in from the side with the
 * rest. */
void effects_claim_window(CompEventKind kind, const CompWindow *w, double ms);

/* "I am drawing this output's *other* desktops": what lets the windows
 * kept only for their contents into the scene (comp.show_stowed_output).
 *
 * Counted, because more than one mode wants it and they overlap: an
 * Alt+Tab row is still fading out when the cube opens, and the row's
 * teardown clearing the flag left the cube's faces empty. Every take
 * needs its release, and the flag only really drops on the last one. */
void effects_show_stowed(int output_id, bool on);

/* The window is going away: drop anything animating it, right now. */
/* Asks every running effect where this window's damage belongs (see
 * damage_map above), and damages that too. The damage where the window
 * really is stays: the effects on the *other* monitors may well still be
 * drawing it there, and repainting a rectangle nothing changed in is
 * cheap next to working out which. */
void effects_damage_window(const CompWindow *w, const CompRect *r);

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
extern const CompEffectModule effect_cover_switch;
extern const CompEffectModule effect_cube;

/* cover-switch driven by the window manager rather than by a hotkey of
 * ours -- see the protocol described in effects/cover-switch.c. `data`
 * is the _KICOMP_SWITCHER property's words; `len` how many there are.
 * cover_switch_available() is what _KICOMP_EFFECTS answers with. */
void cover_switch_external(const uint32_t *data, int len);
bool cover_switch_available(void);
extern const CompEffectModule effect_expo;
extern const CompEffectModule effect_visual_bell;
extern const CompEffectModule effect_zoom;
extern const CompEffectModule effect_stats;

#endif /* KICOMP_EFFECT_H */
