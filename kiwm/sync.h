/* _NET_WM_SYNC_REQUEST: resizing in step with the client.
 *
 * Without it a resize is two things that happen to overlap: the WM sets
 * the new size, the server exposes what that uncovered, and the client
 * repaints when it gets to it. For a few milliseconds every step the
 * frame is one size and the content another, and a client that takes
 * longer than a frame to paint falls steps behind while the frame keeps
 * going -- which is what a resize that is "not smooth" is.
 *
 * With it (EWMH 1.3+), a client that advertises the protocol and names an
 * XSync counter is told, before each WM-initiated size change, a value
 * it should set that counter to once it has finished drawing at the new
 * size. kiwm then holds the *next* step until that value is reached (or
 * a timeout passes, so a stalled client never freezes the drag). The
 * client is never asked for a size faster than it can draw one, its
 * content and its frame move together, and no repaint is queued only to
 * be overwritten -- which is why kwin's resize looks smooth while
 * costing the client less.
 *
 * All of it is optional twice over: a server without the SYNC extension,
 * or a client that does not opt in, gets exactly the old behaviour. */
#ifndef KIWM_SYNC_H
#define KIWM_SYNC_H

#include "wm.h"

void sync_init(void);

/* Reads the client's counter and protocol support; creates the alarm
 * that will report its acknowledgements. Called at manage() and again
 * when _NET_WM_SYNC_REQUEST_COUNTER changes. */
void sync_track_client(Client *c);
void sync_untrack_client(Client *c);

/* Whether a size change may be sent now: no acknowledgement is owed,
 * or the one owed is older than the timeout. Always true for a client
 * that does not do sync. */
bool sync_client_ready(Client *c, double now);

/* Tell the client a size change is coming and what to say when it has
 * drawn it. Called by apply_frame_geometry_told() before the client's
 * ConfigureWindow; a no-op for a client that does not do sync. */
void sync_request(Client *c);

/* The SYNC extension's AlarmNotify, dispatched from handle_event(). */
bool sync_is_alarm_event(uint8_t response_type);
void sync_handle_alarm(xcb_generic_event_t *event);

#endif /* KIWM_SYNC_H */
