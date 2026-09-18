/* kiconf - Telas tab: xrandr layout, draggable canvas.
 * See kiconf.c's top doc comment for the overall design. */
#include "../common.h"
#include "../tabs.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

/* Telas tab (xrandr) widgets + state -- see build_telas_tab()'s doc
 * comment for what's deliberately NOT ported (scale/DPI/generic driver
 * properties). */
#define MAX_OUTPUTS 16
#define MAX_MODES 32
#define MAX_RATES 16
typedef struct {
    char rate[16];
    int is_current;
} ModeRate;
typedef struct {
    char name[16];
    ModeRate rates[MAX_RATES];
    int n_rates;
} OutMode;
typedef struct {
    char name[NAME_LEN];
    int connected, enabled, primary;
    int x, y, width, height;
    char rotation[16];
    char current_mode[16], current_rate[16];
    OutMode modes[MAX_MODES];
    int n_modes;
    /* Not detectable from plain `xrandr` (only --verbose shows them), so
     * these three start blank/neutral on every detect_outputs() call and
     * are restored from the previous in-memory state by
     * screens_restore_extras() right after -- meaning the "baseline" for
     * them is really "what kiconf last set them to", not "what the
     * hardware reports", unlike every other field here. */
    char mirror_of[NAME_LEN];
    int dpi;
    double scale_x, scale_y;
} ScreenOutput;
static ScreenOutput g_outputs[MAX_OUTPUTS];
static ScreenOutput g_outputs_baseline[MAX_OUTPUTS];
static int g_n_outputs = 0;
static int g_screens_selected = -1;
static int g_screens_dragging = 0;
static double g_screens_drag_dx, g_screens_drag_dy;
/* World transform frozen for the duration of a drag gesture (see
 * screens_canvas_press()) -- computed once when the drag starts and
 * reused by every motion event and by expose while dragging, instead of
 * recomputing compute_world() on every step. Recomputing live was the
 * cause of the "canvas zooms out while dragging" bug: moving an output
 * far away grew the world bounding box, which shrank the fit-to-canvas
 * scale in the middle of the gesture. */
static double g_screens_drag_ox, g_screens_drag_oy, g_screens_drag_scale;
static int g_screens_syncing = 0;
static GtkWidget *g_screens_canvas;
static GtkWidget *g_screens_res_combo, *g_screens_rate_combo, *g_screens_rot_combo;
static GtkWidget *g_screens_enabled_chk, *g_screens_primary_chk;
static GtkWidget *g_screens_mirror_combo;
static GtkWidget *g_screens_dpi_spin, *g_screens_scale_spin;
static GtkWidget *g_screens_status_label;

/* ---- Telas tab: xrandr layout, draggable canvas ----------------------- */
/*
 * Ported subset of xisconf.py's Screens tab: connect/enable/disable,
 * resolution+refresh rate, position (via drag on the canvas -- outputs
 * always dock flush against their nearest neighbor, see screens_dock(),
 * so the layout stays gap-free and the canvas zoom never changes mid-
 * drag), rotation, primary output, mirror/DPI/scale, all diffed against a
 * baseline before Aplicar sends only what changed (see apply_output_diff(),
 * which mirrors xisconf.py's _output_diff_args() field for field).
 * Deliberately NOT ported -- xisconf.py's generic
 * "advanced driver properties" system (TearFree, underscan, PRIME Sync,
 * etc., discovered from `xrandr --verbose`'s per-output "supported:"/
 * "range:" sub-lines): needs parsing --verbose output (a second xrandr
 * call with a much richer, driver-dependent format) and a dynamic
 * per-property widget system, which didn't fit this pass. Only plain
 * `xrandr` (no --verbose) is parsed here -- which also means mirror/DPI/
 * scale can't be read back from hardware, only written (see the
 * ScreenOutput struct's comment and screens_redetect_preserving_extras()).
 */

static int is_xid_token(const char *t)
{
    size_t n = strlen(t);
    if (n < 4 || t[0] != '(' || t[n - 1] != ')') {
        return 0;
    }
    if (strncmp(t + 1, "0x", 2) != 0) {
        return 0;
    }
    for (size_t i = 3; i < n - 1; i++) {
        if (!isxdigit((unsigned char)t[i])) {
            return 0;
        }
    }
    return 1;
}

static int is_rotation_word(const char *t)
{
    return !strcmp(t, "normal") || !strcmp(t, "left") || !strcmp(t, "right") || !strcmp(t, "inverted");
}

static int tokenize_ws(char *line, char *tokens[], int max)
{
    int n = 0;
    char *save = NULL;
    char *tok = strtok_r(line, " \t", &save);
    while (tok && n < max) {
        tokens[n++] = tok;
        tok = strtok_r(NULL, " \t", &save);
    }
    return n;
}

/* Mirrors xisconf.py's _parse_header(): NAME connected|disconnected
 * [primary] [WxH+X+Y] [(0xID)] [rotation] (...) [WWmm x HHmm]. Only the
 * fields this tab actually uses are extracted. */
