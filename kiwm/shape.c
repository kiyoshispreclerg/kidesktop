/* Client-shape propagation onto the frame -- see shape.h. */
#include "shape.h"
#include "client.h"
#include "decoration.h"

#include <xcb/shape.h>

#include <stdlib.h>
#include <string.h>

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

    /* The one time it is worth asking: adopting a window that was already
     * on screen before kiwm managed it, whose shape (if any) was set
     * before there was anyone selecting for the event that announces it.
     * Every change after this arrives as a ShapeNotify. */
    xcb_shape_query_extents_reply_t *ext = xcb_shape_query_extents_reply(
        wm.conn, xcb_shape_query_extents(wm.conn, c->window), NULL);
    c->client_shaped = ext && ext->bounding_shaped;
    free(ext);
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

    /* This event *is* the answer shape_update_frame() used to stop and ask
     * the server for. Recorded before the call below, which now reads it. */
    if (ev->shape_kind == XCB_SHAPE_SK_BOUNDING)
        c->client_shaped = ev->shaped;

    /* The client may have swapped one non-rectangular mask for another of
     * the same size -- nothing the signature below records would differ,
     * and the frame would keep the old silhouette. This event is the one
     * thing that knows better, so it forces the recomputation. */
    c->shape_sig_valid = false;

    shape_update_frame(c);
    xcb_flush(wm.conn);
}

void shape_update_frame(Client *c)
{
    if (!wm.shape_ext_present)
        return;

    /* Read, not asked. This used to be a ShapeQueryExtents *round trip*,
     * and it ran on every due frame of every drag, for the dragged client
     * and for each resize neighbour -- kiwm stopping dead mid-drag to
     * wait for the server to answer a question whose answer had not
     * changed since the window was mapped. kiwm already selects for
     * ShapeNotify on every client (shape_track_client above), so the
     * answer arrives on its own the moment it stops being true. */
    bool client_shaped = c->client_shaped;

    /* The shape is a function of the frame's size, its corners and
     * whether the client carves one of its own -- and of nothing else. In
     * particular it is not a function of where the window *is*, so a move
     * drag was re-sending a byte-identical shape at the refresh rate for
     * as long as it lasted. Recomputed when one of its inputs actually
     * moves, and skipped otherwise. */
    int sig_bt, sig_th;
    deco_insets(c, &sig_bt, &sig_th);

    /* Zeroed whole before any field is set: this is compared with
     * memcmp(), and whatever padding the struct carries has to be a known
     * value on both sides or two identical signatures can compare
     * different and the skip silently never happens. */
    struct KiWMShapeSig sig;
    memset(&sig, 0, sizeof(sig));
    sig.x = c->x;
    sig.y = c->y;
    sig.frame_w = c->frame_width;
    sig.frame_h = c->frame_height;
    sig.output = c->output;
    sig.bt = sig_bt;
    sig.th = sig_th;
    sig.r_tl = wm.radius_tl;
    sig.r_tr = wm.radius_tr;
    sig.r_br = wm.radius_br;
    sig.r_bl = wm.radius_bl;
    sig.client_shaped = client_shaped;
    sig.shaded = c->shaded;
    sig.maximized = client_maximized(c);
    sig.round_maximized = wm.round_maximized;

    if (c->shape_sig_valid && memcmp(&sig, &c->shape_sig, sizeof(sig)) == 0)
        return;

    c->shape_sig = sig;
    c->shape_sig_valid = true;

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

    c->frame_shaped = true;

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

    c->frame_shaped = true;
}
