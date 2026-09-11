/* See sync.h. */
#include "sync.h"
#include "client.h"
#include "shape.h"
#include "decoration.h"

#include <xcb/sync.h>

#include <stdlib.h>

/* How long a client gets to acknowledge one step before the drag goes on
 * without it. Longer than any frame a client that is keeping up would
 * take, and short enough that one that has stopped answering (blocked,
 * debugging, swapped out) costs the user a stutter, not a frozen drag. */
#define SYNC_ACK_TIMEOUT_MS 200.0

/* ...and once one has, how long it is treated as not doing sync at all.
 * Without this a client that has stopped answering costs a timeout on
 * *every* step -- a resize at five frames a second for as long as the
 * drag lasts. With it the user sees one hitch, the drag goes on at the
 * output's rate, and the client gets another chance a couple of seconds
 * later, when whatever it was doing may be over. */
#define SYNC_GIVE_UP_MS 2000.0

void sync_init(void)
{
    const xcb_query_extension_reply_t *ext = xcb_get_extension_data(wm.conn, &xcb_sync_id);
    wm.sync_ext_present = ext && ext->present;
    wm.sync_event_base = wm.sync_ext_present ? ext->first_event : 0;
    if (!wm.sync_ext_present)
        return;

    /* The extension refuses everything until this handshake. */
    xcb_sync_initialize_reply_t *r = xcb_sync_initialize_reply(wm.conn,
        xcb_sync_initialize(wm.conn, XCB_SYNC_MAJOR_VERSION, XCB_SYNC_MINOR_VERSION), NULL);
    if (!r)
        wm.sync_ext_present = false;
    free(r);
}

static xcb_sync_int64_t to_int64(uint64_t v)
{
    xcb_sync_int64_t i;
    i.hi = (int32_t)(v >> 32);
    i.lo = (uint32_t)(v & 0xffffffffu);
    return i;
}

static uint64_t from_int64(xcb_sync_int64_t i)
{
    return ((uint64_t)(uint32_t)i.hi << 32) | i.lo;
}

void sync_untrack_client(Client *c)
{
    if (c->sync_alarm != XCB_NONE) {
        xcb_sync_destroy_alarm(wm.conn, c->sync_alarm);
        c->sync_alarm = XCB_NONE;
    }
    c->sync_counter = XCB_NONE;
    c->sync_pending = false;
    c->sync_missed = false;
}

void sync_track_client(Client *c)
{
    sync_untrack_client(c);
    if (!wm.sync_ext_present)
        return;

    xcb_get_property_reply_t *p = xcb_get_property_reply(wm.conn,
        xcb_get_property(wm.conn, 0, c->window, wm.atoms.net_wm_sync_request_counter,
                         XCB_ATOM_CARDINAL, 0, 1), NULL);
    xcb_sync_counter_t counter = XCB_NONE;
    if (p && p->type == XCB_ATOM_CARDINAL && p->format == 32 &&
        xcb_get_property_value_length(p) >= 4)
        counter = *(uint32_t *)xcb_get_property_value(p);
    free(p);
    if (counter == XCB_NONE)
        return;

    /* The protocol is what makes the counter mean this; a counter
     * without it is some other use of XSync. */
    if (!client_supports_protocol(c->window, wm.atoms.net_wm_sync_request))
        return;

    /* Start from where the client's counter is, so the first value asked
     * for is above it and an alarm set on it cannot fire on the spot. */
    xcb_sync_query_counter_reply_t *q = xcb_sync_query_counter_reply(wm.conn,
        xcb_sync_query_counter(wm.conn, counter), NULL);
    if (!q)
        return;             /* the counter is not one the server knows */
    c->sync_value = from_int64(q->counter_value);
    free(q);

    /* One alarm per client, re-aimed at each new value (sync_request).
     * POSITIVE_COMPARISON: fires once the counter reaches the value; a
     * zero delta makes it go inactive after, and ChangeAlarm with the
     * next value brings it back. Nothing to poll, ever. */
    c->sync_counter = counter;
    c->sync_alarm = xcb_generate_id(wm.conn);
    xcb_sync_create_alarm_value_list_t v;
    v.counter = counter;
    v.valueType = XCB_SYNC_VALUETYPE_ABSOLUTE;
    v.value = to_int64(c->sync_value + 1);
    v.testType = XCB_SYNC_TESTTYPE_POSITIVE_COMPARISON;
    v.delta = to_int64(0);
    v.events = 1;
    xcb_sync_create_alarm_aux(wm.conn, c->sync_alarm,
                              XCB_SYNC_CA_COUNTER | XCB_SYNC_CA_VALUE_TYPE | XCB_SYNC_CA_VALUE |
                              XCB_SYNC_CA_TEST_TYPE | XCB_SYNC_CA_DELTA | XCB_SYNC_CA_EVENTS, &v);
}

