/*
 * toast.c - notification toast popups, see the doc comment in xispanel.h.
 *
 * Each toast is its own small override-redirect window (same pattern as
 * tooltip.c/menu.c's popups: XMatchVisualInfo for a 32-bit ARGB visual if
 * one's available, falling back to the default visual/opaque background
 * otherwise), stacked bottom-up in the bottom-right corner of the default
 * screen. Fixed compact layout (icon + two ellipsized text lines, plus a
 * row of action buttons when the sender gave any) rather
 * than word-wrapping the body -- keeps sizing/positioning trivial, matches
 * how tooltip.c already treats a window title (one ellipsized line, not a
 * paragraph layout).
 *
 * g_toasts is kept as a dense array in arrival order (oldest first, newest
 * last) so restacking after a dismissal/expiry is just "shift the tail
 * down one slot" -- reposition_all() then recomputes every window's screen
 * position from its array index alone.
 */
#include "xispanel.h"

#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <cairo/cairo-xlib.h>

#include <stdio.h>
#include <string.h>
#include <strings.h>

/* Hard upper bound on the array itself -- the *actual* number shown at
 * once is computed dynamically from the usable screen height (see
 * toast_fits() below), never a fixed count. This is just generous
 * headroom no real screen height could plausibly exceed. */
#define TOAST_ARRAY_CAP 32
#define TOAST_W 320
#define TOAST_H 72
#define TOAST_PAD 12
#define TOAST_ICON 40
#define TOAST_GAP 8
#define TOAST_MARGIN 16
#define TOAST_SUMMARY_SIZE 14.0
#define TOAST_BODY_SIZE 12.0
/* Action buttons (Notify()'s actions[], minus "default"): one row under
 * the text, which grows the toast by TOAST_BTN_ROW. */
#define TOAST_BTN_H 26
#define TOAST_BTN_GAP 6
#define TOAST_BTN_SIZE 12.0
#define TOAST_BTN_ROW (TOAST_BTN_H + TOAST_GAP)
/* Close "X" in the top-right corner: square hit box, dismisses without
 * invoking the default action. */
#define TOAST_CLOSE 18

/* Default lifetime for an OSD toast (toast_show_osd()) that doesn't pass
 * its own timeout_ms -- shorter than a DBus notification's own ~5s
 * default (notifd.c's own substitute for a sender's -1), since an OSD is
 * meant to confirm something the user just did (a scroll, a hotkey) and
 * should get out of the way quickly rather than linger like a message
 * someone else sent. */
#define TOAST_OSD_DEFAULT_MS 2500

/* A job only gets a toast once it has run this long, so a quick copy
 * doesn't flash one (kifiles waits about as long for its own dialog). */
#define TOAST_JOB_DELAY_MS 1000

typedef struct {
    /* 0 for an OSD toast (toast_show_osd()) -- notifd.c's own ids start
     * at 1 (see g_next_id in notifd.c), so 0 doubles as "not a real
     * notification, don't call notifd_mark_read() for this one". */
    unsigned int notif_id;
    Window win;
    cairo_surface_t *surface;
    cairo_t *cr;
    /* X-DENSITY (density.c) for this toast's own window -- see
     * toast_density_paint()/g_density_toast's doc comment above for why
     * this array being memmove()'d on dismissal is safe (the pointer
     * itself just rides along) while a `Toast *` baked into it would not
     * be. */
    DensityLayer *density;
    /* Rounded-corner state (panel_round_corners()): whether win currently
     * carries an XShape mask, and whether it should instead be rounded by
     * clipping the painted content to a real alpha edge -- see
     * draw_toast()'s use of the latter. Same pair as Panel::shaped/
     * corner_alpha_clip, one per toast since each is its own window. */
    int shaped;
    int alpha_clip;
    uint64_t expire_ms; /* 0 = never auto-expire */
    /* Original requested lifetime in ms (0 = never-expire), kept alongside
     * expire_ms so a LeaveNotify can restart the full countdown rather
     * than just resuming from wherever it was paused -- see hovered
     * below. */
    uint64_t timeout_ms;
    /* 1 while the pointer sits inside this toast -- toast_tick() skips
     * the expiry check entirely for a hovered toast, so parking the mouse
     * on one keeps it on screen indefinitely; leaving restarts expire_ms
     * from `now + timeout_ms` (a fresh full countdown, not a resume). */
    int hovered;
    char app_name[NOTIFD_APP_NAME_MAX];
    char summary[NOTIFD_SUMMARY_MAX];
    char body[NOTIFD_BODY_MAX];
    /* Borrowed from notifd.c's ring entry (a DBus arrival) or from
     * whatever the toast_show_osd() caller already resolved (an OSD) --
     * either way, not owned/freed here. For a DBus arrival this is safe
     * because a toast's own ~5s default lifetime is always far shorter
     * than it'd take NOTIFD_MAX (50) more notifications to arrive and
     * evict the entry this pointer came from. */
    cairo_surface_t *icon;
    /* -1 = no level bar (every DBus-arrived toast). 0-100 draws one under
     * the text, for volume/brightness-style OSDs -- see toast_show_osd(). */
    int level;
    /* Stored but not yet acted on -- see toast_show_osd()'s doc comment
     * in xispanel.h. Reserved for a later pass that colors the toast (or
     * plays a sound) differently for ToastUrgency's CRITICAL/LOW ends. */
    ToastUrgency urgency;
    /* "" for every DBus-arrived toast (never coalesced -- each Notify()
     * is its own message). An OSD toast with a non-empty tag reuses an
     * already-showing toast with the same tag in place (see
     * toast_show_osd()) instead of stacking a new one -- volume changing
     * three times in a second from a scroll wheel should update one
     * popup, not stack three. */
    char tag[32];
    int h; /* TOAST_H, plus TOAST_BTN_ROW when it has action buttons */
    /* Copied from the NotifEntry (DBus arrivals only); buttons are every
     * action but "default", which is the click on the body instead. */
    NotifAction actions[NOTIFD_ACTIONS_MAX];
    int n_actions;
    int hover_btn; /* index into actions[] under the pointer, -1 = none */
    int hover_close; /* pointer over the close X */
    /* A running job's progress (jobviewd.c), 0 otherwise: never expires,
     * follows the job (see sync_jobs()), and its buttons act on the job
     * instead of dismissing anything. */
    unsigned int job_id;
    unsigned int job_serial; /* JobEntry::serial last painted */
    cairo_surface_t *own_icon; /* job toasts resolve their own icon: freed with the toast */
    /* toast_show_action(): one button that calls this instead of anything
     * D-Bus; the body and the X just dismiss. NULL for every other toast. */
    void (*action_fn)(void *ctx);
    void *action_ctx;
} Toast;

static Toast g_toasts[TOAST_ARRAY_CAP];
static int g_n = 0;
static ToastCorner g_corner = TOAST_CORNER_BOTTOM_RIGHT;

