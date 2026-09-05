/*
 * kicomp - XiS compositor for kiwm
 *
 * First prototype, per kiwm-kicomp-projeto.md (Fase 5).
 *
 * Scope of this prototype:
 *   - Capability detection at runtime for Composite/Damage/XFixes/Render/
 *     RandR (section 17/30). Nothing here assumes XiS, FLIP, or GL.
 *   - Manual redirection of root's children, painted onto the Composite
 *     overlay window.
 *   - One scene, one drawable, one dirty flag per RandR output
 *     (sections 4/18/39). A window crossing two outputs is clipped into
 *     each output's scene separately (section 26).
 *   - Renderer and presenter behind their own vtables (sections 15/28),
 *     with XRender and COPY as the first implementations.
 *   - Real per-window alpha: depth-32 windows blend, and
 *     _NET_WM_WINDOW_OPACITY is honoured. That is the only intended
 *     visible difference from an uncomposited screen -- the goal of this
 *     milestone is "exactly the same desktop, plus working transparency".
 *   - Idle costs nothing: the process sleeps in poll() and repaints only
 *     the outputs damage actually touched.
 *   - Effects behind their own vtable (sections 23/42), geometry change
 *     as the first one, and a per-output frame clock to drive them
 *     (section 19). Durations are time, never frames, and every one of
 *     them is a multiple of a single number in kicomp.conf (section 20).
 *
 * Explicitly NOT here yet: UST-derived pacing and the XiS FLIP presenter
 * (the rest of Fase 7/8), parity for the GL renderer (Fase 6 -- it draws
 * the desktop, but shadows, shape clipping and the density layers are
 * still XRender-only, and the effects that want GL come after that),
 * unredirect of a fullscreen output, and any kiwm<->kicomp IPC
 * (section 32) -- this version learns everything from plain X events, so
 * kiwm needs no changes at all to be composited, and killing kicomp
 * returns the session to the uncomposited path (section 31).
 */

#define _POSIX_C_SOURCE 200809L

#define KICOMP_VERSION "0.2.36"

#include "comp.h"
#include "output.h"
#include "window.h"
#include "scene.h"
#include "renderer.h"
#include "presenter.h"
#include "effect.h"
#include "animation.h"
#include "scheduler.h"
#include "config.h"
#include "shadow.h"
#include "desktop.h"
#include "region.h"
#include "damage.h"
#include "inputscale.h"
#include "density.h"
#include "input.h"

#include <xcb/randr.h>
#include <xcb/shape.h>
#include <xcb/xkb.h>
#include <xcb/present.h>

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

KiComp comp;

