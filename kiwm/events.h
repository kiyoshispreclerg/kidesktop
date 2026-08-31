#ifndef KIWM_EVENTS_H
#define KIWM_EVENTS_H

#include <xcb/xcb.h>

void handle_event(xcb_generic_event_t *event);

/* Ends a move/resize drag (and clears an armed titlebar button) whose
 * ButtonRelease never arrived, once no mouse button is held any more --
 * see events.c. main.c's event loop calls this on a timer while either is
 * in flight; a cheap no-op otherwise. */
void events_poll_stale_drag(void);

#endif /* KIWM_EVENTS_H */