static void parse_output_header(char *line, ScreenOutput *o)
{
    memset(o, 0, sizeof(*o));
    snprintf(o->rotation, sizeof(o->rotation), "normal");
    o->scale_x = 1.0;
    o->scale_y = 1.0;
    char *tokens[32];
    int n = tokenize_ws(line, tokens, 32);
    if (n < 2) {
        return;
    }
    snprintf(o->name, sizeof(o->name), "%s", tokens[0]);
    o->connected = !strcmp(tokens[1], "connected");
    int i = 2;
    if (i < n && !strcmp(tokens[i], "primary")) {
        o->primary = 1;
        i++;
    }
    if (i < n) {
        int w, h, x, y;
        if (sscanf(tokens[i], "%dx%d+%d+%d", &w, &h, &x, &y) == 4) {
            o->width = w;
            o->height = h;
            o->x = x;
            o->y = y;
            o->enabled = 1;
            i++;
        }
    }
    if (i < n && is_xid_token(tokens[i])) {
        i++;
    }
    if (i < n && is_rotation_word(tokens[i])) {
        snprintf(o->rotation, sizeof(o->rotation), "%s", tokens[i]);
    }
}

static OutMode *find_or_add_mode(ScreenOutput *o, const char *name)
{
    for (int i = 0; i < o->n_modes; i++) {
        if (!strcmp(o->modes[i].name, name)) {
            return &o->modes[i];
        }
    }
    if (o->n_modes >= MAX_MODES) {
        return &o->modes[0];
    }
    OutMode *m = &o->modes[o->n_modes++];
    snprintf(m->name, sizeof(m->name), "%s", name);
    m->n_rates = 0;
    return m;
}

/* Mode lines are indented exactly 3 spaces ("   1920x1080   60.00*+ ..."),
 * same delimiter xisconf.py's _MODE_LINE_RE uses to tell them apart from
 * anything else. */
static int parse_mode_line(const char *line, int *w, int *h, char *rest, size_t restsz)
{
    if (strncmp(line, "   ", 3) != 0 || line[3] == ' ') {
        return 0;
    }
    int consumed = 0;
    if (sscanf(line + 3, "%dx%d%n", w, h, &consumed) != 2) {
        return 0;
    }
    const char *r = line + 3 + consumed;
    while (*r == ' ') {
        r++;
    }
    snprintf(rest, restsz, "%s", r);
    return 1;
}

static void parse_rate_tokens(ScreenOutput *o, const char *modename, char *rest)
{
    OutMode *m = find_or_add_mode(o, modename);
    char *save = NULL;
    char *tok = strtok_r(rest, " \t", &save);
    while (tok) {
        int is_cur = strchr(tok, '*') != NULL;
        char rate[16];
        int k = 0;
        for (const char *p = tok; *p && k < 15; p++) {
            if (*p == '*' || *p == '+') {
                continue;
            }
            rate[k++] = *p;
        }
        rate[k] = '\0';
        if (m->n_rates < MAX_RATES) {
            snprintf(m->rates[m->n_rates].rate, sizeof(m->rates[0].rate), "%s", rate);
            m->rates[m->n_rates].is_current = is_cur;
            m->n_rates++;
        }
        if (is_cur) {
            snprintf(o->current_mode, sizeof(o->current_mode), "%s", modename);
            snprintf(o->current_rate, sizeof(o->current_rate), "%s", rate);
        }
        tok = strtok_r(NULL, " \t", &save);
    }
}

static int detect_outputs(ScreenOutput *outs, int max)
{
    char *argv[] = {"xrandr", NULL};
    char out[16384];
    if (!run_capture(argv, out, sizeof(out))) {
        return 0;
    }
    int n = 0;
    ScreenOutput *cur = NULL;
    char *save = NULL;
    char *line = strtok_r(out, "\n", &save);
    while (line) {
        if (!isspace((unsigned char)line[0])) {
            char tmp[512];
            snprintf(tmp, sizeof(tmp), "%s", line);
            char *save2 = NULL;
            char *t1 = strtok_r(tmp, " \t", &save2);
            char *t2 = t1 ? strtok_r(NULL, " \t", &save2) : NULL;
            if (t2 && (!strcmp(t2, "connected") || !strcmp(t2, "disconnected")) && n < max) {
                cur = &outs[n++];
                char linecopy[512];
                snprintf(linecopy, sizeof(linecopy), "%s", line);
                parse_output_header(linecopy, cur);
            } else {
                cur = NULL;
            }
        } else if (cur) {
            int w, h;
            char rest[256];
            if (parse_mode_line(line, &w, &h, rest, sizeof(rest))) {
                char modename[16];
                snprintf(modename, sizeof(modename), "%dx%d", w, h);
                parse_rate_tokens(cur, modename, rest);
            }
        }
        line = strtok_r(NULL, "\n", &save);
    }
    return n;
}

/* Mirrors xisconf.py's _output_args()/_output_diff_args() field for
 * field, including the write-only mirror/scale/DPI fields (see
 * ScreenOutput's comment on why their "baseline" isn't hardware truth). */
