/* --replace: ICCCM manager-selection handoff (WM_Sn). */
#include "selection.h"
#include "wm.h"
#include "atoms.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

bool acquire_wm_selection(int screen_nbr, bool replace)
{
    char selname[32];
    snprintf(selname, sizeof(selname), "WM_S%d", screen_nbr);
    wm.sn_atom = intern_atom(selname);
    wm.atoms.manager = intern_atom("MANAGER");

    /* Interned here because this is the one place that knows the screen
     * number. Asking who owns it is compositor_running() below. */
    snprintf(selname, sizeof(selname), "_NET_WM_CM_S%d", screen_nbr);
    wm.cm_atom = intern_atom(selname);

    xcb_get_selection_owner_reply_t *owner_reply = xcb_get_selection_owner_reply(
        wm.conn, xcb_get_selection_owner(wm.conn, wm.sn_atom), NULL);
    xcb_window_t old_owner = owner_reply ? owner_reply->owner : XCB_NONE;
    free(owner_reply);

    if (old_owner != XCB_NONE && !replace) {
        fprintf(stderr, "kiwm: another window manager is already running "
                        "(pass --replace to take over)\n");
        return false;
    }

    wm.sel_win = xcb_generate_id(wm.conn);
    xcb_create_window(wm.conn, XCB_COPY_FROM_PARENT, wm.sel_win, wm.root,
                      -1, -1, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      wm.screen->root_visual, 0, NULL);

    if (old_owner != XCB_NONE) {
        /* A well-behaved previous kiwm (see the SELECTION_CLEAR handler in
         * events.c) watches its own selection window and releases
         * SubstructureRedirect + exits as soon as it loses WM_Sn -- no
         * StructureNotify polling on old_owner needed for that case. This
         * DestroyNotify wait is only a fallback for WMs that just quit
         * outright without the SelectionClear protocol. */
        uint32_t mask = XCB_EVENT_MASK_STRUCTURE_NOTIFY;
        xcb_change_window_attributes(wm.conn, old_owner, XCB_CW_EVENT_MASK, &mask);
    }

    xcb_set_selection_owner(wm.conn, wm.sel_win, wm.sn_atom, XCB_CURRENT_TIME);
    xcb_flush(wm.conn);

    if (old_owner != XCB_NONE) {
        fprintf(stderr, "kiwm: waiting for previous window manager to release control...\n");
        time_t start = time(NULL);
        for (;;) {
            xcb_generic_error_t *err = xcb_request_check(wm.conn,
                xcb_change_window_attributes_checked(wm.conn, wm.root,
                    XCB_CW_EVENT_MASK, (uint32_t[]){ XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT }));
            if (!err) {
                /* Old WM already let go: undo the probe grab, real setup
                 * below re-selects the full event mask anyway. */
                break;
            }
            free(err);

            xcb_generic_event_t *ev = xcb_poll_for_event(wm.conn);
            if (ev) {
                uint8_t type = ev->response_type & ~0x80;
                if (type == XCB_DESTROY_NOTIFY &&
                    ((xcb_destroy_notify_event_t *)ev)->window == old_owner) {
                    free(ev);
                    break;
                }
                free(ev);
            } else {
                if (time(NULL) - start > 3) {
                    fprintf(stderr, "kiwm: timed out waiting for previous WM, continuing anyway\n");
                    break;
                }
                struct timespec ts = { 0, 20000000L };
                nanosleep(&ts, NULL);
            }
        }
    }

    xcb_client_message_event_t ev = { 0 };
    ev.response_type = XCB_CLIENT_MESSAGE;
    ev.format = 32;
    ev.window = wm.root;
    ev.type = wm.atoms.manager;
    ev.data.data32[0] = XCB_CURRENT_TIME;
    ev.data.data32[1] = wm.sn_atom;
    ev.data.data32[2] = wm.sel_win;
    xcb_send_event(wm.conn, 0, wm.root, XCB_EVENT_MASK_STRUCTURE_NOTIFY, (const char *)&ev);
    xcb_flush(wm.conn);

    return true;
}

/* How stale the cached answer below may get. A compositor starting or
 * stopping is noticed within this long, which is far below anything a
 * person would call a delay, and it is the difference between one round
 * trip every quarter second and one per repaint -- the decoration is
 * redrawn at the display's refresh rate for the whole length of a
 * move/resize drag, so a query per repaint is not an option. */
#define COMPOSITOR_CACHE_MS 250.0

