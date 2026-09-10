/* The invisible resize ring around a window -- see grip.h. */
#include "grip.h"
#include "client.h"

#include <xcb/xcb.h>

/* Every grip window's geometry, in ring-relative coordinates, for a ring
 * of margin `g` around a frame of `fw` x `fh`. The ring itself is at
 * (x - g, y - g) and is (fw + 2g) x (fh + 2g), so the frame occupies the
 * box (g, g, fw, fh) inside it and these eight rectangles are exactly the
 * margin around that box. */
typedef struct { int x, y, w, h; } GripRect;

static GripRect grip_geometry(GripEdge edge, int g, int fw, int fh)
{
    switch (edge) {
    case GRIP_NW: return (GripRect){ 0,      0,      g,  g  };
    case GRIP_N:  return (GripRect){ g,      0,      fw, g  };
    case GRIP_NE: return (GripRect){ g + fw, 0,      g,  g  };
    case GRIP_W:  return (GripRect){ 0,      g,      g,  fh };
    case GRIP_E:  return (GripRect){ g + fw, g,      g,  fh };
    case GRIP_SW: return (GripRect){ 0,      g + fh, g,  g  };
    case GRIP_S:  return (GripRect){ g,      g + fh, fw, g  };
    case GRIP_SE: return (GripRect){ g + fw, g + fh, g,  g  };
    default:      return (GripRect){ 0,      0,      g,  g  };
    }
}

/* The cursor each edge shows. This is the whole reason the ring has eight
 * children instead of being one window: a window has one cursor, and a
 * cursor that is simply correct for the window under the pointer needs no
 * grab to display. */
static xcb_cursor_t grip_cursor(GripEdge edge)
{
    switch (edge) {
    case GRIP_N:  return wm.cursor_resize_n;
    case GRIP_NE: return wm.cursor_resize_ne;
    case GRIP_E:  return wm.cursor_resize_e;
    case GRIP_SE: return wm.cursor_resize_se;
    case GRIP_S:  return wm.cursor_resize_s;
    case GRIP_SW: return wm.cursor_resize_sw;
    case GRIP_W:  return wm.cursor_resize_w;
    case GRIP_NW: return wm.cursor_resize_nw;
    default:      return XCB_NONE;
    }
}

void grip_drag_params(GripEdge edge, int *right, int *bottom,
                      bool *axis_x, bool *axis_y)
{
    /* -1 means "this axis is not moving", which is what begin_drag_at()
     * reads to leave a dimension alone: dragging a window's side must not
     * also change its height. A corner moves both. */
    switch (edge) {
    case GRIP_N:  *right = -1; *bottom = 0;  *axis_x = false; *axis_y = true;  break;
    case GRIP_S:  *right = -1; *bottom = 1;  *axis_x = false; *axis_y = true;  break;
    case GRIP_W:  *right = 0;  *bottom = -1; *axis_x = true;  *axis_y = false; break;
    case GRIP_E:  *right = 1;  *bottom = -1; *axis_x = true;  *axis_y = false; break;
    case GRIP_NW: *right = 0;  *bottom = 0;  *axis_x = true;  *axis_y = true;  break;
    case GRIP_NE: *right = 1;  *bottom = 0;  *axis_x = true;  *axis_y = true;  break;
    case GRIP_SW: *right = 0;  *bottom = 1;  *axis_x = true;  *axis_y = true;  break;
    case GRIP_SE: *right = 1;  *bottom = 1;  *axis_x = true;  *axis_y = true;  break;
    default:      *right = -1; *bottom = -1; *axis_x = false; *axis_y = false; break;
    }
}

/* Should this window have a reachable grip at all, right now?
 *
 * The same rule the old in-frame grip applied, and for the same reasons: a
 * maximized or fullscreen window fills its output, so there is nothing to
 * drag its edges towards and its ring would sit over whatever is on the
 * next screen; a shaded window is nothing but titlebar; and a window whose
 * WM_NORMAL_HINTS say it is fixed-size (Client::allow_resize) has no
 * resize to offer. Half-tiled windows *do* keep their ring -- dragging the
 * shared edge of a tiled pair without a modifier is what
 * link_resize_neighbors= is for. */
static bool grip_wanted(const Client *c)
{
    return wm.resize_grip > 0 && c->grip_ring != XCB_NONE &&
           c->grip_frame_mapped &&
           c->allow_resize && !c->shaded &&
           !client_maximized(c) && !c->fullscreen;
}

