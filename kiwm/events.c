/* X event dispatch: the WM's whole runtime behavior after startup lives
 * here, delegating actual state changes to client.c/output.c/ewmh.c. */
#include "events.h"
#include "wm.h"
#include "client.h"
#include "output.h"
#include "decoration.h"
#include "density.h"
#include "ewmh.h"
#include "keybind.h"
#include "osd.h"
#include "menu.h"
#include "outline.h"
#include "shape.h"
#include "selection.h"
#include "grip.h"

#include <xcb/randr.h>

#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>
#include <stdlib.h>

/* Shared by handle_map_request (app remaps an already-managed window via
 * a fresh MapRequest -- can't happen normally since MapRequest only
 * fires for children of *root*, but kept for whatever unmanage/remanage
 * edge case might route back through here) and handle_map_notify (the
 * common real-world case: an app just calls XMapWindow directly on its
 * own already-reparented-into-our-frame window to re-show a popup it
 * kept around and only hid via XUnmapWindow -- e.g. Plasma's kickoff
 * menu toggling the same window every open/close instead of recreating
 * it. Root only redirects MapRequest for its own direct children, so
 * once a window is reparented into our frame, further XMapWindow calls
 * on it go straight through as a plain MapNotify, no MapRequest -- if we
 * only ever handled MapRequest, the frame (still unmapped from the
 * previous hide) would never come back, so the popup would appear to
 * work exactly once and then silently stop responding to its own toggle
 * until the app that owns it is restarted). */
static void remap_existing_client(Client *c)
{
    /* The content window itself, not just the frame. A client that hides
     * by unmapping its own window (Qt's hide() -- OpenSnitch's prompt
     * dialog, krunner, anything that keeps a window around between
     * showings) leaves it unmapped, and a MapRequest for it is *denied*
     * until the WM maps it: that's the whole point of the frame's
     * SubstructureRedirect. Mapping only the frame brings back a window
     * whose inside is empty -- kiwm's own decoration around a hole showing
     * whatever is behind it -- and no amount of moving/resizing/maximizing
     * fixes it, because there is nothing there to repaint; only something
     * that happens to map the content window again (shade+unshade, which
     * unmaps and remaps it on purpose) does. Redundant but harmless in the
     * other case that gets here, an unminimize, where the content window
     * was never unmapped in the first place -- kiwm minimizes by hiding
     * the frame. Skipped while shaded, where the content window is
     * deliberately unmapped and toggle_shade() owns remapping it. */
    if (!c->shaded)
        xcb_map_window(wm.conn, c->window);

    if (c->sticky || wm.outputs[c->output].desktop == c->desktop) {
        xcb_map_window(wm.conn, c->frame);
        c->mapped = true;
    }
    c->minimized = false;
    set_icccm_wm_state(c, WM_STATE_NORMAL);
    ewmh_update_wm_state(c);
    /* Mapping a frame doesn't restack it, so a window coming back after
     * being hidden reappears at whatever stacking position it was left in
     * -- which for a transient that was hidden while its parent got raised
     * (VirtualBox's auto-hiding mini-toolbar, exactly) means underneath the
     * very window it's supposed to float over. An app showing a window
     * again means it to be seen, so it goes to the top of its own layer --
     * the same restack_all_raising() focus_client() uses, and what keeps
     * krunner from coming back *behind* whatever was focused since it last
     * hid itself. */
    if (c->mapped)
        restack_all_raising(c);
    else
        restack_all();
}

static void handle_map_request(xcb_map_request_event_t *ev)
{
    Client *c = find_client_window(ev->window);
    if (!c)
        manage(ev->window, true);
    else
        remap_existing_client(c);
    xcb_flush(wm.conn);
}

static void handle_map_notify(xcb_map_notify_event_t *ev)
{
    Client *c = find_client_window(ev->window);

    /* The *frame* appearing, from whichever of the many paths mapped it
     * (minimize/restore, a desktop switch, an output reassignment). The
     * invisible resize ring follows it -- see grip.h for why this is the
     * hook rather than each of those call sites. */
    if (c && ev->window == c->frame)
        grip_frame_mapped(c, true);

    if (c && ev->window == c->window && c->ignore_map > 0) {
        /* The server's own re-map at the end of a reparent, not the app
         * asking to be shown -- see wm.h's Client::ignore_map. */
        c->ignore_map--;
        return;
    }
    if (c && ev->window == c->window && !c->mapped) {
        remap_existing_client(c);
        xcb_flush(wm.conn);
    }
}

static void handle_configure_request(xcb_configure_request_event_t *ev)
{
    Client *c = find_client_window(ev->window);

    if (!c) {
        uint16_t mask = ev->value_mask;
        uint32_t values[7];
        int n = 0;
        if (mask & XCB_CONFIG_WINDOW_X)            values[n++] = (uint32_t)ev->x;
        if (mask & XCB_CONFIG_WINDOW_Y)            values[n++] = (uint32_t)ev->y;
        if (mask & XCB_CONFIG_WINDOW_WIDTH)        values[n++] = ev->width;
        if (mask & XCB_CONFIG_WINDOW_HEIGHT)       values[n++] = ev->height;
        if (mask & XCB_CONFIG_WINDOW_BORDER_WIDTH) values[n++] = ev->border_width;
        if (mask & XCB_CONFIG_WINDOW_SIBLING)      values[n++] = ev->sibling;
        if (mask & XCB_CONFIG_WINDOW_STACK_MODE)   values[n++] = ev->stack_mode;
        xcb_configure_window(wm.conn, ev->window, mask, values);
        xcb_flush(wm.conn);
        return;
    }

    if (client_maximized(c)) {
        /* Ignore geometry requests while maximized; just re-affirm current state. */
        configure_frame(c);
        xcb_flush(wm.conn);
        return;
    }

    /* `c` is only ever found here for an *already-reparented* window (see
     * client.c's manage(): the frame's own SubstructureRedirect is what
     * makes this fire at all, and that's only selected on a frame that
     * already has the client reparented into it -- there's no window where
     * find_client_window() could match a still-child-of-root window). So
     * ev->x/ev->y are parent-relative to the *frame*, but the app that
     * issued this ConfigureWindow on its own (reparented, from its own
     * point of view still-a-root-child per ICCCM's transparency
     * requirement) window meant them as its own absolute *screen* position
     * -- i.e. where it wants its *content*, not the frame, to end up.
     * Converting that back to c->x/c->y (which this whole codebase treats
     * as the frame's own top-left) needs subtracting the same insets
     * deco_insets() everywhere else adds going the other way. Getting this
     * backwards (assigning ev->x/y to c->x/c->y directly, as if they were
     * already frame-relative) is exactly the "content displaced by
     * whatever the window's old position used to be, cut off in a corner"
     * bug this fixes -- width/height need no such translation, since
     * those the app really is requesting for its own content, unaffected
     * by insets. */
    int bt, th;
    deco_insets(c, &bt, &th);

    /* Same ICCCM gravity rule manage() applies when it first frames a
     * window (see client.c): with StaticGravity -- what every Qt/GTK
     * window asks for, and the case that made this matter -- the position
     * names where the *content* should end up, so the frame goes above and
     * left of it by the decoration insets. Under NorthWest (the default
     * for anything that doesn't ask) the position names the frame itself,
     * and no translation applies. */
    int gx = (c->gravity == XCB_GRAVITY_STATIC) ? bt : 0;
    int gy = (c->gravity == XCB_GRAVITY_STATIC) ? th : 0;

    if (ev->value_mask & XCB_CONFIG_WINDOW_X)      c->x = ev->x - gx;
    if (ev->value_mask & XCB_CONFIG_WINDOW_Y)      c->y = ev->y - gy;
    if (ev->value_mask & XCB_CONFIG_WINDOW_WIDTH)  c->width = ev->width < c->min_w ? c->min_w : ev->width;
    if (ev->value_mask & XCB_CONFIG_WINDOW_HEIGHT) c->height = ev->height < c->min_h ? c->min_h : ev->height;

    configure_frame(c);

    /* A plain XRaiseWindow/XLowerWindow arrives here too, and used to be
     * dropped on the floor -- an app asking to be raised (VirtualBox's
     * mini-toolbar does exactly that when it slides back into view) simply
     * never was. Applied to the *frame*, since that's what actually sits in
     * the root's stacking order, and then handed to restack_all() so the
     * request still lands within whatever layer the client belongs to
     * rather than jumping the whole stack. The requested sibling is
     * deliberately ignored: it's expressed in terms of the client's
     * would-be siblings, which under a reparenting WM aren't the frame's. */
    if (ev->value_mask & XCB_CONFIG_WINDOW_STACK_MODE) {
        uint32_t mode = ev->stack_mode;
        if (mode == XCB_STACK_MODE_ABOVE || mode == XCB_STACK_MODE_TOP_IF ||
            mode == XCB_STACK_MODE_BELOW || mode == XCB_STACK_MODE_BOTTOM_IF) {
            uint32_t v = (mode == XCB_STACK_MODE_ABOVE || mode == XCB_STACK_MODE_TOP_IF)
                             ? XCB_STACK_MODE_ABOVE : XCB_STACK_MODE_BELOW;
            xcb_configure_window(wm.conn, c->frame, XCB_CONFIG_WINDOW_STACK_MODE, &v);
            restack_all();
        }
    }

    xcb_flush(wm.conn);
}

/* Populates wm.resize_neighbors_x/y and wm.resize_edge_x/y_start for a
 * DRAG_RESIZE about to start on `c`, once wm.resize_right/resize_bottom
 * are already decided (see begin_drag(), which calls this right after) --
 * see wm.h's ResizeNeighbor for the overall idea. Scans every other
 * mapped, visible client on c's own output (same-output only, unlike
 * magnet_snap_move()'s cross-output candidates -- resizing a neighbor only
 * makes sense against a window that could plausibly share the same
 * screen) whose frame edge sits within RESIZE_NEIGHBOR_EPSILON_PX of the
 * specific edge about to be dragged, gated by the same perpendicular-
 * overlap test magnet_snap_move()/_resize() use. wm.resize_right/
 * resize_bottom already say which *of our own* edges is moving, so which
 * of the *neighbor's* edges could plausibly be the touching one follows
 * directly -- a neighbor a resize_right drag could touch sits to our
 * right, so only its left edge is tested (never its right edge, which
 * would mean it's overlapping us, not adjacent to us). */