/* The rect toasts are confined to -- set by widgets/notif.c's init() to
 * its own panel's output, minus that panel's own dock strip (see
 * toast_set_output_rect()'s doc comment). area_w == 0 means "never set",
 * in which case toast_area() falls back to the whole default screen. */
static int g_area_x = 0, g_area_y = 0, g_area_w = 0, g_area_h = 0;

/* The xisserve window (launcher / network / audio / ... pages -- one
 * toplevel for all of them) while it's on screen, so toasts can stay out
 * of its way like they do a panel. Cached: toast_area() only reads these;
 * toast_avoid_refresh() is the one place that talks to the server, and
 * only while a toast is showing (or one is arriving). g_av_sig is a hash
 * of the last client list scanned, so an absent window isn't re-searched
 * (a class-hint round trip per client) until the list actually changes. */
static Window g_av_win = None;
static int g_av_x, g_av_y, g_av_w, g_av_h;
static unsigned long g_av_sig;

/* Panel theme colors, mirrored from widgets/notif.c's own panel via
 * toast_set_colors() -- defaults match the original hardcoded look
 * (dark, near-opaque background / white text) until a notif widget
 * actually sets them. */
static double g_bg_r = 0.12, g_bg_g = 0.12, g_bg_b = 0.12, g_bg_a = 0.92;
static double g_fg_r = 1.0, g_fg_g = 1.0, g_fg_b = 1.0, g_fg_a = 1.0;

/* Borrowed from the owning panel (not owned/freed here -- see
 * toast_set_bg_image()'s doc comment); NULL means "no bitmap theme, use
 * the flat g_bg_* color above". */
static cairo_surface_t *g_bg_image = NULL;
/* Borrowed exactly like g_bg_image (owned by the Panel, re-synced from
 * notif.c's on_tick): the theme's popup frame, preferred over the panel
 * background image when a theme ships one. NULL = none. */
static const PanelSkin *g_skin = NULL;
static int g_bg_slice_l = 0, g_bg_slice_t = 0, g_bg_slice_r = 0, g_bg_slice_b = 0;

/* Mirrors the owning panel's tooltip_toast_padding_extra (see that field's
 * doc comment in xispanel.h) -- added on top of TOAST_PAD when positioning
 * the icon/text inside the TOAST_W x Toast::h box, so a toast's
 * content can be pulled in from the box's edges independently of the
 * panel's own spacing. */
static int g_pad_extra = 0;

/* Mirrors the owning panel's border_radius=, same borrowed-from-notif.c's
 * on_tick pattern as everything else above -- see toast_set_border_
 * radius(). 0 = square, the original look. */
static int g_border_radius = 0;

/* The owning panel itself, borrowed under the same rules as g_skin, for
 * its theme's text look -- font_weight=, title_shadow= and title_outline=
 * (see pango_show_text_boxed()). NULL = plain text, no notif widget yet. */
static const Panel *g_text_panel = NULL;

static Visual *g_visual = NULL;
static int g_depth = 0;
static Colormap g_cmap = None;
static int g_visual_ready = 0;

static void ensure_visual(void)
{
    if (g_visual_ready) {
        return;
    }
    g_visual_ready = 1;
    XVisualInfo vinfo;
    if (XMatchVisualInfo(g_dpy, g_screen, 32, TrueColor, &vinfo)) {
        g_visual = vinfo.visual;
        g_depth = vinfo.depth;
        g_cmap = XCreateColormap(g_dpy, g_root, g_visual, AllocNone);
    } else {
        g_visual = DefaultVisual(g_dpy, g_screen);
        g_depth = DefaultDepth(g_dpy, g_screen);
        g_cmap = DefaultColormap(g_dpy, g_screen);
    }
}

static int av_find_window(void)
{
    Window *list = NULL;
    int n = 0;
    if (!ewmh_get_client_list(&list, &n)) {
        return 0;
    }
    unsigned long sig = (unsigned long)n;
    for (int i = 0; i < n; i++) {
        sig = sig * 31 + (unsigned long)list[i];
    }
    if (sig == g_av_sig) {
        XFree(list);
        return 0;
    }
    g_av_sig = sig;
    for (int i = 0; i < n; i++) {
        XClassHint ch;
        if (!XGetClassHint(g_dpy, list[i], &ch)) {
            continue;
        }
        int hit = (ch.res_name && !strcasecmp(ch.res_name, "xisserve")) ||
                  (ch.res_class && !strcasecmp(ch.res_class, "xisserve"));
        if (ch.res_name) {
            XFree(ch.res_name);
        }
        if (ch.res_class) {
            XFree(ch.res_class);
        }
        if (hit) {
            g_av_win = list[i];
            break;
        }
    }
    XFree(list);
    return g_av_win != None;
}

/* Re-reads the xisserve window's on-screen rect into the cache; returns
 * whether it changed. */
static int av_refresh(void)
{
    int x = 0, y = 0, w = 0, h = 0;
    XWindowAttributes wa = {0};
    wa.map_state = IsUnmapped;
    if (g_av_win == None && !av_find_window()) {
        goto done;
    }
    if (!XGetWindowAttributes(g_dpy, g_av_win, &wa)) {
        g_av_win = None; /* gone */
        g_av_sig = 0;
        goto done;
    }
    if (wa.map_state != IsViewable) {
        goto done; /* hidden: keep the id, it comes back */
    }
    Window child;
    XTranslateCoordinates(g_dpy, g_av_win, DefaultRootWindow(g_dpy), 0, 0, &x, &y, &child);
    w = wa.width;
    h = wa.height;
done:
    if (x == g_av_x && y == g_av_y && w == g_av_w && h == g_av_h) {
        return 0;
    }
    g_av_x = x;
    g_av_y = y;
    g_av_w = w;
    g_av_h = h;
    return 1;
}

/* The rect toasts are confined to: whatever widgets/notif.c last set via
 * toast_set_output_rect() (its own panel's *output*, not the whole
 * possibly-multi-monitor screen -- see that function's doc comment),
 * falling back to the whole default screen if no notif widget has set
 * one yet. */
