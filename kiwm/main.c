/*
 * kiwm - XiS Window Manager
 *
 * Built along kiwm-kicomp-projeto.md's phased plan; what it does today
 * covers Fase 1 and most of Fase 2/3/4, and is what the desktop is
 * actually run on.
 *
 * Scope:
 *   - XCB connection, root ownership, --replace (ICCCM manager selection).
 *   - RandR outputs (xcb_randr_get_monitors), hotplug-aware.
 *   - Virtual desktops tracked independently per output (not a single
 *     global workspace number): _NET_CURRENT_DESKTOP/_NET_NUMBER_OF_DESKTOPS
 *     mirror the *primary* output only, for compatibility with plain
 *     pagers/taskbars. Real per-output state lives in custom root
 *     properties: _KIWM_OUTPUTS, _KIWM_OUTPUT_DESKTOP, _KIWM_NUM_OUTPUT_DESKTOPS.
 *   - map / unmap / move / resize / maximize / minimize / raise+focus.
 *   - Already-open windows are managed at startup too, not just windows
 *     mapped afterward (see client.c's manage_existing_windows).
 *   - Cairo + Imlib2 decoration, hardcoded to greenxp/bg.png (like
 *     xispanel's theme background loader), cached at startup; plain
 *     black/white fallback when no theme PNG is found.
 *   - Configurable global keybindings (kiwm.conf's key_*, see keybind.c):
 *     Alt+Tab/Alt+Shift+Tab cycle focus, Meta+Tab/Meta+Shift+Tab cycle the
 *     focused output's virtual desktop, Meta+Up maximizes/restores,
 *     Meta+Down minimizes, Meta+Left/Right tile to half the screen, by
 *     default. Mouse gestures follow kiwm.conf's mod_key= directly (Meta
 *     by default): mod+drag anywhere on a window moves it, mod+right-drag
 *     resizes it.
 *   - Enough EWMH/ICCCM for a taskbar (xispanel's tasklist widget) to
 *     list/activate/close/minimize/maximize windows.
 *
 * See wm.h for the shared types/state and each module's own header for
 * its slice of the WM: atoms.c (EWMH/ICCCM atom interning), output.c
 * (RandR outputs + per-output virtual desktops), decoration.c (Cairo/
 * Imlib2 frame painting), ewmh.c (EWMH property bookkeeping on clients),
 * client.c (manage/unmanage, focus, move/resize/maximize/minimize),
 * events.c (X event dispatch), keybind.c (configurable global keyboard
 * shortcuts), osd.c (window/desktop switcher overlays), selection.c
 * (--replace).
 */

#define _POSIX_C_SOURCE 200809L

#define KIWM_VERSION "0.4.13"

#include "wm.h"
#include "config.h"
#include "atoms.h"
#include "output.h"
#include "decoration.h"
#include "ewmh.h"
#include "client.h"
#include "events.h"
#include "keybind.h"
#include "osd.h"
#include "selection.h"
#include "shape.h"

#include <xcb/randr.h>
#include <xcb/shape.h>
#include <xcb/xcb_cursor.h>
#include <cairo/cairo.h>

#include <X11/keysym.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>
#include <time.h>

KiWM wm;

/* Self-pipe for SIGTERM/SIGINT: a plain signal handler can't safely touch
 * wm.running and have the main loop notice promptly, since that loop
 * blocks in poll() waiting on the X connection's fd, not on a flag. The
 * handler only does the one thing safe in async-signal context -- write a
 * byte -- and poll() wakes up on it like any other fd. Without this,
 * killing kiwm hits the default SIGTERM disposition (immediate process
 * death, no cleanup()), which orphans every reparented client window: the
 * X server destroys kiwm's own frame windows when its connection drops,
 * and destroying a window recursively destroys its still-reparented
 * children too. See cleanup()'s doc comment. */
static int g_sigpipe[2] = { -1, -1 };

/* KIWM_DEBUG_RESIZE=1 (wm.debug_resize, checked once at startup): logs,
 * per MotionNotify actually processed (i.e. after coalescing -- see the
 * event loop below), how many consecutive queued MotionNotify events
 * were dropped in favor of this one, and how long handle_event() itself
 * took. events.c's handle_motion() further breaks that down into
 * configure_window/draw_decoration/apply_rounded_shape/flush when this
 * is on, for chasing exactly *where* resize/move responsiveness goes.
 * Purely a diagnostic knob, no cost when off. */
double monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static void handle_term_signal(int sig)
{
    (void)sig;
    char b = 1;
    ssize_t ignored = write(g_sigpipe[1], &b, 1);
    (void)ignored;
}

static void die(const char *msg)
{
    fprintf(stderr, "kiwm: %s\n", msg);
    exit(EXIT_FAILURE);
}

static xcb_visualtype_t *find_root_visual(xcb_screen_t *screen)
{
    xcb_depth_iterator_t depth_iter = xcb_screen_allowed_depths_iterator(screen);

    for (; depth_iter.rem; xcb_depth_next(&depth_iter)) {
        xcb_visualtype_iterator_t visual_iter =
            xcb_depth_visuals_iterator(depth_iter.data);

        for (; visual_iter.rem; xcb_visualtype_next(&visual_iter))
            if (visual_iter.data->visual_id == screen->root_visual)
                return visual_iter.data;
    }
    return NULL;
}

/* The screen's 32-bit TrueColor visual, if it has one. Its existence is
 * what lets an ARGB client keep its alpha channel through kiwm's frame
 * (see wm.h's argb_visual and client.c's manage()); a screen without one
 * just gets root-depth frames everywhere, exactly as before. */
static xcb_visualtype_t *find_argb_visual(xcb_screen_t *screen)
{
    xcb_depth_iterator_t depth_iter = xcb_screen_allowed_depths_iterator(screen);

    for (; depth_iter.rem; xcb_depth_next(&depth_iter)) {
        if (depth_iter.data->depth != 32)
            continue;

        xcb_visualtype_iterator_t visual_iter =
            xcb_depth_visuals_iterator(depth_iter.data);

        for (; visual_iter.rem; xcb_visualtype_next(&visual_iter))
            if (visual_iter.data->_class == XCB_VISUAL_CLASS_TRUE_COLOR)
                return visual_iter.data;
    }
    return NULL;
}

static xcb_keycode_t keysym_to_keycode(xcb_keysym_t keysym)
{
    const xcb_setup_t *setup = xcb_get_setup(wm.conn);
    xcb_keycode_t min_kc = setup->min_keycode;
    xcb_keycode_t max_kc = setup->max_keycode;

    xcb_get_keyboard_mapping_reply_t *reply = xcb_get_keyboard_mapping_reply(
        wm.conn, xcb_get_keyboard_mapping(wm.conn, min_kc, (uint8_t)(max_kc - min_kc + 1)), NULL);
    if (!reply)
        return 0;

    xcb_keysym_t *syms = xcb_get_keyboard_mapping_keysyms(reply);
    int per = reply->keysyms_per_keycode;

    for (xcb_keycode_t kc = min_kc; kc <= max_kc; kc++) {
        for (int i = 0; i < per; i++) {
            if (syms[(kc - min_kc) * per + i] == keysym) {
                free(reply);
                return kc;
            }
        }
    }
    free(reply);
    return 0;
}

/* One glyph from the core X "cursor" font -> a usable xcb_cursor_t. Glyph
 * indices come in source/mask pairs (the font's even glyph is the visible
 * shape, the odd one right after it is its mask) -- see X11/cursorfont.h,
 * which just names these same numbers as XC_* constants for Xlib callers.
 * Used only as a last-resort fallback when the user's actual Xcursor theme
 * (load_theme_cursor() below) has no matching cursor at all and doesn't
 * already fall back to this itself. */
static xcb_cursor_t make_glyph_cursor(uint16_t glyph)
{
    xcb_cursor_t cursor = xcb_generate_id(wm.conn);
    xcb_create_glyph_cursor(wm.conn, cursor, wm.cursor_font, wm.cursor_font,
                            glyph, glyph + 1,
                            0, 0, 0,
                            0xffff, 0xffff, 0xffff);
    return cursor;
}

/* Loads whichever of `names` the user's *actual* cursor theme (XCURSOR_
 * THEME / Xcursor.theme X resource, read by xcb_cursor_context_new() the
 * same way libXcursor would) has a cursor for, so move/resize actually
 * match the rest of the desktop's pointer look instead of the plain core
 * font cursor. Tries each name in order since cursor themes disagree on
 * which of the (mostly-standard) names they ship -- e.g. some only have
 * "top_left_corner" and not "nw-resize", or vice versa. xcb_cursor_load_
 * cursor() already falls back to the core font's cursor by itself when a
 * name isn't found in the theme at all, so `fallback_glyph` only matters
 * if `ctx` itself is NULL (theme lookup couldn't even be set up). */