static void detect_resize_neighbors(Client *c)
{
    wm.resize_neighbors_x_count = 0;
    wm.resize_neighbors_y_count = 0;

    int moving_x = wm.resize_right  ? (c->x + c->frame_width)  : c->x;
    int moving_y = wm.resize_bottom ? (c->y + c->frame_height) : c->y;
    wm.resize_edge_x_start = moving_x;
    wm.resize_edge_y_start = moving_y;

    for (Client *o2 = wm.clients; o2; o2 = o2->next) {
        if (o2 == c || !o2->mapped || o2->minimized || o2->shaded || o2->output != c->output)
            continue;
        if (!o2->sticky && wm.outputs[o2->output].desktop != o2->desktop)
            continue;
        /* Not a window whose geometry belongs to kiwm rather than to the
         * user. A fullscreen or maximized window has its edges exactly on
         * its output's, so *every* window snapped against a screen edge
         * finds one within the epsilon and drags it along -- which is how
         * opening a dialog beside a fullscreen VM and resizing the dialog
         * ended up resizing the VM. And there would be no point even if it
         * looked right: the next refit re-applies the output's geometry
         * over whatever the drag left. resize_grip_at() already refuses to
         * give these windows grips of their *own*, for the same reason;
         * this is the other half of that rule. */
        if (o2->fullscreen || client_maximized(o2))
            continue;

        int rl = o2->x, rr = o2->x + o2->frame_width;
        int rt = o2->y, rb = o2->y + o2->frame_height;

        bool y_overlap = (c->y + c->frame_height > rt) && (c->y < rb);
        if (y_overlap && wm.resize_neighbors_x_count < MAX_RESIZE_NEIGHBORS) {
            int edge = wm.resize_right ? rl : rr;
            int d = edge - moving_x;
            if ((d < 0 ? -d : d) <= RESIZE_NEIGHBOR_EPSILON_PX) {
                ResizeNeighbor *n = &wm.resize_neighbors_x[wm.resize_neighbors_x_count++];
                n->client = o2;
                n->orig_x = o2->x; n->orig_y = o2->y;
                n->orig_w = o2->width; n->orig_h = o2->height;
            }
        }

        bool x_overlap = (c->x + c->frame_width > rl) && (c->x < rr);
        if (x_overlap && wm.resize_neighbors_y_count < MAX_RESIZE_NEIGHBORS) {
            int edge = wm.resize_bottom ? rt : rb;
            int d = edge - moving_y;
            if ((d < 0 ? -d : d) <= RESIZE_NEIGHBOR_EPSILON_PX) {
                ResizeNeighbor *n = &wm.resize_neighbors_y[wm.resize_neighbors_y_count++];
                n->client = o2;
                n->orig_x = o2->x; n->orig_y = o2->y;
                n->orig_w = o2->width; n->orig_h = o2->height;
            }
        }
    }
}

/* Whether a DRAG_RESIZE about to start on `c` should preserve its half-snap
 * state instead of detiling back to its pre-snap floating size (see wm.h's
 * KiWM::drag_preserve_snap) -- true only when link_resize_neighbors is on,
 * c is currently half-snapped (Client::snap_side LEFT/RIGHT -- SNAP_TOP/
 * maximized never qualifies, there's no "other half" to preserve against),
 * the click landed on c's *shared* edge (the one touching its counterpart
 * -- e.g. a SNAP_LEFT window's right edge), and there's actually a same-
 * output neighbor half-snapped to the complementary side still touching
 * that edge. Must be checked *before* client.c's detile_for_drag() runs --
 * once it does, c->snap_side is already cleared and there's nothing left
 * to check. */
static bool should_preserve_snap_resize(Client *c, int root_x)
{
    if (!wm.link_resize_neighbors || (c->snap_side != SNAP_LEFT && c->snap_side != SNAP_RIGHT))
        return false;

    bool resize_right = (root_x - c->x) > c->frame_width / 2;
    bool shared_edge = (c->snap_side == SNAP_LEFT && resize_right) ||
                       (c->snap_side == SNAP_RIGHT && !resize_right);
    if (!shared_edge)
        return false;

    SnapSide want_side = (c->snap_side == SNAP_LEFT) ? SNAP_RIGHT : SNAP_LEFT;
    int edge = resize_right ? (c->x + c->frame_width) : c->x;

    for (Client *o2 = wm.clients; o2; o2 = o2->next) {
        if (o2 == c || !o2->mapped || o2->minimized || o2->output != c->output)
            continue;
        if (o2->snap_side != want_side)
            continue;
        if (!o2->sticky && wm.outputs[o2->output].desktop != o2->desktop)
            continue;
        int other_edge = resize_right ? o2->x : (o2->x + o2->frame_width);
        int d = other_edge - edge;
        if ((d < 0 ? -d : d) <= RESIZE_NEIGHBOR_EPSILON_PX)
            return true;
    }
    return false;
}

/* Starts a move or resize drag at a given root position, whatever asked
 * for it: a titlebar click, a mod_key-drag from anywhere on
 * the window (both via begin_drag() below), or the client itself asking
 * through _NET_WM_MOVERESIZE (handle_moveresize()). Captures
 * wm.drag_start_x/y/w/h and grabs the pointer.
 *
 * `preserve_snap` is should_preserve_snap_resize()'s answer, only ever
 * true for a resize on the shared edge of two half-snapped windows.
 * Otherwise a maximized/tiled window is detiled first (client.c's
 * detile_for_drag(), a no-op when already floating) so the rest of the
 * drag builds on a floating baseline -- except for a *move*, which defers
 * that until the pointer has actually gone somewhere (see
 * KiWM::drag_detile_pending).
 *
 * For a resize, `corner_right`/`corner_bottom` name which corner grows;
 * the opposite one stays fixed for the whole drag (see handle_motion).
 * Pass -1 for either to pick it from the press position, nearest-corner,
 * kwin/compiz-style -- which is what a plain drag does.
 *
 * `axis_x`/`axis_y` say which dimensions the resize may change at all:
 * both, for a corner drag, or just one for an edge grip (kiwm.conf's
 * resize_grip=) or an edge _NET_WM_MOVERESIZE direction, so dragging a
 * window's side doesn't also change its height. Ignored for a move. */
static void begin_drag_at(Client *c, DragMode mode, int root_x, int root_y,
                          bool preserve_snap, int corner_right, int corner_bottom,
                          bool axis_x, bool axis_y)
{
    /* A fullscreen window is never *resized*: its geometry is its
     * output's, kiwm owns it for as long as the state lasts, and any size
     * dragged onto it would just be re-applied away on the next refit.
     * Moving it is a different question -- carrying it to another output,
     * still fullscreen, is the only way to get it off a screen without
     * leaving fullscreen first, and kwin allows exactly that. So a move
     * becomes an output move (see KiWM::drag_fullscreen_move): nothing
     * follows the pointer, the outline shows which output would take it,
     * and the release re-applies fullscreen there. */
    if (c->fullscreen && mode != DRAG_MOVE)
        return;

    /* A window that declares it can't be moved or resized (Motif's
     * functions field, or a fixed min==max size -- see client.c's
     * update_client_actions()) doesn't get dragged either, whether the
     * drag started on its titlebar, via a modifier from anywhere on it, or
     * from the client's own request. */
    if (mode == DRAG_MOVE && !c->allow_move)
        return;
    if (mode == DRAG_RESIZE && !c->allow_resize)
        return;

    wm.drag_fullscreen_move = c->fullscreen;
    /* Nothing to detile for a fullscreen move: fullscreen isn't a tiling
     * state, and detile_for_drag() would hand the window its pre-maximize
     * geometry in the middle of a drag that isn't changing its size. */
    wm.drag_preserve_snap = preserve_snap || wm.drag_fullscreen_move;
    wm.drag_detile_pending = false;
    if (!wm.drag_preserve_snap) {
        if (mode == DRAG_MOVE && (c->max_horz || c->max_vert || c->snap_side != SNAP_NONE))
            wm.drag_detile_pending = true;
        else
            detile_for_drag(c, root_x, root_y);
    }

    wm.drag_mode = mode;
    wm.drag_client = c;
    wm.drag_snap_side = SNAP_NONE;
    wm.last_drag_apply_ms = 0; /* don't let a previous drag's timestamp throttle this new one's first frame */
    wm.drag_start_root_x = root_x;
    wm.drag_start_root_y = root_y;
    wm.drag_start_x = c->x;
    wm.drag_start_y = c->y;
    wm.drag_start_w = c->width;
    wm.drag_start_h = c->height;

    xcb_cursor_t cursor;
    if (mode == DRAG_RESIZE) {
        wm.resize_right = (corner_right >= 0) ? (corner_right != 0)
                                              : ((root_x - c->x) > c->frame_width / 2);
        wm.resize_bottom = (corner_bottom >= 0) ? (corner_bottom != 0)
                                                : ((root_y - c->y) > c->frame_height / 2);
        wm.resize_axis_x = axis_x;
        wm.resize_axis_y = axis_y;
        if (!axis_x)
            cursor = wm.resize_bottom ? wm.cursor_resize_s : wm.cursor_resize_n;
        else if (!axis_y)
            cursor = wm.resize_right ? wm.cursor_resize_e : wm.cursor_resize_w;
        else if (wm.resize_right)
            cursor = wm.resize_bottom ? wm.cursor_resize_se : wm.cursor_resize_ne;
        else
            cursor = wm.resize_bottom ? wm.cursor_resize_sw : wm.cursor_resize_nw;
        if (wm.link_resize_neighbors)
            detect_resize_neighbors(c);
        else {
            wm.resize_neighbors_x_count = 0;
            wm.resize_neighbors_y_count = 0;
        }
    } else {
        cursor = wm.cursor_move;
        wm.resize_neighbors_x_count = 0;
        wm.resize_neighbors_y_count = 0;
    }

    /* The grab has to actually succeed, and it can genuinely fail: a
     * client that grabs the pointer for itself -- VirtualBox capturing
     * input for its guest is the one that does -- owns it until it lets
     * go, and a drag started without a grab would never see its own
     * ButtonRelease. That left wm.drag_client set forever, with every
     * later pointer motion treated as part of a drag that can't end,
     * which is exactly what "kiwm froze" looked like. So the state is
     * unwound and the drag simply doesn't start. */
    xcb_grab_pointer_reply_t *grab = xcb_grab_pointer_reply(wm.conn,
        xcb_grab_pointer(wm.conn, 0, wm.root,
                         XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_POINTER_MOTION,
                         XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
                         XCB_NONE, cursor, XCB_CURRENT_TIME), NULL);
    bool grabbed = grab && grab->status == XCB_GRAB_STATUS_SUCCESS;
    free(grab);

    if (!grabbed) {
        fprintf(stderr, "kiwm: pointer grab refused -- not starting a drag "
                        "(another client is holding the pointer)\n");
        wm.drag_client = NULL;
        wm.drag_mode = DRAG_NONE;
        wm.drag_snap_side = SNAP_NONE;
        wm.drag_detile_pending = false;
        wm.drag_fullscreen_move = false;
        wm.resize_preview_active = false;
        wm.resize_neighbors_x_count = 0;
        wm.resize_neighbors_y_count = 0;
        return;
    }

    xcb_flush(wm.conn);
}

/* The two pointer-driven drag-start sites (titlebar-click-move and
 * mod_key-drag). */
static void begin_drag(Client *c, DragMode mode, xcb_button_press_event_t *ev)
{
    begin_drag_at(c, mode, ev->root_x, ev->root_y,
                  (mode == DRAG_RESIZE) && should_preserve_snap_resize(c, ev->root_x),
                  -1, -1, true, true);
}

/* Which configured titlebar element (see wm.h's DecoElemKind/
 * wm.deco_layout) sits at a given rel_x, if any -- shared by
 * handle_button_press's hit-testing and update_button_hover(), so they
 * can never disagree about where a button actually is. Fills `slots` with
 * the whole computed layout (caller may only care about the one index
 * returned, but computing the rest is nearly free and callers that do
 * care can just index it). */
static int deco_slot_at(Client *c, int rel_x, DecoSlot *slots, int max_slots)
{
    int n = compute_deco_layout(c, c->frame_width, slots, max_slots);
    for (int i = 0; i < n; i++)
        if (rel_x >= slots[i].x && rel_x < slots[i].x + slots[i].width)
            return i;
    return -1;
}

/* Titlebar elements that behave like buttons -- everything except the
 * title text and the icon, which have their own click behavior. */
static bool deco_kind_is_button(DecoElemKind kind)
{
    switch (kind) {
    case DECO_CLOSE:
    case DECO_MAXIMIZE:
    case DECO_MINIMIZE:
    case DECO_SHADE:
    case DECO_KEEP_ABOVE:
    case DECO_KEEP_ALL_DESKTOPS:
    case DECO_APPMENU:
        return true;
    default:
        return false;
    }
}

