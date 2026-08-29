#ifndef KIWM_MENU_H
#define KIWM_MENU_H

#include "wm.h"

/* The window context menu: right-click a titlebar (or left-click its icon)
 * and this is what opens -- minimize, maximize/restore, shade, move to
 * another desktop, all desktops, keep above, close.
 *
 * Everything the menu offers lives in one table at the top of menu.c
 * (`entries[]`), so a new action is a row there plus its case in
 * run_action() -- see menu.c's file comment. */

/* Opens the menu for `c` with its top-left corner at (root_x, root_y),
 * kept inside the output it lands on. Closes whatever menu was already
 * open first; a no-op if `c` is NULL. */
void window_menu_open(Client *c, int root_x, int root_y);

/* Closes the menu without picking anything. No-op when none is open. */
void window_menu_close(void);

/* Whether a menu is currently open (holding the pointer and keyboard) --
 * events.c routes key presses here instead of to the shortcut table while
 * it is. */
bool window_menu_active(void);

/* Whether `window` is one of the menu's own popup windows, currently up.
 * Like the switcher overlay (osd.c), these are override-redirect windows
 * and never Clients, so this is how client.c's restack_all() recognizes
 * them and keeps them in the topmost layer. */
bool window_menu_owns_window(xcb_window_t window);

/* Repaints a menu popup after an Expose on it -- a no-op for any other
 * window, or when no menu is open. */
void window_menu_handle_expose(xcb_window_t window);

/* The three input paths, all fed from events.c's dispatch. Each returns
 * whether it consumed the event, which is false (and does nothing) when no
 * menu is open -- the normal case. */
bool window_menu_handle_button_press(xcb_button_press_event_t *ev);
bool window_menu_handle_motion(xcb_motion_notify_event_t *ev);
bool window_menu_handle_key_press(xcb_key_press_event_t *ev);

/* The client the open menu belongs to has gone away (client.c's
 * unmanage()) -- closes the menu rather than leaving it pointed at freed
 * memory. A no-op for any other client. */
void window_menu_client_destroyed(Client *c);

#endif /* KIWM_MENU_H */
