/* osd.c - themed on-screen overlays for Alt+Tab-style window switching and
 * Meta+Tab-style per-output desktop switching.
 *
 * Both work the same way: the first Tab press of a hold opens a themed
 * override-redirect window (chrome styled like the decoration -- same
 * background/border colors and corner radius, no compositor needed, same
 * XCB SHAPE clipping trick client.c's frames already use) and actively
 * grabs the keyboard (xcb_grab_keyboard()) for as long as it stays open --
 * every further KeyPress of the same Tab (any client's focus is bypassed
 * while grabbed, exactly like a real Alt+Tab) just moves the highlighted
 * selection, and nothing is actually focused/switched-to until the
 * driving modifier key itself is *released* (osd_handle_key_release(),
 * reached only because of the grab -- a passive xcb_grab_key() alone
 * never sees a bare modifier release). Escape cancels without committing.
 * wm.osd_enabled=0 skips all of this and falls back to kiwm's original
 * immediate-switch-on-every-Tab behavior.
 *
 * The window-switcher is built behind a small TabBoxOps vtable (see
 * osd.h) on purpose -- kwin's tabbox is the same idea, a swappable
 * "model+view" for Alt+Tab separate from the hold/release mechanics here.
 * Only one implementation exists today (simple_list_tabbox_ops, a plain
 * vertical list); a future grid/thumbnail presentation is a new TabBoxOps
 * and a one-line change to active_tabbox_ops, nothing else. The desktop
 * switcher has no such vtable -- it's a single fixed pager-style grid
 * (xispanel's pager widget, same idea: squares proportional to the real
 * output resolution, current one highlighted), not asked to be swappable.
 */
#include "osd.h"
#include "decoration.h"
#include "client.h"
#include "output.h"
#include "outline.h"

#include <cairo/cairo-xcb.h>
#include <xcb/shape.h>

#include <stdio.h>
#include <stdlib.h>

#define OSD_PAD           18
#define OSD_ROW_H         34
#define OSD_ICON_SIZE     22
#define OSD_LIST_W        340
#define OSD_DESK_ROW_H    64
#define OSD_DESK_GAP       8

typedef enum { OSD_NONE, OSD_WINDOWS, OSD_DESKTOPS } OsdKind;

static OsdKind kind = OSD_NONE;
static int osd_output = -1;

static xcb_window_t osd_win = XCB_NONE;
static bool osd_mapped = false;

/* Whether this hold took the pointer grab (see grab_for_hold()) -- it
 * doesn't when a move/resize drag is already holding one of its own, and
 * closing must then leave that drag's grab alone. */
static bool pointer_grabbed = false;

/* Window-switcher state -- see TabBoxOps in osd.h. */
static TabBoxState tb_state;
static const TabBoxOps *active_tabbox_ops = &simple_list_tabbox_ops;

/* Desktop-switcher state. */
static int desk_n = 0;
static int desk_selected = 0;
static double desk_aspect = 1.0;

/* What was actually focused/current *before* this hold started -- restored
 * by osd_cancel() (Escape), and also what wm.osd_live_preview's every-step
 * apply needs to revert to if the client it last previewed gets destroyed
 * mid-hold (see osd_client_destroyed()). original_desktop is only
 * meaningful while kind == OSD_DESKTOPS. */
static Client *original_focused = NULL;
static int original_desktop = 0;

/* Which output a *newly opening* overlay belongs to -- polled once right
 * when the hold starts (osd_windows_step()/osd_desktops_step() only ever
 * call output_for_effects() from their "not already open" branch) and then
 * fixed for the whole hold via osd_output, same as everything else about
 * an open overlay. The policy itself (focused window's output vs. the
 * pointer's, kiwm.conf's osd_output_follows_pointer=) lives in output.c,
 * shared with the non-overlay effects that must agree with it -- see
 * output_for_effects(). */

/* ---- window-switcher TabBoxOps: simple vertical list ---- */

