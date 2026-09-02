/*
 * pager widget - a grid of desktop buttons, click switches to one, current
 * desktop highlighted -- the classic taskbar pager.
 *
 * Two data sources, picked automatically at every tick (see ewmh.c's
 * ewmh_kiwm_get_outputs() doc comment for why): kiwm's own per-output
 * desktop extension (kiwm/PROTOCOL.md) when a _KIWM_OUTPUTS property is
 * present on the root window, otherwise the plain global EWMH pair
 * (_NET_NUMBER_OF_DESKTOPS/_NET_CURRENT_DESKTOP) every other WM exposes.
 * Standard EWMH has no concept of "this output is on desktop 2 while that
 * one is on desktop 0" at all -- under plain EWMH there's only ever one
 * desktop set, shared by the whole session, so same_output_only=yes is a
 * no-op there (nothing to restrict *to*).
 *
 * `same_output_only` (kiwm mode only, defaults to yes): show just the
 * output this panel's own THEME/PANEL `output=` name matches, instead of
 * every output's desktops side by side -- this mirrors plain EWMH mode,
 * where there's only ever one desktop *count* to show (the global one);
 * defaulting kiwm mode to "just this output's own desktops" gives the
 * same single-group-of-squares look by default in both modes, with the
 * multi-output view as an opt-in (`same_output_only=no`). Matched by name
 * against kiwm's _KIWM_OUTPUTS list -- a panel configured with output=*
 * (spanning every output, no single one to restrict to) falls back to
 * showing all of them regardless of this setting.
 *
 * Multiple outputs (kiwm mode, same_output_only=no) are drawn as separate
 * button groups side by side, each with its own PAGER_GROUP_GAP-wide
 * separator -- kiwm's model has a *uniform* desktop count across every
 * output (see kiwm/PROTOCOL.md), so every group has the same grid shape,
 * just a different active-desktop highlight and (since each output can be
 * a different real resolution) a different button aspect ratio.
 *
 * Grid shape: read (never written -- kiconf will own writing this later)
 * from _NET_DESKTOP_LAYOUT, the same property any other EWMH pager would
 * publish for the WM's own directional desktop-switch keys. Since kiwm
 * has one uniform desktop *count* across every output, the same grid
 * shape applies to every group -- only which cell is "active" differs.
 * No property at all (not every WM sets it) falls back to a single row.
 *
 * Button aspect ratio: each button is sized proportional to the real
 * pixel resolution it represents, via RandR -- the specific output's own
 * resolution in kiwm mode (panel_lookup_output_size()), or the whole
 * screen's in plain EWMH mode (DisplayWidth/DisplayHeight), rather than a
 * fixed square.
 *
 * `show_windows` (default no): draw each desktop's open windows as small
 * outlines inside its own square, positioned/sized proportionally to
 * where they really are on that output -- the miniature-desktop look
 * every full pager has. Only outlines (no contents): a real preview
 * would mean one XComposite pixmap per window per repaint, which is what
 * tasklist's hover thumbnails are for.
 *
 * Scrolling anywhere over the widget switches desktop on the group under
 * the pointer (wrapping at both ends), the same gesture plasmashell's
 * pager uses. It goes through the same ewmh_kiwm_set_output_desktop()/
 * ewmh_set_current_desktop() call a click does, so under kiwm the switch
 * is a normal one and kiwm shows its own desktop OSD for it.
 *
 * Hovering a square shows a tooltip listing the windows on that desktop.
 */
#include "../xispanel.h"

#include <X11/Xlib.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGER_MAX_OUTPUTS 16
#define PAGER_MAX_DESKTOPS 32
#define PAGER_GROUP_GAP 10
#define PAGER_POLL_MS 500
/* Cap on the windows tracked for the outlines/tooltip -- a pathological
 * session with more than this just shows the first ones found, rather
 * than growing an unbounded per-repaint scan. */
#define PAGER_MAX_WINDOWS 96
/* Windows are re-scanned at most this often (each scan is one
 * XGetWindowAttributes+XTranslateCoordinates round trip per client), so
 * a pointer sweeping across the widget doesn't re-query the whole client
 * list on every MotionNotify. */
#define PAGER_WIN_SCAN_MS 400

typedef struct {
    Window win;
    int desktop;    /* -1 = sticky: belongs to every desktop */
    int group;      /* index into the displayed groups, -1 = on none of them */
    int x, y, w, h; /* root coordinates */
    char title[128];
} PagerWindow;