static void apply_output_diff(const ScreenOutput *o, const ScreenOutput *base)
{
    if (!o->connected) {
        return;
    }
    char *argv[24];
    int ac = 0;
    argv[ac++] = "xrandr";
    argv[ac++] = "--output";
    argv[ac++] = (char *)o->name;

    char modebuf[32], ratebuf[16], posbuf[32], scalebuf[32], dpibuf[16];

    if (o->enabled != base->enabled) {
        if (!o->enabled) {
            argv[ac++] = "--off";
            argv[ac] = NULL;
            run_fire(argv);
            return;
        }
        /* Turning an output back on needs a full description -- there's
         * no sensible "diff" starting from a disabled state. */
        if (o->mirror_of[0]) {
            argv[ac++] = "--same-as";
            argv[ac++] = (char *)o->mirror_of;
        } else {
            if (o->current_mode[0]) {
                argv[ac++] = "--mode";
                snprintf(modebuf, sizeof(modebuf), "%s", o->current_mode);
                argv[ac++] = modebuf;
            }
            if (o->current_rate[0]) {
                argv[ac++] = "--rate";
                snprintf(ratebuf, sizeof(ratebuf), "%s", o->current_rate);
                argv[ac++] = ratebuf;
            }
            snprintf(posbuf, sizeof(posbuf), "%dx%d", o->x, o->y);
            argv[ac++] = "--pos";
            argv[ac++] = posbuf;
        }
        argv[ac++] = "--rotate";
        argv[ac++] = (char *)o->rotation;
        argv[ac++] = o->primary ? "--primary" : "--noprimary";
        double sx = fabs(o->scale_x) > 1e-6 ? o->scale_x : 1.0;
        double sy = fabs(o->scale_y) > 1e-6 ? o->scale_y : 1.0;
        snprintf(scalebuf, sizeof(scalebuf), "%.4fx%.4f", sx, sy);
        argv[ac++] = "--scale";
        argv[ac++] = scalebuf;
        if (o->dpi) {
            argv[ac++] = "--set";
            argv[ac++] = "DPI";
            snprintf(dpibuf, sizeof(dpibuf), "%d", o->dpi);
            argv[ac++] = dpibuf;
        }
        argv[ac] = NULL;
        run_fire(argv);
        return;
    }
    if (!o->enabled) {
        return;
    }

    int changed = 0;
    int mirror_changed = strcmp(o->mirror_of, base->mirror_of) != 0;
    if (mirror_changed) {
        if (o->mirror_of[0]) {
            argv[ac++] = "--same-as";
            argv[ac++] = (char *)o->mirror_of;
        } else {
            if (o->current_mode[0]) {
                argv[ac++] = "--mode";
                snprintf(modebuf, sizeof(modebuf), "%s", o->current_mode);
                argv[ac++] = modebuf;
            }
            if (o->current_rate[0]) {
                argv[ac++] = "--rate";
                snprintf(ratebuf, sizeof(ratebuf), "%s", o->current_rate);
                argv[ac++] = ratebuf;
            }
            snprintf(posbuf, sizeof(posbuf), "%dx%d", o->x, o->y);
            argv[ac++] = "--pos";
            argv[ac++] = posbuf;
        }
        changed = 1;
    } else if (!o->mirror_of[0]) {
        if (strcmp(o->current_mode, base->current_mode)) {
            argv[ac++] = "--mode";
            snprintf(modebuf, sizeof(modebuf), "%s", o->current_mode);
            argv[ac++] = modebuf;
            changed = 1;
        }
        if (strcmp(o->current_rate, base->current_rate)) {
            argv[ac++] = "--rate";
            snprintf(ratebuf, sizeof(ratebuf), "%s", o->current_rate);
            argv[ac++] = ratebuf;
            changed = 1;
        }
        if (o->x != base->x || o->y != base->y) {
            snprintf(posbuf, sizeof(posbuf), "%dx%d", o->x, o->y);
            argv[ac++] = "--pos";
            argv[ac++] = posbuf;
            changed = 1;
        }
    }
    if (strcmp(o->rotation, base->rotation)) {
        argv[ac++] = "--rotate";
        argv[ac++] = (char *)o->rotation;
        changed = 1;
    }
    if (o->primary != base->primary) {
        argv[ac++] = o->primary ? "--primary" : "--noprimary";
        changed = 1;
    }
    if (lround(o->scale_x * 10000) != lround(base->scale_x * 10000) ||
        lround(o->scale_y * 10000) != lround(base->scale_y * 10000)) {
        double sx = fabs(o->scale_x) > 1e-6 ? o->scale_x : 1.0;
        double sy = fabs(o->scale_y) > 1e-6 ? o->scale_y : 1.0;
        snprintf(scalebuf, sizeof(scalebuf), "%.4fx%.4f", sx, sy);
        argv[ac++] = "--scale";
        argv[ac++] = scalebuf;
        changed = 1;
    }
    if (o->dpi != base->dpi && o->dpi) {
        argv[ac++] = "--set";
        argv[ac++] = "DPI";
        snprintf(dpibuf, sizeof(dpibuf), "%d", o->dpi);
        argv[ac++] = dpibuf;
        changed = 1;
    }
    argv[ac] = NULL;
    if (changed) {
        run_fire(argv);
    }
}

static void output_visual_size(const ScreenOutput *o, int *w, int *h)
{
    *w = o->width;
    *h = o->height;
    if (!strcmp(o->rotation, "left") || !strcmp(o->rotation, "right")) {
        int t = *w;
        *w = *h;
        *h = t;
    }
}

static void compute_world(double *ox, double *oy, double *scale, int canvas_w, int canvas_h)
{
    int min_x = INT_MAX, min_y = INT_MAX, max_x = INT_MIN, max_y = INT_MIN;
    int any = 0;
    for (int i = 0; i < g_n_outputs; i++) {
        ScreenOutput *o = &g_outputs[i];
        if (!o->connected || !o->enabled) {
            continue;
        }
        any = 1;
        int w, h;
        output_visual_size(o, &w, &h);
        if (o->x < min_x) {
            min_x = o->x;
        }
        if (o->y < min_y) {
            min_y = o->y;
        }
        if (o->x + w > max_x) {
            max_x = o->x + w;
        }
        if (o->y + h > max_y) {
            max_y = o->y + h;
        }
    }
    if (!any) {
        *ox = 0;
        *oy = 0;
        *scale = 0.05;
        return;
    }
    *ox = min_x;
    *oy = min_y;
    double world_w = max_x - min_x > 0 ? max_x - min_x : 1;
    double world_h = max_y - min_y > 0 ? max_y - min_y : 1;
    double sx = (canvas_w - 40) / world_w;
    double sy = (canvas_h - 40) / world_h;
    *scale = sx < sy ? sx : sy;
    if (*scale <= 0) {
        *scale = 0.01;
    }
}