static void list_build(TabBoxState *state, int output_idx, int desktop)
{
    state->count = 0;
    state->selected = 0;
    /* Same eligibility rule client.c's cycle_focus() uses. */
    for (Client *c = wm.clients; c && state->count < MAX_CLIENTS; c = c->next) {
        /* skip_taskbar is the client saying it isn't a window the user
         * switches to (VirtualBox's mini-toolbar, splash-ish helpers) --
         * listing it here would offer a switch target that does nothing
         * useful. */
        /* Minimized windows are listed too -- switching to one is the
         * most ordinary reason to reach for Alt+Tab, and leaving them out
         * makes the list disagree with the taskbar about what exists.
         * Committing to one restores it (activate_client()). What stays
         * out is a window that isn't on this output/desktop at all, and
         * one the client itself says isn't a switch target. */
        if (c->output == output_idx && (c->sticky || c->desktop == desktop) &&
            (c->mapped || c->minimized) && !c->skip_taskbar) {
            state->items[state->count++] = c;
        }
    }

    /* Most-recently-used order (kiwm.conf's osd_order=mru): X has no focus
     * history to read -- EWMH stops at _NET_CLIENT_LIST_STACKING, which is
     * stacking order and only resembles use order while click-to-focus
     * raises everything -- so this sorts on kiwm's own record of it (see
     * Client::last_focus_serial). Insertion sort: the list is one output's
     * worth of windows, and it's built once per hold. */
    if (wm.osd_mru_order) {
        for (int i = 1; i < state->count; i++) {
            Client *item = state->items[i];
            int j = i - 1;
            while (j >= 0 && state->items[j]->last_focus_serial < item->last_focus_serial) {
                state->items[j + 1] = state->items[j];
                j--;
            }
            state->items[j + 1] = item;
        }
    }

    /* The hold starts on whatever is focused, so the first Tab step lands
     * on the next entry -- which in MRU order is the previously used
     * window, the flip-between-two behavior every desktop has. */
    for (int i = 0; i < state->count; i++) {
        if (state->items[i] == wm.focused) {
            state->selected = i;
            break;
        }
    }
}

static void list_measure(const TabBoxState *state, int *out_w, int *out_h)
{
    *out_w = OSD_LIST_W;
    *out_h = (state->count > 0 ? state->count : 1) * OSD_ROW_H;
}

static void list_paint(cairo_t *cr, const TabBoxState *state, int w, int h)
{
    (void)h;
    double fg_r, fg_g, fg_b;
    if (wm.have_theme_colors) {
        fg_r = wm.fg_active_r; fg_g = wm.fg_active_g; fg_b = wm.fg_active_b;
    } else {
        fg_r = wm.deco_fg_r; fg_g = wm.deco_fg_g; fg_b = wm.deco_fg_b;
    }

    for (int i = 0; i < state->count; i++) {
        double y = i * OSD_ROW_H;
        if (i == state->selected) {
            cairo_set_source_rgba(cr, fg_r, fg_g, fg_b, 0.2);
            cairo_rectangle(cr, 0, y, w, OSD_ROW_H);
            cairo_fill(cr);
        }

        Client *c = state->items[i];
        double icon_x = 6;
        double text_x = icon_x + OSD_ICON_SIZE + 10;

        if (c->icon) {
            int iw = cairo_image_surface_get_width(c->icon);
            int ih = cairo_image_surface_get_height(c->icon);
            if (iw > 0 && ih > 0) {
                cairo_save(cr);
                cairo_translate(cr, icon_x, y + (OSD_ROW_H - OSD_ICON_SIZE) / 2.0);
                cairo_scale(cr, (double)OSD_ICON_SIZE / iw, (double)OSD_ICON_SIZE / ih);
                cairo_set_source_surface(cr, c->icon, 0, 0);
                cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
                cairo_paint(cr);
                cairo_restore(cr);
            }
        }

        /* Minimized entries are dimmed, the same "this one isn't on
         * screen right now" cue a taskbar gives them. */
        cairo_set_source_rgba(cr, fg_r, fg_g, fg_b, c->minimized ? 0.55 : 1.0);
        pango_show_text_boxed(cr, text_x, y, OSD_ROW_H, w - text_x - 6, wm.title_font_size + 1,
                              c->title[0] ? c->title : "(untitled)", false, NULL);
    }
}

const TabBoxOps simple_list_tabbox_ops = {
    .name = "simple_list",
    .build = list_build,
    .measure = list_measure,
    .paint = list_paint,
};