/* Is a compositor running?
 *
 * The convention every compositor follows (kicomp included): it owns the
 * _NET_WM_CM_Sn manager selection for as long as it is compositing, and
 * releases it when it stops. So the answer is who owns that selection.
 *
 * Cached for COMPOSITOR_CACHE_MS rather than watched, deliberately. Being
 * *told* would mean selecting StructureNotify on the owner window to catch
 * it going away and handling the MANAGER client message to catch a new one
 * arriving -- two more moving parts, for a fact that is only ever used to
 * choose between two shades of the same colour. Re-asking occasionally is
 * the proportionate mechanism, and it cannot get stuck: there is no state
 * to go out of sync, only an answer that may be a quarter second old.
 *
 * kiwm must never *depend* on the answer -- nothing stops working without
 * a compositor (density.h says the same about its own feature). It is
 * asked only where the honest drawing differs: an alpha the server is
 * going to throw away has to be flattened rather than sent, because X
 * displays the premultiplied colour of a window nobody composites and the
 * theme's colour would arrive darker than the theme names it.
 */
static bool cached;
static double asked_at = -1.0;

void compositor_running_reset(void)
{
    asked_at = -1.0;
}

bool compositor_running(void)
{

    if (wm.cm_atom == XCB_ATOM_NONE)
        return false;

    double now = monotonic_ms();
    if (asked_at >= 0.0 && now - asked_at < COMPOSITOR_CACHE_MS)
        return cached;

    xcb_get_selection_owner_reply_t *r = xcb_get_selection_owner_reply(
        wm.conn, xcb_get_selection_owner(wm.conn, wm.cm_atom), NULL);
    if (!r)
        return cached;      /* keep the last answer rather than inventing one */

    bool was = cached;
    cached = r->owner != XCB_NONE;
    /* The fallback for a compositor that does not announce itself with
     * MANAGER (compositor_watch_event below is the prompt path): the
     * flip is noticed at whatever repaint asks next, and the main loop
     * reframes then. Only flagged -- never acted on here, because this
     * is called from inside draw_decoration(). */
    if (asked_at >= 0.0 && cached != was)
        wm.compositor_changed = true;
    if (r->owner != wm.cm_owner)
        compositor_watch_owner(r->owner);
    asked_at = now;
    free(r);
    return cached;
}

/* Watching the compositor rather than asking about it. Two events say
 * everything: the ICCCM MANAGER message a compositor sends to the root
 * on taking _NET_WM_CM_Sn (arrival), and the DestroyNotify of the window
 * that owns it (departure -- a compositor that exits takes its windows
 * with it, and StructureNotify is selected on that window here for
 * exactly that). The cached answer above is dropped on either, so the
 * next compositor_running() asks afresh. */
void compositor_watch_owner(xcb_window_t owner)
{
    wm.cm_owner = owner;
    if (owner == XCB_NONE)
        return;
    uint32_t mask = XCB_EVENT_MASK_STRUCTURE_NOTIFY;
    xcb_change_window_attributes(wm.conn, owner, XCB_CW_EVENT_MASK, &mask);
}

void compositor_watch_init(void)
{
    if (wm.cm_atom == XCB_ATOM_NONE)
        return;
    xcb_get_selection_owner_reply_t *r = xcb_get_selection_owner_reply(
        wm.conn, xcb_get_selection_owner(wm.conn, wm.cm_atom), NULL);
    if (!r)
        return;
    compositor_watch_owner(r->owner);
    free(r);
}

static void compositor_forget_cache(void)
{
    /* Force the next compositor_running() to ask, whatever the cache
     * says: the answer has just been seen to change. */
    compositor_running_reset();
}

bool compositor_watch_event(xcb_generic_event_t *event)
{
    uint8_t type = event->response_type & ~0x80;

    if (type == XCB_CLIENT_MESSAGE) {
        xcb_client_message_event_t *ev = (xcb_client_message_event_t *)event;
        if (ev->type != wm.atoms.manager || ev->format != 32 ||
            ev->data.data32[1] != wm.cm_atom)
            return false;
        compositor_watch_owner((xcb_window_t)ev->data.data32[2]);
        compositor_forget_cache();
        wm.compositor_changed = true;
        return true;
    }

    if (type == XCB_DESTROY_NOTIFY) {
        xcb_destroy_notify_event_t *ev = (xcb_destroy_notify_event_t *)event;
        if (wm.cm_owner == XCB_NONE || ev->window != wm.cm_owner)
            return false;
        wm.cm_owner = XCB_NONE;
        compositor_forget_cache();
        wm.compositor_changed = true;
        return true;
    }

    return false;
}