static int screens_hit_test(double px, double py, int canvas_w, int canvas_h)
{
    double ox, oy, scale;
    compute_world(&ox, &oy, &scale, canvas_w, canvas_h);
    for (int i = g_n_outputs - 1; i >= 0; i--) {
        ScreenOutput *o = &g_outputs[i];
        if (!o->connected || !o->enabled) {
            continue;
        }
        int w, h;
        output_visual_size(o, &w, &h);
        double rx = 20 + (o->x - ox) * scale, ry = 20 + (o->y - oy) * scale;
        double rw = w * scale, rh = h * scale;
        if (px >= rx && px <= rx + rw && py >= ry && py <= ry + rh) {
            return i;
        }
    }
    return -1;
}

/* Snaps `*x`/`*y` (the dragged output's candidate top-left, in xrandr
 * world units) to align an edge with any other connected+enabled
 * output's edges, if within SNAP_CANVAS_PX *canvas* pixels at the
 * current zoom -- converted to world units via `scale` so the snap
 * distance feels the same regardless of how zoomed out the layout is.
 * Left/right and top/bottom are snapped independently, each picking
 * whichever candidate-edge/other-edge pairing is closest. */
#define SNAP_CANVAS_PX 10

static void screens_snap(int idx, int *x, int *y, int w, int h, double scale)
{
    if (scale <= 0) {
        return;
    }
    double thresh = SNAP_CANVAS_PX / scale;
    double best_dx = 0, best_dx_dist = thresh + 1;
    double best_dy = 0, best_dy_dist = thresh + 1;
    int have_dx = 0, have_dy = 0;

    for (int i = 0; i < g_n_outputs; i++) {
        if (i == idx) {
            continue;
        }
        ScreenOutput *o = &g_outputs[i];
        if (!o->connected || !o->enabled) {
            continue;
        }
        int ow, oh;
        output_visual_size(o, &ow, &oh);
        int ol = o->x, orr = o->x + ow, ot = o->y, ob = o->y + oh;
        int cl = *x, cr = *x + w, ct = *y, cb = *y + h;

        int xpairs[4][2] = {{cl, ol}, {cl, orr}, {cr, ol}, {cr, orr}};
        for (int k = 0; k < 4; k++) {
            double d = fabs((double)(xpairs[k][0] - xpairs[k][1]));
            if (d <= thresh && d < best_dx_dist) {
                best_dx_dist = d;
                best_dx = *x + (xpairs[k][1] - xpairs[k][0]);
                have_dx = 1;
            }
        }
        int ypairs[4][2] = {{ct, ot}, {ct, ob}, {cb, ot}, {cb, ob}};
        for (int k = 0; k < 4; k++) {
            double d = fabs((double)(ypairs[k][0] - ypairs[k][1]));
            if (d <= thresh && d < best_dy_dist) {
                best_dy_dist = d;
                best_dy = *y + (ypairs[k][1] - ypairs[k][0]);
                have_dy = 1;
            }
        }
    }
    if (have_dx) {
        *x = (int)lround(best_dx);
    }
    if (have_dy) {
        *y = (int)lround(best_dy);
    }
}

/* Outputs must sit side by side with no gaps -- the user can only choose
 * which side of the nearest other output the dragged one docks to, and
 * where along that shared edge, not an arbitrary free position. Finds the
 * other connected+enabled output closest (by rect center) to the
 * dragged rect's candidate position, then:
 *   - picks the dock axis (x or y) as whichever the drag moved further
 *     along, relative to that anchor's center;
 *   - locks that axis flush against the anchor's near edge (zero gap);
 *   - clamps the other (free) axis so the two rectangles keep at least
 *     1 world unit of overlap, instead of drifting arbitrarily far and
 *     re-inflating the world bounding box (which is what let the canvas
 *     zoom out in the first place -- see compute_world()).
 * Only touches *x and *y -- w/h are the dragged output's own visual size. */