/* ---- desktop-switcher: fixed pager-style grid ---- */

static void paint_desktop_grid(cairo_t *cr, int w, int h)
{
    (void)w;
    int row_h = h;
    int bw = (int)(row_h * desk_aspect + 0.5);
    if (bw < 1)
        bw = 1;

    double fg_r, fg_g, fg_b;
    if (wm.have_theme_colors) {
        fg_r = wm.fg_active_r; fg_g = wm.fg_active_g; fg_b = wm.fg_active_b;
    } else {
        fg_r = wm.deco_fg_r; fg_g = wm.deco_fg_g; fg_b = wm.deco_fg_b;
    }

    for (int i = 0; i < desk_n; i++) {
        double x = i * (bw + OSD_DESK_GAP);
        if (i == desk_selected) {
            cairo_set_source_rgba(cr, fg_r, fg_g, fg_b, 0.25);
            cairo_rectangle(cr, x, 0, bw, row_h);
            cairo_fill(cr);
        }
        cairo_set_source_rgba(cr, fg_r, fg_g, fg_b, 0.6);
        cairo_set_line_width(cr, 1.5);
        cairo_rectangle(cr, x + 1, 1, bw - 2, row_h - 2);
        cairo_stroke(cr);

        char label[16];
        snprintf(label, sizeof(label), "%d", i + 1);
        cairo_set_source_rgba(cr, fg_r, fg_g, fg_b, 1.0);
        pango_show_text_boxed(cr, x, 0, row_h, bw, wm.title_font_size + 2, label, true, NULL);
    }
}

/* ---- shared chrome + window management ---- */

static void ensure_osd_window(void)
{
    if (osd_win != XCB_NONE)
        return;

    osd_win = xcb_generate_id(wm.conn);
    uint32_t values[] = { wm.screen->black_pixel, 1, XCB_EVENT_MASK_EXPOSURE };
    xcb_create_window(wm.conn, wm.screen->root_depth, osd_win, wm.root,
                      -10, -10, 10, 10, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, wm.screen->root_visual,
                      XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK, values);
}

/* Sizes/positions/reshapes/repaints osd_win for `content_w`x`content_h`
 * (osd.c's own OSD_PAD border added around it), centered on osd_output,
 * then hands off to `paint_content` (already translated so 0,0 is the
 * content area's own top-left corner) to draw whatever kind of OSD is
 * currently open. Same "off-screen pixmap, one atomic xcb_copy_area() at
 * the end" pattern decoration.c's draw_decoration() uses, to avoid any
 * layer-by-layer flicker. */