static void toast_area(int *out_x, int *out_y, int *out_w, int *out_h)
{
    if (g_area_w > 0 && g_area_h > 0) {
        *out_x = g_area_x;
        *out_y = g_area_y;
        *out_w = g_area_w;
        *out_h = g_area_h;
    } else {
        *out_x = 0;
        *out_y = 0;
        *out_w = DisplayWidth(g_dpy, g_screen);
        *out_h = DisplayHeight(g_dpy, g_screen);
    }
    /* Keep clear of the xisserve window when it's docked against one
     * side of the area (the usual popup-from-a-panel spot): shrink the
     * area from whichever side it hugs. A floating/dragged window away
     * from every edge is left to the user. */
    if (g_av_w > 0 && g_av_h > 0) {
        int ax = *out_x, ay = *out_y, aw = *out_w, ah = *out_h;
        int rx = g_av_x, ry = g_av_y, rr = g_av_x + g_av_w, rb = g_av_y + g_av_h;
        if (rr > ax && rx < ax + aw && rb > ay && ry < ay + ah) {
            int gap[4] = {rx - ax, ax + aw - rr, ry - ay, ay + ah - rb}; /* L R T B */
            int e = 0;
            for (int i = 1; i < 4; i++) {
                if (gap[i] < gap[e]) {
                    e = i;
                }
            }
            if (gap[e] <= 24) {
                if (e == 0) {
                    *out_x = rr;
                    *out_w = ax + aw - rr;
                } else if (e == 1) {
                    *out_w = rx - ax;
                } else if (e == 2) {
                    *out_y = rb;
                    *out_h = ay + ah - rb;
                } else {
                    *out_h = ry - ay;
                }
                if (*out_w < 1 || *out_h < 1) {
                    *out_x = ax;
                    *out_y = ay;
                    *out_w = aw;
                    *out_h = ah;
                }
            }
        }
    }
}

/* Whether one more toast `h` tall fits stacked in the current
 * toast_area() alongside every toast already showing -- see the file
 * comment on why this replaces a fixed count. A lone toast always fits,
 * even in a tiny/unusual area, same "never just silently show nothing"
 * spirit as the rest of xispanel's degrade-gracefully helpers. */
static int toast_fits(int h)
{
    if (g_n == 0) {
        return 1;
    }
    if (g_n >= TOAST_ARRAY_CAP) {
        return 0;
    }
    int ax, ay, aw, ah;
    toast_area(&ax, &ay, &aw, &ah);
    (void)ax;
    (void)ay;
    (void)aw;
    int used = h;
    for (int i = 0; i < g_n; i++) {
        used += g_toasts[i].h + TOAST_GAP;
    }
    return used <= ah - 2 * TOAST_MARGIN;
}

/* Screen position for the toast currently at array index `idx` (0 =
 * oldest) -- the newest toast (index g_n-1) sits closest to g_corner,
 * older ones stacked away from it. Positioned within toast_area(), not
 * the raw screen, so toasts stay on the right output and never sit under
 * a panel. Horizontal anchor (left/center/right) and vertical anchor
 * (top/center/bottom) are independent; the vertical anchor also decides
 * stack direction -- top stacks downward, bottom stacks upward, center
 * stacks downward from the vertical midpoint (an arbitrary but
 * unambiguous default, same as top's). */
static void toast_screen_pos(int idx, int *out_x, int *out_y)
{
    int step = 0; /* room taken by the newer toasts, closer to the corner */
    for (int i = idx + 1; i < g_n; i++) {
        step += g_toasts[i].h + TOAST_GAP;
    }
    int h = g_toasts[idx].h;
    int ax, ay, aw, ah;
    toast_area(&ax, &ay, &aw, &ah);

    int at_left = (g_corner == TOAST_CORNER_TOP_LEFT || g_corner == TOAST_CORNER_CENTER_LEFT ||
                   g_corner == TOAST_CORNER_BOTTOM_LEFT);
    int at_right = (g_corner == TOAST_CORNER_TOP_RIGHT || g_corner == TOAST_CORNER_CENTER_RIGHT ||
                    g_corner == TOAST_CORNER_BOTTOM_RIGHT);
    int at_top = (g_corner == TOAST_CORNER_TOP_LEFT || g_corner == TOAST_CORNER_TOP_CENTER ||
                  g_corner == TOAST_CORNER_TOP_RIGHT);
    int at_bottom = (g_corner == TOAST_CORNER_BOTTOM_LEFT || g_corner == TOAST_CORNER_BOTTOM_CENTER ||
                     g_corner == TOAST_CORNER_BOTTOM_RIGHT);

    if (at_left) {
        *out_x = ax + TOAST_MARGIN;
    } else if (at_right) {
        *out_x = ax + aw - TOAST_MARGIN - TOAST_W;
    } else {
        *out_x = ax + (aw - TOAST_W) / 2;
    }

    int base_y = at_top      ? ay + TOAST_MARGIN
                 : at_bottom ? ay + ah - TOAST_MARGIN - h
                             : ay + (ah - g_toasts[g_n - 1].h) / 2;
    *out_y = at_bottom ? base_y - step : base_y + step;
}

/* Left edge of the text column -- also where the button row starts. */
static double toast_text_x(const Toast *t)
{
    int pad = TOAST_PAD + g_pad_extra;
    return t->icon ? pad * 2 + TOAST_ICON : pad;
}

/* Rect of the action button for actions[k], 0 if actions[k] has none
 * ("default", or out of range). Buttons split the text column's width
 * evenly, in actions[] order. */
static int toast_button_rect(const Toast *t, int k, double *x, double *y, double *w, double *h)
{
    if (k < 0 || k >= t->n_actions || !strcmp(t->actions[k].key, "default")) {
        return 0;
    }
    int n = 0, pos = 0;
    for (int i = 0; i < t->n_actions; i++) {
        if (strcmp(t->actions[i].key, "default") != 0) {
            if (i == k) {
                pos = n;
            }
            n++;
        }
    }
    int pad = TOAST_PAD + g_pad_extra;
    double x0 = toast_text_x(t);
    double bw = (TOAST_W - pad - x0 - TOAST_BTN_GAP * (n - 1)) / n;
    *x = x0 + pos * (bw + TOAST_BTN_GAP);
    *y = TOAST_H - TOAST_GAP / 2.0;
    *w = bw;
    *h = TOAST_BTN_H;
    return 1;
}

/* Close X rect; 0 for level-bar OSD toasts, which have nothing to dismiss. */
static int toast_close_rect(const Toast *t, double *x, double *y)
{
    if (t->level >= 0 && !t->job_id) {
        return 0;
    }
    *x = TOAST_W - TOAST_CLOSE - 4;
    *y = 4;
    return 1;
}

static int toast_close_at(const Toast *t, int px, int py)
{
    double x, y;
    return toast_close_rect(t, &x, &y) && px >= x && px < x + TOAST_CLOSE && py >= y && py < y + TOAST_CLOSE;
}

static int toast_button_at(const Toast *t, int px, int py)
{
    for (int k = 0; k < t->n_actions; k++) {
        double x, y, w, h;
        if (toast_button_rect(t, k, &x, &y, &w, &h) && px >= x && px < x + w && py >= y && py < y + h) {
            return k;
        }
    }
    return -1;
}

/* The actual content draw, parameterized on `cr` so it can target either
 * t->cr (the normal on-screen path, scale 1) or a density layer's own
 * offscreen img_cr (X-DENSITY, see toast_density_paint() below) -- same
 * split as tooltip.c's draw_popup(). */
