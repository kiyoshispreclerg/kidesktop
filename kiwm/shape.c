/* Client-shape propagation onto the frame -- see shape.h. */
#include "shape.h"
#include "client.h"
#include "decoration.h"

#include <xcb/shape.h>

#include <stdlib.h>

void shape_init(void)
{
    const xcb_query_extension_reply_t *ext = xcb_get_extension_data(wm.conn, &xcb_shape_id);
    wm.shape_ext_present = ext && ext->present;
    wm.shape_event_base = wm.shape_ext_present ? ext->first_event : 0;
}

void shape_track_client(Client *c)
{
    if (!wm.shape_ext_present)
        return;
    xcb_shape_select_input(wm.conn, c->window, 1);
}

bool shape_is_notify_event(uint8_t response_type)
{
    /* ShapeNotify is the extension's event 0, so its runtime number is
     * exactly first_event. Guarded on the extension being present at all,
     * since shape_event_base is then 0 -- which is a real (error) response
     * type that must not be mistaken for a shape event. */
    return wm.shape_ext_present && response_type == (uint8_t)wm.shape_event_base;
}

void shape_handle_notify(xcb_generic_event_t *event)
{
    xcb_shape_notify_event_t *ev = (xcb_shape_notify_event_t *)event;
    Client *c = find_client_window(ev->affected_window);
    if (!c || ev->affected_window != c->window)
        return;
    shape_update_frame(c);
    xcb_flush(wm.conn);
}

void shape_update_frame(Client *c)
{
    if (!wm.shape_ext_present)
        return;

    xcb_shape_query_extents_reply_t *ext = xcb_shape_query_extents_reply(
        wm.conn, xcb_shape_query_extents(wm.conn, c->window), NULL);
    bool client_shaped = ext && ext->bounding_shaped;
    free(ext);

    /* A shaded client's content window is unmapped and the frame is
     * nothing but decoration, so its shape is kiwm's business alone --
     * clipping the frame to the (still-set) client shape would leave the
     * titlebar itself full of holes. */
    if (!client_shaped || c->shaded) {
        apply_rounded_shape(c);
        return;
    }

    int bt, th;
    deco_insets(c, &bt, &th);

    /* Both kinds matter and they're not the same thing: BOUNDING is what
     * the frame *draws* within, INPUT is what it *receives pointer events*
     * within. Forwarding only the former leaves an invisible frame still
     * eating every click across the carved-away area -- which for a
     * window like VirtualBox's mini-toolbar is the entire screen over the
     * VM. A client that sets no input shape of its own has one implicitly
     * equal to its bounding shape (per the SHAPE spec), so reading INPUT
     * from the client is correct either way. */
    xcb_shape_combine(wm.conn, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_BOUNDING, XCB_SHAPE_SK_BOUNDING,
                      c->frame, (int16_t)bt, (int16_t)th, c->window);
    xcb_shape_combine(wm.conn, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_INPUT, XCB_SHAPE_SK_INPUT,
                      c->frame, (int16_t)bt, (int16_t)th, c->window);

    if (th <= 0 && bt <= 0)
        return;

    /* The decoration isn't part of the client's shape (the client doesn't
     * know it exists), so union kiwm's own titlebar/border rectangles back
     * on top of it -- otherwise framing a shaped window clips its titlebar
     * away along with everything else the client carved out. No corner
     * rounding on this path: a shaped client is already telling kiwm
     * exactly what silhouette it wants, and rounding the union of the two
     * would just eat into it for no visual gain. */
    xcb_rectangle_t deco[4];
    int n = 0;
    if (th > 0)
        deco[n++] = (xcb_rectangle_t){ 0, 0, (uint16_t)c->frame_width, (uint16_t)th };
    if (bt > 0) {
        deco[n++] = (xcb_rectangle_t){ 0, 0, (uint16_t)bt, (uint16_t)c->frame_height };
        deco[n++] = (xcb_rectangle_t){ (int16_t)(c->frame_width - bt), 0,
                                       (uint16_t)bt, (uint16_t)c->frame_height };
        deco[n++] = (xcb_rectangle_t){ 0, (int16_t)(c->frame_height - bt),
                                       (uint16_t)c->frame_width, (uint16_t)bt };
    }

    xcb_shape_rectangles(wm.conn, XCB_SHAPE_SO_UNION, XCB_SHAPE_SK_BOUNDING,
                         XCB_CLIP_ORDERING_UNSORTED, c->frame, 0, 0, (uint32_t)n, deco);
    xcb_shape_rectangles(wm.conn, XCB_SHAPE_SO_UNION, XCB_SHAPE_SK_INPUT,
                         XCB_CLIP_ORDERING_UNSORTED, c->frame, 0, 0, (uint32_t)n, deco);
}
