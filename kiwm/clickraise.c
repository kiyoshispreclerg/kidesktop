/* focus_raise_on_release= (clickraise.h).
 *
 * kiwm sees a content click through a synchronous passive grab and replays
 * it to the application (events.c's handle_button_press()); from then on
 * the application holds the pointer, as the implicit grab of its own
 * press, and a drag-and-drop source takes an active grab of its own. So
 * kiwm must not grab anything to wait for the release: an active grab
 * would take the pointer away from the application, which then never sees
 * the motion that starts the drag (kiwm-raise-on-release tried exactly
 * that). Two things reach kiwm without touching anyone's grab:
 *
 *   - XInput 2 raw button events, which the server delivers to the root
 *     window of every client that asked, grabs or not (XI 2.1+);
 *   - XFixes selection-owner notifications: a drag-and-drop source claims
 *     XdndSelection when the drag starts.
 *
 * So: the press is replayed without raising; a claim of XdndSelection
 * before the release means a drag started, and the pending raise is
 * dropped; otherwise the release raises and focuses the window, which is
 * all a plain click ever needed. */
#include "clickraise.h"
#include "client.h"

#include <xcb/xfixes.h>
#include <xcb/xinput.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool ready;
static uint8_t xi_opcode;
static uint8_t xfixes_event_base;
static xcb_atom_t xdnd_selection;
static Client *pending;

void clickraise_init(void)
{
    if (!wm.focus_raise_on_release)
        return;

    const xcb_query_extension_reply_t *xi = xcb_get_extension_data(wm.conn, &xcb_input_id);
    const xcb_query_extension_reply_t *xf = xcb_get_extension_data(wm.conn, &xcb_xfixes_id);
    if (!xi || !xi->present || !xf || !xf->present) {
        fprintf(stderr, "kiwm: focus_raise_on_release needs XInput 2 and XFixes; raising on press\n");
        return;
    }

    /* Raw events reach a client while someone else holds a grab only
     * from XI 2.1 on. */
    xcb_input_xi_query_version_reply_t *xv = xcb_input_xi_query_version_reply(wm.conn,
        xcb_input_xi_query_version(wm.conn, 2, 2), NULL);
    bool xi_ok = xv && (xv->major_version > 2 || (xv->major_version == 2 && xv->minor_version >= 1));
    free(xv);
    xcb_xfixes_query_version_reply_t *fv = xcb_xfixes_query_version_reply(wm.conn,
        xcb_xfixes_query_version(wm.conn, 5, 0), NULL);
    bool xf_ok = fv != NULL;
    free(fv);
    if (!xi_ok || !xf_ok) {
        fprintf(stderr, "kiwm: focus_raise_on_release needs XInput 2.1 and XFixes; raising on press\n");
        return;
    }

    xcb_intern_atom_reply_t *a = xcb_intern_atom_reply(wm.conn,
        xcb_intern_atom(wm.conn, 0, strlen("XdndSelection"), "XdndSelection"), NULL);
    xdnd_selection = a ? a->atom : XCB_ATOM_NONE;
    free(a);
    if (xdnd_selection == XCB_ATOM_NONE)
        return;

    struct {
        xcb_input_event_mask_t head;
        uint32_t mask;
    } m = { { XCB_INPUT_DEVICE_ALL_MASTER, 1 }, XCB_INPUT_XI_EVENT_MASK_RAW_BUTTON_RELEASE };
    xcb_input_xi_select_events(wm.conn, wm.root, 1, &m.head);
    xcb_xfixes_select_selection_input(wm.conn, wm.root, xdnd_selection,
                                      XCB_XFIXES_SELECTION_EVENT_MASK_SET_SELECTION_OWNER);

    xi_opcode = xi->major_opcode;
    xfixes_event_base = xf->first_event;
    ready = true;
}

bool clickraise_wants(Client *c)
{
    return ready && c != wm.focused;
}

void clickraise_defer(Client *c)
{
    pending = c;
}

void clickraise_forget(Client *c)
{
    if (pending == c)
        pending = NULL;
}

static void on_release(void)
{
    if (!pending)
        return;

    /* Raw events come from the physical device, before any button
     * mapping, so rather than guess which number is the left button: the
     * click is over once no button is down any more. */
    xcb_query_pointer_reply_t *q = xcb_query_pointer_reply(wm.conn,
        xcb_query_pointer(wm.conn, wm.root), NULL);
    uint16_t buttons = XCB_BUTTON_MASK_1 | XCB_BUTTON_MASK_2 | XCB_BUTTON_MASK_3;
    bool still_down = q && (q->mask & buttons);
    free(q);
    if (still_down)
        return;

    Client *c = pending;
    pending = NULL;
    if (c->mapped && !c->minimized)
        focus_client(c);
}

bool clickraise_handle_event(xcb_generic_event_t *event)
{
    if (!ready)
        return false;
    uint8_t type = event->response_type & ~0x80;

    if (type == XCB_GE_GENERIC) {
        xcb_ge_generic_event_t *ge = (xcb_ge_generic_event_t *)event;
        if (ge->extension != xi_opcode || ge->event_type != XCB_INPUT_RAW_BUTTON_RELEASE)
            return false;
        on_release();
        return true;
    }

    if (type == (uint8_t)(xfixes_event_base + XCB_XFIXES_SELECTION_NOTIFY)) {
        xcb_xfixes_selection_notify_event_t *ev = (xcb_xfixes_selection_notify_event_t *)event;
        if (ev->selection != xdnd_selection)
            return false;
        /* A drag started out of the window the click landed on: it stays
         * where it is, and so does the focus. */
        if (ev->owner != XCB_NONE)
            pending = NULL;
        return true;
    }
    return false;
}