/* `button` is the mouse button that armed this press. Only the maximize
 * button distinguishes them, the way kwin's does: left maximizes both
 * directions (and from a single-axis state takes the window the rest of
 * the way, so the *next* left click restores it to the geometry from
 * before any of it), right maximizes horizontally only, middle
 * vertically only. Every other button ignores anything but the left. */
/* kiwm.conf's appmenu_command= with %w/%x/%y filled in, run detached.
 *
 * The double fork is what keeps kiwm from collecting zombies without
 * installing a SIGCHLD handler: the intermediate child exits immediately
 * (and is reaped right here), leaving the actual command reparented to
 * init. setsid() then keeps it out of kiwm's process group, so a menu
 * helper doesn't die with the terminal kiwm was started from. */
static void run_appmenu_command(Client *c, int root_x, int root_y)
{
    if (!wm.appmenu_command[0])
        return;

    char cmd[1024];
    size_t o = 0;
    for (const char *p = wm.appmenu_command; *p && o + 1 < sizeof(cmd); p++) {
        if (p[0] != '%' || !p[1]) {
            cmd[o++] = *p;
            continue;
        }
        char sub[32];
        switch (p[1]) {
        case 'w': snprintf(sub, sizeof(sub), "%u", (unsigned)c->window); break;
        case 'x': snprintf(sub, sizeof(sub), "%d", root_x); break;
        case 'y': snprintf(sub, sizeof(sub), "%d", root_y); break;
        case '%': snprintf(sub, sizeof(sub), "%%"); break;
        default:  sub[0] = '\0'; break;
        }
        if (sub[0] == '\0' && p[1] != '%') {
            /* Not a placeholder kiwm knows -- pass it through untouched
             * rather than eating it, so a command can contain a literal
             * percent without needing to double it. */
            cmd[o++] = *p;
            continue;
        }
        o += (size_t)snprintf(cmd + o, sizeof(cmd) - o, "%s", sub);
        p++;
        if (o >= sizeof(cmd))
            break;
    }
    cmd[o < sizeof(cmd) ? o : sizeof(cmd) - 1] = '\0';

    pid_t pid = fork();
    if (pid == 0) {
        if (fork() == 0) {
            setsid();
            execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
            _exit(127);
        }
        _exit(0);
    } else if (pid > 0) {
        waitpid(pid, NULL, 0);
    }
}

static void run_deco_button(Client *c, DecoElemKind kind, uint8_t button, int slot_x)
{
    if (kind == DECO_MAXIMIZE) {
        switch (button) {
        case XCB_BUTTON_INDEX_1: toggle_maximize(c, -1); break;
        case XCB_BUTTON_INDEX_3: toggle_maximize_horz(c, -1); break;
        case XCB_BUTTON_INDEX_2: toggle_maximize_vert(c, -1); break;
        default: break;
        }
        return;
    }

    if (button != XCB_BUTTON_INDEX_1)
        return;

    switch (kind) {
    case DECO_CLOSE:             close_client(c); break;
    /* Anchored under the button, the same place the window menu opens
     * from the icon -- the helper gets root coordinates and decides
     * nothing else about placement. */
    case DECO_APPMENU:           run_appmenu_command(c, c->x + slot_x, c->y + TITLEBAR_H); break;
    case DECO_MINIMIZE:          minimize_client(c); break;
    case DECO_SHADE:             toggle_shade(c, -1); break;
    case DECO_KEEP_ABOVE:        toggle_keep_above(c, -1); break;
    case DECO_KEEP_ALL_DESKTOPS: toggle_sticky(c, -1); break;
    default:                     break;
    }
}

static void handle_button_press(xcb_button_press_event_t *ev)
{
    /* A press on one of the invisible ring windows outside a frame
     * (grip.h): resize from that edge, no modifier needed. Checked before
     * anything else because a grip window is not a client window and every
     * lookup below would miss it.
     *
     * The edge decides which corner grows and which axes may change at
     * all, so dragging a window's side doesn't also change its height --
     * the ring's shape carries what resize_grip_at()'s arithmetic used to
     * work out from the pointer position. */
    {
        Client *gc;
        GripEdge edge;
        if (ev->detail == XCB_BUTTON_INDEX_1 &&
            grip_lookup(ev->event, &gc, &edge)) {
            focus_client(gc);

            int right, bottom;
            bool axis_x, axis_y;
            grip_drag_params(edge, &right, &bottom, &axis_x, &axis_y);

            /* Same rule a modifier-drag resize follows: grabbing the
             * shared edge of two half-tiled windows resizes both in place
             * (link_resize_neighbors=) instead of detiling this one back
             * to whatever floating geometry it had before it was snapped.
             */
            begin_drag_at(gc, DRAG_RESIZE, ev->root_x, ev->root_y,
                          should_preserve_snap_resize(gc, ev->root_x),
                          right, bottom, axis_x, axis_y);
            return;
        }
    }

    Client *c = find_client_window(ev->event);
    if (!c)
        c = find_client_window(ev->child);
    if (!c)
        return;

    focus_client(c);

    int rel_x = ev->root_x - c->x;
    int rel_y = ev->root_y - c->y;
    bool on_titlebar = client_deco_visible(c) && rel_y >= 0 && rel_y < TITLEBAR_H;

    /* Scroll wheel over the titlebar (detail 4 = up, 5 = down) shades/
     * unshades -- X has no separate "scroll" event, wheel motion is just
     * ButtonPress with these detail values. */
    if (on_titlebar && (ev->detail == 4 || ev->detail == 5)) {
        toggle_shade(c, ev->detail == 4 ? 1 : 0);
        return;
    }

    /* A titlebar *button* takes left, right and middle clicks -- the
     * maximize button does something different with each (see
     * run_deco_button()) -- so this is checked before the right-click
     * window menu below, which owns every other part of the decoration.
     * Modifier-drags still win: those are the move/resize gestures. */
    bool plain_click = !(ev->state & wm.mod_key);
    if (on_titlebar && plain_click &&
        (ev->detail == XCB_BUTTON_INDEX_1 || ev->detail == XCB_BUTTON_INDEX_2 ||
         ev->detail == XCB_BUTTON_INDEX_3)) {
        DecoSlot slots[MAX_DECO_ELEMS];
        int idx = deco_slot_at(c, rel_x, slots, MAX_DECO_ELEMS);
        if (idx >= 0 && deco_kind_is_button(slots[idx].kind)) {
            /* Pressing only *arms* the button: the action fires on
             * release, and only if the release lands on the same button
             * (handle_button_release()), so a click that landed on the
             * wrong one can be taken back by dragging off it -- the way
             * buttons behave everywhere else. It's also what makes a
             * held-down state worth drawing at all; see btns.png's third
             * row (wm.h's BTNCOL_* comment). */
            wm.pressed_client = c;
            wm.pressed_btn = idx;
            wm.pressed_button = ev->detail;
            draw_decoration(c);
            xcb_flush(wm.conn);
            return;
        }
    }

    /* Right-click anywhere else on the decoration opens the window menu
     * (menu.c) -- unless a modifier is held, which is the resize gesture
     * below. Not just the titlebar: the border counts too, same as every
     * other WM. */
    if (client_deco_visible(c) && ev->detail == 3 && plain_click && ev->event == c->frame) {
        window_menu_open(c, ev->root_x, ev->root_y);
        return;
    }

    if (on_titlebar && ev->detail == 1) {
        DecoSlot slots[MAX_DECO_ELEMS];
        int idx = deco_slot_at(c, rel_x, slots, MAX_DECO_ELEMS);
        /* The window icon is the menu's other, older home: clicking it
         * opens the same menu, anchored just under the titlebar. A menu
         * opens on press on purpose -- it's the one titlebar element you
         * can press and drag straight into. */
        if (idx >= 0 && slots[idx].kind == DECO_ICON) {
            window_menu_open(c, c->x + slots[idx].x, c->y + TITLEBAR_H);
            return;
        }

        /* Plain titlebar area (icon/title, or no element under the
         * click): double-click
         * toggles maximize, same as most desktops -- X has no double-click
         * event of its own, so this compares consecutive ButtonPress
         * timestamps by hand (see wm.h's DOUBLE_CLICK_MS). */
        bool is_double = (c->frame == wm.last_titlebar_click_frame) &&
                         (xcb_timestamp_t)(ev->time - wm.last_titlebar_click_time) < DOUBLE_CLICK_MS;
        wm.last_titlebar_click_frame = c->frame;
        wm.last_titlebar_click_time = ev->time;
        if (is_double) {
            wm.last_titlebar_click_time = 0; /* consume -- don't chain into a triple-click */
            toggle_maximize(c, -1);
            return;
        }

        begin_drag(c, DRAG_MOVE, ev);
        return;
    }

    /* wm.mod_key-drag (Meta by default) moves the window with the left
     * button and resizes it with the right one, from whichever corner is
     * nearest the click -- see kiwm.conf's mod_key= key. */
    if (ev->state & wm.mod_key) {
        if (ev->detail == 1)
            begin_drag(c, DRAG_MOVE, ev);
        else if (ev->detail == 3)
            begin_drag(c, DRAG_RESIZE, ev);
        return;
    }

    if (rel_y >= (client_deco_visible(c) ? TITLEBAR_H : 0)) {
        xcb_allow_events(wm.conn, XCB_ALLOW_REPLAY_POINTER, ev->time);
        xcb_flush(wm.conn);
    }
}

/* Where a window would end up if it snapped to `side` on the output whose
 * workarea is (wx, wy, ww, wh) -- as a *frame* rect, which is what the
 * outline wants. Deliberately the plain geometry, without the min-size
 * clamps apply_drag_snap() applies: this is a preview of the intent, and a
 * window whose minimum size doesn't fit half a screen is rare enough that
 * a few pixels of difference between the outline and the final size is a
 * better trade than duplicating the clamping in two places. */
static void snap_target_rect(Client *c, SnapSide side, int wx, int wy, int ww, int wh,
                             int *out_x, int *out_y, int *out_w, int *out_h)
{
    (void)c;
    *out_y = wy;
    *out_h = wh;

    switch (side) {
    case SNAP_LEFT:
        *out_x = wx;
        *out_w = ww / 2;
        break;
    case SNAP_RIGHT:
        *out_x = wx + (ww - ww / 2);
        *out_w = ww / 2;
        break;
    case SNAP_TOP:
    default:
        *out_x = wx;
        *out_w = ww;
        break;
    }
}

/* Actually puts the window into (or back out of) a drag snap. Split out of
 * try_edge_snap() because it has two callers now: that function, when
 * live_snap_resize=1 applies snaps as the pointer crosses the edge zone,
 * and handle_button_release(), when the default preview mode applies the
 * snap the outline has been showing all along. */
/* The snap a client will actually accept. A window that says it cannot be
 * resized cannot be tiled to half a screen and cannot be maximized -- and
 * a drag to the top edge *is* a maximize, the same state under a different
 * gesture, so it has to answer to the same permission that the titlebar
 * button and Meta+Up already answer to (client.c's toggle_maximize and
 * snap_client_to_side both check it; this path used to set max_horz and
 * max_vert by hand and check nothing).
 *
 * Asked here rather than only where the snap is applied so that the
 * preview outline never offers a snap that release would refuse. */
static SnapSide snap_side_allowed(const Client *c, SnapSide side)
{
    if (side == SNAP_TOP && !c->allow_maximize)
        return SNAP_NONE;
    if ((side == SNAP_LEFT || side == SNAP_RIGHT) && !c->allow_resize)
        return SNAP_NONE;
    return side;
}

