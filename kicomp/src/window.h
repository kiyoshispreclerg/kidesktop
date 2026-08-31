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

/* Adds a top-level we didn't know about, with ConfigureNotify's
 * above_sibling semantics: `above` is the sibling it sits directly on top
 * of, and XCB_NONE means the *bottom* of the stack. */
void window_add(xcb_window_t id, xcb_window_t above);

/* Adds one at the top of the stack, which is where X itself puts a window
 * it has just created or just reparented -- and CreateNotify carries no
 * sibling to go by at all. Adding those at the bottom instead is
 * invisible for anything the WM restacks a moment later (every managed
 * window), and fatal for anything it doesn't: an override-redirect menu
 * or tooltip ends up under every other window, including the desktop,
 * i.e. it simply never appears. */
void window_add_top(xcb_window_t id);
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