static void draw_toast(cairo_t *cr, Toast *t)
{
    cairo_save(cr);
    /* Unconditional, regardless of alpha_clip this frame: t->cr/img_cr are
     * reused across repaints, so a corner XShape was rounding last time
     * (compositor off, say) can still hold opaque pixels a clip only
     * holds *new* painting back from -- see panel_paint_content()'s own
     * doc comment for the same reasoning. */
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    /* Real alpha instead of XShape when there's a compositor to render it
     * (panel_round_corners()) -- covers the whole rest of this function,
     * not just the background, so the border stroke comes out hugging the
     * curve on its own and nothing (icon, text, level bar) can bleed past
     * it into a corner that's supposed to be transparent. */
    if (t->alpha_clip && g_border_radius > 0) {
        int r = g_border_radius;
        int max_r = (TOAST_W < t->h ? TOAST_W : t->h) / 2;
        if (r > max_r) {
            r = max_r;
        }
        panel_trace_rounded_rect(cr, TOAST_W, t->h, r);
        cairo_clip(cr);
    }

    if (g_skin && panel_draw_skin(g_skin, cr, SKIN_NORMAL, 0, 0, TOAST_W, t->h)) {
        /* themed popup frame -- see g_skin */
    } else if (g_bg_image) {
        int sw = cairo_image_surface_get_width(g_bg_image);
        int sh = cairo_image_surface_get_height(g_bg_image);
        panel_draw_9slice(cr, g_bg_image, sw, sh, g_bg_slice_l, g_bg_slice_t, g_bg_slice_r, g_bg_slice_b, TOAST_W,
                           t->h);
    } else {
        cairo_set_source_rgba(cr, g_bg_r, g_bg_g, g_bg_b, g_bg_a);
        cairo_paint(cr);
    }

    cairo_set_source_rgba(cr, g_fg_r, g_fg_g, g_fg_b, 0.15);
    cairo_set_line_width(cr, 1);
    cairo_rectangle(cr, 0.5, 0.5, TOAST_W - 1, t->h - 1);
    cairo_stroke(cr);

    int pad = TOAST_PAD + g_pad_extra;
    double text_x = pad;
    double text_w = TOAST_W - pad * 2;
    if (t->icon) {
        draw_icon_scaled(cr, t->icon, pad, (TOAST_H - TOAST_ICON) / 2.0, TOAST_ICON);
        text_x = pad * 2 + TOAST_ICON;
        text_w = TOAST_W - text_x - pad;
    }
    double cx, cy;
    int has_close = toast_close_rect(t, &cx, &cy);
    double title_w = has_close ? text_w - TOAST_CLOSE : text_w;

    cairo_set_source_rgba(cr, g_fg_r, g_fg_g, g_fg_b, g_fg_a);
    const char *summary = t->summary[0] ? t->summary : t->app_name;
    pango_show_text_boxed(cr, text_x, pad - 2, TOAST_SUMMARY_SIZE + 6, title_w, TOAST_SUMMARY_SIZE, summary, NULL,
                           g_text_panel);
    if (t->body[0]) {
        cairo_set_source_rgba(cr, g_fg_r, g_fg_g, g_fg_b, g_fg_a * 0.75);
        pango_show_text_boxed(cr, text_x, pad - 2 + TOAST_SUMMARY_SIZE + 6, TOAST_BODY_SIZE + 6, text_w,
                               TOAST_BODY_SIZE, t->body, NULL, g_text_panel);
    }

    /* Level bar (volume/brightness-style OSD only -- see toast_show_osd()).
     * Sits along the bottom of the padded area regardless of whether body
     * text is present: an OSD toast has no body in practice, and the rare
     * combination isn't worth a layout special case for. */
    if (t->level >= 0) {
        double bar_h = 4;
        double bar_y = TOAST_H - pad - bar_h; /* above a job toast's buttons */
        int level = t->level > 100 ? 100 : t->level;
        cairo_set_source_rgba(cr, g_fg_r, g_fg_g, g_fg_b, 0.25);
        cairo_rectangle(cr, text_x, bar_y, text_w, bar_h);
        cairo_fill(cr);
        cairo_set_source_rgba(cr, g_fg_r, g_fg_g, g_fg_b, 0.95);
        cairo_rectangle(cr, text_x, bar_y, text_w * (level / 100.0), bar_h);
        cairo_fill(cr);
    }

    if (has_close) {
        if (t->hover_close) {
            cairo_set_source_rgba(cr, g_fg_r, g_fg_g, g_fg_b, 0.2);
            cairo_save(cr);
            cairo_translate(cr, cx, cy);
            panel_trace_rounded_rect(cr, TOAST_CLOSE, TOAST_CLOSE, 4);
            cairo_fill(cr);
            cairo_restore(cr);
        }
        cairo_set_source_rgba(cr, g_fg_r, g_fg_g, g_fg_b, t->hover_close ? 0.95 : 0.55);
        cairo_set_line_width(cr, 1.5);
        double m = 5.5;
        cairo_move_to(cr, cx + m, cy + m);
        cairo_line_to(cr, cx + TOAST_CLOSE - m, cy + TOAST_CLOSE - m);
        cairo_move_to(cr, cx + TOAST_CLOSE - m, cy + m);
        cairo_line_to(cr, cx + m, cy + TOAST_CLOSE - m);
        cairo_stroke(cr);
    }

    for (int k = 0; k < t->n_actions; k++) {
        double bx, by, bw, bh;
        if (!toast_button_rect(t, k, &bx, &by, &bw, &bh)) {
            continue;
        }
        cairo_set_source_rgba(cr, g_fg_r, g_fg_g, g_fg_b, k == t->hover_btn ? 0.28 : 0.12);
        cairo_save(cr);
        cairo_translate(cr, bx, by);
        panel_trace_rounded_rect(cr, (int)bw, (int)bh, 4);
        cairo_fill(cr);
        cairo_restore(cr);
        double tw;
        pango_text_extents_panel(cr, t->actions[k].label, TOAST_BTN_SIZE, bw - 8, &tw, NULL, g_text_panel);
        cairo_set_source_rgba(cr, g_fg_r, g_fg_g, g_fg_b, g_fg_a);
        pango_show_text_boxed(cr, bx + (bw - tw) / 2.0, by, bh, bw - 8, TOAST_BTN_SIZE, t->actions[k].label, NULL,
                               g_text_panel);
    }

    cairo_restore(cr);
}

/* Set right before density_layer_render() below, for the duration of that
 * one synchronous call -- g_toasts[] is a dense array that shifts entries
 * down on a dismissal (see the file doc comment), so a `Toast *` baked
 * into the DensityLayer at registration time could outlive the slot it
 * pointed to. Reading the always-current `t` through this global instead
 * (density_layer_register()'s `ctx` stays NULL) sidesteps that: the
 * paint callback only ever runs inside this one call, never stored for
 * later, so there's nothing to go stale. */
static Toast *g_density_toast;