static void apply_drag_snap(Client *c, SnapSide side, int wx, int wy, int ww, int wh, int dx, int dy)
{
    side = snap_side_allowed(c, side);

    if (side != SNAP_NONE) {
        /* Remember the true pre-drag floating geometry as the maximize
         * "restore" target too, so a later plain un-maximize (titlebar
         * button, Meta+Up) after this drag restores to it correctly
         * instead of to wherever the window happened to be mid-drag. */
        c->saved_x = wm.drag_start_x;
        c->saved_y = wm.drag_start_y;
        c->saved_w = wm.drag_start_w;
        c->saved_h = wm.drag_start_h;
    }

    switch (side) {
    case SNAP_TOP: {
        unshade_now(c);
        c->snap_side = SNAP_NONE;
        c->max_horz = true;
        c->max_vert = true;
        int bt, th;
        bool deco = client_deco_visible(c);
        th = deco ? TITLEBAR_H : 0;
        bt = deco ? wm.border_thickness : 0;
        c->x = wx;
        c->y = wy;
        c->width = ww - bt * 2;
        c->height = wh - th - bt;
        if (c->width < c->min_w) c->width = c->min_w;
        if (c->height < c->min_h) c->height = c->min_h;
        break;
    }
    case SNAP_LEFT:
    case SNAP_RIGHT:
        snap_client_to_side(c, side);
        break;
    case SNAP_NONE:
    default:
        unsnap_client(c, wm.drag_start_x + dx, wm.drag_start_y + dy,
                      wm.drag_start_w, wm.drag_start_h);
        break;
    }

    configure_frame(c);
    ewmh_update_wm_state(c);
    ewmh_update_frame_extents(c);
    xcb_flush(wm.conn);
}

/* Windows7/kwin-style edge snap while dragging a window by its titlebar or
 * via mod_key-drag: the pointer getting within kiwm.conf's
 * snap_threshold= of an output workarea edge snaps the window there (top =
 * maximize, left/right = half-width); moving the pointer back out of that
 * zone before releasing the button cancels it again. Only engages/
 * disengages on a *change* of which edge (if any) the pointer is currently
 * within threshold of, so nothing jitters while the pointer sits still
 * inside the same edge zone.
 *
 * What "engages" means depends on kiwm.conf's live_snap_resize=. Off (the
 * default), it draws the destination as an outline and leaves the window
 * alone until the button is released. On, it resizes the window then and
 * there -- kiwm's original behavior -- and moving back out restores the
 * exact pre-drag geometry offset by however far the pointer has moved
 * since, so the window keeps following the cursor as if it had never been
 * snapped. Returns whether it fully handled this motion event (which only
 * the live path ever does; the preview path still wants the window moved
 * normally underneath the outline). */
static bool try_edge_snap(Client *c, xcb_motion_notify_event_t *ev, int dx, int dy)
{
    if (wm.snap_threshold <= 0)
        return false;

    int output_idx = output_index_for_point(ev->root_x, ev->root_y);
    if (output_idx < 0)
        output_idx = c->output >= 0 ? c->output : 0;

    int wx, wy, ww, wh;
    compute_output_workarea(output_idx, &wx, &wy, &ww, &wh);

    SnapSide want;
    if (ev->root_y - wy <= wm.snap_threshold)
        want = SNAP_TOP;
    else if (ev->root_x - wx <= wm.snap_threshold)
        want = SNAP_LEFT;
    else if (wx + ww - ev->root_x <= wm.snap_threshold)
        want = SNAP_RIGHT;
    else
        want = SNAP_NONE;

    want = snap_side_allowed(c, want);

    if (want == wm.drag_snap_side)
        return wm.live_snap_resize && want != SNAP_NONE; /* already settled into this state (or none) */

    wm.drag_snap_side = want;
    /* Snapping against another screen's edge moves the window to that
     * screen for good -- including which of its desktops the window now
     * belongs to, which is exactly what dragging it there means (see
     * client_reassign_output()). */
    client_reassign_output(c, output_idx);

    if (!wm.live_snap_resize) {
        if (want == SNAP_NONE) {
            outline_hide();
        } else {
            int x, y, w, h;
            snap_target_rect(c, want, wx, wy, ww, wh, &x, &y, &w, &h);
            outline_show(x, y, w, h);
        }
        return false; /* nothing applied -- the caller still moves the window */
    }

    apply_drag_snap(c, want, wx, wy, ww, wh, dx, dy);
    return true;
}

/* Tracks which titlebar button (if any) the pointer currently sits over,
 * for btns.png's hover row (see decoration.c's draw_button()) -- a no-op
 * whenever no button theme is loaded, so plain-fallback decoration
 * doesn't pay for tracking/repainting it never uses. */
static void update_button_hover(xcb_motion_notify_event_t *ev)
{
    if (!wm.deco_btns)
        return;

    Client *c = find_client_window(ev->event);
    int slot = -1;
    if (c && client_deco_visible(c)) {
        int rel_x = ev->root_x - c->x;
        int rel_y = ev->root_y - c->y;
        if (rel_y >= 0 && rel_y < TITLEBAR_H) {
            DecoSlot slots[MAX_DECO_ELEMS];
            int idx = deco_slot_at(c, rel_x, slots, MAX_DECO_ELEMS);
            if (idx >= 0 && slots[idx].kind != DECO_TITLE && slots[idx].kind != DECO_ICON)
                slot = idx;
        }
    }

    if (c == wm.hover_client && slot == wm.hover_btn)
        return;

    Client *old = wm.hover_client;
    wm.hover_client = (slot >= 0) ? c : NULL;
    wm.hover_btn = slot;

    if (old && old != wm.hover_client)
        draw_decoration(old);
    if (wm.hover_client)
        draw_decoration(wm.hover_client);
    xcb_flush(wm.conn);
}

/* Nudges *edge toward cand if they're within wm.magnet_threshold and cand is
 * the closest candidate seen so far (tracked via best_delta/best_abs,
 * which the caller seeds to {0, wm.magnet_threshold + 1} so "nothing found"
 * naturally means "no delta applied"). Shared by both axes in
 * magnet_snap(). */
static void magnet_consider(int edge, int cand, int *best_delta, int *best_abs)
{
    int d = cand - edge;
    int ad = d < 0 ? -d : d;
    if (ad <= wm.magnet_threshold && ad < *best_abs) {
        *best_abs = ad;
        *best_delta = d;
    }
}

/* Upper bound for gather_magnet_edges_x/y()'s output buffer: the output's
 * own 2 screen edges, plus 2 per dock and 2 per client (left+right or
 * top+bottom). */
#define MAGNET_MAX_EDGES (2 + MAX_DOCKS * 2 + MAX_CLIENTS * 2)

/* Collects every edge coordinate (on the X axis; gather_magnet_edges_y()
 * below is the Y-axis mirror) that a window whose current frame spans
 * [ry, ry+rh) on the Y axis could plausibly snap against on this output:
 * the output's own left/right screen edges (always eligible -- they span
 * the whole output, so no Y-overlap gating needed), every dock/panel/
 * taskbar-type window's left/right edges *but only on this same output*
 * (a panel on a different monitor is irrelevant to a window that isn't
 * there -- unlike plain Clients below, which count regardless of output,
 * a dock's edges only ever matter to windows actually sharing its
 * screen), and every other visible Client's left/right edges (any client,
 * *any* output -- two windows on neighboring monitors can still plausibly
 * be lined up against each other, e.g. dragged across the boundary). A
 * dock or client's edges only make it in when its Y span actually
 * overlaps [ry, ry+rh) -- otherwise sharing an X coordinate is
 * coincidence, not two things that could be touching side by side.
 * `skip` excludes the dragged client itself from the Client scan. Returns
 * the number of edges written to `out` (caller-sized, see
 * MAGNET_MAX_EDGES). */
static int gather_magnet_edges_x(Client *skip, int for_output, int ry, int rh, int *out)
{
    int n = 0;

    if (for_output >= 0 && for_output < wm.output_count) {
        XisOutput *o = &wm.outputs[for_output];
        out[n++] = o->x;
        out[n++] = o->x + o->width;
    }

    for (int i = 0; i < wm.dock_count; i++) {
        DockWindow *d = &wm.docks[i];
        if (d->output != for_output || d->width <= 0 || d->height <= 0)
            continue;
        if (ry + rh > d->y && ry < d->y + d->height) {
            out[n++] = d->x;
            out[n++] = d->x + d->width;
        }
    }

    for (Client *o2 = wm.clients; o2; o2 = o2->next) {
        if (o2 == skip || !o2->mapped || o2->minimized)
            continue;
        if (!o2->sticky && wm.outputs[o2->output].desktop != o2->desktop)
            continue;
        if (ry + rh > o2->y && ry < o2->y + o2->frame_height) {
            out[n++] = o2->x;
            out[n++] = o2->x + o2->frame_width;
        }
    }

    return n;
}

/* Y-axis mirror of gather_magnet_edges_x() -- see its comment. */
static int gather_magnet_edges_y(Client *skip, int for_output, int rx, int rw, int *out)
{
    int n = 0;

    if (for_output >= 0 && for_output < wm.output_count) {
        XisOutput *o = &wm.outputs[for_output];
        out[n++] = o->y;
        out[n++] = o->y + o->height;
    }

    for (int i = 0; i < wm.dock_count; i++) {
        DockWindow *d = &wm.docks[i];
        if (d->output != for_output || d->width <= 0 || d->height <= 0)
            continue;
        if (rx + rw > d->x && rx < d->x + d->width) {
            out[n++] = d->y;
            out[n++] = d->y + d->height;
        }
    }

    for (Client *o2 = wm.clients; o2; o2 = o2->next) {
        if (o2 == skip || !o2->mapped || o2->minimized)
            continue;
        if (!o2->sticky && wm.outputs[o2->output].desktop != o2->desktop)
            continue;
        if (rx + rw > o2->x && rx < o2->x + o2->frame_width) {
            out[n++] = o2->y;
            out[n++] = o2->y + o2->frame_height;
        }
    }

    return n;
}

/* Window-to-window and window-to-screen magnetic edge snapping while
 * *moving* a window by its titlebar/mod-drag (kiwm.conf's
 * magnet_threshold=, default 10px, 0 disables) -- distinct from
 * try_edge_snap() above: that's a screen-edge *tiling* snap (maximize/
 * half-width, a whole different geometry engaging Client::maximized/
 * snap_side); this just nudges x/y a few pixels so the dragged window's
 * frame ends up touching another one exactly, instead of stopping just
 * short or just past it. Each axis snaps independently to whichever
 * nearby edge (of either side -- moving a window can snap its left *or*
 * right edge against the same candidate) is closest. `fw`/`fh` are the
 * dragged client's current frame size (unchanged during a move, so the
 * caller's already-computed c->frame_width/frame_height are exactly
 * right). */
static void magnet_snap_move(Client *c, int *x, int *y, int fw, int fh)
{
    if (wm.magnet_threshold <= 0 || c->output < 0 || c->output >= wm.output_count)
        return;

    int my_left = *x, my_right = *x + fw;
    int my_top = *y, my_bottom = *y + fh;

    int edges[MAGNET_MAX_EDGES];
    int n, i;

    int best_dx = 0, best_dx_abs = wm.magnet_threshold + 1;
    n = gather_magnet_edges_x(c, c->output, my_top, fh, edges);
    for (i = 0; i < n; i++) {
        magnet_consider(my_left,  edges[i], &best_dx, &best_dx_abs);
        magnet_consider(my_right, edges[i], &best_dx, &best_dx_abs);
    }

    int best_dy = 0, best_dy_abs = wm.magnet_threshold + 1;
    n = gather_magnet_edges_y(c, c->output, my_left, fw, edges);
    for (i = 0; i < n; i++) {
        magnet_consider(my_top,    edges[i], &best_dy, &best_dy_abs);
        magnet_consider(my_bottom, edges[i], &best_dy, &best_dy_abs);
    }

    if (best_dx_abs <= wm.magnet_threshold)
        *x += best_dx;
    if (best_dy_abs <= wm.magnet_threshold)
        *y += best_dy;
}