static void screens_dock(int idx, int *x, int *y, int w, int h, double scale)
{
    int have_other = 0, best = -1;
    double best_dist = 0;
    double dcx = *x + w / 2.0, dcy = *y + h / 2.0;

    for (int i = 0; i < g_n_outputs; i++) {
        if (i == idx) {
            continue;
        }
        ScreenOutput *o = &g_outputs[i];
        if (!o->connected || !o->enabled) {
            continue;
        }
        int ow, oh;
        output_visual_size(o, &ow, &oh);
        double ocx = o->x + ow / 2.0, ocy = o->y + oh / 2.0;
        double dist = hypot(dcx - ocx, dcy - ocy);
        if (!have_other || dist < best_dist) {
            have_other = 1;
            best_dist = dist;
            best = i;
        }
    }
    if (!have_other) {
        return; /* only output on the canvas -- nothing to dock against */
    }

    ScreenOutput *r = &g_outputs[best];
    int rw, rh;
    output_visual_size(r, &rw, &rh);
    double rcx = r->x + rw / 2.0, rcy = r->y + rh / 2.0;
    double dx = dcx - rcx, dy = dcy - rcy;
    int dock_x = fabs(dx) >= fabs(dy);

    if (dock_x) {
        *x = dx >= 0 ? r->x + rw : r->x - w;
        int ymin = r->y - h + 1, ymax = r->y + rh - 1;
        if (*y < ymin) {
            *y = ymin;
        }
        if (*y > ymax) {
            *y = ymax;
        }
    } else {
        *y = dy >= 0 ? r->y + rh : r->y - h;
        int xmin = r->x - w + 1, xmax = r->x + rw - 1;
        if (*x < xmin) {
            *x = xmin;
        }
        if (*x > xmax) {
            *x = xmax;
        }
    }

    /* Soft-snap the free axis to any other output's edges within the
     * usual pixel threshold, for a nicer "click into alignment" feel
     * (e.g. lining up the tops of two side-by-side monitors). This can
     * only move the free axis in practice since the docked one is
     * already flush against its nearest neighbor, but re-enforce it
     * anyway in case a different, closer output won the snap. */
    screens_snap(idx, x, y, w, h, scale);
    if (dock_x) {
        *x = dx >= 0 ? r->x + rw : r->x - w;
    } else {
        *y = dy >= 0 ? r->y + rh : r->y - h;
    }
}

static void sync_screens_form(void);

static gboolean screens_canvas_expose(GtkWidget *widget, GdkEventExpose *event, gpointer data)
{
    (void)event;
    (void)data;
    cairo_t *cr = gdk_cairo_create(widget->window);
    int cw = widget->allocation.width, ch = widget->allocation.height;
    cairo_set_source_rgb(cr, 0.14, 0.15, 0.16);
    cairo_paint(cr);

    double ox, oy, scale;
    if (g_screens_dragging) {
        ox = g_screens_drag_ox;
        oy = g_screens_drag_oy;
        scale = g_screens_drag_scale;
    } else {
        compute_world(&ox, &oy, &scale, cw, ch);
    }

    for (int i = 0; i < g_n_outputs; i++) {
        ScreenOutput *o = &g_outputs[i];
        if (!o->connected || !o->enabled) {
            continue;
        }
        int w, h;
        output_visual_size(o, &w, &h);
        double rx = 20 + (o->x - ox) * scale, ry = 20 + (o->y - oy) * scale;
        double rw = w * scale, rh = h * scale;

        int selected = (g_screens_selected == i);
        cairo_set_source_rgb(cr, selected ? 0.20 : 0.16, selected ? 0.45 : 0.30, selected ? 0.75 : 0.45);
        cairo_rectangle(cr, rx, ry, rw, rh);
        cairo_fill_preserve(cr);
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_set_line_width(cr, selected ? 2 : 1);
        cairo_stroke(cr);

        cairo_move_to(cr, rx + 6, ry + 16);
        cairo_show_text(cr, o->name);
        if (o->primary) {
            cairo_move_to(cr, rx + 6, ry + 32);
            cairo_show_text(cr, "(primary)");
        }
    }
    cairo_destroy(cr);
    return TRUE;
}

static gboolean screens_canvas_press(GtkWidget *widget, GdkEventButton *event, gpointer data)
{
    (void)data;
    int cw = widget->allocation.width, ch = widget->allocation.height;
    int idx = screens_hit_test(event->x, event->y, cw, ch);
    if (idx >= 0) {
        double ox, oy, scale;
        compute_world(&ox, &oy, &scale, cw, ch);
        ScreenOutput *o = &g_outputs[idx];
        double rx = 20 + (o->x - ox) * scale, ry = 20 + (o->y - oy) * scale;
        g_screens_selected = idx;
        g_screens_dragging = 1;
        g_screens_drag_dx = event->x - rx;
        g_screens_drag_dy = event->y - ry;
        g_screens_drag_ox = ox;
        g_screens_drag_oy = oy;
        g_screens_drag_scale = scale;
        sync_screens_form();
        gtk_widget_queue_draw(widget);
    }
    return TRUE;
}

static gboolean screens_canvas_motion(GtkWidget *widget, GdkEventMotion *event, gpointer data)
{
    (void)data;
    if (!g_screens_dragging || g_screens_selected < 0) {
        return TRUE;
    }
    (void)widget;
    double ox = g_screens_drag_ox, oy = g_screens_drag_oy, scale = g_screens_drag_scale;
    if (scale <= 0) {
        return TRUE;
    }
    double new_rx = event->x - g_screens_drag_dx, new_ry = event->y - g_screens_drag_dy;
    ScreenOutput *o = &g_outputs[g_screens_selected];
    o->x = (int)lround(ox + (new_rx - 20) / scale);
    o->y = (int)lround(oy + (new_ry - 20) / scale);
    if (!o->mirror_of[0]) {
        int w, h;
        output_visual_size(o, &w, &h);
        screens_dock(g_screens_selected, &o->x, &o->y, w, h, scale);
    }
    gtk_widget_queue_draw(g_screens_canvas);
    return TRUE;
}

static gboolean screens_canvas_release(GtkWidget *widget, GdkEventButton *event, gpointer data)
{
    (void)widget;
    (void)event;
    (void)data;
    g_screens_dragging = 0;
    return TRUE;
}