typedef struct {
    int same_output_only;
    int show_windows;

    /* Refreshed every on_tick() -- see the file comment. */
    int is_kiwm;
    int n_desktops; /* per group -- uniform across every displayed group */
    int n_groups;
    int kiwm_output_idx[PAGER_MAX_OUTPUTS]; /* real _KIWM_OUTPUTS index per displayed group, kiwm mode only */
    int active_desktop[PAGER_MAX_OUTPUTS];  /* highlighted desktop within each group */
    double aspect[PAGER_MAX_OUTPUTS];       /* real width/height of the group's output (or whole screen) */
    /* Root-coordinate rect of what each group represents (one output in
     * kiwm mode, the whole screen otherwise) -- the source rect windows
     * are mapped from when drawing outlines. */
    int group_rect[PAGER_MAX_OUTPUTS][4];

    /* Window cache for show_windows/the tooltip, refreshed at most every
     * PAGER_WIN_SCAN_MS by pager_collect_windows(). Heap-allocated rather
     * than inline so PagerPriv stays small enough for pager_refresh()'s
     * by-value snapshot; wins_sig is a cheap hash of everything drawn
     * from it, so that snapshot doesn't have to cover the array. */
    PagerWindow *wins;
    int n_wins;
    unsigned long wins_sig;
    uint64_t wins_scanned_ms;

    /* Grid shape from _NET_DESKTOP_LAYOUT (or the single-row fallback),
     * same across every group -- see the file comment. */
    int cols, rows;
    int orientation;     /* 0 = horz (row-major), 1 = vert (column-major) */
    int starting_corner; /* 0=TOPLEFT 1=TOPRIGHT 2=BOTTOMRIGHT 3=BOTTOMLEFT */

    /* Recomputed by pager_compute_geometry() every measure/paint/on_button
     * call from the widget's real allotted thickness, same "never
     * disagree" pattern tasklist/tray/winctl/globalmenu all use for their
     * own hit-testing. */
    int row_h;
    int btn_w[PAGER_MAX_OUTPUTS];   /* per-group button width (aspect differs per real output) */
    int group_x[PAGER_MAX_OUTPUTS]; /* left edge of group g's first button */
} PagerPriv;

static int pager_init(PanelWidget *w)
{
    PagerPriv *pp = w->priv;
    char buf[16];
    pp->same_output_only = !(kv_get(w->config_kv, "same_output_only", buf, sizeof(buf)) && !strcmp(buf, "no"));
    pp->show_windows = kv_get(w->config_kv, "show_windows", buf, sizeof(buf)) && !strcmp(buf, "yes");
    pp->wins = calloc(PAGER_MAX_WINDOWS, sizeof(PagerWindow));
    w->next_tick_ms = now_ms();
    return 0;
}

static void pager_destroy(PanelWidget *w)
{
    PagerPriv *pp = w->priv;
    free(pp->wins);
    pp->wins = NULL;
}

/* Derives the actual grid shape from _NET_DESKTOP_LAYOUT's raw values
 * (either of which may be 0, meaning "as many as needed" per the spec) and
 * the real desktop count -- or a single row if the property isn't set at
 * all. */
static void pager_derive_grid(int n_desktops, int layout_ok, int raw_cols, int raw_rows, int *out_cols,
                               int *out_rows)
{
    int cols = raw_cols, rows = raw_rows;
    if (!layout_ok || (cols <= 0 && rows <= 0)) {
        cols = n_desktops;
        rows = 1;
    } else if (cols <= 0) {
        cols = (n_desktops + rows - 1) / rows;
    } else if (rows <= 0) {
        rows = (n_desktops + cols - 1) / cols;
    }
    *out_cols = cols < 1 ? 1 : cols;
    *out_rows = rows < 1 ? 1 : rows;
}

/* Maps a grid cell to its desktop index per _NET_DESKTOP_LAYOUT's
 * orientation/starting_corner semantics -- see the EWMH spec's
 * _NET_DESKTOP_LAYOUT section. Used both to paint each cell (iterating
 * cells, looking up which desktop belongs there) and to hit-test a click
 * (same lookup, on the clicked cell). */
static int pager_rc_to_desktop(int r, int c, int cols, int rows, int orientation, int corner)
{
    if (corner == 1 || corner == 2) {
        c = cols - 1 - c;
    }
    if (corner == 2 || corner == 3) {
        r = rows - 1 - r;
    }
    return orientation == 1 ? c * rows + r : r * cols + c;
}