/* Same idea as magnet_snap_move(), but for a *resize* drag: only the one
 * edge actually being dragged (wm.resize_right/resize_bottom -- the same
 * flags the rest of the resize math already uses) is eligible to snap; the
 * anchored opposite corner never moves, so testing it against candidates
 * the way magnet_snap_move() tests both sides of a moving window wouldn't
 * make sense here. Adjusts new_w/new_h (content size, pre-min-clamp) in
 * place -- since the anchored edge never moves, shifting just the dragged
 * edge by `delta` changes the content size by exactly `delta` too, however
 * much decoration inset separates content from frame. `bt`/`th` (from
 * client.c's deco_insets()) are only needed to locate the dragged edge's
 * *current* absolute frame position for comparison against candidates,
 * which are already expressed in frame coordinates the same way every
 * other Client/dock/screen edge is. */
static void magnet_snap_resize(Client *c, int *new_w, int *new_h, int bt, int th)
{
    if (wm.magnet_threshold <= 0 || c->output < 0 || c->output >= wm.output_count)
        return;

    int frame_w = *new_w + bt * 2;
    int frame_h = c->shaded ? th : (*new_h + th + bt);
    int frame_left = wm.resize_right ? c->x : (wm.drag_start_x + wm.drag_start_w + bt * 2 - frame_w);
    int frame_top  = wm.resize_bottom ? c->y : (wm.drag_start_y + wm.drag_start_h + th + bt - frame_h);

    int moving_x = wm.resize_right ? (frame_left + frame_w) : frame_left;
    int moving_y = wm.resize_bottom ? (frame_top + frame_h) : frame_top;

    int edges[MAGNET_MAX_EDGES];
    int n, i;

    int best_dx = 0, best_dx_abs = wm.magnet_threshold + 1;
    n = gather_magnet_edges_x(c, c->output, frame_top, frame_h, edges);
    for (i = 0; i < n; i++)
        magnet_consider(moving_x, edges[i], &best_dx, &best_dx_abs);

    int best_dy = 0, best_dy_abs = wm.magnet_threshold + 1;
    n = gather_magnet_edges_y(c, c->output, frame_left, frame_w, edges);
    for (i = 0; i < n; i++)
        magnet_consider(moving_y, edges[i], &best_dy, &best_dy_abs);

    if (best_dx_abs <= wm.magnet_threshold)
        *new_w += wm.resize_right ? best_dx : -best_dx;
    if (best_dy_abs <= wm.magnet_threshold)
        *new_h += wm.resize_bottom ? best_dy : -best_dy;
}

/* Recomputes every registered resize-neighbor's geometry (wm.resize_
 * neighbors_x/y) from the total displacement of the dragged client's own
 * moving edge since detect_resize_neighbors() captured it -- see wm.h's
 * ResizeNeighbor comment. Only touches each neighbor Client's own x/y/
 * width/height fields; the caller still has to push that to the X server
 * per neighbor the same way it does for the dragged client itself
 * (apply_frame_geometry(), then the throttled apply_rounded_shape()/
 * draw_decoration() pair). `bt`/`th` are the *dragged* client's own insets
 * (the caller already has them, from deco_insets(), for
 * magnet_snap_resize()) -- each neighbor's own insets are looked up
 * individually since a neighbor's decoration state needn't match the
 * dragged client's. */
/* Where every linked resize neighbor (kiwm.conf's link_resize_neighbors=,
 * see detect_resize_neighbors()) ends up for a given geometry of the
 * window being resized: each one keeps its far edge anchored and follows
 * the moving edge with its near one, so the two stay touching.
 *
 * Takes the driving window's geometry as parameters rather than reading
 * c->x/y/width/height, and either applies the result (`apply`) or reports
 * it (`out`/`out_n`, frame rects) -- because with live_resize=0 the same
 * answer is needed twice: to draw the neighbors' outlines during the drag,
 * from a geometry the window doesn't have yet, and to apply them for real
 * on release. One function so the preview can't drift from what actually
 * happens. `out` needs room for 2 * MAX_RESIZE_NEIGHBORS. */
static void resolve_resize_neighbors(Client *c, int cx, int cy, int cw, int ch,
                                     int bt, int th, bool apply,
                                     OutlineRect *out, int *out_n)
{
    (void)c;
    int n_out = 0;

    if (wm.resize_neighbors_x_count > 0) {
        int cur_x = wm.resize_right ? (cx + cw + bt * 2) : cx;
        int delta = cur_x - wm.resize_edge_x_start;

        for (int i = 0; i < wm.resize_neighbors_x_count; i++) {
            ResizeNeighbor *n = &wm.resize_neighbors_x[i];
            Client *nc = n->client;
            int nbt, nth;
            deco_insets(nc, &nbt, &nth);

            int new_w, new_x;
            if (wm.resize_right) {
                /* Neighbor's left edge (the touching one) follows our
                 * moving right edge; its right edge is the anchor and
                 * never moves. */
                int anchor_right = n->orig_x + n->orig_w + nbt * 2;
                int new_left = n->orig_x + delta;
                new_w = (anchor_right - new_left) - nbt * 2;
                if (new_w < nc->min_w) new_w = nc->min_w;
                new_x = anchor_right - (new_w + nbt * 2);
            } else {
                int anchor_left = n->orig_x;
                int new_right = n->orig_x + n->orig_w + nbt * 2 + delta;
                new_w = (new_right - anchor_left) - nbt * 2;
                if (new_w < nc->min_w) new_w = nc->min_w;
                new_x = anchor_left;
            }

            if (apply) {
                nc->x = new_x;
                nc->width = new_w;
            } else if (out) {
                out[n_out++] = (OutlineRect){ new_x, nc->y,
                                              new_w + nbt * 2, nc->height + nth + nbt };
            }
        }
    }

    if (wm.resize_neighbors_y_count > 0) {
        int cur_y = wm.resize_bottom ? (cy + ch + th + bt) : cy;
        int delta = cur_y - wm.resize_edge_y_start;

        for (int i = 0; i < wm.resize_neighbors_y_count; i++) {
            ResizeNeighbor *n = &wm.resize_neighbors_y[i];
            Client *nc = n->client;
            int nbt, nth;
            deco_insets(nc, &nbt, &nth);

            int new_h, new_y;
            if (wm.resize_bottom) {
                int anchor_bottom = n->orig_y + n->orig_h + nth + nbt;
                int new_top = n->orig_y + delta;
                new_h = (anchor_bottom - new_top) - nth - nbt;
                if (new_h < nc->min_h) new_h = nc->min_h;
                new_y = anchor_bottom - (new_h + nth + nbt);
            } else {
                int anchor_top = n->orig_y;
                int new_bottom = n->orig_y + n->orig_h + nth + nbt + delta;
                new_h = (new_bottom - anchor_top) - nth - nbt;
                if (new_h < nc->min_h) new_h = nc->min_h;
                new_y = anchor_top;
            }

            if (apply) {
                nc->y = new_y;
                nc->height = new_h;
            } else if (out) {
                out[n_out++] = (OutlineRect){ nc->x, new_y,
                                              nc->width + nbt * 2, new_h + nth + nbt };
            }
        }
    }

    if (out_n)
        *out_n = n_out;
}

static void update_resize_neighbors(Client *c, int bt, int th)
{
    resolve_resize_neighbors(c, c->x, c->y, c->width, c->height, bt, th, true, NULL, NULL);
}

