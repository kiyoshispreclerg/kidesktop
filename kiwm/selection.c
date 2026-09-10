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

/* Is a compositor running right now?
 *
 * The convention every compositor follows (kicomp included): it owns the
 * _NET_WM_CM_Sn manager selection for as long as it is compositing, and
 * releases it when it stops. Asked fresh rather than cached, because a
 * compositor can come and go at any moment and there is no event kiwm
 * selects for that would say so -- and the one caller (osd.c) asks once
 * per overlay it draws, which is far too rarely for a round trip to
 * matter.
 *
 * kiwm must never *depend* on the answer -- nothing here stops working
 * without a compositor (density.h says the same about its own feature).
 * It is asked only where the honest drawing differs: an alpha the server
 * will throw away has to be flattened rather than sent.
 */
bool compositor_running(void)
{
    if (wm.cm_atom == XCB_ATOM_NONE)
        return false;

    xcb_get_selection_owner_reply_t *r = xcb_get_selection_owner_reply(
        wm.conn, xcb_get_selection_owner(wm.conn, wm.cm_atom), NULL);
    if (!r)
        return false;

    bool running = r->owner != XCB_NONE;
    free(r);
    return running;
}
