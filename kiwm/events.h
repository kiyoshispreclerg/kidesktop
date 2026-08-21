#ifndef KIWM_EVENTS_H
#define KIWM_EVENTS_H

#include <xcb/xcb.h>

void handle_event(xcb_generic_event_t *event);

#endif /* KIWM_EVENTS_H */