static xcb_cursor_t load_theme_cursor(xcb_cursor_context_t *ctx, const char *const *names, int n,
                                      uint16_t fallback_glyph)
{
    if (ctx) {
        for (int i = 0; i < n; i++) {
            xcb_cursor_t cur = xcb_cursor_load_cursor(ctx, names[i]);
            if (cur != XCB_NONE)
                return cur;
        }
    }
    return make_glyph_cursor(fallback_glyph);
}

static void setup_wm(bool replace)
{
    /* Needs no X connection -- pure file I/O -- so it can set
     * num_desktops/mod_key/deco_* before anything below
     * that depends on them (key grabs, decoration). */
    config_load();

    int preferred_screen = 0;
    wm.conn = xcb_connect(NULL, &preferred_screen);
    if (xcb_connection_has_error(wm.conn))
        die("could not connect to X server");

    const xcb_setup_t *setup = xcb_get_setup(wm.conn);
    xcb_screen_iterator_t it = xcb_setup_roots_iterator(setup);
    for (int i = 0; i < preferred_screen && it.rem; i++)
        xcb_screen_next(&it);

    wm.screen = it.data;
    wm.root = wm.screen->root;
    wm.visual = find_root_visual(wm.screen);
    if (!wm.visual)
        die("could not find root visual");

    /* Optional: only ARGB clients use it, and only when the screen has a
     * 32-bit visual at all. A window whose depth differs from its parent's
     * needs its own colormap (and an explicit border pixel) or the server
     * answers BadMatch -- so the colormap is made here once and shared by
     * every ARGB frame rather than per client. */
    wm.argb_visual = find_argb_visual(wm.screen);
    if (wm.argb_visual) {
        wm.argb_colormap = xcb_generate_id(wm.conn);
        xcb_create_colormap(wm.conn, XCB_COLORMAP_ALLOC_NONE, wm.argb_colormap,
                            wm.root, wm.argb_visual->visual_id);
    }

    ewmh_init_atoms();

    if (!acquire_wm_selection(preferred_screen, replace)) {
        xcb_disconnect(wm.conn);
        exit(EXIT_FAILURE);
    }

    /* kiwm.conf's hide_deco_on_maximize= already set wm.hide_deco_on_maximize
     * above (config_load()); the env var is just a quick override on top,
     * for testing without touching the config file. */
    const char *hide_deco_env = getenv("KIWM_HIDE_DECO_ON_MAXIMIZE");
    if (hide_deco_env)
        wm.hide_deco_on_maximize = !(strcmp(hide_deco_env, "0") == 0 || strcmp(hide_deco_env, "no") == 0);

    /* SHAPE, used for two independent things (see shape.c): rounded
     * corners (decoration.c's apply_rounded_shape()) clip the frame's
     * bounding shape instead of real alpha blending, since kiwm has no
     * compositor; and a client's *own* non-rectangular shape has to be
     * forwarded onto the frame kiwm wraps it in. Must run before the first
     * configure_frame() call either way. */
    shape_init();

    /* Reused by every draw_decoration() call (decoration.c) to blit its
     * off-screen pixmap onto the actual frame -- created once here rather
     * than per-draw to skip a create/free round trip on every single
     * redraw, which during a fast resize drag is a lot of redraws. */
    wm.deco_gc = xcb_generate_id(wm.conn);
    xcb_create_gc(wm.conn, wm.deco_gc, wm.root, 0, NULL);

    /* ...and the same thing for depth-32 frames (see wm.h's argb_visual):
     * both a GC and a CopyArea are bound to one depth, so an ARGB frame
     * can't be blitted with the root-depth GC above. Created from a
     * throwaway depth-32 pixmap purely because a GC needs *a* drawable of
     * the right depth to be born on; the pixmap is freed immediately and
     * the GC outlives it, which is explicitly allowed. */
    if (wm.argb_visual) {
        xcb_pixmap_t seed = xcb_generate_id(wm.conn);
        xcb_create_pixmap(wm.conn, 32, seed, wm.root, 1, 1);
        wm.deco_gc_argb = xcb_generate_id(wm.conn);
        xcb_create_gc(wm.conn, wm.deco_gc_argb, seed, 0, NULL);
        xcb_free_pixmap(wm.conn, seed);
    }

    /* Move/resize drag cursors, swapped in via begin_drag()'s
     * xcb_grab_pointer() call. Loaded from the user's actual Xcursor theme
     * when possible (load_theme_cursor(), libxcb-cursor -- same theme
     * lookup rules as libXcursor: XCURSOR_THEME env, Xcursor.theme X
     * resource), falling back to the plain core "cursor" font glyphs (see
     * X11/cursorfont.h's XC_* numbering) only if that lookup itself can't
     * be set up at all. */
    wm.cursor_font = xcb_generate_id(wm.conn);
    xcb_open_font(wm.conn, wm.cursor_font, (uint16_t)strlen("cursor"), "cursor");

    xcb_cursor_context_t *cursor_ctx = NULL;
    if (xcb_cursor_context_new(wm.conn, wm.screen, &cursor_ctx) < 0)
        cursor_ctx = NULL;

    wm.cursor_move = load_theme_cursor(cursor_ctx, (const char *[]){ "move", "fleur" }, 2, 52);
    wm.cursor_resize_nw = load_theme_cursor(cursor_ctx,
        (const char *[]){ "nw-resize", "top_left_corner" }, 2, 134);
    wm.cursor_resize_ne = load_theme_cursor(cursor_ctx,
        (const char *[]){ "ne-resize", "top_right_corner" }, 2, 136);
    wm.cursor_resize_sw = load_theme_cursor(cursor_ctx,
        (const char *[]){ "sw-resize", "bottom_left_corner" }, 2, 12);
    wm.cursor_resize_se = load_theme_cursor(cursor_ctx,
        (const char *[]){ "se-resize", "bottom_right_corner" }, 2, 14);
    /* ...and the four single-axis ones, for edge grips (kiwm.conf's
     * resize_grip=, see events.c's handle_button_press()). */
    wm.cursor_resize_n = load_theme_cursor(cursor_ctx,
        (const char *[]){ "n-resize", "top_side", "sb_v_double_arrow" }, 3, 138);
    wm.cursor_resize_s = load_theme_cursor(cursor_ctx,
        (const char *[]){ "s-resize", "bottom_side", "sb_v_double_arrow" }, 3, 16);
    wm.cursor_resize_e = load_theme_cursor(cursor_ctx,
        (const char *[]){ "e-resize", "right_side", "sb_h_double_arrow" }, 3, 96);
    wm.cursor_resize_w = load_theme_cursor(cursor_ctx,
        (const char *[]){ "w-resize", "left_side", "sb_h_double_arrow" }, 3, 70);

    /* Cursor IDs created via the context stay valid after freeing it --
     * only the lookup machinery itself is torn down here (per xcb_cursor.h). */
    if (cursor_ctx)
        xcb_cursor_context_free(cursor_ctx);

    load_decoration();

    /* Pango title text rendering (pango_text.c) -- glyph fallback across
     * scripts, so titles in languages the default font doesn't cover still
     * show up instead of leaving blank gaps. Family comes from the theme's
     * colors file (font=, load_decoration() above already parsed it into
     * wm.title_font) or "sans-serif" if empty/no theme. Call once, before
     * the first draw_decoration(). */
    pango_text_init(wm.title_font);

    /* RandR: outputs are the unit of presentation (section 4) even in a
     * WM without a compositor -- we still need it for per-output desktops. */
    const xcb_query_extension_reply_t *randr_ext = xcb_get_extension_data(wm.conn, &xcb_randr_id);
    if (randr_ext && randr_ext->present) {
        wm.randr_event_base = randr_ext->first_event;
        xcb_randr_select_input(wm.conn, wm.root, XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE);
    }
    outputs_refresh();

    /* The only keycode kiwm still resolves by hand: Escape isn't a
     * configurable shortcut but the fixed "cancel" key for whatever modal
     * hold is in progress (osd.c's overlays), and it's never grabbed --
     * it's only ever read during the active keyboard grab such a hold
     * already has. Everything else lives in keybind.c's table. */
    wm.key_escape = keysym_to_keycode(XK_Escape);

    /* SubstructureRedirectMask is the actual WM ownership lock; only one
     * client can select it on the root window at a time. */
    uint32_t mask = XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT |
                    XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY |
                    XCB_EVENT_MASK_STRUCTURE_NOTIFY |
                    XCB_EVENT_MASK_PROPERTY_CHANGE |
                    XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE |
                    XCB_EVENT_MASK_POINTER_MOTION |
                    XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_LEAVE_WINDOW |
                    XCB_EVENT_MASK_KEY_PRESS;

    xcb_generic_error_t *error = xcb_request_check(wm.conn,
        xcb_change_window_attributes_checked(wm.conn, wm.root, XCB_CW_EVENT_MASK, &mask));
    if (error) {
        free(error);
        die("another window manager is already running (this should not "
            "happen after acquiring the manager selection)");
    }

    /* Every global keyboard shortcut (kiwm.conf's key_*), resolved and
     * grabbed from keybind.c's one table. */
    keybind_init();

    ewmh_init_supported();
    ewmh_init_supporting_wm_check();
    manage_existing_windows();
    ewmh_update_client_list();
    ewmh_update_active_window();

    xcb_flush(wm.conn);

    fprintf(stderr, "kiwm: started on screen %dx%d (workspaces 1-%d per output)\n",
            wm.screen_w, wm.screen_h, wm.num_desktops);
}