static void populate_rate_combo(void)
{
    if (g_screens_selected < 0) {
        return;
    }
    ScreenOutput *o = &g_outputs[g_screens_selected];
    gchar *modename = gtk_combo_box_get_active_text(GTK_COMBO_BOX(g_screens_res_combo));
    if (!modename) {
        return;
    }
    /* rebuild rate combo (GTK2 has no "remove all" for the text-combo
     * convenience API, so destroy and recreate its model instead) */
    GtkTreeModel *model = gtk_combo_box_get_model(GTK_COMBO_BOX(g_screens_rate_combo));
    gtk_list_store_clear(GTK_LIST_STORE(model));
    int active = -1;
    for (int i = 0; i < o->n_modes; i++) {
        if (strcmp(o->modes[i].name, modename)) {
            continue;
        }
        for (int r = 0; r < o->modes[i].n_rates; r++) {
            gtk_combo_box_append_text(GTK_COMBO_BOX(g_screens_rate_combo), o->modes[i].rates[r].rate);
            if (!strcmp(o->modes[i].rates[r].rate, o->current_rate)) {
                active = r;
            }
        }
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(g_screens_rate_combo), active >= 0 ? active : 0);
    g_free(modename);
}

static void sync_screens_form(void)
{
    if (g_screens_selected < 0) {
        return;
    }
    ScreenOutput *o = &g_outputs[g_screens_selected];
    g_screens_syncing = 1;

    GtkTreeModel *resmodel = gtk_combo_box_get_model(GTK_COMBO_BOX(g_screens_res_combo));
    gtk_list_store_clear(GTK_LIST_STORE(resmodel));
    int active = -1;
    for (int i = 0; i < o->n_modes; i++) {
        gtk_combo_box_append_text(GTK_COMBO_BOX(g_screens_res_combo), o->modes[i].name);
        if (!strcmp(o->modes[i].name, o->current_mode)) {
            active = i;
        }
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(g_screens_res_combo), active >= 0 ? active : (o->n_modes > 0 ? 0 : -1));
    populate_rate_combo();

    const char *rots[] = {"normal", "left", "right", "inverted"};
    int rot_idx = 0;
    for (int i = 0; i < 4; i++) {
        if (!strcmp(rots[i], o->rotation)) {
            rot_idx = i;
        }
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(g_screens_rot_combo), rot_idx);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_screens_enabled_chk), o->enabled);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_screens_primary_chk), o->primary);

    GtkTreeModel *mirmodel = gtk_combo_box_get_model(GTK_COMBO_BOX(g_screens_mirror_combo));
    gtk_list_store_clear(GTK_LIST_STORE(mirmodel));
    gtk_combo_box_append_text(GTK_COMBO_BOX(g_screens_mirror_combo), "(nenhum)");
    int mirror_idx = 0;
    for (int i = 0, pos = 1; i < g_n_outputs; i++) {
        if (i == g_screens_selected || !g_outputs[i].connected) {
            continue;
        }
        gtk_combo_box_append_text(GTK_COMBO_BOX(g_screens_mirror_combo), g_outputs[i].name);
        if (o->mirror_of[0] && !strcmp(g_outputs[i].name, o->mirror_of)) {
            mirror_idx = pos;
        }
        pos++;
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(g_screens_mirror_combo), mirror_idx);

    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_screens_dpi_spin), o->dpi > 0 ? o->dpi : 96);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_screens_scale_spin), fabs(o->scale_x) > 1e-6 ? o->scale_x : 1.0);

    g_screens_syncing = 0;
}

static void on_screens_res_changed(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    if (g_screens_syncing || g_screens_selected < 0) {
        return;
    }
    populate_rate_combo();
    gchar *rate = gtk_combo_box_get_active_text(GTK_COMBO_BOX(g_screens_rate_combo));
    gchar *mode = gtk_combo_box_get_active_text(GTK_COMBO_BOX(g_screens_res_combo));
    ScreenOutput *o = &g_outputs[g_screens_selected];
    if (mode) {
        snprintf(o->current_mode, sizeof(o->current_mode), "%s", mode);
    }
    if (rate) {
        snprintf(o->current_rate, sizeof(o->current_rate), "%s", rate);
    }
    g_free(mode);
    g_free(rate);
    gtk_widget_queue_draw(g_screens_canvas);
}

static void on_screens_rate_changed(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    if (g_screens_syncing || g_screens_selected < 0) {
        return;
    }
    gchar *rate = gtk_combo_box_get_active_text(GTK_COMBO_BOX(g_screens_rate_combo));
    if (rate) {
        snprintf(g_outputs[g_screens_selected].current_rate, sizeof(g_outputs[0].current_rate), "%s", rate);
    }
    g_free(rate);
}

static void on_screens_rot_changed(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    if (g_screens_syncing || g_screens_selected < 0) {
        return;
    }
    gchar *rot = gtk_combo_box_get_active_text(GTK_COMBO_BOX(g_screens_rot_combo));
    if (rot) {
        snprintf(g_outputs[g_screens_selected].rotation, sizeof(g_outputs[0].rotation), "%s", rot);
    }
    g_free(rot);
    gtk_widget_queue_draw(g_screens_canvas);
}

static void on_screens_enabled_toggled(GtkWidget *widget, gpointer data)
{
    (void)data;
    if (g_screens_syncing || g_screens_selected < 0) {
        return;
    }
    g_outputs[g_screens_selected].enabled = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
    gtk_widget_queue_draw(g_screens_canvas);
}

static void on_screens_primary_toggled(GtkWidget *widget, gpointer data)
{
    (void)data;
    if (g_screens_syncing || g_screens_selected < 0) {
        return;
    }
    int active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
    if (active) {
        for (int i = 0; i < g_n_outputs; i++) {
            g_outputs[i].primary = (i == g_screens_selected);
        }
    } else {
        g_outputs[g_screens_selected].primary = 0;
    }
    gtk_widget_queue_draw(g_screens_canvas);
}

