#ifndef KIWM_SHAPE_H
#define KIWM_SHAPE_H

#include "wm.h"

/* Non-rectangular client windows (XCB SHAPE extension).
 *
 * A reparenting WM has to forward a client's own shape onto the frame it
 * put around it, because the frame is what the X server actually clips
 * against once the client is a child of it: an unshaped frame stays a
 * solid rectangle covering (and swallowing clicks over) everything the
 * client itself carved away. That's not an exotic case -- VirtualBox's
 * fullscreen mini-toolbar is a full-screen-sized window whose bounding
 * shape is just the little bar at the top, sitting over the VM window --
 * and it's the one thing that made that toolbar show up as a big opaque
 * rectangle across the VM screen under kiwm while working everywhere else.
 *
 * Separate from decoration.c's rounded corners, which shape the frame for
 * kiwm's *own* reasons; shape_update_frame() below is the single place
 * that decides which of the two applies. */

/* Extension presence + event base. Call once at startup, before any
 * client is managed. */
void shape_init(void);

/* Ask for ShapeNotify on a newly managed client, so a shape it sets (or
 * changes) after being mapped is picked up too, not just the one it
 * happened to have at manage() time. */
void shape_track_client(Client *c);

/* Whether this event is the SHAPE extension's ShapeNotify (its event
 * number is assigned at runtime, so it can't be a plain case label). */
bool shape_is_notify_event(uint8_t response_type);
void shape_handle_notify(xcb_generic_event_t *event);

/* Re-derives the frame's shape: the client's own shape offset by the
 * decoration insets (plus the decoration's own rectangles unioned back
 * in) when the client is shaped, or decoration.c's rounded corners when
 * it isn't. Called from client.c's configure_frame() and from every
 * ShapeNotify. */
void shape_update_frame(Client *c);

#endif /* KIWM_SHAPE_H */