static void toast_density_paint(cairo_t *cr, double scale, void *ctx)
{
    (void)ctx;
    cairo_save(cr);
    cairo_scale(cr, scale, scale);
    draw_toast(cr, g_density_toast);
    cairo_restore(cr);
}

static void paint_toast(Toast *t)
{
    draw_toast(t->cr, t);
    g_density_toast = t;
    density_layer_render(t->density, TOAST_W, t->h, TOAST_SUMMARY_SIZE);
    g_density_toast = NULL;
}

static void destroy_toast_window(Toast *t)
{
    if (t->own_icon) {
        cairo_surface_destroy(t->own_icon);
    }
    if (t->cr) {
        cairo_destroy(t->cr);
    }
    if (t->surface) {
        cairo_surface_destroy(t->surface);
    }
    density_layer_unregister(t->density);
    if (t->win) {
        XDestroyWindow(g_dpy, t->win);
    }
    memset(t, 0, sizeof(*t));
}

static void reposition_all(void)
{
    for (int i = 0; i < g_n; i++) {
        int x, y;
        toast_screen_pos(i, &x, &y);
        XMoveWindow(g_dpy, g_toasts[i].win, x, y);
    }
    XFlush(g_dpy);
}

/* Jobs whose toast went away (closed, or pushed out by newer toasts): their
 * progress stays in xisserve's --notifications page instead of coming back
 * here on the next update. Job ids only grow, so a small ring is enough. */
static unsigned int g_hidden_jobs[JOBVIEWD_MAX];
static int g_hidden_next = 0;

static int job_hidden(unsigned int id)
{
    for (int i = 0; i < JOBVIEWD_MAX; i++) {
        if (g_hidden_jobs[i] == id) {
            return 1;
        }
    }
    return 0;
}

/* Removes the toast at array index `idx`, shifting the tail down and
 * restacking everyone still visible. */
static void remove_toast(int idx)
{
    if (idx < 0 || idx >= g_n) {
        return;
    }
    if (g_toasts[idx].job_id && !job_hidden(g_toasts[idx].job_id)) {
        g_hidden_jobs[g_hidden_next] = g_toasts[idx].job_id;
        g_hidden_next = (g_hidden_next + 1) % JOBVIEWD_MAX;
    }
    destroy_toast_window(&g_toasts[idx]);
    for (int i = idx; i < g_n - 1; i++) {
        g_toasts[i] = g_toasts[i + 1];
    }
    g_n--;
    memset(&g_toasts[g_n], 0, sizeof(g_toasts[g_n]));
    reposition_all();
}

/* A toast going away on its own (expired, clicked, pushed out): tells
 * notifd, which emits NotificationClosed -- a no-op for an OSD (id 0). */
static void drop_toast(int idx, int reason)
{
    notifd_closed(g_toasts[idx].notif_id, reason);
    remove_toast(idx);
}

static int find_toast_by_notif(unsigned int id)
{
    for (int i = 0; id && i < g_n; i++) {
        if (g_toasts[i].notif_id == id) {
            return i;
        }
    }
    return -1;
}

/* notifd asking for its popup to go (CloseNotification(), an entry
 * removed from the history) -- it reports the reason itself. */
static void toast_close_notif(unsigned int id)
{
    int i = find_toast_by_notif(id);
    if (i >= 0) {
        remove_toast(i);
    }
}

/* Common half of showing any toast, DBus-arrived or OSD: evicts enough
 * oldest-first room for one more (see the loop's own comment) and hands
 * back a zeroed slot for the caller to fill in. Doesn't map/paint the
 * window itself -- see map_and_show_toast() below -- since the two
 * callers fill different fields (notif_id/app_name for a real
 * notification vs. level/urgency for an OSD) before that part runs. */
static Toast *claim_toast_slot(int h)
{
    ensure_visual();
    av_refresh();
    while (!toast_fits(h)) {
        drop_toast(0, NOTIF_CLOSED_EXPIRED); /* drop the oldest visible toast to make room */
    }
    Toast *t = &g_toasts[g_n];
    memset(t, 0, sizeof(*t));
    t->level = -1; /* caller overrides for an OSD with a level bar */
    t->h = h;
    t->hover_btn = -1;
    return t;
}

/* Creates `t`'s window, paints it, and restacks everyone else -- the
 * part every toast needs regardless of where its content came from.
 * `t` must be the slot claim_toast_slot() just returned (i.e. still at
 * g_toasts[g_n]); this is what actually advances g_n. */
static void map_and_show_toast(Toast *t)
{
    g_n++; /* toast_screen_pos() positions by index within the *new* total
             * count, so this new toast counts itself -- incrementing first
             * makes toast_screen_pos(g_n - 1, ...) below identical to what
             * reposition_all() computes for the same slot right after. */

    int x, y;
    toast_screen_pos(g_n - 1, &x, &y);

    XSetWindowAttributes attrs;
    memset(&attrs, 0, sizeof(attrs));
    attrs.override_redirect = True;
    attrs.colormap = g_cmap;
    attrs.border_pixel = 0;
    attrs.background_pixel = 0;
    /* PropertyChangeMask: needed for _X_DENSITY_REQUESTED (density.c). */
    attrs.event_mask =
        ExposureMask | ButtonPressMask | EnterWindowMask | LeaveWindowMask | PropertyChangeMask | PointerMotionMask;

    t->win = XCreateWindow(g_dpy, g_root, x, y, TOAST_W, (unsigned)t->h, 0, g_depth, InputOutput, g_visual,
                            CWOverrideRedirect | CWColormap | CWBorderPixel | CWBackPixel | CWEventMask, &attrs);
    XChangeProperty(g_dpy, t->win, g_atom_wm_window_type, XA_ATOM, 32, PropModeReplace,
                     (unsigned char *)&g_atom_wm_window_type_tooltip, 1);
    t->alpha_clip = panel_round_corners(t->win, TOAST_W, t->h, g_border_radius, g_depth, &t->shaped);
    t->surface = cairo_xlib_surface_create(g_dpy, t->win, g_visual, TOAST_W, t->h);
    t->cr = cairo_create(t->surface);
    t->density = density_layer_register(t->win, g_visual, g_depth, toast_density_paint, NULL);

    XMapWindow(g_dpy, t->win);
    XRaiseWindow(g_dpy, t->win);
    paint_toast(t);

    /* Every already-visible toast was positioned as "closest to the
     * corner" at its *own* arrival time -- this new one just took that
     * slot, so everyone else needs to shift away from the corner one step.
     * reposition_all() recomputes every window's position from scratch off
     * the current g_n/array order, so it's simplest to just always call it
     * here rather than only moving the older ones by hand. */
    reposition_all();
}