static void on_screens_mirror_changed(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    if (g_screens_syncing || g_screens_selected < 0) {
        return;
    }
    gchar *sel = gtk_combo_box_get_active_text(GTK_COMBO_BOX(g_screens_mirror_combo));
    ScreenOutput *o = &g_outputs[g_screens_selected];
    if (!sel || !strcmp(sel, "(nenhum)")) {
        o->mirror_of[0] = '\0';
    } else {
        snprintf(o->mirror_of, sizeof(o->mirror_of), "%s", sel);
    }
    g_free(sel);
    gtk_widget_queue_draw(g_screens_canvas);
}

static void on_screens_dpi_changed(GtkWidget *widget, gpointer data)
{
    (void)data;
    if (g_screens_syncing || g_screens_selected < 0) {
        return;
    }
    g_outputs[g_screens_selected].dpi = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(widget));
}

static void on_screens_scale_changed(GtkWidget *widget, gpointer data)
{
    (void)data;
    if (g_screens_syncing || g_screens_selected < 0) {
        return;
    }
    double v = gtk_spin_button_get_value(GTK_SPIN_BUTTON(widget));
    g_outputs[g_screens_selected].scale_x = v;
    g_outputs[g_screens_selected].scale_y = v;
}

/* mirror_of/dpi/scale aren't detected from plain `xrandr` (see
 * ScreenOutput's comment), so a raw detect_outputs() call would silently
 * wipe them back to blank/1.0 every time -- these snapshot them by output
 * name before re-detecting and restore them after, so Apply/Detectar
 * novamente only ever lose what actually vanished (the output itself). */
typedef struct {
    char name[NAME_LEN];
    char mirror_of[NAME_LEN];
    int dpi;
    double scale_x, scale_y;
} ScreensExtra;

static void screens_redetect_preserving_extras(void)
{
    static ScreensExtra snap[MAX_OUTPUTS];
    int n_snap = 0;
    for (int i = 0; i < g_n_outputs && n_snap < MAX_OUTPUTS; i++) {
        snprintf(snap[n_snap].name, sizeof(snap[n_snap].name), "%s", g_outputs[i].name);
        snprintf(snap[n_snap].mirror_of, sizeof(snap[n_snap].mirror_of), "%s", g_outputs[i].mirror_of);
        snap[n_snap].dpi = g_outputs[i].dpi;
        snap[n_snap].scale_x = g_outputs[i].scale_x;
        snap[n_snap].scale_y = g_outputs[i].scale_y;
        n_snap++;
    }

    g_n_outputs = detect_outputs(g_outputs, MAX_OUTPUTS);

    for (int i = 0; i < g_n_outputs; i++) {
        for (int j = 0; j < n_snap; j++) {
            if (!strcmp(snap[j].name, g_outputs[i].name)) {
                snprintf(g_outputs[i].mirror_of, sizeof(g_outputs[i].mirror_of), "%s", snap[j].mirror_of);
                g_outputs[i].dpi = snap[j].dpi;
                g_outputs[i].scale_x = snap[j].scale_x;
                g_outputs[i].scale_y = snap[j].scale_y;
                break;
            }
        }
    }
    memcpy(g_outputs_baseline, g_outputs, sizeof(g_outputs));
}

static void on_screens_apply(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    for (int i = 0; i < g_n_outputs; i++) {
        apply_output_diff(&g_outputs[i], &g_outputs_baseline[i]);
    }
    screens_redetect_preserving_extras();
    if (g_screens_selected >= g_n_outputs) {
        g_screens_selected = -1;
    }
    char status[64];
    snprintf(status, sizeof(status), "%d saida(s) detectada(s).", g_n_outputs);
    gtk_label_set_text(GTK_LABEL(g_screens_status_label), status);
    if (g_screens_selected >= 0) {
        sync_screens_form();
    }
    gtk_widget_queue_draw(g_screens_canvas);
}

static void on_screens_refresh(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    screens_redetect_preserving_extras();
    if (g_screens_selected >= g_n_outputs) {
        g_screens_selected = -1;
    }
    char status[64];
    snprintf(status, sizeof(status), "%d saida(s) detectada(s).", g_n_outputs);
    gtk_label_set_text(GTK_LABEL(g_screens_status_label), status);
    if (g_screens_selected >= 0) {
        sync_screens_form();
    }
    gtk_widget_queue_draw(g_screens_canvas);
}