static void handle_motion(xcb_motion_notify_event_t *ev)
{
    if (!wm.drag_client || wm.drag_mode == DRAG_NONE) {
        update_button_hover(ev);
        /* Nothing else to do: the resize cursor is now a plain attribute
         * of the grip window under the pointer (grip.h), so hovering an
         * edge needs no work here at all. */
        return;
    }

    Client *c = wm.drag_client;
    int dx = ev->root_x - wm.drag_start_root_x;
    int dy = ev->root_y - wm.drag_start_root_y;

    /* A fullscreen window doesn't follow the pointer at all -- only the
     * outline of whichever output would take it does, and the release
     * applies it (see KiWM::drag_fullscreen_move). */
    if (wm.drag_fullscreen_move) {
        int idx = output_index_for_point(ev->root_x, ev->root_y);
        if (idx >= 0 && idx < wm.output_count)
            outline_show(wm.outputs[idx].x, wm.outputs[idx].y,
                         wm.outputs[idx].width, wm.outputs[idx].height);
        return;
    }

    /* A move-drag on a maximized/tiled window hasn't detiled it yet: the
     * window stays exactly where it is until the pointer has travelled far
     * enough to mean it (see KiWM::drag_detile_pending), so clicking a
     * maximized titlebar -- or nudging it a couple of pixels while
     * mod-dragging -- doesn't restore the window out from under the click.
     * Once it does, the window is detiled under the cursor and the drag
     * re-anchors there, as if it had started at this point. */
    if (wm.drag_detile_pending) {
        if ((dx < 0 ? -dx : dx) < DRAG_DETILE_THRESHOLD &&
            (dy < 0 ? -dy : dy) < DRAG_DETILE_THRESHOLD)
            return;

        detile_for_drag(c, ev->root_x, ev->root_y);
        wm.drag_detile_pending = false;
        wm.drag_start_root_x = ev->root_x;
        wm.drag_start_root_y = ev->root_y;
        wm.drag_start_x = c->x;
        wm.drag_start_y = c->y;
        wm.drag_start_w = c->width;
        wm.drag_start_h = c->height;
        xcb_flush(wm.conn);
        return;
    }

    if (wm.drag_mode == DRAG_MOVE && try_edge_snap(c, ev, dx, dy))
        return; /* settled into a snapped state this motion event; nothing else to do */

    if (c->max_horz || c->max_vert)
        toggle_maximize(c, 0);
    /* A resize preserving a half-snap's state (wm.drag_preserve_snap, see
     * begin_drag()'s should_preserve_snap_resize()) must keep
     * Client::snap_side set for the whole drag -- clearing it here (like
     * every other drag does) is exactly the detile this feature exists to
     * skip. Only ever true for DRAG_RESIZE; a plain drag still clears it
     * every event same as before. */
    if (!wm.drag_preserve_snap)
        c->snap_side = SNAP_NONE;

    if (wm.drag_mode == DRAG_MOVE) {
        c->x = wm.drag_start_x + dx;
        c->y = wm.drag_start_y + dy;
        magnet_snap_move(c, &c->x, &c->y, c->frame_width, c->frame_height);
    } else {
        /* Resize from whichever corner was nearest the initial click
         * (wm.resize_right/resize_bottom, decided once in
         * handle_button_press's begin_drag()) -- the *opposite* corner
         * stays fixed: recompute x/y from the (possibly c->min_w/min_h-
         * clamped) new size so that fixed corner's absolute position
         * never drifts, kwin/compiz-style, instead of always anchoring
         * top-left and growing toward bottom-right regardless of which
         * corner was actually grabbed. */
        /* An edge grip only moves the edge it grabbed: the other axis
         * keeps the size the drag started with (see
         * KiWM::resize_axis_x/resize_axis_y). */
        int new_w = !wm.resize_axis_x ? wm.drag_start_w
                  : (wm.resize_right ? wm.drag_start_w + dx : wm.drag_start_w - dx);
        int new_h = !wm.resize_axis_y ? wm.drag_start_h
                  : (wm.resize_bottom ? wm.drag_start_h + dy : wm.drag_start_h - dy);

        int bt, th;
        deco_insets(c, &bt, &th);
        magnet_snap_resize(c, &new_w, &new_h, bt, th);

        if (new_w < c->min_w) new_w = c->min_w;
        if (new_h < c->min_h) new_h = c->min_h;

        int new_x = c->x, new_y = c->y;
        if (wm.resize_axis_x && !wm.resize_right)
            new_x = wm.drag_start_x + (wm.drag_start_w - new_w);
        if (wm.resize_axis_y && !wm.resize_bottom)
            new_y = wm.drag_start_y + (wm.drag_start_h - new_h);

        /* With live_resize=0 the window itself is left alone for the whole
         * drag and only an outline of where it's heading is drawn; the
         * real geometry lands once, on release (handle_button_release()).
         * Linked resize neighbors are previewed right along with it --
         * they're part of what releasing the button will do, so leaving
         * them out would make the preview a lie. */
        if (!wm.live_resize) {
            wm.resize_preview_active = true;
            wm.resize_preview_x = new_x;
            wm.resize_preview_y = new_y;
            wm.resize_preview_w = new_w;
            wm.resize_preview_h = new_h;

            OutlineRect rects[1 + 2 * MAX_RESIZE_NEIGHBORS];
            rects[0] = (OutlineRect){ new_x, new_y, new_w + bt * 2, new_h + th + bt };
            int n = 0;
            resolve_resize_neighbors(c, new_x, new_y, new_w, new_h, bt, th, false,
                                     &rects[1], &n);
            outline_show_rects(rects, n + 1);
            return;
        }

        c->width = new_w;
        c->height = new_h;
        c->x = new_x;
        c->y = new_y;

        update_resize_neighbors(c, bt, th);
    }

    /* One clock for this drag step, consulted twice below: whether the
     * geometry of a *resize* is applied, and whether the painted chrome
     * is repainted. Paced by the client's own output, so a 144 Hz screen
     * gets 144 steps a second and a 60 Hz one gets 60, independent of how
     * often the input device reports motion (DRAG_REDRAW_FALLBACK_MS
     * covers an output with no refresh rate to ask). */
    double interval_ms = DRAG_REDRAW_FALLBACK_MS;
    if (c->output >= 0 && c->output < wm.output_count && wm.outputs[c->output].refresh_hz > 0)
        interval_ms = 1000.0 / wm.outputs[c->output].refresh_hz;

    double now = monotonic_ms();
    bool due = (now - wm.last_drag_apply_ms >= interval_ms);

    /* Moving is cheap and stays uncapped; resizing is not, and does not.
     *
     * The two look alike from here -- both are apply_frame_geometry() --
     * but they cost the *rest of the system* completely different
     * amounts. A move sends the frame new x/y and nothing else has to
     * happen: no pixels change owner, no buffer is reallocated, and the
     * client is not even told until the drag settles. That is why it is
     * worth doing on every single motion event, and it is what makes
     * kiwm's move track the pointer as immediately as an uncomposited
     * kwin's instead of visibly stepping at the refresh rate.
     *
     * A resize sends the client a new width/height, and that makes the
     * server reallocate its backing pixmap and the client repaint itself
     * at the new size -- an entire frame's worth of drawing, in a process
     * kiwm never sees and whose cost never shows up in kiwm's own
     * profile. It lands on the GPU all the same. Measured on a 1200x800
     * client: 60 resizes a second cost ~24% GPU busy, 125 (the rate a
     * wireless mouse reports at) ~38%, 250 ~58% -- linear in the number
     * of configures, and an order of magnitude above anything the
     * decoration repaint below does.
     *
     * So a resize is applied once per frame. Nobody can see a size the
     * monitor never displayed, and the steps that get skipped were going
     * to be overwritten by the next one anyway. The model (c->width,
     * c->height, set above) is already current either way, and
     * finish_drag() applies it unconditionally when the drag ends, so a
     * skipped last step cannot leave a stale size on screen. */
    if (wm.drag_mode == DRAG_MOVE || due) {
        apply_frame_geometry(c);
        for (int i = 0; i < wm.resize_neighbors_x_count; i++)
            apply_frame_geometry(wm.resize_neighbors_x[i].client);
        for (int i = 0; i < wm.resize_neighbors_y_count; i++)
            apply_frame_geometry(wm.resize_neighbors_y[i].client);
    }

    /* The *painted* chrome -- rounded-corner XShape re-clip and the
     * off-screen decoration repaint (title, buttons, border) -- is capped
     * on the same clock, for the same reason it always was: there is no
     * point re-painting faster than the display can show it. Skipping it
     * here just defers catching the chrome up until the next due event,
     * or until handle_button_release()'s unconditional final apply if the
     * drag ends first. */
    if (!due) {
        xcb_flush(wm.conn);
        return;
    }
    wm.last_drag_apply_ms = now;

    shape_update_frame(c);
    draw_decoration(c);
    for (int i = 0; i < wm.resize_neighbors_x_count; i++) {
        shape_update_frame(wm.resize_neighbors_x[i].client);
        draw_decoration(wm.resize_neighbors_x[i].client);
    }
    for (int i = 0; i < wm.resize_neighbors_y_count; i++) {
        shape_update_frame(wm.resize_neighbors_y[i].client);
        draw_decoration(wm.resize_neighbors_y[i].client);
    }
    xcb_flush(wm.conn);
}

/* The end of a move/resize drag, whatever ends it: the ButtonRelease
 * itself, or the event loop noticing the button is no longer held when
 * that release never arrived (events_poll_stale_drag()). Takes the
 * pointer position rather than an event, since the poll has no event. */
static void finish_drag(int root_x, int root_y)
{
    if (!wm.drag_client)
        return;
    /* Whatever else this does below, the ring has been left behind at the
     * geometry the drag started from -- grip_sync() refuses to run for the
     * client being dragged (grip.h). Caught up at the end of the function,
     * once wm.drag_client is clear. */
    {
        /* The deferred (live_resize=0) resize lands here, once, from
         * wherever the outline had got to -- see handle_motion(). */
        if (wm.resize_preview_active) {
            Client *rc = wm.drag_client;
            rc->x = wm.resize_preview_x;
            rc->y = wm.resize_preview_y;
            rc->width = wm.resize_preview_w;
            rc->height = wm.resize_preview_h;
            /* ...and every neighbor the preview had been dragging along
             * with it, applied from the same geometry it was drawn from
             * (see resolve_resize_neighbors()). The configure_frame() pass
             * right below is what puts them on screen. */
            int bt, th;
            deco_insets(rc, &bt, &th);
            resolve_resize_neighbors(rc, rc->x, rc->y, rc->width, rc->height, bt, th,
                                     true, NULL, NULL);
            wm.resize_preview_active = false;
        }

        /* Force one final apply regardless of the redraw throttle above
         * -- otherwise the window could be left showing a stale size if
         * the very last motion event of the drag happened to land inside
         * the throttle window and got skipped. Same for every resize
         * neighbor that got dragged along (see update_resize_neighbors()). */
        configure_frame(wm.drag_client);
        for (int i = 0; i < wm.resize_neighbors_x_count; i++)
            configure_frame(wm.resize_neighbors_x[i].client);
        for (int i = 0; i < wm.resize_neighbors_y_count; i++)
            configure_frame(wm.resize_neighbors_y[i].client);
        wm.resize_neighbors_x_count = 0;
        wm.resize_neighbors_y_count = 0;

        Client *c = wm.drag_client;

        /* A fullscreen window carried to another output: re-apply
         * fullscreen there, and shift the geometry it would restore to by
         * the same amount, so leaving fullscreen later lands on the screen
         * it was moved to rather than back on the old one. */
        if (wm.drag_fullscreen_move) {
            int idx = output_index_for_point(root_x, root_y);
            if (idx >= 0 && idx < wm.output_count && idx != c->output) {
                int dx_out = wm.outputs[idx].x - wm.outputs[c->output].x;
                int dy_out = wm.outputs[idx].y - wm.outputs[c->output].y;
                c->fs_saved_x += dx_out;
                c->fs_saved_y += dy_out;
                c->saved_x += dx_out;
                c->saved_y += dy_out;

                client_reassign_output(c, idx);
                client_apply_fullscreen_geometry(c);
                configure_frame(c);
                restack_all();
            }
            outline_hide();
            xcb_ungrab_pointer(wm.conn, XCB_CURRENT_TIME);
            wm.drag_client = NULL;
            wm.drag_mode = DRAG_NONE;
            wm.drag_snap_side = SNAP_NONE;
            wm.drag_fullscreen_move = false;
            grip_sync(c);
            xcb_flush(wm.conn);
            return;
        }

        /* The default (live_snap_resize=0) preview path: the window has
         * been following the pointer all along with only an outline
         * showing where it was headed, so the snap itself happens now,
         * once, on release -- see try_edge_snap(). */
        if (!wm.live_snap_resize && wm.drag_mode == DRAG_MOVE && wm.drag_snap_side != SNAP_NONE) {
            int output_idx = c->output >= 0 ? c->output : 0;
            int wx, wy, ww, wh;
            compute_output_workarea(output_idx, &wx, &wy, &ww, &wh);
            apply_drag_snap(c, wm.drag_snap_side, wx, wy, ww, wh, 0, 0);
        }
        outline_hide();

        client_reassign_output(c, output_index_for_point(c->x + c->width / 2, c->y + c->height / 2));
        xcb_ungrab_pointer(wm.conn, XCB_CURRENT_TIME);
        wm.drag_client = NULL;
        wm.drag_mode = DRAG_NONE;
        wm.drag_snap_side = SNAP_NONE;
        wm.drag_detile_pending = false;
        wm.drag_fullscreen_move = false;
        /* wm.drag_client is clear, so this is the call that actually
         * moves the ring -- and the neighbours a linked resize dragged
         * along were never skipped, so only this one is owed. */
        grip_sync(c);
        xcb_flush(wm.conn);
    }
}

static void handle_button_release(xcb_button_release_event_t *ev)
{
    /* An armed titlebar button (see handle_button_press()) fires here, and
     * only if this release is still over the same button -- releasing
     * anywhere else cancels it and does nothing at all. */
    if (wm.pressed_client) {
        Client *c = wm.pressed_client;
        int armed = wm.pressed_btn;
        uint8_t armed_button = wm.pressed_button;
        wm.pressed_client = NULL;
        wm.pressed_btn = -1;
        wm.pressed_button = 0;

        /* Only the button that armed it can fire it: with three different
         * actions on the maximize button, a release from some *other*
         * button held at the same time must not stand in for it. */
        if (ev->detail != armed_button) {
            draw_decoration(c);
            xcb_flush(wm.conn);
            return;
        }

        int rel_x = ev->root_x - c->x;
        int rel_y = ev->root_y - c->y;
        DecoSlot slots[MAX_DECO_ELEMS];
        int idx = deco_slot_at(c, rel_x, slots, MAX_DECO_ELEMS);
        bool on_titlebar = client_deco_visible(c) && rel_y >= 0 && rel_y < TITLEBAR_H;

        /* Repaint out of the held-down look *before* running anything:
         * the action can destroy the frame this would be drawing on. */
        draw_decoration(c);

        if (on_titlebar && idx >= 0 && idx == armed)
            run_deco_button(c, slots[idx].kind, armed_button, slots[idx].x);
        xcb_flush(wm.conn);
        return;
    }

    finish_drag(ev->root_x, ev->root_y);
}


