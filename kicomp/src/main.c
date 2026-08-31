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
 *
 * Explicitly NOT here yet: effects, per-output frame clocks/pacing
 * (Fase 7), the XiS FLIP presenter (Fase 8), region-based repaint,
 * unredirect of a fullscreen output, and any kiwm<->kicomp IPC
 * (section 32) -- this version learns everything from plain X events, so
 * kiwm needs no changes at all to be composited, and killing kicomp
 * returns the session to the uncomposited path (section 31).
 */

#define _POSIX_C_SOURCE 200809L

#define KICOMP_VERSION "0.1.1"

#include "comp.h"
#include "output.h"
#include "window.h"
#include "scene.h"
#include "renderer.h"
#include "presenter.h"

#include <xcb/randr.h>
#include <xcb/shape.h>

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

    /* Present and per-CRTC FLIP are probed in Fase 7/8; declared false
     * here so no code path can accidentally believe in them. */
    comp.caps.present = false;
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

static void paint_dirty_outputs(void)
{
    bool painted = false;

    for (int i = 0; i < comp.output_count; i++) {
        CompOutput *o = &comp.outputs[i];
        if (!o->dirty || !o->target)
            continue;

        scene_build(&scene, o);
        comp_log("paint %s: %d node%s", o->name, scene.count,
                 scene.count == 1 ? "" : "s");

        renderer->begin(o);
        renderer->draw_scene(o, &scene);
        renderer->end(o);

        presenter->present(o, COMP_PRESENT_COPY);

        o->dirty = false;
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

    if (comp.caps.damage && type == comp.damage_event + XCB_DAMAGE_NOTIFY) {
        xcb_damage_notify_event_t *e = (xcb_damage_notify_event_t *)ev;
        CompWindow *w = window_find(e->drawable);
        if (w) {
            /* Acknowledge the region so the server will report the next
             * one; this prototype repaints the whole output anyway, so
             * the region itself is discarded (region-based repaint is a
             * later optimization, not an interface change). */
            xcb_damage_subtract(comp.conn, w->damage,
                                XCB_XFIXES_REGION_NONE, XCB_XFIXES_REGION_NONE);
            CompRect r = window_rect(w);
            output_damage_rect(&r);
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
        if (e->parent == comp.root)
            window_add_top(e->window);   /* reparenting stacks it on top */
        else
            window_remove(e->window);
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
        if (e->window == comp.root &&
            (e->atom == comp.atoms.xrootpmap_id ||
             e->atom == comp.atoms.esetroot_pmap_id)) {
            renderer_background_invalidate();
            output_damage_all();
        } else if (e->atom == comp.atoms.net_wm_window_opacity) {
            CompWindow *w = window_find(e->window);
            if (w)
                window_update_opacity(w);
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
           "usage: kicomp [--replace] [--single-drawable] [--skip-wm-layers]\n"
           "              [-v|--verbose] [--version] [--help]\n"
           "\n"
           "  --replace          take over from a running compositor\n"
           "  --single-drawable  legacy mode: one drawable for the whole\n"
           "                     screen instead of one per output\n"
           "  --skip-wm-layers   don't composite kiwm's own overlay windows\n"
           "                     (_KIWM_LAYER: the switcher OSD, the\n"
           "                     move/resize wireframe)\n"
           "\n"
           "kicomp is optional: kiwm is fully usable without it, and\n"
           "killing kicomp returns the session to the uncomposited path.\n");
}

int main(int argc, char **argv)
{
    bool replace = false;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--replace")) {
            replace = true;
        } else if (!strcmp(argv[i], "--single-drawable")) {
            comp.single_drawable = true;
        } else if (!strcmp(argv[i], "--skip-wm-layers")) {
            comp.skip_wm_layers = true;
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
        xcb_randr_select_input(comp.conn, comp.root,
                               XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE);

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

    renderer = renderer_xrender();
    presenter = presenter_copy();

    comp_info("kicomp " KICOMP_VERSION " on %s screen %d (%dx%d)",
              getenv("DISPLAY") ? getenv("DISPLAY") : "?", comp.screen_num,
              comp.root_w, comp.root_h);
    comp_info("renderer=%s presenter=%s", renderer->name, presenter->name);
    comp_info("capabilities: composite=%d overlay=%d damage=%d xfixes=%d "
              "render=%d randr=%d present=%d flip-per-crtc=%d",
              comp.caps.composite, comp.caps.overlay, comp.caps.damage,
              comp.caps.xfixes, comp.caps.render, comp.caps.randr,
              comp.caps.present, comp.caps.flip_per_crtc);
    if (comp.skip_wm_layers)
        comp_info("skipping kiwm's own layers (_KIWM_LAYER)");

    /* Prints the drawable count/geometry itself, here and on every later
     * output change. */
    outputs_refresh();
    windows_scan();
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

        if (xcb_connection_has_error(comp.conn)) {
            comp_log("X connection lost");
            break;
        }

        paint_dirty_outputs();
        xcb_flush(comp.conn);

        if (!comp.running)
            break;

        /* Idle: block here. No timers, no polling, no rendering when
         * nothing changed (section 38). Fase 7 replaces this with the
         * per-output frame clocks, which is where a timeout appears --
         * and only while an animation is actually running. */
        struct pollfd p = { fd, POLLIN, 0 };
        if (poll(&p, 1, -1) < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
    }

    comp_log("shutting down");
    shutdown_compositor();
    return 0;
}