void grip_create(Client *c)
{
    c->grip_ring = XCB_NONE;
    for (int i = 0; i < GRIP_COUNT; i++)
        c->grips[i] = XCB_NONE;

    if (wm.resize_grip <= 0)
        return;

    int g = wm.resize_grip;
    int fw = c->frame_width > 0 ? c->frame_width : 1;
    int fh = c->frame_height > 0 ? c->frame_height : 1;

    /* InputOnly: depth 0 and CopyFromParent for the visual are required,
     * and the only attributes such a window accepts are the event mask,
     * the cursor, override_redirect and do_not_propagate -- no background
     * and no border, which is why there is nothing here to draw. */
    c->grip_ring = xcb_generate_id(wm.conn);
    xcb_create_window(wm.conn, 0, c->grip_ring, wm.root,
                      (int16_t)(c->x - g), (int16_t)(c->y - g),
                      (uint16_t)(fw + g * 2), (uint16_t)(fh + g * 2), 0,
                      XCB_WINDOW_CLASS_INPUT_ONLY, XCB_COPY_FROM_PARENT,
                      0, NULL);

    for (int i = 0; i < GRIP_COUNT; i++) {
        GripRect r = grip_geometry((GripEdge)i, g, fw, fh);

        c->grips[i] = xcb_generate_id(wm.conn);
        uint32_t mask = XCB_CW_EVENT_MASK | XCB_CW_CURSOR;
        /* Ascending bit order: CW_EVENT_MASK (0x800) before CW_CURSOR
         * (0x4000). ButtonRelease and PointerMotion are here because the
         * press starts a drag that grabs the pointer itself -- but the
         * grab is taken in response to an event that has to arrive first.
         */
        uint32_t values[] = {
            XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE |
            XCB_EVENT_MASK_POINTER_MOTION,
            grip_cursor((GripEdge)i),
        };
        xcb_create_window(wm.conn, 0, c->grips[i], c->grip_ring,
                          (int16_t)r.x, (int16_t)r.y,
                          (uint16_t)(r.w > 0 ? r.w : 1), (uint16_t)(r.h > 0 ? r.h : 1), 0,
                          XCB_WINDOW_CLASS_INPUT_ONLY, XCB_COPY_FROM_PARENT,
                          mask, values);
        xcb_map_window(wm.conn, c->grips[i]);
    }

    /* Below its own frame, so the ring -- which covers the frame's area as
     * well as the margin -- never takes an event meant for the window it
     * belongs to. restack_all() maintains this from here on. */
    uint32_t stack[] = { c->frame, XCB_STACK_MODE_BELOW };
    xcb_configure_window(wm.conn, c->grip_ring,
                         XCB_CONFIG_WINDOW_SIBLING | XCB_CONFIG_WINDOW_STACK_MODE, stack);

    grip_sync(c);
}

void grip_destroy(Client *c)
{
    /* Destroying the parent destroys the children with it, but clear the
     * ids so a stale one can never be looked up by grip_lookup() between
     * here and the client being freed. */
    if (c->grip_ring != XCB_NONE)
        xcb_destroy_window(wm.conn, c->grip_ring);

    c->grip_ring = XCB_NONE;
    for (int i = 0; i < GRIP_COUNT; i++)
        c->grips[i] = XCB_NONE;
}

void grip_sync(Client *c)
{
    if (c->grip_ring == XCB_NONE)
        return;

    /* Mid-drag there is nothing to sync *to*: the drag holds the pointer
     * grab, so no grip window can be entered, and apply_frame_geometry()
     * -- which is where this is called from -- runs on every motion event.
     * Nine ConfigureWindow requests per motion event, for a ring nobody
     * can reach, is exactly the sort of cost that makes a resize expensive
     * for no visible gain. finish_drag() calls this once at the end. */
    if (wm.drag_client == c)
        return;

    if (!grip_wanted(c)) {
        if (c->grip_mapped) {
            xcb_unmap_window(wm.conn, c->grip_ring);
            c->grip_mapped = false;
        }
        return;
    }

    int g = wm.resize_grip;
    int fw = c->frame_width > 0 ? c->frame_width : 1;
    int fh = c->frame_height > 0 ? c->frame_height : 1;

    uint32_t rv[] = {
        (uint32_t)(c->x - g), (uint32_t)(c->y - g),
        (uint32_t)(fw + g * 2), (uint32_t)(fh + g * 2)
    };
    xcb_configure_window(wm.conn, c->grip_ring,
                         XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                         XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, rv);

    for (int i = 0; i < GRIP_COUNT; i++) {
        if (c->grips[i] == XCB_NONE)
            continue;
        GripRect r = grip_geometry((GripEdge)i, g, fw, fh);
        uint32_t cv[] = {
            (uint32_t)r.x, (uint32_t)r.y,
            (uint32_t)(r.w > 0 ? r.w : 1), (uint32_t)(r.h > 0 ? r.h : 1)
        };
        xcb_configure_window(wm.conn, c->grips[i],
                             XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                             XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, cv);
    }

    if (!c->grip_mapped) {
        xcb_map_window(wm.conn, c->grip_ring);
        c->grip_mapped = true;
    }
}

bool grip_lookup(xcb_window_t window, Client **out, GripEdge *edge)
{
    if (window == XCB_NONE)
        return false;

    for (Client *c = wm.clients; c; c = c->next) {
        for (int i = 0; i < GRIP_COUNT; i++) {
            if (c->grips[i] != window)
                continue;
            if (out)
                *out = c;
            if (edge)
                *edge = (GripEdge)i;
            return true;
        }
    }
    return false;
}

bool grip_owns_window(xcb_window_t window)
{
    if (window == XCB_NONE)
        return false;

    for (Client *c = wm.clients; c; c = c->next) {
        if (c->grip_ring == window)
            return true;
        for (int i = 0; i < GRIP_COUNT; i++)
            if (c->grips[i] == window)
                return true;
    }
    return false;
}

xcb_window_t grip_ring_window(const Client *c)
{
    return c->grip_ring;
}

void grip_frame_mapped(Client *c, bool mapped)
{
    if (c->grip_frame_mapped == mapped)
        return;
    c->grip_frame_mapped = mapped;
    grip_sync(c);
}
