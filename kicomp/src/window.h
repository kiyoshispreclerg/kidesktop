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

/* Re-reads _NET_WM_WINDOW_TYPE (comp.h's CompWindowKind). Windows are
 * classified when adopted and again when mapped, since a frame is often
 * created before the client is reparented into it. */
void window_refresh_kind(CompWindow *w);

/* Keeps a window in the scene past its own disappearance, for an effect
 * that is still drawing it (fade out, scale out, and later minimize and
 * the desktop wall). Every retain must be matched by a release -- an
 * effect does that in its destroy op, so a cancelled effect frees the
 * window just as a finished one does. Releasing the last reference on a
 * window X has already destroyed is what finally drops it from the
 * mirror. */
void window_retain(CompWindow *w);
void window_release(CompWindow *w);

/* The window's on-screen rectangle including its X border, which is what
 * NameWindowPixmap covers. */
CompRect window_rect(const CompWindow *w);

/* Do these two windows belong to the same application? Answered from
 * WM_TRANSIENT_FOR, WM_CLIENT_LEADER and _NET_WM_PID, in that order of
 * confidence. What it is for: an effect that moves windows out of each
 * other's way has no business making an application's own windows dodge
 * one another -- VirtualBox's machine window and its detached mini-toolbar
 * are one thing on screen, whatever X thinks. */
bool windows_same_group(const CompWindow *a, const CompWindow *b);

/* Was `a` drawn on top of `b` *before* the batch of events being
 * classified? The current stacking can't answer this: a raise and the
 * focus that comes with it arrive together, so by classification time the
 * raise has happened and everything is already below the newly focused
 * window. This compares the snapshot taken at the end of the previous
 * batch (comp.h's z_before) -- what was covering what a moment ago. */
bool window_was_above(const CompWindow *a, const CompWindow *b);

/* Whether any window is waiting to be classified. The main loop uses it
 * to decide whether a round trip to the server is worth making before the
 * flush -- see windows_flush_events(). */
bool windows_have_pending(void);

/* Turns the batch of X events just drained into the desktop's own
 * vocabulary -- opened, closed, minimized, maximized, shaded... -- and
 * hands those to the effects. Called once per iteration of the main loop,
 * after the event queue is empty and before anything is painted: X's
 * order is not the desktop's, and one beat's delay is what makes "this
 * unmap was a minimize" -- or "this resize was a shade" -- knowable at
 * all. */
void windows_flush_events(void);

/* A property that carries window state changed (_NET_WM_STATE, WM_STATE
 * on the client window). */
void window_state_changed(CompWindow *w);

/* The WM marked (or unmarked) this window as being held on screen for
 * its picture only -- kiwm/PROTOCOL.md's _KIWM_HELD, comp.h's held. Set
 * before the map it explains and cleared after the unmap, so the window
 * carries it for the whole of both. */
void window_held_changed(CompWindow *w, bool held);

/* _NET_ACTIVE_WINDOW changed on the root: emits focus/unfocus. */
void window_focus_changed(xcb_window_t active);

/* A window rang the bell (main.c, XKB). Reported straight through rather
 * than deferred like the rest: nothing about the window changed, so there
 * is nothing for a flush to resolve, and a bell answered a frame late is
 * a bell answered late. */
void window_bell(xcb_window_t which);

/* The client inside `frame` was reconfigured: what that window covers has
 * to be worked out again (comp.h's opaque). */
void window_client_reconfigured(xcb_window_t frame, xcb_window_t child,
                                int x, int y, int width, int height, int border);

/* Where this window is certainly opaque, in root coordinates; empty when
 * nothing about it is certain. What the scene uses to leave out windows
 * nobody can see (scene.c). */
CompRect window_opaque_rect(CompWindow *w);

/* The WM has reparented `client` into a frame we track: that client is
 * where the EWMH properties live, so this is what stops the frame from
 * being anonymous (and what makes a shade distinguishable from a
 * resize). */
void window_client_reparented(xcb_window_t frame_id, xcb_window_t client);

/* The mirror entry whose client window is `client`, for property events
 * that arrive on the client rather than on the frame. */
CompWindow *window_find_by_client(xcb_window_t client);

/* Initial adoption of everything already mapped when kicomp starts, so
 * running it mid-session composites the existing desktop instead of a
 * black screen (mirrors kiwm's manage_existing_windows). */
void windows_scan(void);

/* Whether a window arriving now was already there before the compositor
 * was ready to watch (window.c's adopting_existing): main.c turns this
 * on around the events it drains before the scan, which describe what
 * happened *during* startup, not something the user just did. windows_scan
 * itself sets and clears it for its own duration. */
void windows_adopting(bool on);
void windows_resync_order(void);
void windows_teardown(void);

#endif /* KICOMP_WINDOW_H */