static void handle_property_notify(xcb_property_notify_event_t *ev)
{
    /* On the root: a compositor saying which part of each monitor is
     * really desktop (output.c's apply_confined_areas). Re-reading the
     * outputs is all it takes -- everything that lays windows out works
     * from wm.outputs[]. */
    if (ev->window == wm.root && ev->atom == wm.atoms.xis_confined_area) {
        fprintf(stderr, "kiwm: _XIS_CONFINED_AREA changed\n");
        outputs_refresh();
        xcb_flush(wm.conn);
        return;
    }

    /* A wallpaper layer saying which desktop it is for. It is not a
     * client and never will be, but which desktop it belongs to is still
     * kiwm's to act on (output.h's desktop_layer_refresh). */
    if (ev->atom == wm.atoms.net_wm_desktop &&
        desktop_layer_refresh(ev->window))
        return;

    /* On a *frame*, not on a client window: a compositor asking this
     * window's decoration to be redrawn densely (density.h). Checked
     * first because frames and clients are different windows and this is
     * the only property kiwm listens for on its own. */
    if (ev->atom == wm.atoms.x_density_requested) {
        Client *fc = find_client_window(ev->window);
        if (fc && fc->frame == ev->window) {
            deco_density_request_changed(fc);
            return;
        }
    }

    Client *c = find_client_window(ev->window);
    if (!c) {
        if ((ev->atom == wm.atoms.net_wm_strut || ev->atom == wm.atoms.net_wm_strut_partial) &&
            dock_refresh_strut(ev->window))
            xcb_flush(wm.conn);
        return;
    }

    if (ev->atom == wm.atoms.net_wm_name || ev->atom == XCB_ATOM_WM_NAME) {
        get_title(c);
        draw_decoration(c);
        xcb_flush(wm.conn);
    }
    if (ev->atom == wm.atoms.net_wm_icon) {
        load_client_icon(c);
        draw_decoration(c);
        xcb_flush(wm.conn);
    }
    if (ev->atom == XCB_ATOM_WM_TRANSIENT_FOR)
        client_refresh_transient_for(c);
    /* Qt/KF5 apps set these a moment *after* mapping, so the appmenu
     * button appears once the menu really exists rather than never. */
    if (ev->atom == wm.atoms.kde_net_wm_appmenu_service_name ||
        ev->atom == wm.atoms.kde_net_wm_appmenu_object_path) {
        bool had = c->has_appmenu;
        client_refresh_appmenu(c);
        if (had != c->has_appmenu) {
            draw_decoration(c);
            xcb_flush(wm.conn);
        }
    }
    if (ev->atom == XCB_ATOM_WM_NORMAL_HINTS) {
        /* Some toolkits only set WM_NORMAL_HINTS after the initial map, so
         * a min-size hint that wasn't there yet in manage() can show up
         * later -- re-read it and clamp the current geometry up to match
         * if it's now too small (e.g. a terminal growing its min size once
         * a font/PTY dimension becomes known). */
        get_size_hints(c);
        /* Same hints decide which titlebar buttons exist at all -- a
         * toolkit that fixes its window size after mapping has just
         * withdrawn the maximize button. */
        update_client_actions(c);
        draw_decoration(c);
        bool grew = false;
        if (c->width < c->min_w)  { c->width = c->min_w;  grew = true; }
        if (c->height < c->min_h) { c->height = c->min_h; grew = true; }
        if (grew) {
            configure_frame(c);
            xcb_flush(wm.conn);
        }
    }
}

static void handle_enter_notify(xcb_enter_notify_event_t *ev)
{
    if (!wm.focus_follows_mouse)
        return;
    Client *c = find_client_window(ev->event);
    if (!c)
        c = find_client_window(ev->child);
    if (c && c->mapped && !c->minimized && (c->sticky || wm.outputs[c->output].desktop == c->desktop))
        focus_client(c);
}

static void handle_key_press(xcb_key_press_event_t *ev)
{
    /* An open window menu (menu.c) has the keyboard: arrows/Enter/Escape
     * drive it, and nothing else fires while it's up. */
    if (window_menu_handle_key_press(ev))
        return;

    /* Escape while either OSD (osd.c) is open cancels it without switching --
     * only meaningful during the active xcb_grab_keyboard() osd.c holds, but
     * osd_cancel() is a no-op otherwise so this is safe unconditionally.
     * Checked before the shortcut table so a hold can always be escaped
     * even if Escape happens to be bound to something as well. */
    if (ev->detail == wm.key_escape && osd_active()) {
        osd_cancel();
        return;
    }

    /* Everything else: kiwm.conf's key_* shortcuts (keybind.c), which is
     * also the only thing that grabbed any key on the root window in the
     * first place -- window/desktop switching, maximize, minimize,
     * half-screen tiling, direct desktop jumps, and whatever else the
     * user has bound. */
    keybind_handle_key_press(ev);
}

static void handle_net_wm_state(Client *c, uint32_t action, xcb_atom_t a1, xcb_atom_t a2)
{
    bool is_max_v = (a1 == wm.atoms.net_wm_state_maximized_vert ||
                     a2 == wm.atoms.net_wm_state_maximized_vert);
    bool is_max_h = (a1 == wm.atoms.net_wm_state_maximized_horz ||
                     a2 == wm.atoms.net_wm_state_maximized_horz);
    bool is_hidden = (a1 == wm.atoms.net_wm_state_hidden || a2 == wm.atoms.net_wm_state_hidden);
    bool is_shaded = (a1 == wm.atoms.net_wm_state_shaded || a2 == wm.atoms.net_wm_state_shaded);
    bool is_above = (a1 == wm.atoms.net_wm_state_above || a2 == wm.atoms.net_wm_state_above);
    bool is_sticky = (a1 == wm.atoms.net_wm_state_sticky || a2 == wm.atoms.net_wm_state_sticky);
    bool is_fullscreen = (a1 == wm.atoms.net_wm_state_fullscreen || a2 == wm.atoms.net_wm_state_fullscreen);
    bool is_below = (a1 == wm.atoms.net_wm_state_below || a2 == wm.atoms.net_wm_state_below);
    bool is_demands = (a1 == wm.atoms.net_wm_state_demands_attention ||
                       a2 == wm.atoms.net_wm_state_demands_attention);

    /* action: 0=remove, 1=add, 2=toggle (_NET_WM_STATE_TOGGLE) */
    if (is_max_v || is_max_h) {
        /* Each axis named in the message moves on its own, and one that
         * isn't named is left exactly as it is -- a client asking only for
         * _NET_WM_STATE_MAXIMIZED_HORZ means only that. Both named at once
         * (the usual "maximize this window" message) works out as the
         * ordinary full maximize. */
        bool want_h = c->max_horz;
        bool want_v = c->max_vert;
        if (is_max_h) want_h = (action == 2) ? !c->max_horz : (action == 1);
        if (is_max_v) want_v = (action == 2) ? !c->max_vert : (action == 1);
        client_set_maximized(c, want_h, want_v);
    }
    if (is_demands) {
        /* A client can raise this itself (a chat window with a new
         * message) and clear it again; kiwm also sets it when it refuses
         * a focus request, and clears it when the window is really
         * focused. Same state either way, so one path handles both. */
        bool want = (action == 2) ? !c->demands_attention : (action == 1);
        if (want != c->demands_attention) {
            c->demands_attention = want;
            ewmh_update_wm_state(c);
        }
    }
    if (is_hidden) {
        bool want_hidden = (action == 2) ? !c->minimized : (action == 1);
        if (want_hidden)
            minimize_client(c);
        else
            restore_client(c);
    }
    if (is_shaded) {
        int want = (action == 2) ? -1 : (action == 1 ? 1 : 0);
        toggle_shade(c, want);
    }
    if (is_above) {
        int want = (action == 2) ? -1 : (action == 1 ? 1 : 0);
        toggle_keep_above(c, want);
    }
    if (is_sticky) {
        int want = (action == 2) ? -1 : (action == 1 ? 1 : 0);
        toggle_sticky(c, want);
    }
    if (is_fullscreen) {
        int want = (action == 2) ? -1 : (action == 1 ? 1 : 0);
        toggle_fullscreen(c, want);
    }
    if (is_below) {
        int want = (action == 2) ? -1 : (action == 1 ? 1 : 0);
        toggle_keep_below(c, want);
    }
}

/* _NET_WM_MOVERESIZE (EWMH): the *client* asking the WM to take over a
 * move or resize it has decided the user started -- which is how a window
 * gets dragged by empty space inside it, with no titlebar involved.
 * Qt's Breeze/Oxygen styles send this from blank areas of toolbars and
 * dialogs, GTK headerbar apps from the headerbar, and undecorated windows
 * that draw their own chrome (Steam's client) from wherever they consider
 * draggable. Without it those drags simply do nothing under kiwm, since
 * the app is deliberately not moving its own window -- it's waiting for
 * the WM to.
 *
 * kiwm resizes from a corner only, so the four *edge* directions fall back
 * to the nearest-corner rule a plain drag uses on the axis they don't
 * name. The keyboard variants are treated as their pointer equivalents:
 * kiwm has no keyboard move/resize mode of its own to hand them to. */
static void handle_moveresize(Client *c, int root_x, int root_y, uint32_t direction)
{
    enum {
        MR_SIZE_TOPLEFT = 0, MR_SIZE_TOP, MR_SIZE_TOPRIGHT, MR_SIZE_RIGHT,
        MR_SIZE_BOTTOMRIGHT, MR_SIZE_BOTTOM, MR_SIZE_BOTTOMLEFT, MR_SIZE_LEFT,
        MR_MOVE, MR_SIZE_KEYBOARD, MR_MOVE_KEYBOARD, MR_CANCEL,
    };

    if (direction == MR_CANCEL) {
        if (wm.drag_client == c) {
            xcb_ungrab_pointer(wm.conn, XCB_CURRENT_TIME);
            wm.drag_client = NULL;
            wm.drag_mode = DRAG_NONE;
            wm.drag_snap_side = SNAP_NONE;
            wm.drag_detile_pending = false;
            wm.resize_preview_active = false;
            outline_hide();
            xcb_flush(wm.conn);
        }
        return;
    }

    if (direction == MR_MOVE || direction == MR_MOVE_KEYBOARD) {
        begin_drag_at(c, DRAG_MOVE, root_x, root_y, false, -1, -1, true, true);
        return;
    }

    int right = -1, bottom = -1;   /* -1 = derive from the pointer position */
    bool axis_x = true, axis_y = true;
    switch (direction) {
    case MR_SIZE_TOPLEFT:     right = 0; bottom = 0; break;
    case MR_SIZE_TOP:                    bottom = 0; axis_x = false; break;
    case MR_SIZE_TOPRIGHT:    right = 1; bottom = 0; break;
    case MR_SIZE_RIGHT:       right = 1;             axis_y = false; break;
    case MR_SIZE_BOTTOMRIGHT: right = 1; bottom = 1; break;
    case MR_SIZE_BOTTOM:                 bottom = 1; axis_x = false; break;
    case MR_SIZE_BOTTOMLEFT:  right = 0; bottom = 1; break;
    case MR_SIZE_LEFT:        right = 0;             axis_y = false; break;
    case MR_SIZE_KEYBOARD:    break;
    default:                  return;
    }
    begin_drag_at(c, DRAG_RESIZE, root_x, root_y, false, right, bottom, axis_x, axis_y);
}