/* Refreshes everything pager_paint()/pager_on_button() need from the live
 * EWMH/kiwm/RandR state -- see the file comment for the two data sources.
 * Returns 1 if anything that affects what's drawn actually changed since
 * the last call. */
static int pager_refresh(PanelWidget *w)
{
    PagerPriv *pp = w->priv;
    Panel *p = w->panel;

    PagerPriv old = *pp;

    char names[PAGER_MAX_OUTPUTS][64];
    int n_outputs = 0;
    pp->is_kiwm = ewmh_kiwm_get_outputs(names, PAGER_MAX_OUTPUTS, &n_outputs);

    if (pp->is_kiwm) {
        int desktops[PAGER_MAX_OUTPUTS];
        int n_read = ewmh_kiwm_get_output_desktops(desktops, PAGER_MAX_OUTPUTS);
        int num_per_output = ewmh_kiwm_get_num_output_desktops();
        pp->n_desktops = num_per_output > 0 ? num_per_output : 1;

        int match = -1;
        if (pp->same_output_only) {
            for (int i = 0; i < n_outputs; i++) {
                if (strcmp(names[i], p->output) == 0) {
                    match = i;
                    break;
                }
            }
        }
        pp->n_groups = 0;
        if (match >= 0) {
            pp->kiwm_output_idx[pp->n_groups] = match;
            pp->active_desktop[pp->n_groups] = match < n_read ? desktops[match] : 0;
            pp->n_groups++;
        } else {
            for (int i = 0; i < n_outputs && pp->n_groups < PAGER_MAX_OUTPUTS; i++) {
                pp->kiwm_output_idx[pp->n_groups] = i;
                pp->active_desktop[pp->n_groups] = i < n_read ? desktops[i] : 0;
                pp->n_groups++;
            }
        }
        for (int g = 0; g < pp->n_groups; g++) {
            int orx, ory, ow, oh;
            if (panel_lookup_output_rect(names[pp->kiwm_output_idx[g]], &orx, &ory, &ow, &oh) && oh > 0) {
                pp->aspect[g] = (double)ow / oh;
                pp->group_rect[g][0] = orx;
                pp->group_rect[g][1] = ory;
                pp->group_rect[g][2] = ow;
                pp->group_rect[g][3] = oh;
            } else {
                pp->aspect[g] = 1.0;
                pp->group_rect[g][0] = pp->group_rect[g][1] = 0;
                pp->group_rect[g][2] = DisplayWidth(g_dpy, g_screen);
                pp->group_rect[g][3] = DisplayHeight(g_dpy, g_screen);
            }
        }
    } else {
        int n = ewmh_get_number_of_desktops();
        pp->n_desktops = n > 0 ? n : 1;
        pp->n_groups = 1;
        int cur = ewmh_get_current_desktop();
        pp->active_desktop[0] = cur >= 0 ? cur : 0;
        int sw = DisplayWidth(g_dpy, g_screen);
        int sh = DisplayHeight(g_dpy, g_screen);
        pp->aspect[0] = sh > 0 ? (double)sw / sh : 1.0;
        pp->group_rect[0][0] = pp->group_rect[0][1] = 0;
        pp->group_rect[0][2] = sw;
        pp->group_rect[0][3] = sh;
    }
    if (pp->n_desktops > PAGER_MAX_DESKTOPS) {
        pp->n_desktops = PAGER_MAX_DESKTOPS;
    }

    int raw_cols = 0, raw_rows = 0, orientation = 0, corner = 0;
    int layout_ok = ewmh_get_desktop_layout(&raw_cols, &raw_rows, &orientation, &corner);
    pager_derive_grid(pp->n_desktops, layout_ok, raw_cols, raw_rows, &pp->cols, &pp->rows);
    pp->orientation = layout_ok ? orientation : 0;
    pp->starting_corner = layout_ok ? corner : 0;

    return old.is_kiwm != pp->is_kiwm || old.n_desktops != pp->n_desktops || old.n_groups != pp->n_groups ||
           old.cols != pp->cols || old.rows != pp->rows || old.orientation != pp->orientation ||
           old.starting_corner != pp->starting_corner ||
           memcmp(old.kiwm_output_idx, pp->kiwm_output_idx, sizeof(old.kiwm_output_idx)) != 0 ||
           memcmp(old.active_desktop, pp->active_desktop, sizeof(old.active_desktop)) != 0 ||
           memcmp(old.aspect, pp->aspect, sizeof(old.aspect)) != 0;
}

