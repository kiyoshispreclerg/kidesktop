#ifndef KIWM_CAPTURE_H
#define KIWM_CAPTURE_H

#include "wm.h"

/* The mark that keeps a window out of screen captures: _KIWM_CAPTURE_EXCLUDE
 * on the client window (PROTOCOL.md). 1 = the window itself was marked (its
 * menu entry, capture_exclude_classes=, a tool); 2 = inherited by a dialog of
 * a window marked that way. Any non-zero value means "excluded"; the
 * difference only lets un-marking a window clear what it handed down and
 * nothing else. */

/* The mark's value on `window`: 0 when absent. */
int capture_mark_get(xcb_window_t window);

/* The window menu's check-mark entry: marks `c` (1) and its dialogs (2), or
 * un-marks it and whatever only it was handing down. */
void capture_toggle(Client *c);

/* A window being managed, before it is mapped: marked 1 when its WM_CLASS
 * (instance or class) is in capture_exclude_classes=, else 2 when it is a
 * dialog of a marked window -- so the mark is there before its first frame. */
void capture_mark_new_client(Client *c);

/* WM_TRANSIENT_FOR changed (or was set after the map): the same question
 * again, both ways -- a dialog that gains a marked parent inherits, one that
 * loses it stops. */
void capture_refresh_inherited(Client *c);

#endif /* KIWM_CAPTURE_H */
