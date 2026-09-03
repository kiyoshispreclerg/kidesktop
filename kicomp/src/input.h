/*
 * kicomp - keyboard and pointer, for the effects that are *modes*.
 *
 * Almost nothing here needs input: a compositor watches, it doesn't
 * listen. Effects animate what the WM did, and the WM is the one holding
 * the keyboard. This file exists for the exception -- an effect like
 * show-windows, which puts the session into a state the user drives
 * directly (a grid to pick from, arrows to move around it, a filter to
 * type into) and hands control back when they choose something.
 *
 * Two things, deliberately kept apart:
 *
 *   - A *hotkey*: one key combination grabbed on the root window, which
 *     stays grabbed for as long as kicomp runs. This is the trigger. It
 *     belongs to whichever module asked for it, and it is grabbed by
 *     kicomp rather than routed from xiskeys because it is a stateful
 *     action -- the daemon that owns the state owns the key, which is
 *     the same rule xispanel and kiwm follow for their own.
 *
 *   - A *grab*: the whole keyboard and pointer, held only while a mode is
 *     running. Every event goes to the module holding it, and nothing
 *     reaches the applications underneath, which is what "all input goes
 *     to the filter" means.
 *
 * A grab that is never released would leave the session unusable, so
 * there is exactly one, and taking a second one is refused rather than
 * queued.
 */
#ifndef KICOMP_INPUT_H
#define KICOMP_INPUT_H

#include "comp.h"

typedef struct CompInputHandler {
    /* A key went down while this handler holds the grab. `keysym` is the
     * one the current layout gives, `text` the character it produces (an
     * empty string for a key that produces none). Return false to say
     * "not mine" -- nothing else will see it either, but the handler is
     * saying so explicitly rather than swallowing it by accident. */
    bool (*key)(void *data, xcb_keysym_t keysym, const char *text,
                uint16_t modifiers);

    /* The pointer moved to, or was pressed at, a root coordinate. */
    void (*motion)(void *data, int root_x, int root_y);
    void (*button)(void *data, int root_x, int root_y, uint8_t button);
} CompInputHandler;

/* Opens the keysym tables. Safe to call when there is nothing to bind. */
bool input_init(void);
void input_shutdown(void);

/* Binds a hotkey spec -- "Meta+W", "Ctrl+Alt+Tab", the same grammar
 * xiskeys and xispanel use -- to a callback. False for a spec that cannot
 * be parsed or a key the server won't give us (another client already
 * grabbed it), with the reason logged: a hotkey that silently does
 * nothing is worse than one that says why. */
bool input_bind_hotkey(const char *spec, void (*fn)(void *data), void *data);

/* Takes the keyboard and the pointer. False if someone already has them
 * (another mode of ours, or a client with an active grab -- a menu is
 * open, a drag is in progress), in which case the caller must not start.
 */
bool input_grab(const CompInputHandler *handler, void *data);
void input_release(void);
bool input_grabbed(void);

/* When the last grab was handed back, as a monotonic timestamp (0 if
 * there has never been one).
 *
 * For the effects that need to know that a window's focus was *chosen*
 * rather than taken: a window the user just picked out of show-windows'
 * grid has not barged in on anything, and an effect answering to focus by
 * shoving its neighbours aside (dodge) would be answering to the user's
 * own decision. */
double input_mode_ended_ms(void);

/* Called by the event loop before anything else looks at the event.
 * True when it was consumed -- a hotkey firing, or anything at all while
 * a grab is held. */
bool input_handle_event(xcb_generic_event_t *ev);

/* The pointer's position right now, for a mode that needs to know where
 * it is starting from. False if the server won't say. */
bool input_pointer_position(int *root_x, int *root_y);

#endif /* KICOMP_INPUT_H */