static void draw_chrome_and_content(int content_w, int content_h, void (*paint_content)(cairo_t *, int, int))
{
    ensure_osd_window();

    int win_w = content_w + OSD_PAD * 2;
    int win_h = content_h + OSD_PAD * 2;

    XisOutput *o = (osd_output >= 0 && osd_output < wm.output_count) ? &wm.outputs[osd_output] : NULL;
    int win_x = o ? o->x + (o->width - win_w) / 2 : 0;
    int win_y = o ? o->y + (o->height - win_h) / 2 : 0;

    uint32_t geo[] = { (uint32_t)win_x, (uint32_t)win_y, (uint32_t)win_w, (uint32_t)win_h,
                       XCB_STACK_MODE_ABOVE };
    xcb_configure_window(wm.conn, osd_win,
                         XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                         XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT |
                         XCB_CONFIG_WINDOW_STACK_MODE, geo);

    if (wm.shape_ext_present) {
        bool square = (wm.radius_tl == 0 && wm.radius_tr == 0 && wm.radius_br == 0 && wm.radius_bl == 0);
        if (square) {
            xcb_shape_mask(wm.conn, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_BOUNDING, osd_win, 0, 0, XCB_PIXMAP_NONE);
        } else {
            xcb_rectangle_t rects[2 * MAX_CORNER_RADIUS + 1];
            int n = build_rounded_rects(win_w, win_h, wm.radius_tl, wm.radius_tr, wm.radius_br, wm.radius_bl,
                                       rects, (int)(sizeof(rects) / sizeof(rects[0])));
            xcb_shape_rectangles(wm.conn, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_BOUNDING, XCB_CLIP_ORDERING_Y_SORTED,
                                 osd_win, 0, 0, (uint32_t)n, rects);
        }
    }

    if (!osd_mapped) {
        xcb_map_window(wm.conn, osd_win);
        osd_mapped = true;
    }
    osd_raise_above_all();

    xcb_pixmap_t pixmap = xcb_generate_id(wm.conn);
    xcb_create_pixmap(wm.conn, wm.screen->root_depth, pixmap, osd_win, (uint16_t)win_w, (uint16_t)win_h);
    cairo_surface_t *surface = cairo_xcb_surface_create(wm.conn, pixmap, wm.visual, win_w, win_h);
    cairo_t *cr = cairo_create(surface);

    double bg_r, bg_g, bg_b, border_r, border_g, border_b;
    if (wm.have_theme_colors) {
        bg_r = wm.bg_active_r; bg_g = wm.bg_active_g; bg_b = wm.bg_active_b;
        border_r = wm.border_active_r; border_g = wm.border_active_g; border_b = wm.border_active_b;
    } else {
        bg_r = wm.deco_bg_r; bg_g = wm.deco_bg_g; bg_b = wm.deco_bg_b;
        border_r = wm.deco_fg_r; border_g = wm.deco_fg_g; border_b = wm.deco_fg_b;
    }

    cairo_set_source_rgb(cr, bg_r, bg_g, bg_b);
    cairo_paint(cr);

    cairo_set_source_rgb(cr, border_r, border_g, border_b);
    cairo_set_line_width(cr, 1.5);
    cairo_rectangle(cr, 0.75, 0.75, win_w - 1.5, win_h - 1.5);
    cairo_stroke(cr);

    cairo_save(cr);
    cairo_translate(cr, OSD_PAD, OSD_PAD);
    paint_content(cr, content_w, content_h);
    cairo_restore(cr);

    cairo_destroy(cr);
    cairo_surface_destroy(surface);

    xcb_copy_area(wm.conn, pixmap, osd_win, wm.deco_gc, 0, 0, 0, 0, (uint16_t)win_w, (uint16_t)win_h);
    xcb_free_pixmap(wm.conn, pixmap);
    xcb_flush(wm.conn);
}

static void paint_windows_wrapper(cairo_t *cr, int w, int h)
{
    active_tabbox_ops->paint(cr, &tb_state, w, h);
}

static void paint_desktops_wrapper(cairo_t *cr, int w, int h)
{
    paint_desktop_grid(cr, w, h);
}

static void repaint_windows(void)
{
    int cw, ch;
    active_tabbox_ops->measure(&tb_state, &cw, &ch);
    draw_chrome_and_content(cw, ch, paint_windows_wrapper);
}

static void repaint_desktops(void)
{
    int bw = (int)(OSD_DESK_ROW_H * desk_aspect + 0.5);
    if (bw < 1)
        bw = 1;
    int cw = desk_n > 0 ? desk_n * bw + (desk_n - 1) * OSD_DESK_GAP : bw;
    draw_chrome_and_content(cw, OSD_DESK_ROW_H, paint_desktops_wrapper);
}

static void close_osd(void)
{
    outline_hide();
    if (osd_win != XCB_NONE && osd_mapped) {
        xcb_unmap_window(wm.conn, osd_win);
        osd_mapped = false;
    }
    xcb_ungrab_keyboard(wm.conn, XCB_CURRENT_TIME);
    if (pointer_grabbed) {
        xcb_ungrab_pointer(wm.conn, XCB_CURRENT_TIME);
        pointer_grabbed = false;
    }
    kind = OSD_NONE;
    osd_output = -1;
    original_focused = NULL;
    xcb_flush(wm.conn);
}

/* Applies whatever the overlay is currently offering (focus the selected
 * window / switch to the selected desktop) and closes it -- what releasing
 * the driving modifier does, shared with every other way a hold can end
 * (osd_poll_release(), osd_handle_button_press()). */