static void toast_on_arrived(const NotifEntry *e, int expire_timeout_ms)
{
    int buttons = 0;
    for (int i = 0; i < e->n_actions; i++) {
        buttons += strcmp(e->actions[i].key, "default") != 0;
    }
    int h = buttons ? TOAST_H + TOAST_BTN_ROW : TOAST_H;
    /* replaces_id (see notifd.c's ring_append()): same id, still showing
     * -- refresh that toast in place instead of stacking a new one. */
    int existing = find_toast_by_notif(e->id);
    Toast *t = existing >= 0 ? &g_toasts[existing] : claim_toast_slot(h);
    t->notif_id = e->id;
    t->hover_btn = -1;
    memcpy(t->actions, e->actions, sizeof(t->actions));
    t->n_actions = e->n_actions;
    snprintf(t->app_name, sizeof(t->app_name), "%s", e->app_name);
    snprintf(t->summary, sizeof(t->summary), "%s", e->summary);
    snprintf(t->body, sizeof(t->body), "%s", e->body);
    t->icon = e->icon;
    t->urgency = TOAST_URGENCY_NORMAL;
    /* expire_timeout_ms is already resolved by notifd.c (see the
     * NotifArrivedFn doc comment in xispanel.h) -- widgets/notif.c's
     * timeout= config only ever changes what notifd.c substitutes for a
     * sender that didn't request its own, so a negative value should
     * never actually reach here, but treat it the same as 0 (immediate
     * default) rather than trust it blindly. */
    t->timeout_ms = expire_timeout_ms > 0 ? (uint64_t)expire_timeout_ms : 0;
    t->expire_ms = t->timeout_ms != 0 ? now_ms() + t->timeout_ms : 0;
    if (existing < 0) {
        map_and_show_toast(t);
        return;
    }
    if (t->h != h) {
        t->h = h;
        XResizeWindow(g_dpy, t->win, TOAST_W, (unsigned)h);
        cairo_xlib_surface_set_size(t->surface, TOAST_W, h);
        t->alpha_clip = panel_round_corners(t->win, TOAST_W, h, g_border_radius, g_depth, &t->shaped);
        reposition_all();
    }
    paint_toast(t);
    XFlush(g_dpy);
}

/* Finds an already-showing OSD toast with this exact tag, or -1 -- "" (no
 * tag) never matches anything, including another untagged toast, so
 * every untagged toast_show_osd() call keeps the old "always a new
 * toast" behavior. */
static int find_toast_by_tag(const char *tag)
{
    if (!tag || !tag[0]) {
        return -1;
    }
    for (int i = 0; i < g_n; i++) {
        if (!strcmp(g_toasts[i].tag, tag)) {
            return i;
        }
    }
    return -1;
}

void toast_show_osd(cairo_surface_t *icon, const char *summary, const char *body, int level, ToastUrgency urgency,
                     int timeout_ms, const char *tag)
{
    int existing = find_toast_by_tag(tag);
    /* Same field-filling either branch takes -- only what happens
     * around it differs (reuse the window in place vs. create one). */
    Toast *t = existing >= 0 ? &g_toasts[existing] : claim_toast_slot(TOAST_H);
    /* t->notif_id stays 0 -- see its own doc comment on the Toast struct. */
    snprintf(t->summary, sizeof(t->summary), "%s", summary ? summary : "");
    snprintf(t->body, sizeof(t->body), "%s", body ? body : "");
    t->icon = icon;
    t->level = level < 0 ? -1 : level;
    t->urgency = urgency;
    t->timeout_ms = (uint64_t)(timeout_ms > 0 ? timeout_ms : TOAST_OSD_DEFAULT_MS);
    t->expire_ms = now_ms() + t->timeout_ms;

    if (existing >= 0) {
        /* Reused in place: same window, same stack position (no
         * restacking -- see this function's own doc comment in
         * xispanel.h), just a fresh paint and a fresh countdown. Cheaper
         * than the new-toast path too: no window (re)creation. */
        paint_toast(t);
        XFlush(g_dpy);
        return;
    }
    snprintf(t->tag, sizeof(t->tag), "%s", tag ? tag : "");
    map_and_show_toast(t);
}

void toast_show_action(cairo_surface_t *icon, const char *summary, const char *body, int timeout_ms,
                        const char *action_label, void (*action_fn)(void *ctx), void *action_ctx)
{
    Toast *t = claim_toast_slot(TOAST_H + TOAST_BTN_ROW);
    snprintf(t->summary, sizeof(t->summary), "%s", summary ? summary : "");
    snprintf(t->body, sizeof(t->body), "%s", body ? body : "");
    t->icon = icon;
    t->urgency = TOAST_URGENCY_NORMAL;
    t->timeout_ms = (uint64_t)(timeout_ms > 0 ? timeout_ms : TOAST_OSD_DEFAULT_MS);
    t->expire_ms = now_ms() + t->timeout_ms;
    snprintf(t->actions[0].key, sizeof(t->actions[0].key), "act");
    snprintf(t->actions[0].label, sizeof(t->actions[0].label), "%s", action_label ? action_label : "");
    t->n_actions = 1;
    t->action_fn = action_fn;
    t->action_ctx = action_ctx;
    map_and_show_toast(t);
}

/* ---- job progress (jobviewd.c) ------------------------------------------ */

static int find_toast_by_job(unsigned int id)
{
    for (int i = 0; id && i < g_n; i++) {
        if (g_toasts[i].job_id == id) {
            return i;
        }
    }
    return -1;
}

static const JobEntry *find_job(unsigned int id)
{
    for (int i = 0; i < jobviewd_count(); i++) {
        const JobEntry *e = jobviewd_get(i);
        if (e && e->id == id) {
            return e;
        }
    }
    return NULL;
}

static void format_rate(uint64_t bps, char *out, size_t out_sz)
{
    const char *units[] = {"B/s", "KiB/s", "MiB/s", "GiB/s"};
    double v = (double)bps;
    int u = 0;
    while (v >= 1024.0 && u < 3) {
        v /= 1024.0;
        u++;
    }
    snprintf(out, out_sz, u ? "%.1f %s" : "%.0f %s", v, units[u]);
}

static void fill_job_toast(Toast *t, const JobEntry *e)
{
    t->job_serial = e->serial;
    snprintf(t->app_name, sizeof(t->app_name), "%s", e->app_name);
    snprintf(t->summary, sizeof(t->summary), "%s", e->title[0] ? e->title : e->app_name);
    const char *what = e->info[0] ? e->info : e->value1;
    if (e->suspended) {
        snprintf(t->body, sizeof(t->body), "%s", _("Pausado"));
    } else if (e->speed) {
        char rate[32];
        format_rate(e->speed, rate, sizeof(rate));
        snprintf(t->body, sizeof(t->body), "%.200s%s%s", what, what[0] ? "  \xc2\xb7  " : "", rate);
    } else {
        snprintf(t->body, sizeof(t->body), "%s", what);
    }
    if (e->percent >= 0) {
        t->level = e->percent;
    } else if (e->total_bytes) {
        t->level = (int)(e->processed_bytes * 100 / e->total_bytes);
    } else {
        t->level = -1;
    }
    t->n_actions = 0;
    if (e->suspendable) {
        NotifAction *a = &t->actions[t->n_actions++];
        snprintf(a->key, sizeof(a->key), "%s", e->suspended ? "resume" : "suspend");
        snprintf(a->label, sizeof(a->label), "%s", e->suspended ? _("Retomar") : _("Pausar"));
    }
    if (e->killable) {
        NotifAction *a = &t->actions[t->n_actions++];
        snprintf(a->key, sizeof(a->key), "cancel");
        snprintf(a->label, sizeof(a->label), "%s", _("Cancelar"));
    }
}