/* Which displayed group (if any) a window belongs to: kiwm publishes the
 * owning output per window (_KIWM_WM_OUTPUT), and for anything that
 * doesn't have it set -- and for plain EWMH, where there is only ever the
 * one group covering the whole screen -- it falls back to "whichever
 * group's rect the window's center sits in". */
static int pager_group_of_window(PagerPriv *pp, Window win, int wx, int wy, int ww, int wh)
{
    if (pp->is_kiwm) {
        int oidx = ewmh_kiwm_get_wm_output(win);
        if (oidx >= 0) {
            for (int g = 0; g < pp->n_groups; g++) {
                if (pp->kiwm_output_idx[g] == oidx) {
                    return g;
                }
            }
            return -1; /* on an output this pager isn't showing */
        }
    }
    int cx = wx + ww / 2, cy = wy + wh / 2;
    for (int g = 0; g < pp->n_groups; g++) {
        const int *r = pp->group_rect[g];
        if (cx >= r[0] && cx < r[0] + r[2] && cy >= r[1] && cy < r[1] + r[3]) {
            return g;
        }
    }
    return -1;
}

/* Refills pp->wins from _NET_CLIENT_LIST, rate-limited to one scan per
 * PAGER_WIN_SCAN_MS (see that constant). Only windows a taskbar would
 * list, and only ones currently viewable -- ewmh_get_window_rect() fails
 * for minimized/unmapped ones, which is exactly right here: a minimized
 * window isn't occupying space on its desktop to draw. Returns 1 if
 * anything that affects what's drawn changed. */
static int pager_collect_windows(PanelWidget *w, uint64_t now)
{
    PagerPriv *pp = w->priv;
    if (!pp->wins || (pp->wins_scanned_ms && now - pp->wins_scanned_ms < PAGER_WIN_SCAN_MS)) {
        return 0;
    }
    pp->wins_scanned_ms = now;

    unsigned long sig = 1469598103934665603UL; /* FNV-1a offset basis */
    int n_wins = 0;
    Window *list = NULL;
    int n = 0;
    if (ewmh_get_client_list(&list, &n)) {
        for (int i = 0; i < n && n_wins < PAGER_MAX_WINDOWS; i++) {
            if (ewmh_skip_taskbar(list[i])) {
                continue;
            }
            int wx, wy, ww, wh;
            if (!ewmh_get_window_rect(list[i], &wx, &wy, &ww, &wh)) {
                continue;
            }
            int group = pager_group_of_window(pp, list[i], wx, wy, ww, wh);
            if (group < 0) {
                continue;
            }
            PagerWindow *e = &pp->wins[n_wins++];
            e->win = list[i];
            e->group = group;
            e->desktop = ewmh_get_desktop(list[i]);
            e->x = wx;
            e->y = wy;
            e->w = ww;
            e->h = wh;
            ewmh_get_title(list[i], e->title, sizeof(e->title));

            unsigned long fields[] = {(unsigned long)e->win, (unsigned long)(e->group + 1),
                                      (unsigned long)(e->desktop + 2), (unsigned long)e->x, (unsigned long)e->y,
                                      (unsigned long)e->w,             (unsigned long)e->h};
            for (size_t k = 0; k < sizeof(fields) / sizeof(fields[0]); k++) {
                sig = (sig ^ fields[k]) * 1099511628211UL;
            }
        }
        XFree(list);
    }
    int changed = (n_wins != pp->n_wins) || sig != pp->wins_sig;
    pp->n_wins = n_wins;
    pp->wins_sig = sig;
    return changed;
}

static int pager_on_tick(PanelWidget *w, uint64_t now)
{
    PagerPriv *pp = w->priv;
    w->next_tick_ms = now + PAGER_POLL_MS;
    int changed = pager_refresh(w);
    /* Only the outlines need the window list kept live between hovers --
     * without show_windows= the tooltip scans on demand instead, so an
     * idle panel doesn't walk the client list at all. */
    if (pp->show_windows && pager_collect_windows(w, now)) {
        changed = 1;
    }
    return changed;
}

/* Shared by measure/paint/on_button -- fills pp->row_h/btn_w[]/group_x[]
 * for the given thickness (the widget's cross-axis size: cross_axis at
 * measure() time, always the same value as w->thickness by the time
 * paint()/on_button() run it again). Returns the total along-panel length. */