static void commit_and_close(void)
{
    if (kind == OSD_WINDOWS) {
        if (tb_state.count > 0 && tb_state.selected >= 0 && tb_state.selected < tb_state.count)
            activate_client(tb_state.items[tb_state.selected]);
    } else if (kind == OSD_DESKTOPS) {
        if (osd_output >= 0)
            switch_workspace(osd_output, desk_selected);
    }
    close_osd();
}

/* Grabs keyboard *and* pointer for the duration of a hold.
 *
 * The keyboard grab is what makes a bare modifier release reach
 * osd_handle_key_release() at all (see the file comment). The pointer grab
 * is a safety net for the case where that release never arrives: a client
 * that grabs the input devices itself while the OSD is up -- VirtualBox
 * capturing input for its guest is the one that actually does this -- can
 * swallow the modifier's release, leaving the overlay stuck on screen with
 * kiwm still holding the keyboard. Any mouse button then ends the hold
 * (osd_handle_button_press()), and the click itself is replayed to whoever
 * would normally have received it -- which is why the pointer is grabbed
 * in SYNC mode: only a synchronous grab can hand the event back with
 * xcb_allow_events(XCB_ALLOW_REPLAY_POINTER).
 *
 * Neither grab is checked for success: both can legitimately fail when
 * another client already holds an active grab, and the overlay must still
 * open and still be escapable. The periodic osd_poll_release() (main.c's
 * event loop) is the backstop that closes it even if *no* input event ever
 * reaches kiwm again. */
static void grab_for_hold(void)
{
    xcb_grab_keyboard_reply_t *kb = xcb_grab_keyboard_reply(wm.conn,
        xcb_grab_keyboard(wm.conn, 0, wm.root, XCB_CURRENT_TIME,
                          XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC), NULL);
    /* ...but never on top of a move/resize drag's own pointer grab
     * (events.c's begin_drag()). Taking a second grab would replace it --
     * same client, so the server allows it -- and closing the overlay
     * would then ungrab entirely, leaving the drag with no grab at all
     * halfway through. This is not a corner case: switching desktops with
     * a window in hand, so it travels along (output.c's switch_workspace),
     * is exactly that gesture. The drag's grab already delivers every
     * button event to kiwm anyway, so nothing is lost by leaving it be. */
    xcb_grab_pointer_reply_t *ptr = NULL;
    if (wm.drag_mode == DRAG_NONE) {
        ptr = xcb_grab_pointer_reply(wm.conn,
            xcb_grab_pointer(wm.conn, 0, wm.root, XCB_EVENT_MASK_BUTTON_PRESS,
                             XCB_GRAB_MODE_SYNC, XCB_GRAB_MODE_ASYNC,
                             XCB_NONE, XCB_NONE, XCB_CURRENT_TIME), NULL);
        pointer_grabbed = (ptr && ptr->status == XCB_GRAB_STATUS_SUCCESS);
    }

    /* Both replies are read (rather than firing the requests off blind)
     * only so a failure can be said out loud: a grab that didn't happen is
     * exactly the situation the polling backstop exists for, and knowing
     * which one failed is the difference between "the overlay closed a
     * fraction of a second late" and a mystery. Failure is not fatal --
     * the overlay opens either way. */
    if (kb && kb->status != XCB_GRAB_STATUS_SUCCESS)
        fprintf(stderr, "kiwm: osd: keyboard grab failed (status %u) -- "
                        "falling back to polling for the modifier release\n", kb->status);
    if (ptr && ptr->status != XCB_GRAB_STATUS_SUCCESS)
        fprintf(stderr, "kiwm: osd: pointer grab failed (status %u) -- "
                        "a click won't close the overlay\n", ptr->status);
    else
        /* An *active* grab in SYNC mode freezes the pointer from the
         * moment it's taken -- no pointer event of any kind is generated
         * again, not even to the grabbing client, until it says otherwise.
         * (That's unlike a passive xcb_grab_button(), where the press that
         * activates the grab is delivered first and the freeze starts
         * after it.) So kiwm has to explicitly let event processing
         * continue: SyncPointer runs it normally until the next button
         * press, and freezes again right there -- which is precisely the
         * state osd_handle_button_press() needs, since only an event the
         * freeze is still holding can be replayed. */
        xcb_allow_events(wm.conn, XCB_ALLOW_SYNC_POINTER, XCB_CURRENT_TIME);
    free(kb);
    free(ptr);
}