/* Reparents every client back to the root window (at its current on-screen
 * position) before destroying its frame, so windows survive kiwm exiting --
 * whether that's a normal shutdown or a caught SIGTERM/SIGINT (see the
 * self-pipe above). Skipping this and just letting frames get destroyed
 * out from under their reparented children is exactly the bug this avoids. */
static void cleanup(void)
{
    Client *c = wm.clients;
    while (c) {
        Client *next = c->next;
        xcb_unmap_window(wm.conn, c->frame);
        /* Nothing for the server's save-set to rescue once we've handed
         * the window back to the root ourselves -- see client.c's
         * manage(), which is what put it there. */
        xcb_change_save_set(wm.conn, XCB_SET_MODE_DELETE, c->window);
        xcb_reparent_window(wm.conn, c->window, wm.root, c->x, c->y);
        /* A window kiwm was keeping hidden (minimized, on another desktop,
         * shaded) must not stay invisible with no WM around to bring it
         * back: kiwm hides those by unmapping the *frame*, so the client
         * window itself is still mapped and the reparent above already
         * makes it viewable again -- except for a shaded one, whose
         * content window kiwm really did unmap. */
        if (c->shaded)
            xcb_map_window(wm.conn, c->window);
        xcb_destroy_window(wm.conn, c->frame);
        free(c);
        c = next;
    }
    wm.clients = NULL;

    if (wm.deco_bg)
        cairo_surface_destroy(wm.deco_bg);

    /* Delete our custom per-output desktop properties (PROTOCOL.md) from
     * the root window before disconnecting -- otherwise they linger on
     * the X server (root window properties outlive the client that set
     * them) and any other WM later started on this same display gets
     * mistaken for kiwm by a client that only checks for _KIWM_OUTPUTS'
     * presence (e.g. xispanel's pager widget), even though it has no
     * idea what these atoms mean. */
    if (wm.conn) {
        xcb_delete_property(wm.conn, wm.root, wm.atoms.kiwm_outputs);
        xcb_delete_property(wm.conn, wm.root, wm.atoms.kiwm_output_desktop);
        xcb_delete_property(wm.conn, wm.root, wm.atoms.kiwm_num_output_desktops);
    }

    if (wm.conn) {
        /* xcb_flush() only guarantees the reparent/unmap/destroy requests
         * above were *written* to the socket -- not that the X server has
         * actually *processed* them yet. Disconnecting right after a bare
         * flush is a real race: if our socket closes before the server
         * gets around to reading those bytes, the server's own client-
         * disconnect cleanup runs first and destroys every window kiwm
         * created (including each frame) while a client is still
         * reparented inside it -- back to square one. A cheap round-trip
         * request (XSync()'s xcb equivalent) blocks until the server has
         * replied, which by X11's per-connection ordering guarantee means
         * everything queued before it -- our whole reparent loop -- has
         * already been fully processed. */
        xcb_get_input_focus_reply(wm.conn, xcb_get_input_focus(wm.conn), NULL);
        xcb_disconnect(wm.conn);
    }
}