static void handle_client_message(xcb_client_message_event_t *ev)
{
    if (ev->type == wm.atoms.kiwm_set_output_desktop) {
        switch_workspace((int)ev->data.data32[0], (int)ev->data.data32[1]);
        return;
    }

    if (ev->type == wm.atoms.kiwm_prime_desktop_layers) {
        desktop_layers_prime();
        return;
    }

    if (ev->type == wm.atoms.kiwm_hold_window) {
        client_hold((xcb_window_t)ev->data.data32[0], (int)ev->data.data32[1]);
        return;
    }

    Client *c = find_client_window(ev->window);
    if (!c)
        return;

    if (ev->type == wm.atoms.net_active_window) {
        /* data32[0] is EWMH's source indication: 2 means a pager or
         * taskbar acting on something the user clicked, anything else is
         * the application asking on its own behalf. */
        activate_client_requested(c, ev->data.data32[0] == 2);
    } else if (ev->type == wm.atoms.net_close_window) {
        close_client(c);
    } else if (ev->type == wm.atoms.wm_change_state) {
        if (ev->data.data32[0] == WM_STATE_ICONIC)
            minimize_client(c);
    } else if (ev->type == wm.atoms.net_wm_state) {
        handle_net_wm_state(c, ev->data.data32[0],
                            (xcb_atom_t)ev->data.data32[1], (xcb_atom_t)ev->data.data32[2]);
    } else if (ev->type == wm.atoms.net_wm_desktop) {
        set_client_desktop(c, (int)ev->data.data32[0]);
    } else if (ev->type == wm.atoms.net_wm_moveresize) {
        handle_moveresize(c, (int)ev->data.data32[0], (int)ev->data.data32[1],
                          ev->data.data32[2]);
    }
}

/* A drag whose ButtonRelease never arrived. begin_drag_at() refuses to
 * start a drag without a pointer grab, which is the main way this used to
 * happen, but a grab can also be *broken* afterwards -- another client
 * grabbing the devices, the server resetting them -- and then the release
 * goes somewhere else and the drag would never end. main.c's event loop
 * calls this on a timer while a drag is in flight: if no mouse button is
 * held any more, the drag is over, wherever the pointer happens to be.
 * The same check clears a titlebar button left armed by a press whose
 * release went missing. */
void events_poll_stale_drag(void)
{
    if (!wm.drag_client && !wm.pressed_client)
        return;

    xcb_query_pointer_reply_t *qp =
        xcb_query_pointer_reply(wm.conn, xcb_query_pointer(wm.conn, wm.root), NULL);
    if (!qp)
        return;

    bool any_button = (qp->mask & (XCB_BUTTON_MASK_1 | XCB_BUTTON_MASK_2 | XCB_BUTTON_MASK_3 |
                                   XCB_BUTTON_MASK_4 | XCB_BUTTON_MASK_5)) != 0;
    int root_x = qp->root_x, root_y = qp->root_y;
    free(qp);

    if (any_button)
        return;

    if (wm.pressed_client) {
        Client *c = wm.pressed_client;
        wm.pressed_client = NULL;
        wm.pressed_btn = -1;
        wm.pressed_button = 0;
        draw_decoration(c);
        xcb_flush(wm.conn);
    }
    finish_drag(root_x, root_y);
}

/* The newest server timestamp kiwm has been handed -- see wm.h's
 * KiWM::last_event_time. Only these event types carry one, and only the
 * ones kiwm actually selects for can turn up here. */
static void note_event_time(uint8_t type, xcb_generic_event_t *event)
{
    xcb_timestamp_t t;

    switch (type) {
    case XCB_KEY_PRESS:
    case XCB_KEY_RELEASE:      t = ((xcb_key_press_event_t *)event)->time; break;
    case XCB_BUTTON_PRESS:
    case XCB_BUTTON_RELEASE:   t = ((xcb_button_press_event_t *)event)->time; break;
    case XCB_MOTION_NOTIFY:    t = ((xcb_motion_notify_event_t *)event)->time; break;
    case XCB_ENTER_NOTIFY:
    case XCB_LEAVE_NOTIFY:     t = ((xcb_enter_notify_event_t *)event)->time; break;
    case XCB_PROPERTY_NOTIFY:  t = ((xcb_property_notify_event_t *)event)->time; break;
    default:                   return;
    }

    if (t != XCB_CURRENT_TIME)
        wm.last_event_time = t;
}

void handle_event(xcb_generic_event_t *event)
{
    uint8_t type = event->response_type & ~0x80;

    note_event_time(type, event);

    if (wm.randr_event_base && type == wm.randr_event_base + XCB_RANDR_SCREEN_CHANGE_NOTIFY) {
        outputs_refresh();
        return;
    }

    /* SHAPE's ShapeNotify: a client changed its own non-rectangular shape,
     * so the frame's has to follow (see shape.c). Like RandR's above, its
     * event number is assigned at runtime and can't be a case label. */
    if (shape_is_notify_event(type)) {
        shape_handle_notify(event);
        return;
    }

    /* A compositor arriving (MANAGER) or leaving (its window destroyed):
     * noted here, acted on by main.c's loop once this batch of events is
     * drained. Not consumed for DestroyNotify -- nothing else claims the
     * compositor's window, but the switch below is harmless on it. */
    compositor_watch_event(event);

    switch (type) {
    case XCB_MAP_REQUEST:
        handle_map_request((xcb_map_request_event_t *)event);
        break;
    case XCB_MAP_NOTIFY:
        handle_map_notify((xcb_map_notify_event_t *)event);
        break;
    case XCB_CONFIGURE_REQUEST:
        handle_configure_request((xcb_configure_request_event_t *)event);
        break;
    case XCB_DESTROY_NOTIFY: {
        xcb_destroy_notify_event_t *ev = (xcb_destroy_notify_event_t *)event;
        Client *c = find_client_window(ev->window);
        if (c) {
            unmanage(c);
        } else {
            popup_focus_released(ev->window);
            dock_forget(ev->window);
            desktop_layer_forget(ev->window);
            /* Avoid chaining the next _NET_WM_WINDOW_TYPE_DESKTOP window
             * (see client.c's manage()) above a now-destroyed sibling --
             * that ConfigureWindow would just fail with BadWindow and
             * leave the new window unstacked (back to the original bug). */
            if (wm.last_desktop_window == ev->window)
                wm.last_desktop_window = XCB_NONE;
        }
        break;
    }
    case XCB_UNMAP_NOTIFY: {
        xcb_unmap_notify_event_t *ev = (xcb_unmap_notify_event_t *)event;
        Client *c = find_client_window(ev->window);
        if (!c)
            popup_focus_released(ev->window);
        /* The frame going away takes its resize ring with it, whatever
         * unmapped it -- the counterpart of handle_map_notify()'s call,
         * and for the same reason (grip.h). */
        if (c && ev->window == c->frame)
            grip_frame_mapped(c, false);
        if (c && ev->window == c->window) {
            if (c->ignore_unmap > 0) {
                /* Spurious auto-unmap from reparenting an already-mapped
                 * pre-existing window at startup -- not a real withdrawal. */
                c->ignore_unmap--;
                break;
            }
            if (c->shaded) {
                /* Our own toggle_shade() unmapping the content window on
                 * purpose (c->shaded is already true by the time this
                 * event arrives, since toggle_shade sets it before
                 * unmapping) -- not a real withdrawal, must NOT also
                 * unmap the frame or mark the client unmapped, or the
                 * whole window (decoration included) vanishes and stays
                 * that way until something unrelated (e.g. a taskbar
                 * minimize+restore) happens to remap the frame again. */
                break;
            }
            /* Anything else is the client unmapping its own window, which
             * ICCCM defines as *withdrawing* it -- the window stops being
             * managed, full stop. kiwm used to just hide the frame and
             * keep the Client around, which left the window listed in
             * _NET_WM_CLIENT_LIST: every OpenSnitch prompt ever answered
             * stayed in the taskbar forever. (kiwm's own ways of hiding a
             * window never reach here: minimizing and switching desktops
             * unmap the *frame*, which leaves the client window mapped --
             * just not viewable -- and generates no UnmapNotify for it,
             * and shading is handled above.) Showing the window again then
             * goes through the normal MapRequest path as a brand-new
             * window, which is also what makes it come back focused. */
            unmanage(c);
            xcb_flush(wm.conn);
        }
        break;
    }
    case XCB_BUTTON_PRESS:
        /* A click while a switcher overlay is up ends the hold first (and
         * gets replayed from there) -- see osd_handle_button_press(). */
        if (osd_handle_button_press((xcb_button_press_event_t *)event))
            break;
        /* An open window menu owns the pointer entirely: it either picks a
         * row or dismisses itself (menu.c). */
        if (window_menu_handle_button_press((xcb_button_press_event_t *)event))
            break;
        handle_button_press((xcb_button_press_event_t *)event);
        break;
    case XCB_BUTTON_RELEASE:
        handle_button_release((xcb_button_release_event_t *)event);
        break;
    case XCB_MOTION_NOTIFY:
        if (window_menu_handle_motion((xcb_motion_notify_event_t *)event))
            break;
        handle_motion((xcb_motion_notify_event_t *)event);
        break;
    case XCB_PROPERTY_NOTIFY:
        handle_property_notify((xcb_property_notify_event_t *)event);
        break;
    case XCB_ENTER_NOTIFY:
        handle_enter_notify((xcb_enter_notify_event_t *)event);
        break;
    case XCB_LEAVE_NOTIFY: {
        /* Motion stops firing once the pointer leaves the frame entirely,
         * so button hover state (see update_button_hover()) needs its own
         * clear here or it'd stay stuck highlighted after the pointer
         * moves away. */
        xcb_leave_notify_event_t *ev = (xcb_leave_notify_event_t *)event;
        if (wm.hover_client && ev->event == wm.hover_client->frame) {
            Client *old = wm.hover_client;
            wm.hover_client = NULL;
            wm.hover_btn = -1;
            draw_decoration(old);
            xcb_flush(wm.conn);
        }
        break;
    }
    case XCB_KEY_PRESS:
        handle_key_press((xcb_key_press_event_t *)event);
        break;
    case XCB_KEY_RELEASE:
        /* Only ever meaningful while osd.c holds its active
         * xcb_grab_keyboard() (see osd_windows_step()/osd_desktops_step()) --
         * a no-op otherwise, but delivered unconditionally since that's the
         * only way a bare modifier-key release (not tied to any specific
         * xcb_grab_key()) ever reaches kiwm at all. */
        osd_handle_key_release((xcb_key_release_event_t *)event);
        break;
    case XCB_CLIENT_MESSAGE:
        handle_client_message((xcb_client_message_event_t *)event);
        break;
    case XCB_SELECTION_CLEAR: {
        xcb_selection_clear_event_t *ev = (xcb_selection_clear_event_t *)event;
        if (ev->owner == wm.sel_win && ev->selection == wm.sn_atom) {
            /* Replaced by --replace: let go of SubstructureRedirect right
             * away so the new WM's acquire_wm_selection() probe succeeds,
             * then unwind normally through cleanup(). */
            fprintf(stderr, "kiwm: replaced by another window manager, exiting\n");
            uint32_t mask = XCB_EVENT_MASK_NO_EVENT;
            xcb_change_window_attributes(wm.conn, wm.root, XCB_CW_EVENT_MASK, &mask);
            xcb_flush(wm.conn);
            wm.running = false;
        }
        break;
    }
    case XCB_EXPOSE: {
        /* A single repaint-worthy change (e.g. one resize step) can be
         * reported as *several* Expose events, one per exposed
         * rectangle -- ev->count is how many more are still queued for
         * this same batch. kiwm always repaints the whole titlebar in
         * one go regardless of which rectangle triggered it, so there's
         * nothing to gain from repainting on every fragment; only the
         * last one in the batch (count == 0) actually needs to redraw.
         * Skipping this was making draw_decoration() fire many times per
         * single configure_frame() call during a resize -- confirmed via
         * KIWM_DEBUG_RESIZE instrumentation. */
        xcb_expose_event_t *ev = (xcb_expose_event_t *)event;
        if (ev->count != 0)
            break;
        osd_handle_expose(ev->window);
        window_menu_handle_expose(ev->window);
        outline_handle_expose(ev->window);
        Client *c = find_client_window(ev->window);
        if (c) {
            draw_decoration(c);
            xcb_flush(wm.conn);
        }
        break;
    }
    default:
        break;
    }
}