static cairo_surface_t *job_icon(const JobEntry *e)
{
    const char *name = e->app_icon[0] ? e->app_icon : e->desktop_entry;
    if (!name[0]) {
        return NULL;
    }
    int size = icon_fetch_size_for(TOAST_ICON);
    return name[0] == '/' ? load_icon_argb(name, size) : resolve_icon_theme_name(name, size);
}

/* Toasts follow jobviewd.c's list: one per job that has run for a while
 * and wasn't closed, repainted when it changes, gone when it ends. */
static void sync_jobs(uint64_t now)
{
    for (int i = g_n - 1; i >= 0; i--) {
        if (g_toasts[i].job_id && !find_job(g_toasts[i].job_id)) {
            remove_toast(i);
        }
    }
    for (int k = 0; k < jobviewd_count(); k++) {
        const JobEntry *e = jobviewd_get(k);
        if (!e) {
            continue;
        }
        int i = find_toast_by_job(e->id);
        if (i >= 0) {
            if (g_toasts[i].job_serial != e->serial) {
                fill_job_toast(&g_toasts[i], e);
                paint_toast(&g_toasts[i]);
                XFlush(g_dpy);
            }
        } else if (now >= e->created_ms + TOAST_JOB_DELAY_MS && !job_hidden(e->id)) {
            int h = e->killable || e->suspendable ? TOAST_H + TOAST_BTN_ROW : TOAST_H;
            Toast *t = claim_toast_slot(h);
            t->job_id = e->id;
            t->own_icon = job_icon(e);
            t->icon = t->own_icon;
            fill_job_toast(t, e);
            map_and_show_toast(t);
        }
    }
}

void toast_init(void)
{
    notifd_set_arrived_callback(toast_on_arrived);
    notifd_set_close_callback(toast_close_notif);
}

/* Changing corners while toasts are already showing (a config RELOAD)
 * just relocates them where the new corner puts them, rather than
 * dismissing/recreating anything -- reposition_all() already recomputes
 * every window's position from g_corner alone. */
void toast_set_corner(ToastCorner corner)
{
    if (g_corner == corner) {
        return;
    }
    g_corner = corner;
    reposition_all();
}

/* See g_area_x/y/w/h's doc comment. reposition_all() is a no-op when
 * g_n == 0 (nothing showing yet), which is the common case (this is set
 * from a widget's init(), before any toast has ever arrived). */
void toast_set_output_rect(int x, int y, int w, int h)
{
    if (g_area_x == x && g_area_y == y && g_area_w == w && g_area_h == h) {
        return;
    }
    g_area_x = x;
    g_area_y = y;
    g_area_w = w;
    g_area_h = h;
    reposition_all();
}

void toast_avoid_refresh(void)
{
    if (g_n > 0 && av_refresh()) {
        reposition_all();
    }
}

void toast_set_colors(double bg_r, double bg_g, double bg_b, double bg_a, double fg_r, double fg_g, double fg_b,
                       double fg_a)
{
    g_bg_r = bg_r;
    g_bg_g = bg_g;
    g_bg_b = bg_b;
    g_bg_a = bg_a;
    g_fg_r = fg_r;
    g_fg_g = fg_g;
    g_fg_b = fg_b;
    g_fg_a = fg_a;
    for (int i = 0; i < g_n; i++) {
        paint_toast(&g_toasts[i]);
    }
    if (g_n > 0) {
        XFlush(g_dpy);
    }
}

/* Mirrors widgets/notif.c's own panel's bg_image_surface/bg_slice_* (see
 * toast_set_colors()'s doc comment for why this lives in notif.c's
 * on_tick rather than init()). `surface` is borrowed -- owned by the
 * Panel struct and reloaded/freed by panel_load_bg_image(), never by
 * this file -- so a panel-image RELOAD is picked up automatically the
 * next time notif_on_tick() re-syncs, without toast.c needing to know
 * anything happened. NULL falls back to the flat g_bg_* color. */
/* Companion to toast_set_bg_image() for a theme's menu.png popup frame,
 * borrowed under the same rules (owned by the Panel, re-synced from
 * notif.c's on_tick, never freed here). A skin with no surface loaded is
 * the same as none at all -- paint_toast() just falls through to the
 * background image or the flat color. */
void toast_set_skin(const PanelSkin *skin)
{
    if (g_skin == skin) {
        return;
    }
    g_skin = skin;
    for (int i = 0; i < g_n; i++) {
        paint_toast(&g_toasts[i]);
    }
    if (g_n > 0) {
        XFlush(g_dpy);
    }
}

void toast_set_text_panel(const Panel *p)
{
    if (g_text_panel == p) {
        return;
    }
    g_text_panel = p;
    for (int i = 0; i < g_n; i++) {
        paint_toast(&g_toasts[i]);
    }
    if (g_n > 0) {
        XFlush(g_dpy);
    }
}

void toast_set_bg_image(cairo_surface_t *surface, int slice_l, int slice_t, int slice_r, int slice_b)
{
    if (g_bg_image == surface && g_bg_slice_l == slice_l && g_bg_slice_t == slice_t && g_bg_slice_r == slice_r &&
        g_bg_slice_b == slice_b) {
        return;
    }
    g_bg_image = surface;
    g_bg_slice_l = slice_l;
    g_bg_slice_t = slice_t;
    g_bg_slice_r = slice_r;
    g_bg_slice_b = slice_b;
    for (int i = 0; i < g_n; i++) {
        paint_toast(&g_toasts[i]);
    }
    if (g_n > 0) {
        XFlush(g_dpy);
    }
}

/* Mirrors widgets/notif.c's own panel's tooltip_toast_padding_extra, same
 * on_tick re-sync pattern as toast_set_colors()/toast_set_bg_image(). */
void toast_set_padding_extra(int extra)
{
    if (extra < 0) {
        extra = 0;
    }
    if (g_pad_extra == extra) {
        return;
    }
    g_pad_extra = extra;
    for (int i = 0; i < g_n; i++) {
        paint_toast(&g_toasts[i]);
    }
    if (g_n > 0) {
        XFlush(g_dpy);
    }
}