void comp_log(const char *fmt, ...)
{
    if (!comp.verbose)
        return;
    va_list ap;
    va_start(ap, fmt);
    fputs("kicomp: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

void comp_info(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("kicomp: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

/* _NET_ACTIVE_WINDOW, the focused window as the WM publishes it. */
static xcb_window_t read_active_window(void)
{
    if (comp.atoms.net_active_window == XCB_NONE)
        return XCB_NONE;

    xcb_get_property_reply_t *r = xcb_get_property_reply(comp.conn,
        xcb_get_property(comp.conn, 0, comp.root, comp.atoms.net_active_window,
                         XCB_ATOM_WINDOW, 0, 1), NULL);
    if (!r)
        return XCB_NONE;

    xcb_window_t win = XCB_NONE;
    if (r->type == XCB_ATOM_WINDOW && xcb_get_property_value_length(r) >= 4)
        win = *(xcb_window_t *)xcb_get_property_value(r);
    free(r);
    return win;
}

static void on_signal(int sig)
{
    (void)sig;
    comp.running = false;
}

/* ------------------------------------------------------------------ */
/* atoms                                                               */
/* ------------------------------------------------------------------ */

static xcb_atom_t intern(const char *name)
{
    xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(comp.conn,
        xcb_intern_atom(comp.conn, 0, (uint16_t)strlen(name), name), NULL);
    if (!r)
        return XCB_NONE;
    xcb_atom_t a = r->atom;
    free(r);
    return a;
}

static void atoms_init(void)
{
    char cm[32];
    snprintf(cm, sizeof(cm), "_NET_WM_CM_S%d", comp.screen_num);

    comp.atoms.net_wm_cm              = intern(cm);
    comp.atoms.net_wm_window_opacity  = intern("_NET_WM_WINDOW_OPACITY");
    comp.atoms.xrootpmap_id           = intern("_XROOTPMAP_ID");
    comp.atoms.esetroot_pmap_id       = intern("ESETROOT_PMAP_ID");
    comp.atoms.kiwm_layer             = intern("_KIWM_LAYER");
    comp.atoms.kiwm_num_desktops      = intern("_KIWM_NUM_OUTPUT_DESKTOPS");
    comp.atoms.kiwm_set_output_desktop = intern("_KIWM_SET_OUTPUT_DESKTOP");
    comp.atoms.kiwm_wm_output         = intern("_KIWM_WM_OUTPUT");
    comp.atoms.net_wm_desktop         = intern("_NET_WM_DESKTOP");
    comp.atoms.net_number_of_desktops = intern("_NET_NUMBER_OF_DESKTOPS");
    /* "Shut down cleanly" -- sent by `kicomp --toggle` to whichever
     * instance already owns the selection. */
    comp.atoms.kicomp_quit            = intern("_KICOMP_QUIT");

    comp.atoms.net_wm_window_type     = intern("_NET_WM_WINDOW_TYPE");
    comp.atoms.type_normal            = intern("_NET_WM_WINDOW_TYPE_NORMAL");
    comp.atoms.type_dialog            = intern("_NET_WM_WINDOW_TYPE_DIALOG");
    comp.atoms.type_utility           = intern("_NET_WM_WINDOW_TYPE_UTILITY");
    comp.atoms.type_toolbar           = intern("_NET_WM_WINDOW_TYPE_TOOLBAR");
    comp.atoms.type_splash            = intern("_NET_WM_WINDOW_TYPE_SPLASH");
    comp.atoms.type_dock              = intern("_NET_WM_WINDOW_TYPE_DOCK");
    comp.atoms.type_desktop           = intern("_NET_WM_WINDOW_TYPE_DESKTOP");
    comp.atoms.type_menu              = intern("_NET_WM_WINDOW_TYPE_MENU");
    comp.atoms.type_dropdown_menu     = intern("_NET_WM_WINDOW_TYPE_DROPDOWN_MENU");
    comp.atoms.type_popup_menu        = intern("_NET_WM_WINDOW_TYPE_POPUP_MENU");
    comp.atoms.type_combo             = intern("_NET_WM_WINDOW_TYPE_COMBO");
    comp.atoms.type_tooltip           = intern("_NET_WM_WINDOW_TYPE_TOOLTIP");
    comp.atoms.type_notification      = intern("_NET_WM_WINDOW_TYPE_NOTIFICATION");
    comp.atoms.type_dnd               = intern("_NET_WM_WINDOW_TYPE_DND");

    comp.atoms.wm_state               = intern("WM_STATE");
    comp.atoms.net_wm_state           = intern("_NET_WM_STATE");
    comp.atoms.state_maximized_horz   = intern("_NET_WM_STATE_MAXIMIZED_HORZ");
    comp.atoms.state_maximized_vert   = intern("_NET_WM_STATE_MAXIMIZED_VERT");
    comp.atoms.state_shaded           = intern("_NET_WM_STATE_SHADED");
    comp.atoms.state_fullscreen       = intern("_NET_WM_STATE_FULLSCREEN");
    comp.atoms.state_hidden           = intern("_NET_WM_STATE_HIDDEN");
    comp.atoms.state_above            = intern("_NET_WM_STATE_ABOVE");
    comp.atoms.wm_client_leader       = intern("WM_CLIENT_LEADER");
    comp.atoms.net_wm_pid             = intern("_NET_WM_PID");
    comp.atoms.net_wm_icon_geometry   = intern("_NET_WM_ICON_GEOMETRY");
    comp.atoms.net_active_window      = intern("_NET_ACTIVE_WINDOW");
    comp.atoms.net_current_desktop    = intern("_NET_CURRENT_DESKTOP");
    comp.atoms.net_desktop_layout     = intern("_NET_DESKTOP_LAYOUT");
    char density_mgr[40];
    snprintf(density_mgr, sizeof(density_mgr), "_X_DENSITY_MANAGER_S%d",
             comp.screen_num);
    comp.atoms.density_manager        = intern(density_mgr);
    comp.atoms.density_requested      = intern("_X_DENSITY_REQUESTED");
    comp.atoms.density_scale          = intern("_X_DENSITY_SCALE");
    comp.atoms.density_pixmap         = intern("_X_DENSITY_PIXMAP");

    /* Not a standard atom: the XLibre fork's per-output DPI property,
     * literally called "DPI" (TESTS/DPI-PER-OUTPUT.md). Interned
     * unconditionally here so the event handler can compare against it;
     * output.c still asks the server with only_if_exists before reading
     * any output's value. */
    comp.atoms.randr_dpi              = intern("DPI");
    comp.atoms.xis_confined_area      = intern("_XIS_CONFINED_AREA");

    comp.atoms.kiwm_outputs           = intern("_KIWM_OUTPUTS");
    comp.atoms.kiwm_output_desktop    = intern("_KIWM_OUTPUT_DESKTOP");
}

/* ------------------------------------------------------------------ */
/* capability detection (sections 17, 30, 45)                          */
/* ------------------------------------------------------------------ */

static bool caps_detect(void)
{
    const xcb_query_extension_reply_t *ext;

    ext = xcb_get_extension_data(comp.conn, &xcb_composite_id);
    if (ext && ext->present) {
        xcb_composite_query_version_reply_t *v =
            xcb_composite_query_version_reply(comp.conn,
                xcb_composite_query_version(comp.conn, 0, 4), NULL);
        if (v) {
            comp.caps.composite = true;
            /* The overlay window arrived in Composite 0.3; without it
             * there is nowhere to present to that doesn't fight the
             * applications for the root window. */
            comp.caps.overlay = (v->major_version > 0 || v->minor_version >= 3);
            comp_log("Composite %u.%u", v->major_version, v->minor_version);
            free(v);
        }
    }

    ext = xcb_get_extension_data(comp.conn, &xcb_xfixes_id);
    if (ext && ext->present) {
        /* XFixes insists on a version handshake before any of its
         * requests, and Damage is defined in terms of XFixes regions. */
        xcb_xfixes_query_version_reply_t *v =
            xcb_xfixes_query_version_reply(comp.conn,
                xcb_xfixes_query_version(comp.conn, 5, 0), NULL);
        if (v) {
            comp.caps.xfixes = true;
            comp.xfixes_event = ext->first_event;
            free(v);
        }
    }

    ext = xcb_get_extension_data(comp.conn, &xcb_damage_id);
    if (ext && ext->present) {
        xcb_damage_query_version_reply_t *v =
            xcb_damage_query_version_reply(comp.conn,
                xcb_damage_query_version(comp.conn, 1, 1), NULL);
        if (v) {
            comp.caps.damage = true;
            comp.damage_event = ext->first_event;
            free(v);
        }
    }

    ext = xcb_get_extension_data(comp.conn, &xcb_render_id);
    if (ext && ext->present) {
        xcb_render_query_version_reply_t *v =
            xcb_render_query_version_reply(comp.conn,
                xcb_render_query_version(comp.conn, 0, 11), NULL);
        if (v) {
            comp.caps.render = true;
            free(v);
        }
    }

    ext = xcb_get_extension_data(comp.conn, &xcb_randr_id);
    if (ext && ext->present) {
        xcb_randr_query_version_reply_t *v =
            xcb_randr_query_version_reply(comp.conn,
                xcb_randr_query_version(comp.conn, 1, 5), NULL);
        if (v) {
            comp.caps.randr = true;
            comp.randr_event = ext->first_event;
            free(v);
        }
    }

    ext = xcb_get_extension_data(comp.conn, &xcb_shape_id);
    if (ext && ext->present) {
        comp.caps.shape = true;
        comp.shape_event = ext->first_event;
    }

    /* XKB, only for BellNotify: a window ringing the bell is the one
     * thing an effect can answer that no other extension reports. Asked
     * for by version, because the extension refuses to speak at all
     * until a client says which one it understands. */
    ext = xcb_get_extension_data(comp.conn, &xcb_xkb_id);
    if (ext && ext->present) {
        xcb_xkb_use_extension_reply_t *use = xcb_xkb_use_extension_reply(comp.conn,
            xcb_xkb_use_extension(comp.conn, XCB_XKB_MAJOR_VERSION,
                                  XCB_XKB_MINOR_VERSION), NULL);
        if (use && use->supported) {
            comp.caps.xkb = true;
            comp.xkb_event = ext->first_event;

            /* Every bell, whoever rings it. The map has to be asked for
             * per event type, and this asks for exactly one. */
            xcb_xkb_select_events(comp.conn, XCB_XKB_ID_USE_CORE_KBD,
                                  XCB_XKB_EVENT_TYPE_BELL_NOTIFY, 0,
                                  XCB_XKB_EVENT_TYPE_BELL_NOTIFY,
                                  0, 0, NULL);
        }
        free(use);
    }

    ext = xcb_get_extension_data(comp.conn, &xcb_present_id);
    if (ext && ext->present) {
        /* 1.0 is all this uses: PresentPixmap with a target CRTC, and
         * CompleteNotify to hear when the frame actually landed. */
        xcb_present_query_version_reply_t *v =
            xcb_present_query_version_reply(comp.conn,
                xcb_present_query_version(comp.conn, 1, 2), NULL);
        if (v) {
            comp.caps.present = true;
            comp.present_opcode = ext->major_opcode;
            comp_log("Present %u.%u", v->major_version, v->minor_version);
            free(v);
        }
    }

    /* Per-CRTC FLIP is Fase 8: declared false so no code path can
     * accidentally believe in it. Present being here does not mean frames
     * are flipping -- an XRender pixmap is not a scanout buffer, so the
     * server will copy it at vblank, which is already the point. */
    comp.caps.flip_per_crtc = false;

    if (!comp.caps.composite || !comp.caps.overlay) {
        fprintf(stderr, "kicomp: server has no Composite overlay window; "
                        "kiwm keeps running uncomposited\n");
        return false;
    }
    if (!comp.caps.render) {
        fprintf(stderr, "kicomp: server has no RENDER extension\n");
        return false;
    }
    if (!comp.caps.damage || !comp.caps.xfixes) {
        fprintf(stderr, "kicomp: server has no DAMAGE/XFIXES; "
                        "cannot know when to repaint\n");
        return false;
    }
    if (!comp.caps.randr)
        comp_log("no RandR: treating the whole root window as one output");

    return true;
}

/* ------------------------------------------------------------------ */
/* _NET_WM_CM_Sn ownership                                             */
/* ------------------------------------------------------------------ */

static xcb_window_t selection_owner(void)
{
    if (comp.atoms.net_wm_cm == XCB_NONE)
        return XCB_NONE;

    xcb_get_selection_owner_reply_t *own = xcb_get_selection_owner_reply(comp.conn,
        xcb_get_selection_owner(comp.conn, comp.atoms.net_wm_cm), NULL);
    xcb_window_t w = own ? own->owner : XCB_NONE;
    free(own);
    return w;
}

/* --toggle: one command bound to one key that turns compositing off if it
 * is on, and on if it is off -- the compositing toggle every desktop has.
 *
 * "Is it on?" is not asked of a process name or a pid file but of the
 * session itself: whoever owns _NET_WM_CM_Sn is the compositor, which is
 * the same question every other client asks and the same answer another
 * compositor would give. So this turns off *a* compositor, not
 * specifically one of ours, which is what a user pressing the key means.
 *
 * Returns true when something was turned off and this process is done.
 */
static bool toggle_off(void)
{
    xcb_window_t owner = selection_owner();
    if (owner == XCB_NONE)
        return false;      /* nothing running: the caller starts up */

    /* Asked, not imposed: a ClientMessage with an empty event mask goes
     * to the client that created the window, and the running instance
     * answers it by leaving its loop -- so it unredirects, releases the
     * overlay, drops any cursor confinement it applied and repaints
     * nothing half-done. */
    xcb_client_message_event_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.response_type = XCB_CLIENT_MESSAGE;
    msg.format = 32;
    msg.window = owner;
    msg.type = comp.atoms.kicomp_quit;
    xcb_send_event(comp.conn, 0, owner, 0, (const char *)&msg);
    xcb_flush(comp.conn);

    /* Gone when the selection is: that is the one fact everything else in
     * the session keys off. */
    int waited = 0;
    bool killed = false;
    while (waited < 3000) {
        struct timespec ts = { 0, 50 * 1000 * 1000 };
        nanosleep(&ts, NULL);
        waited += 50;

        if (selection_owner() != owner)
            return true;

        /* An instance too old to know the message, or one wedged: take
         * the connection out from under it. The server then reverts
         * everything that client did -- the redirection, the overlay, the
         * confinement -- which is exactly the uncomposited session, just
         * without the tidy exit. */
        if (waited >= 1000 && !killed) {
            fprintf(stderr, "kicomp: the running compositor did not answer, "
                            "closing its connection\n");
            xcb_kill_client(comp.conn, owner);
            xcb_flush(comp.conn);
            killed = true;
        }
    }

    fprintf(stderr, "kicomp: a compositor still owns _NET_WM_CM_S%d\n",
            comp.screen_num);
    return true;   /* done either way: never two compositors at once */
}

static bool acquire_selection(bool replace)
{
    if (comp.atoms.net_wm_cm == XCB_NONE)
        return false;

    xcb_get_selection_owner_reply_t *own = xcb_get_selection_owner_reply(comp.conn,
        xcb_get_selection_owner(comp.conn, comp.atoms.net_wm_cm), NULL);
    xcb_window_t previous = own ? own->owner : XCB_NONE;
    free(own);

    if (previous != XCB_NONE && !replace) {
        fprintf(stderr, "kicomp: another compositor is running "
                        "(use --replace to take over)\n");
        return false;
    }

    comp.cm_window = xcb_generate_id(comp.conn);
    uint32_t values[] = { 1, XCB_EVENT_MASK_PROPERTY_CHANGE };
    xcb_create_window(comp.conn, XCB_COPY_FROM_PARENT, comp.cm_window, comp.root,
                      -1, -1, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      XCB_COPY_FROM_PARENT,
                      XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK, values);
    xcb_change_property(comp.conn, XCB_PROP_MODE_REPLACE, comp.cm_window,
                        XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8, 6, "kicomp");

    if (previous != XCB_NONE) {
        /* Watch the old owner so we can wait for it to actually let go
         * before we start redirecting -- two compositors redirecting at
         * once is how you get a black screen. */
        uint32_t mask = XCB_EVENT_MASK_STRUCTURE_NOTIFY;
        xcb_change_window_attributes(comp.conn, previous, XCB_CW_EVENT_MASK, &mask);
    }

    xcb_set_selection_owner(comp.conn, comp.cm_window, comp.atoms.net_wm_cm,
                            XCB_CURRENT_TIME);
    xcb_flush(comp.conn);

    own = xcb_get_selection_owner_reply(comp.conn,
        xcb_get_selection_owner(comp.conn, comp.atoms.net_wm_cm), NULL);
    bool ours = own && own->owner == comp.cm_window;
    free(own);
    if (!ours) {
        fprintf(stderr, "kicomp: failed to acquire %s\n", "_NET_WM_CM_Sn");
        return false;
    }

    if (previous != XCB_NONE) {
        /* Bounded wait: an old owner that ignores the handoff shouldn't
         * hang us forever. */
        int waited_ms = 0;
        while (waited_ms < 3000) {
            xcb_generic_event_t *ev = xcb_poll_for_event(comp.conn);
            if (!ev) {
                struct pollfd p = { xcb_get_file_descriptor(comp.conn), POLLIN, 0 };
                if (poll(&p, 1, 50) < 0 && errno != EINTR)
                    break;
                waited_ms += 50;
                continue;
            }
            bool gone = ((ev->response_type & 0x7f) == XCB_DESTROY_NOTIFY) &&
                        ((xcb_destroy_notify_event_t *)ev)->window == previous;
            free(ev);
            if (gone)
                break;
        }
        comp_log("previous compositor released the selection");
    }

    return true;
}

/* ------------------------------------------------------------------ */
/* overlay                                                             */
/* ------------------------------------------------------------------ */

static bool overlay_acquire(void)
{
    xcb_composite_get_overlay_window_reply_t *r =
        xcb_composite_get_overlay_window_reply(comp.conn,
            xcb_composite_get_overlay_window(comp.conn, comp.root), NULL);
    if (!r) {
        fprintf(stderr, "kicomp: cannot get the Composite overlay window\n");
        return false;
    }
    comp.overlay = r->overlay_win;
    free(r);

    /* The overlay sits above every application window, so it must be
     * transparent to input or the desktop becomes unclickable: empty
     * input shape, unrestricted bounding shape. */
    xcb_xfixes_region_t empty = xcb_generate_id(comp.conn);
    xcb_xfixes_create_region(comp.conn, empty, 0, NULL);
    xcb_xfixes_set_window_shape_region(comp.conn, comp.overlay,
                                       XCB_SHAPE_SK_BOUNDING, 0, 0,
                                       XCB_XFIXES_REGION_NONE);
    xcb_xfixes_set_window_shape_region(comp.conn, comp.overlay,
                                       XCB_SHAPE_SK_INPUT, 0, 0, empty);
    xcb_xfixes_destroy_region(comp.conn, empty);

    return true;
}

/* ------------------------------------------------------------------ */
/* paint                                                               */
/* ------------------------------------------------------------------ */

/* One scene buffer reused across outputs and frames: scene_build() fills
 * it from scratch each time, and there is exactly one render thread
 * (section 41). */
static CompScene scene;

static void paint_dirty_outputs(double now)
{
    bool painted = false;

    for (int i = 0; i < comp.output_count; i++) {
        CompOutput *o = &comp.outputs[i];
        /* render_data, not target: the latter is the XRender backend's
         * Picture, and a GL backend has no such thing. What every backend
         * does have is its own per-output state, and having it is exactly
         * what "this output can be painted" means. */
        if (!o->dirty || !o->render_data)
            continue;

        /* A frame is still in flight for this output (Present): painting
         * another one now would queue latency behind it rather than show
         * anything sooner. It stays dirty and is painted the moment the
         * completion arrives, which is what makes the loop vblank-paced
         * rather than timer-paced. */
        if (presenter && presenter->busy && presenter->busy(o))
            continue;

        /* Its own clock decides, not the event that dirtied it: a burst
         * of damage becomes one frame, and an output stays at its own
         * refresh rate while another animates at a different one
         * (section 19). An output whose slot already passed paints right
         * away, so nothing waits for a deadline that isn't there. */
        if (!scheduler_may_paint(o, now))
            continue;

        /* What of it to repaint (section 39). Passed to the renderer and
         * the presenter rather than read by them off the output, so the
         * frame works from one region that cannot change under it. */
        CompRegion region;
        output_paint_region(o, &region);

        scene_build(&scene, o);
        effects_apply(&scene, o);
        /* After the effects, never before: what covers what is exactly
         * what they change (scene.h). */
        scene_cull_occluded(&scene, o);
        comp_log("paint %s: %d node%s, %s", o->name, scene.count,
                 scene.count == 1 ? "" : "s",
                 region_is_full(&region) ? "whole output" : "damaged parts");

        renderer->begin(o, &region);
        renderer->draw_scene(o, &scene, &region);
        renderer->end(o);

        presenter->present(o, COMP_PRESENT_COPY, &region);

        output_painted(o);
        painted = true;
    }

    if (painted)
        xcb_flush(comp.conn);
}

/* ------------------------------------------------------------------ */
/* events                                                              */
/* ------------------------------------------------------------------ */

static void handle_event(xcb_generic_event_t *ev)
{
    uint8_t type = ev->response_type & 0x7f;

    /* Keyboard and pointer first, and only when something asked for them
     * (input.h): a hotkey firing, or every event there is while an effect
     * that is a *mode* holds the grab. */
    if (input_handle_event(ev))
        return;

    /* The presenter gets first refusal: Present's completions arrive as
     * XGE generic events, and nothing else here knows what those are. */
    if (presenter && presenter->handle_event && presenter->handle_event(ev))
        return;

    if (comp.caps.damage && type == comp.damage_event + XCB_DAMAGE_NOTIFY) {
        xcb_damage_notify_event_t *e = (xcb_damage_notify_event_t *)ev;
        CompWindow *w = window_find(e->drawable);
        if (w) {
            /* Only noted, not answered: the region is collected once per
             * frame, for every window at once (damage.h). A window
             * damaging itself five hundred times between two frames costs
             * exactly what one damaging itself once costs. */
            damage_window_reported(w);
        }
        return;
    }

    /* The bell. XKB reports which window it was rung *at* when the client
     * used XkbBell with one; a plain XBell() from a terminal names none,
     * and then the window that has focus is the one that rang -- that is
     * where the keystroke went. */
    if (comp.caps.xkb && type == comp.xkb_event) {
        xcb_xkb_bell_notify_event_t *e = (xcb_xkb_bell_notify_event_t *)ev;
        if (e->xkbType == XCB_XKB_BELL_NOTIFY) {
            xcb_window_t at = e->window != XCB_NONE ? e->window : comp.active_window;
            window_bell(at);
        }
        return;
    }

    /* ShapeNotify: the window's silhouette changed (kiwm reshaping a
     * frame it just resized, or a client changing its own shape). The
     * cached region has to go with it -- see renderer-xrender.c's
     * window_shape(). Extension event numbers are runtime-assigned, hence
     * the if-chain rather than case labels. */
    if (comp.caps.shape && type == comp.shape_event + XCB_SHAPE_NOTIFY) {
        xcb_shape_notify_event_t *e = (xcb_shape_notify_event_t *)ev;
        CompWindow *w = window_find(e->affected_window);
        if (w) {
            renderer_window_shape_invalidate(w);
            CompRect r = window_rect(w);
            output_damage_rect(&r);
        }
        return;
    }

    if (comp.caps.randr && type == comp.randr_event + XCB_RANDR_NOTIFY) {
        xcb_randr_notify_event_t *e = (xcb_randr_notify_event_t *)ev;
        if (e->subCode == XCB_RANDR_NOTIFY_OUTPUT_PROPERTY &&
            e->u.op.atom == comp.atoms.randr_dpi) {
            /* The DPI of some output changed. Which one hardly matters:
             * rebuilding the outputs re-reads all of them, re-applies the
             * cursor confinement and re-asks every window for the density
             * its (possibly new) scale wants -- the same three things
             * that happen on a hotplug, for the same reason. */
            comp_log("RandR: DPI changed");
            outputs_refresh();
            renderer_background_invalidate();
            output_damage_all();
        }
        return;
    }

    if (comp.caps.randr && type == comp.randr_event + XCB_RANDR_SCREEN_CHANGE_NOTIFY) {
        comp_log("RandR screen change");
        outputs_refresh();
        renderer_background_invalidate();
        output_damage_all();
        return;
    }

    switch (type) {
    case XCB_CREATE_NOTIFY: {
        xcb_create_notify_event_t *e = (xcb_create_notify_event_t *)ev;
        if (e->parent == comp.root)
            window_add_top(e->window);   /* where X just put it */
        break;
    }
    case XCB_DESTROY_NOTIFY: {
        xcb_destroy_notify_event_t *e = (xcb_destroy_notify_event_t *)ev;
        window_remove(e->window);
        break;
    }
    case XCB_MAP_NOTIFY: {
        xcb_map_notify_event_t *e = (xcb_map_notify_event_t *)ev;
        window_map(e->window);
        break;
    }
    case XCB_UNMAP_NOTIFY: {
        xcb_unmap_notify_event_t *e = (xcb_unmap_notify_event_t *)ev;
        window_unmap(e->window);
        break;
    }
    case XCB_REPARENT_NOTIFY: {
        /* kiwm reparents every managed client into a frame: the client
         * stops being a top-level (we drop it, its pixels now arrive as
         * part of the frame's) and the frame appears as one instead. */
        xcb_reparent_notify_event_t *e = (xcb_reparent_notify_event_t *)ev;
        if (e->parent == comp.root) {
            window_add_top(e->window);   /* reparenting stacks it on top */
        } else {
            window_remove(e->window);
            /* Into one of our frames: that window is the frame's client,
             * and the frame's properties are really its properties. */
            window_client_reparented(e->parent, e->window);
        }
        break;
    }
    case XCB_CONFIGURE_NOTIFY: {
        xcb_configure_notify_event_t *e = (xcb_configure_notify_event_t *)ev;
        if (e->window == comp.root) {
            comp_log("root resized to %ux%u", e->width, e->height);
            outputs_refresh();
            output_damage_all();
        } else if (e->event == comp.root) {
            window_configure(e->window, e->x, e->y, e->width, e->height,
                             e->border_width, e->above_sibling);
        } else {
            /* A window inside one of ours -- the client in its frame.
             * Nothing about the frame changed, but where the client sits
             * in it did, and that is what decides how much of the frame
             * is opaque (window.h). */
            window_client_reconfigured(e->event, e->window, e->x, e->y,
                                       e->width, e->height, e->border_width);
        }
        break;
    }
    case XCB_CIRCULATE_NOTIFY: {
        xcb_circulate_notify_event_t *e = (xcb_circulate_notify_event_t *)ev;
        xcb_window_t above = XCB_NONE;
        if (e->place == XCB_PLACE_ON_TOP) {
            for (CompWindow *w = comp.stack; w; w = w->next)
                if (w->id != e->window)
                    above = w->id;
        }
        window_restack(e->window, above);
        break;
    }
    case XCB_PROPERTY_NOTIFY: {
        xcb_property_notify_event_t *e = (xcb_property_notify_event_t *)ev;

        if (e->window == comp.root) {
            if (e->atom == comp.atoms.xrootpmap_id ||
                e->atom == comp.atoms.esetroot_pmap_id) {
                renderer_background_invalidate();
                output_damage_all();
            } else if (e->atom == comp.atoms.net_active_window) {
                window_focus_changed(read_active_window());
            } else if (e->atom == comp.atoms.net_current_desktop ||
                       e->atom == comp.atoms.kiwm_output_desktop ||
                       e->atom == comp.atoms.kiwm_outputs ||
                       e->atom == comp.atoms.net_desktop_layout) {
                /* Windows that vanish or appear right after this left or
                 * arrived with a desktop rather than being closed or
                 * opened -- see window.c's windows_flush_events(). And
                 * which desktop each output moved to, and in which
                 * direction, which is what the wall slides along. */
                comp.desktop_changed_ms = comp_now_ms();
                desktop_refresh();
            }
            break;
        }

        if (e->atom == comp.atoms.net_wm_window_opacity) {
            CompWindow *w = window_find(e->window);
            if (w)
                window_update_opacity(w);
        } else if (e->atom == comp.atoms.density_scale ||
                   e->atom == comp.atoms.density_pixmap) {
            /* The client answering a density request -- or saying it drew
             * a new frame into the same pixmap (density.h). */
            CompWindow *w = window_find_by_client(e->window);
            if (!w)
                w = window_find(e->window);
            if (w)
                density_property_changed(w, e->window);
        } else if (e->atom == comp.atoms.net_wm_state ||
                   e->atom == comp.atoms.wm_state) {
            /* These live on the client window inside the frame, which is
             * not the window the compositor tracks -- hence the lookup by
             * client. */
            CompWindow *w = window_find_by_client(e->window);
            if (!w)
                w = window_find(e->window);
            if (w)
                window_state_changed(w);
        }
        break;
    }
    case XCB_CLIENT_MESSAGE: {
        xcb_client_message_event_t *e = (xcb_client_message_event_t *)ev;
        /* `kicomp --toggle` asking us to stand down. Leaving the loop
         * rather than exiting here: the shutdown path below is what puts
         * the session back the way it was. */
        if (e->type == comp.atoms.kicomp_quit && comp.atoms.kicomp_quit != XCB_NONE) {
            comp_info("asked to quit; compositing off");
            comp.running = false;
        }
        break;
    }

    case XCB_EXPOSE:
        /* Something drew over the overlay (or it was just mapped): the
         * only correct answer is to present again. */
        output_damage_all();
        break;
    case 0: {
        xcb_generic_error_t *e = (xcb_generic_error_t *)ev;
        comp_log("X error %u (major %u minor %u, resource 0x%x)",
                 e->error_code, e->major_code, e->minor_code, e->resource_id);
        break;
    }
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* setup / teardown                                                    */
/* ------------------------------------------------------------------ */

static void shutdown_compositor(void)
{
    input_shutdown();
    effects_shutdown();
    density_shutdown();
    inputscale_shutdown();
    desktop_shutdown();
    windows_teardown();
    outputs_teardown();
    presenter_shutdown();
    renderer_shutdown();

    if (comp.overlay != XCB_NONE) {
        xcb_composite_release_overlay_window(comp.conn, comp.overlay);
        comp.overlay = XCB_NONE;
    }
    /* Hand the screen back to the server's own painting, so the session
     * continues exactly as it did before kicomp started (section 31). */
    xcb_composite_unredirect_subwindows(comp.conn, comp.root,
                                        XCB_COMPOSITE_REDIRECT_MANUAL);
    if (comp.cm_window != XCB_NONE)
        xcb_destroy_window(comp.conn, comp.cm_window);

    xcb_flush(comp.conn);
    xcb_disconnect(comp.conn);
}

static void usage(void)
{
    printf("kicomp " KICOMP_VERSION " - compositor for kiwm\n"
           "usage: kicomp [--replace] [--toggle] [--single-drawable]\n"
           "              [--skip-wm-layers]\n"
           "              [--effects|--no-effects] [--anim-ms=N]\n"
           "              [--renderer=NAME] [--presenter=NAME]\n"
           "              [-v|--verbose] [--version] [--help]\n"
           "\n"
           "  --replace          take over from a running compositor\n"
           "  --toggle           turn compositing off if a compositor is\n"
           "                     running, on if none is -- one key binding\n"
           "  --single-drawable  legacy mode: one drawable for the whole\n"
           "                     screen instead of one per output\n"
           "  --skip-wm-layers   don't composite kiwm's own overlay windows\n"
           "                     (_KIWM_LAYER: the switcher OSD, the\n"
           "                     move/resize wireframe)\n"
           "  --effects, --no-effects\n"
           "                     turn animations on/off (kicomp.conf: effects=)\n"
           "  --anim-ms=N        global animation unit in ms; every effect's\n"
           "                     duration is a multiple of it\n"
           "                     (kicomp.conf: animation_duration=)\n"
           "  --renderer=NAME    auto|xrender (kicomp.conf: renderer=)\n"
           "  --presenter=NAME   auto|present|copy (kicomp.conf: presenter=)\n"
           "\n"
           "kicomp is optional: kiwm is fully usable without it, and\n"
           "killing kicomp returns the session to the uncomposited path.\n");
}

int main(int argc, char **argv)
{
    bool replace = false;
    bool toggle = false;

    /* The file first, the command line second: an option always wins
     * over kicomp.conf. */
    config_load();

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--replace")) {
            replace = true;
        } else if (!strcmp(argv[i], "--toggle")) {
            toggle = true;
        } else if (!strcmp(argv[i], "--single-drawable")) {
            comp.single_drawable = true;
        } else if (!strcmp(argv[i], "--skip-wm-layers")) {
            comp.skip_wm_layers = true;
        } else if (!strcmp(argv[i], "--no-effects")) {
            comp.effects = false;
        } else if (!strcmp(argv[i], "--effects")) {
            comp.effects = true;
        } else if (!strncmp(argv[i], "--anim-ms=", 10)) {
            comp.anim_duration_ms = atof(argv[i] + 10);
        } else if (!strncmp(argv[i], "--presenter=", 12)) {
            snprintf(comp.presenter_name, sizeof(comp.presenter_name), "%s", argv[i] + 12);
        } else if (!strncmp(argv[i], "--renderer=", 11)) {
            snprintf(comp.renderer_name, sizeof(comp.renderer_name), "%s", argv[i] + 11);
        } else if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose")) {
            comp.verbose = true;
        } else if (!strcmp(argv[i], "--version")) {
            printf("kicomp " KICOMP_VERSION "\n");
            return 0;
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage();
            return 0;
        } else {
            fprintf(stderr, "kicomp: unknown option '%s'\n", argv[i]);
            usage();
            return 1;
        }
    }

    comp.conn = xcb_connect(NULL, &comp.screen_num);
    if (xcb_connection_has_error(comp.conn)) {
        fprintf(stderr, "kicomp: cannot connect to the X server\n");
        return 1;
    }

    const xcb_setup_t *setup = xcb_get_setup(comp.conn);
    xcb_screen_iterator_t it = xcb_setup_roots_iterator(setup);
    for (int i = 0; i < comp.screen_num; i++)
        xcb_screen_next(&it);
    comp.screen = it.data;
    comp.root = comp.screen->root;
    comp.root_w = comp.screen->width_in_pixels;
    comp.root_h = comp.screen->height_in_pixels;

    atoms_init();

    /* Before any capability is checked or anything is claimed: if this is
     * the "off" half of the toggle there is nothing to set up. */
    if (toggle && toggle_off()) {
        xcb_flush(comp.conn);
        xcb_disconnect(comp.conn);
        return 0;
    }

    if (!caps_detect()) {
        xcb_disconnect(comp.conn);
        return 1;
    }

    if (!acquire_selection(replace)) {
        xcb_disconnect(comp.conn);
        return 1;
    }

    /* Listen before scanning, so a window created during startup is
     * caught by the event rather than missed by both. */
    uint32_t root_mask = XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY |
                         XCB_EVENT_MASK_STRUCTURE_NOTIFY |
                         XCB_EVENT_MASK_PROPERTY_CHANGE |
                         XCB_EVENT_MASK_EXPOSURE;
    xcb_generic_error_t *err = xcb_request_check(comp.conn,
        xcb_change_window_attributes_checked(comp.conn, comp.root,
                                             XCB_CW_EVENT_MASK, &root_mask));
    if (err) {
        fprintf(stderr, "kicomp: cannot select root events (error %u)\n",
                err->error_code);
        free(err);
        xcb_disconnect(comp.conn);
        return 1;
    }

    if (comp.caps.randr)
        /* OUTPUT_PROPERTY as well as SCREEN_CHANGE: the per-output "DPI"
         * property is what decides an output's scale (output.c), and it
         * can be changed at any moment with `xrandr --set DPI`. Without
         * this the compositor would keep magnifying by yesterday's
         * factor until something else happened to rebuild the outputs. */
        xcb_randr_select_input(comp.conn, comp.root,
                               XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE |
                               XCB_RANDR_NOTIFY_MASK_OUTPUT_PROPERTY);

    err = xcb_request_check(comp.conn,
        xcb_composite_redirect_subwindows_checked(comp.conn, comp.root,
                                                  XCB_COMPOSITE_REDIRECT_MANUAL));
    if (err) {
        fprintf(stderr, "kicomp: another compositor already redirects "
                        "this screen (error %u)\n", err->error_code);
        free(err);
        xcb_disconnect(comp.conn);
        return 1;
    }

    if (!overlay_acquire()) {
        xcb_composite_unredirect_subwindows(comp.conn, comp.root,
                                            XCB_COMPOSITE_REDIRECT_MANUAL);
        xcb_disconnect(comp.conn);
        return 1;
    }

    /* One renderer so far, so "auto" and "xrender" land in the same place
     * -- but the choice is made here, by name, so that adding
     * renderer-gl.c is a line in this function and nothing else. */
    bool want_glx = (strcmp(comp.renderer_name, "glx") == 0);
    if (!want_glx && strcmp(comp.renderer_name, "auto") &&
        strcmp(comp.renderer_name, "xrender"))
        fprintf(stderr, "kicomp: no renderer named '%s'; using xrender\n",
                comp.renderer_name);

    /* `auto` stays on XRender: the GL backend is the newer one and does
     * not do everything the older one does yet (shadows, shape clipping,
     * the density layers), so it is asked for by name until it does. */
    renderer = want_glx ? renderer_glx() : renderer_xrender();

    /* Presenter: capability decides, name overrides. `auto` takes Present
     * when the server has it -- a frame that lands at vblank instead of
     * whenever the copy happens to reach the scanout is strictly better,
     * and it is the only one of the two that can say when the frame
     * actually appeared. `copy` is the fallback and the way to compare
     * the two. */
    if (want_glx) {
        /* With GL the frame *is* the drawable's back buffer, so the swap
         * is the presentation -- an X presenter has no pixmap of ours to
         * copy. The two come as a pair (presenter-glx.c). */
        if (strcmp(comp.presenter_name, "auto") &&
            strcmp(comp.presenter_name, "glx"))
            fprintf(stderr, "kicomp: the glx renderer presents its own frames; "
                            "ignoring presenter=%s\n", comp.presenter_name);
        presenter = presenter_glx();
    } else if (strcmp(comp.presenter_name, "present") == 0) {
        if (comp.caps.present) {
            presenter = presenter_present();
        } else {
            fprintf(stderr, "kicomp: no Present extension; using copy\n");
            presenter = presenter_copy();
        }
    } else if (strcmp(comp.presenter_name, "copy") == 0) {
        presenter = presenter_copy();
    } else {
        if (strcmp(comp.presenter_name, "auto"))
            fprintf(stderr, "kicomp: no presenter named '%s'; using auto\n",
                    comp.presenter_name);
        presenter = comp.caps.present ? presenter_present() : presenter_copy();
    }

    /* Before the capability line, because it is one of the capabilities
     * that line reports -- and because whether outputs may be scaled at
     * all depends on the answer (inputscale.h). */
    inputscale_init();

    comp_info("kicomp " KICOMP_VERSION " on %s screen %d (%dx%d)",
              getenv("DISPLAY") ? getenv("DISPLAY") : "?", comp.screen_num,
              comp.root_w, comp.root_h);
    comp_info("renderer=%s presenter=%s", renderer->name, presenter->name);
    comp_info("capabilities: composite=%d overlay=%d damage=%d xfixes=%d "
              "render=%d randr=%d present=%d input-scale=%d flip-per-crtc=%d",
              comp.caps.composite, comp.caps.overlay, comp.caps.damage,
              comp.caps.xfixes, comp.caps.render, comp.caps.randr,
              comp.caps.present, comp.caps.input_scale, comp.caps.flip_per_crtc);
    if (comp.skip_wm_layers)
        comp_info("skipping kiwm's own layers (_KIWM_LAYER)");
    if (comp_shadow.enabled)
        comp_info("shadows: radius %d/%d, opacity %.2f/%.2f, offset %+d%+d/%+d%+d "
                  "(focused/unfocused)",
                  comp_shadow.active.radius, comp_shadow.inactive.radius,
                  comp_shadow.active.opacity, comp_shadow.inactive.opacity,
                  comp_shadow.active.offset_x, comp_shadow.active.offset_y,
                  comp_shadow.inactive.offset_x, comp_shadow.inactive.offset_y);
    if (comp.effects) {
        /* Before the effects, so a module can arm a hotkey of its own as
         * it is created (effect.h's init). */
        input_init();
        effects_init();
    } else {
        comp_info("effects off");
    }

    /* Before the outputs are built: whether an output may be scaled at
     * all depends on this extension being there (inputscale.h). */
    density_init();

    /* Prints the drawable count/geometry itself, here and on every later
     * output change. */
    outputs_refresh();
    windows_scan();
    /* The windows that were already on screen never "appear", so this is
     * where they get asked for the density their output wants -- the
     * appear/move paths in window.c cover every later one. */
    density_update_all();
    /* Read once now so the *first* desktop switch has an old value to be
     * a change from; without it the wall would sit out the first one. */
    desktop_refresh();
    window_focus_changed(read_active_window());
    output_damage_all();

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    comp.running = true;
    int fd = xcb_get_file_descriptor(comp.conn);

    while (comp.running) {
        /* Drain everything queued, *then* paint once: a burst of damage
         * from one application costs one repaint, not one per event. */
        xcb_generic_event_t *ev;
        while ((ev = xcb_poll_for_event(comp.conn))) {
            handle_event(ev);
            free(ev);
        }

        /* Something is waiting to be classified, and the evidence may be
         * one event behind: a shade arrives as a ConfigureNotify plus the
         * _NET_WM_STATE that explains it, and if the property is read
         * before it lands the resize is classified as a plain move and
         * the wrong effect runs.
         *
         * A round trip pulls in everything the server has already
         * generated, which covers the case where both were sent and only
         * one had been read. It is not a guarantee -- the WM's request may
         * still be sitting unprocessed -- so the real fix lives in the WM,
         * which publishes state *before* the geometry that carries it out
         * (see kiwm's client.c). This is the best-effort half, for window
         * managers that don't. */
        if (windows_have_pending()) {
            free(xcb_get_input_focus_reply(comp.conn,
                                           xcb_get_input_focus(comp.conn), NULL));
            while ((ev = xcb_poll_for_event(comp.conn))) {
                handle_event(ev);
                free(ev);
            }
            /* And the same question asked of the desktop properties, by
             * reading them rather than by waiting for their PropertyNotify:
             * kiwm unmaps the outgoing windows *before* publishing the new
             * desktop (its output.c), so the unmaps can be classified a
             * beat before the property event that explains them -- which
             * is a desktop switch coming out as a window being closed, and
             * the wrong effect running on it. */
            desktop_refresh();
        }

        if (xcb_connection_has_error(comp.conn)) {
            comp_log("X connection lost");
            break;
        }

        /* The batch is complete: now it can be said what actually
         * happened to each window (window.c), and the effects get told in
         * those terms rather than in X's. */
        windows_flush_events();

        /* Everything that damaged itself since the last frame, collected
         * in one batch (damage.h): one round trip for the whole frame,
         * and only when something actually damaged itself. */
        damage_collect();

        double now = comp_now_ms();

        /* Animations advance on the wall clock, never on a frame count
         * (section 20). update() is also where an effect posts the damage
         * for what it is about to change, so it runs before the paint. */
        if (effects_active()) {
            effects_update(now);
            scheduler_tick(now);
        }

        paint_dirty_outputs(now);
        xcb_flush(comp.conn);

        if (!comp.running)
            break;

        /* Idle costs nothing: with nothing animating and nothing waiting
         * for its slot, this blocks until X has something to say
         * (section 38). While an animation runs, the timeout is the next
         * output's frame deadline -- each output on its own clock, none
         * waiting for another (section 19/49). */
        int timeout = scheduler_timeout(comp_now_ms());
        struct pollfd p = { fd, POLLIN, 0 };
        if (poll(&p, 1, timeout) < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
    }

    comp_log("shutting down");
    shutdown_compositor();
    return 0;
}
