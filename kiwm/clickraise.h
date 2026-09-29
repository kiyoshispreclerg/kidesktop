#pragma once

/* kiwm.conf's focus_raise_on_release=: a click on the content of a window
 * that isn't focused goes through to the application without raising or
 * focusing anything; the window comes up when the button is released --
 * unless that press turned into a drag-and-drop, which then leaves both
 * the stacking and the focus exactly as they were, so the drag can be
 * dropped on whatever is in front. See clickraise.c. */

#include "wm.h"

#include <stdbool.h>
#include <xcb/xcb.h>

/* Called once at startup, after config_load(). No-op with the option off
 * or without XInput 2 / XFixes on the server. */
void clickraise_init(void);

/* Whether a plain content click on c should wait for its release --
 * handle_button_press() then replays it without calling focus_client()
 * and hands c to clickraise_defer(). */
bool clickraise_wants(Client *c);
void clickraise_defer(Client *c);

/* Whether an application's own request to activate c (_NET_ACTIVE_WINDOW
 * from the app, not a pager) is just the echo of a deferred click still
 * in progress -- Qt sends one on every click into an inactive window.
 * Such a request is ignored; the release decides. */
bool clickraise_absorbs_activation(Client *c);

/* c is going away (unmanage()). */
void clickraise_forget(Client *c);

/* XInput raw button release / XFixes selection owner change; true if the
 * event was one of ours. */
bool clickraise_handle_event(xcb_generic_event_t *event);