/* Mirrors widgets/notif.c's own panel's border_radius=, same on_tick
 * re-sync pattern as toast_set_colors()/toast_set_padding_extra() -- see
 * panel_round_corners()'s doc comment for how a toast picks between
 * XShape and a real alpha clip, same rule as the panel bar itself. Every
 * toast is TOAST_W wide (heights differ, see Toast::h), so one call covers them
 * all. Re-applied (and repainted) immediately rather than waiting for
 * the next paint_toast(): a plain XShape change alone would dirty
 * nothing further, but the alpha-clip case rounds through the painted
 * content itself, so it needs the repaint to actually show. */
void toast_set_border_radius(int r)
{
    if (r < 0) {
        r = 0;
    }
    if (g_border_radius == r) {
        return;
    }
    g_border_radius = r;
    for (int i = 0; i < g_n; i++) {
        Toast *t = &g_toasts[i];
        t->alpha_clip = panel_round_corners(t->win, TOAST_W, t->h, g_border_radius, g_depth, &t->shaped);
        paint_toast(t);
    }
    if (g_n > 0) {
        XFlush(g_dpy);
    }
}

void toast_tick(uint64_t now)
{
    sync_jobs(now);
    /* Walk front-to-back and only ever remove one per call -- remove_toast()
     * already restacks everyone else, and another expiry (if any) gets
     * caught on the very next tick, which is cheap enough (main loop ticks
     * at least once a second regardless, see xispanel.c's timeout_ms
     * fallback) not to bother batching. A hovered toast is skipped
     * entirely -- see the `hovered` doc comment on Toast. */
    for (int i = 0; i < g_n; i++) {
        if (!g_toasts[i].hovered && g_toasts[i].expire_ms != 0 && now >= g_toasts[i].expire_ms) {
            drop_toast(i, NOTIF_CLOSED_EXPIRED);
            return;
        }
    }
}

uint64_t toast_next_wake_ms(void)
{
    uint64_t soonest = 0;
    for (int k = 0; k < jobviewd_count(); k++) {
        const JobEntry *e = jobviewd_get(k);
        if (e && find_toast_by_job(e->id) < 0 && !job_hidden(e->id)) {
            uint64_t due = e->created_ms + TOAST_JOB_DELAY_MS;
            if (soonest == 0 || due < soonest) {
                soonest = due;
            }
        }
    }
    for (int i = 0; i < g_n; i++) {
        if (g_toasts[i].hovered || g_toasts[i].expire_ms == 0) {
            continue;
        }
        if (soonest == 0 || g_toasts[i].expire_ms < soonest) {
            soonest = g_toasts[i].expire_ms;
        }
    }
    return soonest;
}

int toast_handle_event(const XEvent *ev)
{
    if (ev->type == ButtonPress) {
        for (int i = 0; i < g_n; i++) {
            if (g_toasts[i].win == ev->xbutton.window) {
                Toast *t = &g_toasts[i];
                if (t->job_id) {
                    /* The X hides it, a button asks the app, the body does
                     * nothing: the job runs on either way. */
                    int k = toast_button_at(t, ev->xbutton.x, ev->xbutton.y);
                    if (toast_close_at(t, ev->xbutton.x, ev->xbutton.y)) {
                        remove_toast(i);
                    } else if (k >= 0 && ev->xbutton.button == Button1) {
                        jobviewd_action(t->job_id, t->actions[k].key);
                    }
                    return 1;
                }
                if (t->action_fn) {
                    int k = toast_button_at(t, ev->xbutton.x, ev->xbutton.y);
                    if (k >= 0 && ev->xbutton.button == Button1 && !toast_close_at(t, ev->xbutton.x, ev->xbutton.y)) {
                        t->action_fn(t->action_ctx);
                    }
                    remove_toast(i);
                    return 1;
                }
                if (toast_close_at(t, ev->xbutton.x, ev->xbutton.y)) {
                    /* just clear it: no action, but it counts as seen */
                    notifd_mark_read(t->notif_id);
                    drop_toast(i, NOTIF_CLOSED_DISMISSED);
                    return 1;
                }
                int k = toast_button_at(t, ev->xbutton.x, ev->xbutton.y);
                if (k < 0) {
                    for (int j = 0; j < t->n_actions; j++) {
                        if (!strcmp(t->actions[j].key, "default")) {
                            k = j;
                        }
                    }
                }
                notifd_mark_read(t->notif_id);
                if (k >= 0 && ev->xbutton.button == Button1) {
                    /* closes the entry (and with it this toast) unless resident */
                    unsigned int id = t->notif_id;
                    notifd_invoke_action(id, t->actions[k].key);
                    int j = find_toast_by_notif(id);
                    if (j >= 0) {
                        remove_toast(j);
                    }
                    return 1;
                }
                drop_toast(i, NOTIF_CLOSED_DISMISSED);
                return 1;
            }
        }
    } else if (ev->type == Expose) {
        for (int i = 0; i < g_n; i++) {
            if (g_toasts[i].win == ev->xexpose.window) {
                paint_toast(&g_toasts[i]);
                XFlush(g_dpy);
                return 1;
            }
        }
    } else if (ev->type == MotionNotify) {
        for (int i = 0; i < g_n; i++) {
            Toast *t = &g_toasts[i];
            if (t->win == ev->xmotion.window) {
                int k = toast_button_at(t, ev->xmotion.x, ev->xmotion.y);
                int hc = toast_close_at(t, ev->xmotion.x, ev->xmotion.y);
                if (k != t->hover_btn || hc != t->hover_close) {
                    t->hover_btn = k;
                    t->hover_close = hc;
                    paint_toast(t);
                    XFlush(g_dpy);
                }
                return 1;
            }
        }
    } else if (ev->type == EnterNotify) {
        for (int i = 0; i < g_n; i++) {
            if (g_toasts[i].win == ev->xcrossing.window) {
                g_toasts[i].hovered = 1;
                return 1;
            }
        }
    } else if (ev->type == LeaveNotify) {
        for (int i = 0; i < g_n; i++) {
            if (g_toasts[i].win == ev->xcrossing.window) {
                g_toasts[i].hovered = 0;
                if (g_toasts[i].hover_btn >= 0 || g_toasts[i].hover_close) {
                    g_toasts[i].hover_btn = -1;
                    g_toasts[i].hover_close = 0;
                    paint_toast(&g_toasts[i]);
                    XFlush(g_dpy);
                }
                /* Restart the full countdown rather than resuming --
                 * see the `hovered`/`timeout_ms` doc comment on Toast. */
                if (g_toasts[i].timeout_ms != 0) {
                    g_toasts[i].expire_ms = now_ms() + g_toasts[i].timeout_ms;
                }
                return 1;
            }
        }
    } else if (ev->type == PropertyNotify) {
        for (int i = 0; i < g_n; i++) {
            if (density_layer_handle_property(g_toasts[i].density, &ev->xproperty)) {
                paint_toast(&g_toasts[i]);
                return 1;
            }
        }
    }
    return 0;
}
