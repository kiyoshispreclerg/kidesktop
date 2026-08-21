/*
 * kiwm - XiS Window Manager
 *
 * First real prototype, per kiwm-kicomp-projeto.md.
 *
 * Scope of this prototype (Fase 1 + slices of Fase 2/3/4):
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
 *   - Hardcoded keybindings: Alt+Tab/Alt+Shift+Tab cycle focus,
 *     Meta+Tab/Meta+Shift+Tab cycle the focused output's virtual desktop,
 *     Meta+Up maximizes/restores, Meta+drag (or Alt+drag) moves, Alt+drag
 *     with the right button resizes.
 *   - Enough EWMH/ICCCM for a taskbar (xispanel's tasklist widget) to
 *     list/activate/close/minimize/maximize windows.
 *
 * See wm.h for the shared types/state and each module's own header for
 * its slice of the WM: atoms.c (EWMH/ICCCM atom interning), output.c
 * (RandR outputs + per-output virtual desktops), decoration.c (Cairo/
 * Imlib2 frame painting), ewmh.c (EWMH property bookkeeping on clients),
 * client.c (manage/unmanage, focus, move/resize/maximize/minimize),
 * events.c (X event dispatch), selection.c (--replace).
 */

#define _POSIX_C_SOURCE 200809L

#include "wm.h"
#include "atoms.h"
#include "output.h"
#include "decoration.h"
#include "ewmh.h"
#include "client.h"
#include "events.h"
#include "selection.h"

#include <xcb/randr.h>
#include <cairo/cairo.h>

#include <X11/keysym.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

KiWM wm;

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

static void setup_wm(bool replace)
{
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

    ewmh_init_atoms();

    if (!acquire_wm_selection(preferred_screen, replace)) {
        xcb_disconnect(wm.conn);
        exit(EXIT_FAILURE);
    }

    wm.hide_deco_on_maximize = false;
    const char *hide_deco_env = getenv("KIWM_HIDE_DECO_ON_MAXIMIZE");
    if (hide_deco_env)
        wm.hide_deco_on_maximize = !(strcmp(hide_deco_env, "0") == 0 || strcmp(hide_deco_env, "no") == 0);

    load_decoration();

    /* RandR: outputs are the unit of presentation (section 4) even in a
     * WM without a compositor -- we still need it for per-output desktops. */
    const xcb_query_extension_reply_t *randr_ext = xcb_get_extension_data(wm.conn, &xcb_randr_id);
    if (randr_ext && randr_ext->present) {
        wm.randr_event_base = randr_ext->first_event;
        xcb_randr_select_input(wm.conn, wm.root, XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE);
    }
    outputs_refresh();

    wm.key_tab = keysym_to_keycode(XK_Tab);
    wm.key_1   = keysym_to_keycode(XK_1);
    wm.key_2   = keysym_to_keycode(XK_2);
    wm.key_3   = keysym_to_keycode(XK_3);
    wm.key_4   = keysym_to_keycode(XK_4);
    wm.key_up  = keysym_to_keycode(XK_Up);

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

    if (wm.key_tab) {
        xcb_grab_key(wm.conn, 1, wm.root, MOD_ALT, wm.key_tab, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
        xcb_grab_key(wm.conn, 1, wm.root, MOD_ALT_SHIFT, wm.key_tab, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
        xcb_grab_key(wm.conn, 1, wm.root, MOD_META, wm.key_tab, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
        xcb_grab_key(wm.conn, 1, wm.root, MOD_META_SHIFT, wm.key_tab, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
    }
    if (wm.key_1) xcb_grab_key(wm.conn, 1, wm.root, MOD_ALT, wm.key_1, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
    if (wm.key_2) xcb_grab_key(wm.conn, 1, wm.root, MOD_ALT, wm.key_2, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
    if (wm.key_3) xcb_grab_key(wm.conn, 1, wm.root, MOD_ALT, wm.key_3, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
    if (wm.key_4) xcb_grab_key(wm.conn, 1, wm.root, MOD_ALT, wm.key_4, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
    if (wm.key_up) xcb_grab_key(wm.conn, 1, wm.root, MOD_META, wm.key_up, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);

    ewmh_init_supported();
    ewmh_init_supporting_wm_check();
    manage_existing_windows();
    ewmh_update_client_list();
    ewmh_update_active_window();

    xcb_flush(wm.conn);

    fprintf(stderr, "kiwm: started on screen %dx%d (workspaces 1-%d per output)\n",
            wm.screen->width_in_pixels, wm.screen->height_in_pixels, NUM_WORKSPACES);
}

static void cleanup(void)
{
    Client *c = wm.clients;
    while (c) {
        Client *next = c->next;
        xcb_unmap_window(wm.conn, c->frame);
        xcb_reparent_window(wm.conn, c->window, wm.root, c->x, c->y);
        xcb_destroy_window(wm.conn, c->frame);
        free(c);
        c = next;
    }
    wm.clients = NULL;

    if (wm.deco_bg)
        cairo_surface_destroy(wm.deco_bg);

    if (wm.conn)
        xcb_disconnect(wm.conn);
}

int main(int argc, char **argv)
{
    bool replace = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--replace") == 0)
            replace = true;
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("usage: kiwm [--replace]\n");
            return EXIT_SUCCESS;
        }
    }

    memset(&wm, 0, sizeof(wm));
    wm.running = true;

    setup_wm(replace);

    while (wm.running) {
        xcb_generic_event_t *event = xcb_wait_for_event(wm.conn);
        if (!event)
            break;
        handle_event(event);
        free(event);
    }

    cleanup();
    return 0;
}