static int pager_compute_geometry(PagerPriv *pp, int thickness)
{
    pp->row_h = pp->rows > 0 ? thickness / pp->rows : thickness;
    if (pp->row_h < 1) {
        pp->row_h = 1;
    }
    int x = 0;
    for (int g = 0; g < pp->n_groups; g++) {
        int bw = (int)(pp->row_h * pp->aspect[g] + 0.5);
        if (bw < 1) {
            bw = 1;
        }
        pp->btn_w[g] = bw;
        pp->group_x[g] = x;
        x += pp->cols * bw + PAGER_GROUP_GAP;
    }
    return pp->n_groups > 0 ? x - PAGER_GROUP_GAP : 0;
}

/* The group a main-axis position falls in, counting the gap after a group
 * as still belonging to it; -1 only when there are no groups at all. Used
 * by the scroll gesture, which is about the group rather than one cell,
 * so it deliberately never misses between squares. */
static int pager_group_at(PagerPriv *pp, int local_x)
{
    for (int g = 0; g < pp->n_groups; g++) {
        if (local_x < pp->group_x[g] + pp->cols * pp->btn_w[g] + PAGER_GROUP_GAP) {
            return g;
        }
    }
    return pp->n_groups > 0 ? pp->n_groups - 1 : -1;
}

/* Exact cell hit-test: fills out_g/out_desktop (and the cell's own
 * main-axis span, for anchoring a tooltip to it) for the square at
 * (local_x, local_y), or returns 0 for a miss -- a gap between groups, a
 * grid cell with no matching desktop, or outside the widget. */
static int pager_cell_at(PagerPriv *pp, int local_x, int local_y, int *out_g, int *out_desktop, int *out_x,
                          int *out_w)
{
    if (pp->row_h <= 0) {
        return 0;
    }
    for (int g = 0; g < pp->n_groups; g++) {
        int group_end = pp->group_x[g] + pp->cols * pp->btn_w[g];
        if (local_x < pp->group_x[g] || local_x >= group_end) {
            continue;
        }
        int c = (local_x - pp->group_x[g]) / pp->btn_w[g];
        int r = local_y / pp->row_h;
        if (c < 0 || c >= pp->cols || r < 0 || r >= pp->rows) {
            return 0;
        }
        int d = pager_rc_to_desktop(r, c, pp->cols, pp->rows, pp->orientation, pp->starting_corner);
        if (d < 0 || d >= pp->n_desktops) {
            return 0;
        }
        *out_g = g;
        *out_desktop = d;
        *out_x = pp->group_x[g] + c * pp->btn_w[g];
        *out_w = pp->btn_w[g];
        return 1;
    }
    return 0;
}

static void pager_measure(PanelWidget *w, int cross_axis, int *out_len, int *out_min_len)
{
    PagerPriv *pp = w->priv;
    int len = pager_compute_geometry(pp, cross_axis);
    *out_len = len;
    *out_min_len = len;
}

/* The miniature-desktop outlines of show_windows=yes: every window of
 * group `g` that lives on desktop `d` (sticky windows, desktop == -1, are
 * on all of them), mapped from the group's real output rect into the
 * cell's inner area. Clipped to the cell rather than scaled to fit, since
 * a window may legitimately hang off the edge of its output. */
static void pager_paint_windows(PanelWidget *w, cairo_t *cr, int g, int d, double bx, double by, double bw, double bh)
{
    PagerPriv *pp = w->priv;
    Panel *p = w->panel;
    const int *r = pp->group_rect[g];
    if (r[2] <= 0 || r[3] <= 0 || bw <= 4 || bh <= 4) {
        return;
    }
    double x0 = bx + 2, y0 = by + 2, x1 = bx + bw - 2, y1 = by + bh - 2;
    double sx = (x1 - x0) / r[2], sy = (y1 - y0) / r[3];

    for (int i = 0; i < pp->n_wins; i++) {
        const PagerWindow *e = &pp->wins[i];
        if (e->group != g || (e->desktop >= 0 && e->desktop != d)) {
            continue;
        }
        double wx = x0 + (e->x - r[0]) * sx;
        double wy = y0 + (e->y - r[1]) * sy;
        double wr = wx + e->w * sx, wb = wy + e->h * sy;
        if (wx < x0) {
            wx = x0;
        }
        if (wy < y0) {
            wy = y0;
        }
        if (wr > x1) {
            wr = x1;
        }
        if (wb > y1) {
            wb = y1;
        }
        if (wr - wx < 2 || wb - wy < 2) {
            continue; /* scaled away to nothing (or entirely off this output) */
        }
        cairo_set_source_rgba(cr, p->fg_r, p->fg_g, p->fg_b, 0.14);
        cairo_rectangle(cr, wx, wy, wr - wx, wb - wy);
        cairo_fill(cr);
        cairo_set_source_rgba(cr, p->fg_r, p->fg_g, p->fg_b, 0.5);
        cairo_rectangle(cr, wx + 0.5, wy + 0.5, wr - wx - 1, wb - wy - 1);
        cairo_set_line_width(cr, 1);
        cairo_stroke(cr);
    }
}