int main(int argc, char **argv)
{
    bool replace = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--replace") == 0)
            replace = true;
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("usage: kiwm [--replace] [--version]\n");
            return EXIT_SUCCESS;
        } else if (strcmp(argv[i], "--version") == 0) {
            printf("kiwm %s\n", KIWM_VERSION);
            return EXIT_SUCCESS;
        }
    }

    memset(&wm, 0, sizeof(wm));
    const char *dbg = getenv("KIWM_DEBUG_RESIZE");
    wm.debug_resize = dbg && strcmp(dbg, "0") != 0;
    wm.running = true;

    if (pipe(g_sigpipe) != 0)
        die("could not create signal self-pipe");
    /* Non-blocking read end: the drain loop below reads until empty, and
     * without O_NONBLOCK the read() that finds the pipe empty again
     * blocks forever instead of returning -1/EAGAIN, hanging the whole
     * process right there instead of reaching cleanup(). */
    fcntl(g_sigpipe[0], F_SETFL, O_NONBLOCK);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_term_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    /* SIGHUP too: kiwm is normally started from a terminal, and closing
     * that terminal hangs up its whole process group. Left at the default
     * disposition that's an immediate death with no cleanup() -- the exact
     * case the save-set now catches (see client.c's manage()), but this
     * turns it back into an orderly shutdown instead of a rescue. */
    sigaction(SIGHUP, &sa, NULL);

    setup_wm(replace);

    int xfd = xcb_get_file_descriptor(wm.conn);
    struct pollfd fds[2] = {
        { .fd = xfd, .events = POLLIN, .revents = 0 },
        { .fd = g_sigpipe[0], .events = POLLIN, .revents = 0 },
    };

    while (wm.running) {
        xcb_generic_event_t *event;
        while ((event = xcb_poll_for_event(wm.conn)) != NULL) {
            /* Coalesce consecutive MotionNotify events. A fast drag (move
             * or resize -- resize doubly so now that it also rebuilds the
             * rounded-corner shape, see decoration.c's
             * apply_rounded_shape()) can queue motion events faster than
             * kiwm can fully redraw+reshape for each one; handling every
             * single one in a backlog makes the window visibly lag well
             * behind where the pointer actually is by the time it catches
             * up. Only the *last* motion event in a contiguous run
             * reflects the pointer's real current position, so drop every
             * earlier one in that run instead of doing full work for each
             * -- standard X11 WM technique for this. */
            int coalesced = 0;
            while ((event->response_type & ~0x80) == XCB_MOTION_NOTIFY) {
                xcb_generic_event_t *next = xcb_poll_for_event(wm.conn);
                if (!next || (next->response_type & ~0x80) != XCB_MOTION_NOTIFY) {
                    double t0 = wm.debug_resize ? monotonic_ms() : 0;
                    handle_event(event);
                    if (wm.debug_resize) {
                        double dt = monotonic_ms() - t0;
                        fprintf(stderr, "kiwm: [resize-debug] motion: coalesced=%d handle_event=%.2fms\n",
                                coalesced, dt);
                    }
                    free(event);
                    event = next;
                    break;
                }
                free(event);
                event = next;
                coalesced++;
            }
            if (!event)
                break;

            handle_event(event);
            free(event);
            if (!wm.running)
                break;
        }
        if (!wm.running || xcb_connection_has_error(wm.conn))
            break;

        /* Blocking wait, except while a switcher overlay is open: then the
         * loop also has to wake up on its own every so often to notice the
         * driving modifier being released when the release event itself
         * never arrives (a client grabbing the input devices for itself can
         * eat it -- see osd_poll_release()). 100ms is well under what reads
         * as a delay when letting go of Alt, and costs one cheap
         * xcb_query_pointer() round trip per tick, only for as long as the
         * overlay is actually up. */
        /* ...and the same 100ms tick while a drag (or an armed titlebar
         * button) is in flight, to notice a release that never arrived --
         * see events_poll_stale_drag(). */
        int timeout = (osd_active() || wm.drag_client || wm.pressed_client) ? 100 : -1;
        /* ...and the same for the delayed repaint rounds a fullscreen
         * window needs after it loses focus (client.c's pending_expose). */
        client_run_pending_expose();
        int expose_in = client_pending_expose_timeout_ms();
        if (expose_in >= 0 && (timeout < 0 || expose_in < timeout))
            timeout = expose_in;

        /* ...and while a wallpaper nobody is looking at is being held up
         * to be photographed (output.h's desktop_layers_prime). */
        desktop_layers_run_prime();
        int prime_in = desktop_layers_prime_timeout_ms();
        if (prime_in >= 0 && (timeout < 0 || prime_in < timeout))
            timeout = prime_in;

        int ready = poll(fds, 2, timeout);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (ready == 0) {
            osd_poll_release();
            events_poll_stale_drag();
            client_run_pending_expose();
            desktop_layers_run_prime();
            continue;
        }
        if (fds[1].revents & POLLIN) {
            char buf[16];
            while (read(g_sigpipe[0], buf, sizeof(buf)) > 0)
                ;
            fprintf(stderr, "kiwm: received termination signal, shutting down cleanly\n");
            break;
        }
    }

    cleanup();
    return 0;
}