/* ---- public API ---- */

bool osd_owns_window(xcb_window_t window)
{
    return osd_win != XCB_NONE && window == osd_win && osd_mapped;
}

void osd_raise_above_all(void)
{
    if (osd_win == XCB_NONE || !osd_mapped)
        return;
    /* Mapping a window doesn't restack it, so opening the overlay needs
     * this one explicit raise; from then on it holds its place through
     * LAYER_OSD like everything else restack_all() orders (see wm.h --
     * it's an override-redirect window, not a Client, so restack_all()
     * recognizes it via osd_owns_window() rather than a Client lookup). */
    xcb_configure_window(wm.conn, osd_win, XCB_CONFIG_WINDOW_STACK_MODE,
                         (uint32_t[]){ XCB_STACK_MODE_ABOVE });
}

void osd_handle_expose(xcb_window_t window)
{
    if (!osd_owns_window(window))
        return;
    /* The overlay paints itself once into the window and then relies on
     * the X server keeping those pixels -- which it doesn't, for any
     * region that gets covered and uncovered again. Without repainting on
     * Expose the overlay is left blank or holding whatever was underneath
     * it, which is what "the OSD gets corrupted" looks like when a
     * fullscreen window drops out of the layer above it. */
    if (kind == OSD_WINDOWS)
        repaint_windows();
    else if (kind == OSD_DESKTOPS)
        repaint_desktops();
    xcb_flush(wm.conn);
}

bool osd_active(void)
{
    return kind != OSD_NONE;
}

void osd_windows_step(int direction)
{
    if (!wm.osd_enabled) {
        cycle_focus(direction);
        return;
    }
    if (kind == OSD_DESKTOPS)
        return; /* the other OSD is open -- shouldn't happen, different mod */

    int output_idx = output_for_effects();
    if (output_idx < 0)
        return;
    int desktop = wm.outputs[output_idx].desktop;

    if (kind != OSD_WINDOWS) {
        active_tabbox_ops->build(&tb_state, output_idx, desktop);
        if (tb_state.count == 0)
            return; /* nothing to switch to -- don't open an empty OSD */
        kind = OSD_WINDOWS;
        osd_output = output_idx;
        original_focused = wm.focused;
        /* Active grab so osd_handle_key_release() sees mod_cycle's own
         * release regardless of which client (if any) has input focus --
         * a passive xcb_grab_key() alone only ever fires for the exact
         * key+modifier combo it was registered for (Tab here), never for
         * a bare release of the modifier key by itself. */
        grab_for_hold();
    }

    tb_state.selected = (tb_state.selected + direction + tb_state.count) % tb_state.count;
    if (wm.osd_live_preview) {
        /* activate_client(), not focus_client(): the list includes
         * minimized windows, and one of those has to be restored before
         * there's anything to focus. */
        activate_client(tb_state.items[tb_state.selected]);
    } else {
        /* Without live preview nothing is raised or focused until the
         * hold ends, so the list alone doesn't say *where* the
         * highlighted window is -- especially when it's buried or on
         * another part of a big desktop. The outline says it (xfwm does
         * the same), and sits in its own layer just below this overlay so
         * the two never cover each other. */
        /* Minimized entries are outlined too, at the geometry they had
         * when they went away -- kiwm never loses it (minimizing just
         * unmaps the frame), and it's exactly where the window will come
         * back if this is the one committed to. See client.c's
         * minimize_client(). */
        Client *sel = tb_state.items[tb_state.selected];
        outline_show(sel->x, sel->y, sel->frame_width, sel->frame_height);
    }
    repaint_windows();
}

void osd_desktops_step(int direction)
{
    if (!wm.osd_enabled) {
        cycle_output_desktop(direction);
        return;
    }
    if (kind == OSD_WINDOWS)
        return;

    int output_idx = output_for_effects();
    if (output_idx < 0 || wm.output_count == 0)
        return;

    if (kind != OSD_DESKTOPS) {
        kind = OSD_DESKTOPS;
        osd_output = output_idx;
        desk_n = wm.num_desktops;
        original_desktop = wm.outputs[output_idx].desktop;
        desk_selected = original_desktop;
        desk_aspect = wm.outputs[output_idx].height > 0
                          ? (double)wm.outputs[output_idx].width / wm.outputs[output_idx].height
                          : 1.0;
        grab_for_hold();
    }

    if (desk_n > 0)
        desk_selected = (desk_selected + direction + desk_n) % desk_n;
    if (wm.osd_live_preview)
        switch_workspace(osd_output, desk_selected);
    repaint_desktops();
}