static void pager_paint(PanelWidget *w, cairo_t *cr)
{
    PagerPriv *pp = w->priv;
    Panel *p = w->panel;
    int ox, oy, owidth, oheight;
    widget_get_rect(w, &ox, &oy, &owidth, &oheight);
    (void)owidth;
    (void)oheight;
    pager_compute_geometry(pp, w->thickness);

    int hover_local_x, hover_local_y;
    int has_hover = panel_widget_hover_local_x(w, &hover_local_x) && panel_widget_hover_local_y(w, &hover_local_y);

    for (int g = 0; g < pp->n_groups; g++) {
        for (int r = 0; r < pp->rows; r++) {
            for (int c = 0; c < pp->cols; c++) {
                int d = pager_rc_to_desktop(r, c, pp->cols, pp->rows, pp->orientation, pp->starting_corner);
                if (d < 0 || d >= pp->n_desktops) {
                    continue; /* grid cell with no matching desktop */
                }
                int bx = ox + pp->group_x[g] + c * pp->btn_w[g];
                int by = oy + r * pp->row_h;
                int local_x = pp->group_x[g] + c * pp->btn_w[g];
                int local_y = r * pp->row_h;

                int cell_hover = has_hover && hover_local_x >= local_x && hover_local_x < local_x + pp->btn_w[g] &&
                                 hover_local_y >= local_y && hover_local_y < local_y + pp->row_h;
                int is_current = d == pp->active_desktop[g];
                /* A theme's pager.png draws the whole cell (normal /
                 * hover / current), replacing the wash + thin outline
                 * below; without one, nothing about this changes. */
                int skin_state = is_current ? SKIN_ACTIVE : (cell_hover ? SKIN_HOVER : SKIN_NORMAL);
                if (!panel_draw_skin(&p->pager_skin, cr, skin_state, bx, by, pp->btn_w[g], pp->row_h)) {
                    if (cell_hover) {
                        widget_paint_hover_cell(w, cr, local_x, local_y, pp->btn_w[g], pp->row_h);
                    }
                    if (is_current) {
                        cairo_set_source_rgba(cr, p->fg_r, p->fg_g, p->fg_b, 0.18);
                        cairo_rectangle(cr, bx, by, pp->btn_w[g], pp->row_h);
                        cairo_fill(cr);
                    }
                    cairo_set_source_rgba(cr, p->fg_r, p->fg_g, p->fg_b, 0.4);
                    cairo_rectangle(cr, bx + 1.5, by + 1.5, pp->btn_w[g] - 3, pp->row_h - 3);
                    cairo_set_line_width(cr, 1);
                    cairo_stroke(cr);
                }

                if (pp->show_windows) {
                    pager_paint_windows(w, cr, g, d, bx, by, pp->btn_w[g], pp->row_h);
                }

                char label[8];
                snprintf(label, sizeof(label), "%d", d + 1);
                double tw;
                pango_text_extents_ellipsized(cr, label, panel_text_size(p), 0, &tw, NULL);
                cairo_set_source_rgba(cr, p->fg_r, p->fg_g, p->fg_b, is_current ? 0.95 : 0.6);
                pango_show_text_boxed(cr, bx + (pp->btn_w[g] - tw) / 2.0, by, pp->row_h, pp->btn_w[g],
                                       panel_text_size(p), label, NULL);
            }
        }
        if (g < pp->n_groups - 1) {
            double sep_x = ox + pp->group_x[g] + pp->cols * pp->btn_w[g] + PAGER_GROUP_GAP / 2.0;
            cairo_set_source_rgba(cr, p->fg_r, p->fg_g, p->fg_b, 0.25);
            cairo_set_line_width(cr, 1);
            cairo_move_to(cr, sep_x, oy + 4);
            cairo_line_to(cr, sep_x, oy + w->thickness - 4);
            cairo_stroke(cr);
        }
    }
}

