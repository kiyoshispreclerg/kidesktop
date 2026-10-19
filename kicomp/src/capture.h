/*
 * kicomp - the capture protocol's control plane (CAPTURE-PROTOCOL.md).
 *
 * What a screen recorder needs to know before it can trust a picture of an
 * output: whether this compositor offers censored capture at all, and, per
 * output and per frame, whether any excluded window is contributing pixels.
 * Everything here is X properties and ClientMessages -- no rendering. The
 * second final assembly (REQUEST/FRAME) is the renderers' part and not
 * built yet, so a session is accepted but every output is declared
 * UNAVAILABLE while an excluded window shows: the recorder censors that
 * output itself, which is what it does without a compositor.
 *
 * Costs nothing with no session open: the only standing work is reading
 * the mark when a window is adopted and when it changes.
 */
#ifndef KICOMP_CAPTURE_H
#define KICOMP_CAPTURE_H

#include "comp.h"

/* kicomp.conf: capture_censor, capture_sessions. */
bool capture_config_key(const char *key, const char *value);

/* Interns the atoms. After config_load(), before the first window. */
void capture_init(void);

/* Publishes _KICOMP_CAPTURE on the CM window (absent when capture_censor
 * is off -- the absence is the answer). After the CM window exists. */
void capture_publish(void);

/* Ends every session (STATUS 0) and withdraws the offer. */
void capture_shutdown(void);

/* _KIWM_CAPTURE_EXCLUDE on a client window: asked as part of reading the
 * window's identity (window.c), answered true for any non-zero value. */
xcb_get_property_cookie_t capture_mark_request(xcb_window_t client);
bool capture_mark_reply(xcb_get_property_cookie_t cookie);

/* Event hooks (main.c). Each returns true when it consumed the event. */
bool capture_property_notify(xcb_window_t window, xcb_atom_t atom);
bool capture_client_message(const xcb_client_message_event_t *e);
void capture_window_destroyed(xcb_window_t window);
void capture_outputs_changed(void);

/* Once per loop iteration, after the effects have run and before the
 * paint: recomputes which windows are excluded and, per session and
 * output, how many contribute pixels -- writing _KICOMP_CAPTURE_STATE
 * *before* the first frame that shows an excluded window is painted
 * (the one-sided ordering the recorder's seqlock depends on). */
void capture_update(void);

/* After an output was painted and presented: makes the censored pictures
 * recorders asked for (REQUEST) -- a second assembly of the scene with the
 * excluded windows substituted, through the renderer -- and answers them. */
void capture_output_painted(CompOutput *o);

/* An output a session watches must not be handed to a fullscreen window
 * while an excluded window shows on it: there would be no scene to
 * assemble the censored picture from. */
bool capture_blocks_unredirect(const CompOutput *o);

#endif /* KICOMP_CAPTURE_H */