GtkWidget *build_telas_tab(void)
{
    g_n_outputs = detect_outputs(g_outputs, MAX_OUTPUTS);
    memcpy(g_outputs_baseline, g_outputs, sizeof(g_outputs));
    for (int i = 0; i < g_n_outputs; i++) {
        if (g_outputs[i].connected && g_outputs[i].enabled) {
            g_screens_selected = i;
            break;
        }
    }

    GtkWidget *outer = gtk_vbox_new(FALSE, 8);
    gtk_container_set_border_width(GTK_CONTAINER(outer), 12);

    GtkWidget *note = gtk_label_new(
        "Arraste as caixas pra reposicionar -- ficam sempre lado a lado,\n"
        "encostadas na saida mais proxima, sem espacos entre elas; so a\n"
        "posicao ao longo da borda compartilhada e livre. Espelho/DPI/\n"
        "Escala nao sao detectados do hardware (xrandr sem --verbose nao\n"
        "expoe isso), so escritos ao Aplicar; propriedades avancadas do\n"
        "driver nao foram portadas.");
    gtk_misc_set_alignment(GTK_MISC(note), 0.0, 0.5);
    gtk_box_pack_start(GTK_BOX(outer), note, FALSE, FALSE, 0);

    g_screens_status_label = gtk_label_new("-");
    gtk_misc_set_alignment(GTK_MISC(g_screens_status_label), 0.0, 0.5);
    gtk_box_pack_start(GTK_BOX(outer), g_screens_status_label, FALSE, FALSE, 0);
    {
        char status[64];
        snprintf(status, sizeof(status), "%d saida(s) detectada(s).", g_n_outputs);
        gtk_label_set_text(GTK_LABEL(g_screens_status_label), status);
    }

    g_screens_canvas = gtk_drawing_area_new();
    gtk_widget_set_size_request(g_screens_canvas, -1, 220);
    gtk_widget_add_events(g_screens_canvas, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK);
    g_signal_connect(g_screens_canvas, "expose-event", G_CALLBACK(screens_canvas_expose), NULL);
    g_signal_connect(g_screens_canvas, "button-press-event", G_CALLBACK(screens_canvas_press), NULL);
    g_signal_connect(g_screens_canvas, "motion-notify-event", G_CALLBACK(screens_canvas_motion), NULL);
    g_signal_connect(g_screens_canvas, "button-release-event", G_CALLBACK(screens_canvas_release), NULL);
    gtk_box_pack_start(GTK_BOX(outer), frame_with("Layout (arraste pra mover)", g_screens_canvas), TRUE, TRUE, 0);

    GtkWidget *form_table = gtk_table_new(8, 2, FALSE);
    g_screens_res_combo = gtk_combo_box_new_text();
    g_signal_connect(g_screens_res_combo, "changed", G_CALLBACK(on_screens_res_changed), NULL);
    labeled_row(form_table, 0, "Resolucao:", g_screens_res_combo);
    g_screens_rate_combo = gtk_combo_box_new_text();
    g_signal_connect(g_screens_rate_combo, "changed", G_CALLBACK(on_screens_rate_changed), NULL);
    labeled_row(form_table, 1, "Taxa de atualizacao:", g_screens_rate_combo);
    g_screens_rot_combo = gtk_combo_box_new_text();
    gtk_combo_box_append_text(GTK_COMBO_BOX(g_screens_rot_combo), "normal");
    gtk_combo_box_append_text(GTK_COMBO_BOX(g_screens_rot_combo), "left");
    gtk_combo_box_append_text(GTK_COMBO_BOX(g_screens_rot_combo), "right");
    gtk_combo_box_append_text(GTK_COMBO_BOX(g_screens_rot_combo), "inverted");
    g_signal_connect(g_screens_rot_combo, "changed", G_CALLBACK(on_screens_rot_changed), NULL);
    labeled_row(form_table, 2, "Rotacao:", g_screens_rot_combo);
    g_screens_enabled_chk = gtk_check_button_new_with_label("Saida ligada");
    g_signal_connect(g_screens_enabled_chk, "toggled", G_CALLBACK(on_screens_enabled_toggled), NULL);
    gtk_table_attach(GTK_TABLE(form_table), g_screens_enabled_chk, 0, 2, 3, 4, GTK_FILL, GTK_FILL, 4, 2);
    g_screens_primary_chk = gtk_check_button_new_with_label("Saida primaria");
    g_signal_connect(g_screens_primary_chk, "toggled", G_CALLBACK(on_screens_primary_toggled), NULL);
    gtk_table_attach(GTK_TABLE(form_table), g_screens_primary_chk, 0, 2, 4, 5, GTK_FILL, GTK_FILL, 4, 2);
    g_screens_mirror_combo = gtk_combo_box_new_text();
    g_signal_connect(g_screens_mirror_combo, "changed", G_CALLBACK(on_screens_mirror_changed), NULL);
    labeled_row(form_table, 5, "Espelhar (mirror):", g_screens_mirror_combo);
    g_screens_dpi_spin = gtk_spin_button_new_with_range(48, 960, 12);
    g_signal_connect(g_screens_dpi_spin, "value-changed", G_CALLBACK(on_screens_dpi_changed), NULL);
    labeled_row(form_table, 6, "DPI:", g_screens_dpi_spin);
    g_screens_scale_spin = gtk_spin_button_new_with_range(0.25, 4.0, 0.05);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(g_screens_scale_spin), 2);
    g_signal_connect(g_screens_scale_spin, "value-changed", G_CALLBACK(on_screens_scale_changed), NULL);
    labeled_row(form_table, 7, "Escala:", g_screens_scale_spin);
    gtk_box_pack_start(GTK_BOX(outer), frame_with("Saida selecionada", form_table), FALSE, FALSE, 0);

    GtkWidget *btnbox = gtk_hbox_new(FALSE, 6);
    GtkWidget *refresh_btn = gtk_button_new_with_label("Detectar novamente");
    GtkWidget *apply_btn = gtk_button_new_with_label("Aplicar");
    g_signal_connect(refresh_btn, "clicked", G_CALLBACK(on_screens_refresh), NULL);
    g_signal_connect(apply_btn, "clicked", G_CALLBACK(on_screens_apply), NULL);
    gtk_box_pack_start(GTK_BOX(btnbox), refresh_btn, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(btnbox), apply_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), btnbox, FALSE, FALSE, 0);

    if (g_screens_selected >= 0) {
        sync_screens_form();
    }

    return outer;
}
