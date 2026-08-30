/*
 * kicomp - the visual mirror of the window stack.
 *
 * The WM is the authority (section 33); this module only tracks what X
 * reports about root's children, in stacking order, and never talks back
 * to the WM.
 */
#ifndef KICOMP_WINDOW_H
#define KICOMP_WINDOW_H

#include "comp.h"

CompWindow *window_find(xcb_window_t id);

/* Adds a top-level we didn't know about. `above` is the sibling it sits
 * directly on top of (XCB_NONE = bottom of the stack). */
void window_add(xcb_window_t id, xcb_window_t above);
void window_remove(xcb_window_t id);

void window_map(xcb_window_t id);
void window_unmap(xcb_window_t id);
void window_configure(xcb_window_t id, int x, int y, int w, int h, int border,
                      xcb_window_t above);
void window_restack(xcb_window_t id, xcb_window_t above);
void window_update_opacity(CompWindow *w);

/* The window's on-screen rectangle including its X border, which is what
 * NameWindowPixmap covers. */
CompRect window_rect(const CompWindow *w);

/* Initial adoption of everything already mapped when kicomp starts, so
 * running it mid-session composites the existing desktop instead of a
 * black screen (mirrors kiwm's manage_existing_windows). */
void windows_scan(void);
void windows_teardown(void);

#endif /* KICOMP_WINDOW_H */