void osd_cancel(void)
{
    if (kind == OSD_NONE)
        return;
    /* Only meaningful when osd_live_preview already applied intermediate
     * steps live -- otherwise nothing was ever actually switched/focused
     * yet, so there's nothing to revert. */
    if (wm.osd_live_preview) {
        if (kind == OSD_WINDOWS) {
            if (original_focused)
                activate_client(original_focused);
        } else {
            switch_workspace(osd_output, original_desktop);
        }
    }
    close_osd();
}

/* Whether the keyboard's *current* state (not just the one event just
 * received) still has `mod`'s bit set -- i.e. whether the modifier driving
 * the open OSD is genuinely still held down. Checked instead of comparing
 * ev->detail against a specific hardcoded modifier keycode: a keyboard
 * layout can carry a given modifier mask on more than one physical key
 * (Alt_L/Alt_R, or a remapped key via xmodmap/setxkbmap), and X's own
 * autorepeat can also fire a real KeyRelease+KeyPress pair for a held-down
 * modifier key on some setups -- either would make a fixed-keycode
 * comparison miss the actual release, or fire on a repeat that isn't a
 * real release at all, leaving osd.c's keyboard grab stuck engaged (every
 * further keystroke system-wide swallowed into a wedged, invisible-once-
 * closed OSD session) until something else forces it shut. Querying the
 * live state instead means it doesn't matter *which* key carries the
 * modifier or whether this specific event was a real release: the OSD
 * only ever commits once the bit is actually gone. */
static bool modifier_still_held(uint16_t mod)
{
    xcb_query_pointer_reply_t *qp = xcb_query_pointer_reply(wm.conn, xcb_query_pointer(wm.conn, wm.root), NULL);
    if (!qp)
        return true; /* can't tell -- assume still held rather than commit on a guess */
    bool held = (qp->mask & mod) != 0;
    free(qp);
    return held;
}

void osd_handle_key_release(xcb_key_release_event_t *ev)
{
    (void)ev;
    osd_poll_release();
}

void osd_poll_release(void)
{
    if (kind == OSD_NONE)
        return;

    uint16_t mod = (kind == OSD_WINDOWS) ? wm.mod_cycle : wm.mod_control;
    if (modifier_still_held(mod))
        return;

    commit_and_close();
}

bool osd_handle_button_press(xcb_button_press_event_t *ev)
{
    (void)ev;
    if (kind == OSD_NONE || !pointer_grabbed)
        return false; /* a drag owns the pointer -- see grab_for_hold() */

    /* Any click ends the hold, exactly as if the modifier had been let go
     * (see grab_for_hold()). The click is then replayed so it still does
     * whatever it was going to do -- raise/focus a window, press a button
     * in a client -- which needs the replay *before* the grab is dropped,
     * hence doing it here rather than letting close_osd()'s plain
     * xcb_ungrab_pointer() discard the frozen event. */
    xcb_allow_events(wm.conn, XCB_ALLOW_REPLAY_POINTER, XCB_CURRENT_TIME);
    commit_and_close();
    return true;
}

void osd_client_destroyed(Client *c)
{
    if (original_focused == c)
        original_focused = NULL;

    if (kind != OSD_WINDOWS)
        return;

    for (int i = 0; i < tb_state.count; i++) {
        if (tb_state.items[i] != c)
            continue;

        for (int j = i; j < tb_state.count - 1; j++)
            tb_state.items[j] = tb_state.items[j + 1];
        tb_state.count--;
        if (tb_state.selected > i)
            tb_state.selected--;
        else if (tb_state.selected >= tb_state.count)
            tb_state.selected = tb_state.count - 1;

        if (tb_state.count == 0)
            close_osd();
        else
            repaint_windows();
        return;
    }
}
