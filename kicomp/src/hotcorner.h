/*
 * kicomp - hot corners: the pointer touching a corner of the screen
 * starts an effect.
 *
 * There is no polling and no pointer-motion stream. Each corner is a tiny
 * InputOnly window, and the server itself says when the pointer enters or
 * leaves it (EnterNotify/LeaveNotify) -- so while the pointer is anywhere
 * else the compositor sleeps exactly as it did before. That is the point
 * of doing it this way rather than watching raw motion: the same
 * machinery that would make this work (a stream of motion events) is a
 * cost paid on every mouse movement all day for something used a few times.
 *
 * The windows live at the very top of the stack and are put back there
 * when something is mapped or raised above them.
 *
 * A corner names an effect module (kicomp.conf's [hotcorners]); the module
 * says what "activate" means for it (effect.h's `activate`). A corner
 * fires once per visit: it must be left before it can fire again.
 */
#ifndef KICOMP_HOTCORNER_H
#define KICOMP_HOTCORNER_H

#include "comp.h"

#define HOTCORNER_CORNERS 4    /* top-left, top-right, bottom-left, bottom-right */
#define HOTCORNER_RULES   8    /* [hotcorners:NAME] sections */
#define HOTCORNER_MAX     (HOTCORNER_CORNERS * 16)

/* One set of corner actions; empty name = corner unused. `set` tells a
 * per-output rule which corners it actually mentions, so that an output
 * naming only one corner still inherits the rest from [hotcorners]. */
typedef struct {
    char name[32];                              /* output; unused for base */
    char action[HOTCORNER_CORNERS][48];
    bool set[HOTCORNER_CORNERS];
} HotcornerRule;

typedef struct {
    HotcornerRule base;
    HotcornerRule rules[HOTCORNER_RULES];
    int n_rules;
    int delay_ms;    /* dwell before firing; 0 = on touch */
    int size;        /* side of the corner window, px */
} CompHotcornerConfig;

extern CompHotcornerConfig comp_hotcorners;

void hotcorner_config_defaults(void);
bool hotcorner_config_key(const char *output, const char *key, const char *value);

/* Creates the windows for the corners that have an action, on every output (after
 * effects_init, so the actions can be resolved). Nothing at all is
 * created when no corner is configured. */
void hotcorner_init(void);
void hotcorner_shutdown(void);

/* The outputs changed: put the corners back in the corners. */
void hotcorner_relayout(void);

/* The event mask a corner window must keep: window_add selects its own
 * mask on every root child, which (masks being per-client) would replace
 * ours. 0 for any window that is not a corner. */
uint32_t hotcorner_event_mask(xcb_window_t id);

/* Events. handle_event is true when the event was ours. note_restack
 * is told about every root-child map/configure/circulate. */
bool hotcorner_handle_event(xcb_generic_event_t *ev);
void hotcorner_note_restack(xcb_window_t id);

/* Once per loop iteration: re-raise if something covered us, fire a
 * corner whose dwell has run out. */
void hotcorner_tick(double now);

/* Milliseconds until a pending dwell fires, or -1 for none. */
int hotcorner_timeout_ms(double now);

#endif /* KICOMP_HOTCORNER_H */