static int pager_on_button(PanelWidget *w, int button, int local_x, int local_y, int root_x, int root_y)
{
    (void)root_x;
    (void)root_y;
    PagerPriv *pp = w->priv;
    if (pp->n_groups <= 0) {
        return 0;
    }
    pager_compute_geometry(pp, w->thickness);
    if (pp->row_h <= 0) {
        return 0;
    }

    /* Scroll switches desktop on whichever group the pointer is over --
     * anywhere in it, including the gaps between squares, since the
     * gesture is about the group, not a particular cell. Wraps at both
     * ends. Goes through the same switch call a click does, so under kiwm
     * this shows kiwm's own desktop-switch OSD. */
    if (button == Button4 || button == Button5) {
        int g = pager_group_at(pp, local_x);
        if (g < 0) {
            g = 0;
        }
        int n = pp->n_desktops;
        if (n <= 1) {
            return 1;
        }
        int d = pp->active_desktop[g] + (button == Button4 ? -1 : 1);
        d = (d % n + n) % n;
        if (pp->is_kiwm) {
            ewmh_kiwm_set_output_desktop(pp->kiwm_output_idx[g], d);
        } else {
            ewmh_set_current_desktop(d);
        }
        pp->active_desktop[g] = d; /* optimistic -- the next tick re-reads the truth */
        XFlush(g_dpy);
        w->panel->dirty = 1;
        return 1;
    }

    if (button != Button1) {
        return 0;
    }

    int g, d, cell_x, cell_w;
    if (!pager_cell_at(pp, local_x, local_y, &g, &d, &cell_x, &cell_w)) {
        return 0;
    }
    if (pp->is_kiwm) {
        ewmh_kiwm_set_output_desktop(pp->kiwm_output_idx[g], d);
    } else {
        ewmh_set_current_desktop(d);
    }
    XFlush(g_dpy);
    return 1;
}

/* Lists the hovered desktop's windows. The window cache is shared with
 * show_windows= -- scanned here on demand when that option is off (the
 * rate limit inside pager_collect_windows() keeps a pointer sweeping
 * across the squares from re-walking the client list every motion
 * event). */
static int pager_get_tooltip(PanelWidget *w, int local_x, char *buf, size_t bufsz, int *anchor_x, int *anchor_w,
                              int *out_closable, void **out_ctx)
{
    (void)out_closable;
    (void)out_ctx;
    PagerPriv *pp = w->priv;
    if (pp->n_groups <= 0) {
        return 0;
    }
    pager_compute_geometry(pp, w->thickness);

    int local_y = 0;
    if (pp->rows > 1 && !panel_widget_hover_local_y(w, &local_y)) {
        return 0; /* multi-row grid: no way to tell which row without the pointer's y */
    }
    int g, d, cell_x, cell_w;
    if (!pager_cell_at(pp, local_x, local_y, &g, &d, &cell_x, &cell_w)) {
        return 0;
    }
    *anchor_x = cell_x;
    *anchor_w = cell_w;

    pager_collect_windows(w, now_ms());

    size_t used = 0;
    used += (size_t)snprintf(buf, bufsz, "Área de trabalho %d", d + 1);
    int n_listed = 0;
    for (int i = 0; i < pp->n_wins && used + 1 < bufsz; i++) {
        const PagerWindow *e = &pp->wins[i];
        if (e->group != g || (e->desktop >= 0 && e->desktop != d)) {
            continue;
        }
        used += (size_t)snprintf(buf + used, bufsz - used, "\n%s", e->title[0] ? e->title : "(sem título)");
        n_listed++;
    }
    if (!n_listed) {
        snprintf(buf + used, bufsz - used, "\n(vazia)");
    }
    return 1;
}

const PanelWidgetOps pager_ops = {
    .type_name = "pager",
    .priv_size = sizeof(PagerPriv),
    .init = pager_init,
    .destroy = pager_destroy,
    .measure = pager_measure,
    .paint = pager_paint,
    .on_button = pager_on_button,
    .get_tooltip = pager_get_tooltip,
    .on_tick = pager_on_tick,
};