bool sync_client_ready(Client *c, double now)
{
    if (c->sync_counter == XCB_NONE || !c->sync_pending)
        return true;
    if (now < c->sync_gave_up_ms + SYNC_GIVE_UP_MS)
        return true;        /* recently stopped answering: not waited for */
    if (now - c->sync_sent_ms >= SYNC_ACK_TIMEOUT_MS) {
        /* Not answering. Go on without it, and keep going without it for
         * a while; an acknowledgement that turns up later is simply late
         * and is ignored as such. */
        c->sync_pending = false;
        c->sync_gave_up_ms = now;
        return true;
    }
    return false;
}

void sync_request(Client *c)
{
    if (c->sync_counter == XCB_NONE)
        return;

    c->sync_value++;

    xcb_sync_change_alarm_value_list_t v;
    v.value = to_int64(c->sync_value);
    xcb_sync_change_alarm_aux(wm.conn, c->sync_alarm, XCB_SYNC_CA_VALUE, &v);

    /* EWMH: WM_PROTOCOLS / _NET_WM_SYNC_REQUEST, the value in two halves,
     * sent *before* the ConfigureWindow it announces. */
    xcb_client_message_event_t ev = { 0 };
    ev.response_type = XCB_CLIENT_MESSAGE;
    ev.format = 32;
    ev.window = c->window;
    ev.type = wm.atoms.wm_protocols;
    ev.data.data32[0] = wm.atoms.net_wm_sync_request;
    ev.data.data32[1] = wm.last_event_time;
    ev.data.data32[2] = (uint32_t)(c->sync_value & 0xffffffffu);
    ev.data.data32[3] = (uint32_t)(c->sync_value >> 32);
    xcb_send_event(wm.conn, 0, c->window, XCB_EVENT_MASK_NO_EVENT, (const char *)&ev);

    c->sync_pending = true;
    c->sync_sent_ms = monotonic_ms();
}

bool sync_is_alarm_event(uint8_t response_type)
{
    return wm.sync_ext_present &&
           response_type == (uint8_t)(wm.sync_event_base + XCB_SYNC_ALARM_NOTIFY);
}

void sync_handle_alarm(xcb_generic_event_t *event)
{
    xcb_sync_alarm_notify_event_t *ev = (xcb_sync_alarm_notify_event_t *)event;

    Client *c = NULL;
    for (Client *it = wm.clients; it; it = it->next)
        if (it->sync_alarm == ev->alarm) { c = it; break; }
    if (!c)
        return;

    /* An answer to an older request, or one that already timed out,
     * is not the answer to the one outstanding. */
    if (from_int64(ev->counter_value) < c->sync_value)
        return;
    c->sync_pending = false;

    /* The drag skipped a step because this was still owed, and the model
     * has moved on since: apply it now rather than at the next motion
     * event -- the client just said it is ready, and the pointer may be
     * still. The same three things a due step does. */
    if (c->sync_missed && wm.drag_client == c && wm.drag_mode == DRAG_RESIZE) {
        c->sync_missed = false;
        apply_frame_geometry_told(c, true);
        shape_update_frame(c);
        draw_decoration(c);
        xcb_flush(wm.conn);
    } else {
        c->sync_missed = false;
    }
}
