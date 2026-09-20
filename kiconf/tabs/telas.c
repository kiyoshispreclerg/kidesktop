/* kiconf - Telas tab: xrandr layout, draggable canvas.
 * See kiconf.c's top doc comment for the overall design. */
#include "../common.h"
#include "../tabs.h"
#include "../../shared/xis_outputs.h"

#include <gdk/gdkx.h>

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <locale.h>
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
 * comment for what's deliberately NOT ported (scale/DPI read-back). */
#define MAX_OUTPUTS 16
#define MAX_MODES 32
#define MAX_RATES 16
#define MAX_EXTRA_PROPS 24
#define MAX_PROP_SUPPORTED 10
typedef struct {
    char rate[16];
    int is_current;
} ModeRate;
typedef struct {
    char name[16];
    ModeRate rates[MAX_RATES];
    int n_rates;
} OutMode;
/* One `xrandr --verbose` driver property (TearFree, underscan, scaling
 * mode, PRIME Synchronization, HDCP, max bpc, non-desktop, etc. --
 * whatever the driver exposes) -- mirrors xisconf.py's extra_props_meta/
 * extra_prop_values dict pair, flattened into one struct per property
 * since C has no dict. See parse_verbose_props() for how `supported`/
 * `has_range` get filled in (an editable property has one or the
 * other), and build_extra_prop_widget() for what kind of GTK control
 * each becomes. */
typedef struct {
    char name[48];
    char value[64];
    char supported[MAX_PROP_SUPPORTED][40];
    int n_supported;
    int has_range;
    long range_lo, range_hi;
} ExtraProp;
typedef struct {
    char name[NAME_LEN];
    /* "edid:VVV:PPPP:SSSSSSSS" (shared/xis_outputs.h), or empty if this
     * connector has no EDID to read -- filled in by populate_edid_ids()
     * right after detect_outputs() parses `xrandr`'s text output (which
     * has no EDID of its own to give us). `name` is still what every
     * live xrandr call and every on-canvas label uses -- name is exactly
     * what the current connector is called *right now*, which is all a
     * live `xrandr --output <name> ...` call ever needs. `edid_id`
     * exists for the one place that outlives "right now": what
     * save_screens_layout() writes to kiconfd-screens.conf, so a saved
     * layout keeps matching the same physical monitor after a reboot
     * even if RandR happens to enumerate/name its connector differently
     * next time (kiconfd's own apply_screens_layout() already resolves
     * an "edid:..." id back to whatever connector it currently is, the
     * same way xisback/xispanel do -- see shared/xis_outputs.h). */
    char edid_id[XIS_OUTPUT_STR_LEN];
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
    /* `xrandr --verbose`'s per-output driver properties, classified the
     * same way xisconf.py's _parse_verbose_props() does: editable (has a
     * "supported:" enum or a "range:", and isn't force-listed as
     * read-only) vs read-only display-only text. Filled in by
     * parse_verbose_props(), called right after detect_outputs() --
     * unlike mirror_of/dpi/scale above, these ARE read back from
     * hardware fresh on every detect, so screens_redetect_preserving_extras()
     * does not need to preserve them across a re-detect. */
    ExtraProp extra_props[MAX_EXTRA_PROPS];
    int n_extra_props;
    ExtraProp extra_readonly[MAX_EXTRA_PROPS];
    int n_extra_readonly;
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
/* Advanced/other driver properties (xrandr --verbose) -- the *_box is
 * what gets torn down and rebuilt (a fresh GtkTable each time, same
 * "destroy and recreate" approach paineis.c's widget dialogs use) on
 * every selection change or re-detect; the *_frame wrapping it is hidden
 * entirely when the selected output has none of that kind. */
static GtkWidget *g_screens_extra_editable_frame, *g_screens_extra_editable_box;
static GtkWidget *g_screens_extra_readonly_frame, *g_screens_extra_readonly_box;

/* ---- Telas tab: xrandr layout, draggable canvas ----------------------- */
/*
 * Ported subset of xisconf.py's Screens tab: connect/enable/disable,
 * resolution+refresh rate, position (via drag on the canvas -- outputs
 * always dock flush against their nearest neighbor, see screens_dock(),
 * so the layout stays gap-free and the canvas zoom never changes mid-
 * drag), rotation, primary output, mirror/DPI/scale, all diffed against a
 * baseline before Aplicar sends only what changed (see apply_output_diff(),
 * which mirrors xisconf.py's _output_diff_args() field for field).
 * Aplicar also writes the resulting layout to kiconfd-screens.conf (see
 * save_screens_layout()), which kiconfd applies via xrandr at the start
 * of every session -- see kiconfd.c's apply_screens_layout() -- since
 * xrandr's own layout doesn't otherwise survive a session restart.
 * Also ports xisconf.py's generic "advanced driver properties" system
 * (TearFree, underscan, PRIME Synchronization, HDCP, max bpc,
 * non-desktop, etc.) via a second `xrandr --verbose` call -- see
 * parse_verbose_props()/rebuild_extra_props_ui(). mirror/DPI/scale are
 * the one thing still write-only (not read back from hardware), by
 * design -- see the ScreenOutput struct's own comment and
 * screens_redetect_preserving_extras().
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

/* Fills each of outs[0..n)'s edid_id by matching its (already-parsed)
 * name against a live XRandR scan -- `xrandr`'s plain text output has no
 * EDID of its own to give detect_outputs() directly (see ScreenOutput's
 * own comment on edid_id), so this is a second, Xlib-side pass over the
 * same connectors. kiconf isn't otherwise an Xlib client of its own
 * (everything else in this program goes through GTK or a subprocess),
 * but the GTK2/GDK connection already open is the same X display --
 * GDK_DISPLAY_XDISPLAY() just hands back its Display*, no second
 * connection made. */
static void populate_edid_ids(ScreenOutput *outs, int n)
{
    Display *dpy = GDK_DISPLAY_XDISPLAY(gdk_display_get_default());
    XisOutput real[XIS_MAX_OUTPUTS];
    /* forced=1: called from detect_outputs(), itself only ever called
     * interactively (tab open, Detectar novamente, Aplicar) -- not a hot
     * path. See xis_list_outputs()'s own doc comment on `forced`. */
    int n_real = xis_list_outputs(dpy, real, XIS_MAX_OUTPUTS, 1);
    for (int i = 0; i < n; i++) {
        outs[i].edid_id[0] = '\0';
        for (int j = 0; j < n_real; j++) {
            if (!strcmp(real[j].name, outs[i].name)) {
                snprintf(outs[i].edid_id, sizeof(outs[i].edid_id), "%s", real[j].id);
                break;
            }
        }
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
    populate_edid_ids(outs, n);
    return n;
}

/* ---- Telas tab: `xrandr --verbose` driver properties ------------------
 * Ports xisconf.py's generic "advanced properties" system: a second
 * xrandr call (--verbose has a much richer, driver-dependent per-output
 * property dump the plain call above doesn't show at all) classified
 * into editable (has a "supported:" enum or a "range:") vs read-only,
 * with a purely cosmetic force-readonly/hidden list for properties that
 * are technically editable-shaped but make no sense as a user control
 * (LUTs, identifiers, timestamps, EDID, ...) -- see _READONLY_PROPS/
 * _HIDDEN_PROPS in xisconf.py, kept in sync with that list, not derived
 * from anything on this side. DPI is skipped entirely here: it already
 * has its own dedicated spin button (write-only by this file's existing
 * design, see the ScreenOutput struct's own comment), so showing it a
 * second time as a generic property would be redundant. */
static const char *const EXTRA_PROP_FORCE_READONLY[] = {
    "GAMMA_LUT_SIZE", "DEGAMMA_LUT_SIZE", "GAMMA_LUT", "DEGAMMA_LUT",
    "CONNECTOR_ID", "vrr_capable", "link-status",
    "Identifier", "Timestamp", "Subpixel", "Gamma", "Brightness", "Clones",
    "CRTC", "CRTCs", NULL,
};
static const char *const EXTRA_PROP_HIDDEN[] = {"EDID", "_KDE_SCREEN_INDEX", "CTM", NULL};

static int name_in_list(const char *name, const char *const *list)
{
    for (int i = 0; list[i]; i++) {
        if (!strcmp(name, list[i])) {
            return 1;
        }
    }
    return 0;
}

static ScreenOutput *find_output_by_name(ScreenOutput *outs, int n, const char *name)
{
    for (int i = 0; i < n; i++) {
        if (!strcmp(outs[i].name, name)) {
            return &outs[i];
        }
    }
    return NULL;
}

static void add_extra_readonly(ScreenOutput *o, const char *name, const char *val)
{
    if (o->n_extra_readonly >= MAX_EXTRA_PROPS) {
        return;
    }
    ExtraProp *p = &o->extra_readonly[o->n_extra_readonly++];
    snprintf(p->name, sizeof(p->name), "%s", name);
    snprintf(p->value, sizeof(p->value), "%s", val);
}

static void add_extra_prop(ScreenOutput *o, const char *name, const char *val,
                            char supported[][40], int n_supported, int has_range, long lo, long hi)
{
    if (o->n_extra_props >= MAX_EXTRA_PROPS) {
        return;
    }
    ExtraProp *p = &o->extra_props[o->n_extra_props++];
    snprintf(p->name, sizeof(p->name), "%s", name);
    snprintf(p->value, sizeof(p->value), "%s", val);
    p->n_supported = n_supported;
    for (int i = 0; i < n_supported; i++) {
        snprintf(p->supported[i], sizeof(p->supported[0]), "%s", supported[i]);
    }
    p->has_range = has_range;
    p->range_lo = lo;
    p->range_hi = hi;
}

/* Splits `buf` in place on '\n' into `lines[0..return)` -- unlike
 * strtok_r-based splitting elsewhere in this file, this keeps every line
 * including blank ones (`lines[j][0] == '\0'`) since parse_verbose_props()
 * below needs correct line-index lookahead (Transform's 3-line skip, the
 * "\t\t"-prefixed metadata peek). */
static int split_lines_keep_blanks(char *buf, char **lines, int max)
{
    int n = 0;
    char *p = buf;
    while (*p && n < max) {
        lines[n++] = p;
        char *nl = strchr(p, '\n');
        if (!nl) {
            break;
        }
        *nl = '\0';
        p = nl + 1;
    }
    return n;
}

#define MAX_VERBOSE_LINES 8192

/* Mirrors xisconf.py's _parse_verbose_props() -- see its own, more
 * detailed doc comment for the property-classification rules this
 * follows line for line. `text` is mutated (line-split in place). */
static void parse_verbose_props(char *text, ScreenOutput *outs, int n)
{
    static char *lines[MAX_VERBOSE_LINES];
    int nlines = split_lines_keep_blanks(text, lines, MAX_VERBOSE_LINES);
    char current[NAME_LEN] = "";
    int i = 0;
    while (i < nlines) {
        char *line = lines[i];
        if (line[0] != '\0' && !isspace((unsigned char)line[0])) {
            char tmp[512];
            snprintf(tmp, sizeof(tmp), "%s", line);
            char *save2 = NULL;
            char *t1 = strtok_r(tmp, " \t", &save2);
            char *t2 = t1 ? strtok_r(NULL, " \t", &save2) : NULL;
            if (t2 && (!strcmp(t2, "connected") || !strcmp(t2, "disconnected"))) {
                snprintf(current, sizeof(current), "%s", t1);
            } else {
                current[0] = '\0';
            }
            i++;
            continue;
        }
        if (line[0] == '\0' || line[0] != '\t' || line[1] == '\t') {
            i++;
            continue;
        }
        ScreenOutput *out = current[0] ? find_output_by_name(outs, n, current) : NULL;
        if (!out) {
            i++;
            continue;
        }
        char *stripped = line + 1;

        if (!strncmp(stripped, "Transform:", 10)) {
            i += 3;
            continue;
        }
        char *colon = strchr(stripped, ':');
        if (!colon) {
            i++;
            continue;
        }
        char name[48];
        size_t namelen = (size_t)(colon - stripped);
        if (namelen >= sizeof(name)) {
            namelen = sizeof(name) - 1;
        }
        memcpy(name, stripped, namelen);
        name[namelen] = '\0';

        char *val = colon + 1;
        while (*val == ' ') {
            val++;
        }
        size_t vlen = strlen(val);
        while (vlen > 0 && (val[vlen - 1] == ' ' || val[vlen - 1] == '\t' || val[vlen - 1] == '\r')) {
            val[--vlen] = '\0';
        }

        char supported[MAX_PROP_SUPPORTED][40];
        int n_supported = 0;
        int has_range = 0;
        long range_lo = 0, range_hi = 0;
        int j = i + 1;
        while (j < nlines && lines[j][0] == '\t' && lines[j][1] == '\t') {
            char *sub = lines[j] + 2;
            while (*sub == ' ') {
                sub++;
            }
            if (!strncmp(sub, "supported:", 10)) {
                char *list = sub + 10;
                while (*list == ' ') {
                    list++;
                }
                char listbuf[256];
                snprintf(listbuf, sizeof(listbuf), "%s", list);
                char *savec = NULL;
                char *tok = strtok_r(listbuf, ",", &savec);
                while (tok && n_supported < MAX_PROP_SUPPORTED) {
                    while (*tok == ' ') {
                        tok++;
                    }
                    size_t tl = strlen(tok);
                    while (tl > 0 && (tok[tl - 1] == ' ' || tok[tl - 1] == '\t')) {
                        tok[--tl] = '\0';
                    }
                    if (tl > 0) {
                        snprintf(supported[n_supported++], sizeof(supported[0]), "%s", tok);
                    }
                    tok = strtok_r(NULL, ",", &savec);
                }
            } else if (!strncmp(sub, "range:", 6)) {
                long lo, hi;
                if (sscanf(sub, "range: (%ld, %ld)", &lo, &hi) == 2 ||
                    sscanf(sub, "range:(%ld,%ld)", &lo, &hi) == 2) {
                    has_range = 1;
                    range_lo = lo;
                    range_hi = hi;
                }
            }
            j++;
        }

        if (name_in_list(name, EXTRA_PROP_HIDDEN) || !strcmp(name, "DPI")) {
            i = j;
            continue;
        }
        if (name_in_list(name, EXTRA_PROP_FORCE_READONLY) || (n_supported == 0 && !has_range)) {
            add_extra_readonly(out, name, val);
        } else {
            add_extra_prop(out, name, val, supported, n_supported, has_range, range_lo, range_hi);
        }
        i = j;
    }
}

static void detect_verbose_props(ScreenOutput *outs, int n)
{
    char *argv[] = {"xrandr", "--verbose", NULL};
    static char out[262144];
    if (!run_capture(argv, out, sizeof(out))) {
        return;
    }
    parse_verbose_props(out, outs, n);
}

/* Formats `v` as "%.4f" using the "C" locale's '.' decimal point
 * regardless of the process's actual LC_NUMERIC -- kiconf calls
 * setlocale(LC_ALL, "") for i18n (see kiconf.c's main()), so plain
 * snprintf("%.4f", ...) renders e.g. "1,0000" under a comma-decimal
 * locale (pt_BR and most of Europe). That breaks two different
 * machine-readable consumers that don't expect a comma: the --scale
 * argument handed straight to `xrandr` below (which would just reject
 * it), and the scale fields written to kiconfd-screens.conf (which
 * failed kiconfd's sscanf("%lf") entirely, discarding the whole line --
 * see apply_screens_layout() in kiconfd.c). Returns `buf` so it can be
 * used inline as a %s argument. */
static const char *fmt_c_double(char *buf, size_t bufsz, double v)
{
    char saved[64];
    const char *cur = setlocale(LC_NUMERIC, NULL);
    snprintf(saved, sizeof(saved), "%s", cur ? cur : "C");
    setlocale(LC_NUMERIC, "C");
    snprintf(buf, bufsz, "%.4f", v);
    setlocale(LC_NUMERIC, saved);
    return buf;
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
        char sxbuf[32], sybuf[32];
        snprintf(scalebuf, sizeof(scalebuf), "%sx%s", fmt_c_double(sxbuf, sizeof(sxbuf), sx),
                  fmt_c_double(sybuf, sizeof(sybuf), sy));
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
        char sxbuf[32], sybuf[32];
        snprintf(scalebuf, sizeof(scalebuf), "%sx%s", fmt_c_double(sxbuf, sizeof(sxbuf), sx),
                  fmt_c_double(sybuf, sizeof(sybuf), sy));
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

/* Sends one `xrandr --output NAME --set PROPNAME value` call per advanced
 * property that changed since `base` -- a separate call per property
 * rather than folding them into apply_output_diff()'s own argv (like
 * xisconf.py's _output_diff_args() does) since there can be many more of
 * these than that fixed-size array has room for. */
static void apply_extra_prop_diffs(const ScreenOutput *o, const ScreenOutput *base)
{
    if (!o->connected || !o->enabled) {
        return;
    }
    for (int i = 0; i < o->n_extra_props; i++) {
        const ExtraProp *p = &o->extra_props[i];
        const char *old_val = NULL;
        for (int j = 0; j < base->n_extra_props; j++) {
            if (!strcmp(base->extra_props[j].name, p->name)) {
                old_val = base->extra_props[j].value;
                break;
            }
        }
        if (old_val && !strcmp(old_val, p->value)) {
            continue;
        }
        char *argv[] = {"xrandr", "--output", (char *)o->name, "--set", (char *)p->name, (char *)p->value, NULL};
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

/* ---- Telas tab: advanced/other properties widgets ---------------------
 * Each ExtraProp* handed to these callbacks points directly into
 * g_outputs[selected].extra_props[]/extra_readonly[] -- stable for as
 * long as the widget referencing it is alive, since only a re-detect
 * (which always tears down and rebuilds every one of these widgets right
 * along with it, see rebuild_extra_props_ui()) ever overwrites that
 * memory. */
static void on_extra_prop_combo_changed(GtkWidget *widget, gpointer data)
{
    ExtraProp *p = (ExtraProp *)data;
    if (g_screens_syncing) {
        return;
    }
    gchar *txt = gtk_combo_box_get_active_text(GTK_COMBO_BOX(widget));
    if (txt) {
        snprintf(p->value, sizeof(p->value), "%s", txt);
        g_free(txt);
    }
}

static void on_extra_prop_bool_changed(GtkWidget *widget, gpointer data)
{
    ExtraProp *p = (ExtraProp *)data;
    if (g_screens_syncing) {
        return;
    }
    snprintf(p->value, sizeof(p->value), "%s", gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget)) ? "1" : "0");
}

static void on_extra_prop_spin_changed(GtkWidget *widget, gpointer data)
{
    ExtraProp *p = (ExtraProp *)data;
    if (g_screens_syncing) {
        return;
    }
    snprintf(p->value, sizeof(p->value), "%d", gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(widget)));
}

static GtkWidget *build_extra_prop_widget(ExtraProp *p)
{
    if (p->n_supported > 0) {
        GtkWidget *w = gtk_combo_box_new_text();
        int active = -1;
        for (int i = 0; i < p->n_supported; i++) {
            gtk_combo_box_append_text(GTK_COMBO_BOX(w), p->supported[i]);
            if (!strcmp(p->supported[i], p->value)) {
                active = i;
            }
        }
        gtk_combo_box_set_active(GTK_COMBO_BOX(w), active >= 0 ? active : 0);
        g_signal_connect(w, "changed", G_CALLBACK(on_extra_prop_combo_changed), p);
        return w;
    }
    if (p->has_range) {
        if (p->range_lo == 0 && p->range_hi == 1) {
            GtkWidget *w = gtk_check_button_new();
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w), p->value[0] && strcmp(p->value, "0"));
            g_signal_connect(w, "toggled", G_CALLBACK(on_extra_prop_bool_changed), p);
            return w;
        }
        GtkWidget *w = gtk_spin_button_new_with_range((double)p->range_lo, (double)p->range_hi, 1);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(w), atof(p->value));
        g_signal_connect(w, "value-changed", G_CALLBACK(on_extra_prop_spin_changed), p);
        return w;
    }
    return gtk_label_new(p->value);
}

static void clear_container(GtkWidget *box)
{
    GList *kids = gtk_container_get_children(GTK_CONTAINER(box));
    for (GList *l = kids; l; l = l->next) {
        gtk_widget_destroy(GTK_WIDGET(l->data));
    }
    g_list_free(kids);
}

/* Rebuilds both property groups for the currently selected output --
 * called by sync_screens_form() (every selection change) and after every
 * re-detect, since the properties themselves are read back from hardware
 * fresh each time (see the ScreenOutput struct's own comment on this). */
static void rebuild_extra_props_ui(void)
{
    clear_container(g_screens_extra_editable_box);
    clear_container(g_screens_extra_readonly_box);
    if (g_screens_selected < 0) {
        gtk_widget_hide(g_screens_extra_editable_frame);
        gtk_widget_hide(g_screens_extra_readonly_frame);
        return;
    }
    ScreenOutput *o = &g_outputs[g_screens_selected];

    if (o->n_extra_props > 0) {
        GtkWidget *table = gtk_table_new(o->n_extra_props, 2, FALSE);
        for (int i = 0; i < o->n_extra_props; i++) {
            char label[64];
            snprintf(label, sizeof(label), "%s:", o->extra_props[i].name);
            GtkWidget *w = build_extra_prop_widget(&o->extra_props[i]);
            labeled_row(table, i, label, w);
        }
        gtk_box_pack_start(GTK_BOX(g_screens_extra_editable_box), table, FALSE, FALSE, 0);
        gtk_widget_show_all(g_screens_extra_editable_box);
        gtk_widget_show(g_screens_extra_editable_frame);
    } else {
        gtk_widget_hide(g_screens_extra_editable_frame);
    }

    if (o->n_extra_readonly > 0) {
        /* 2 side-by-side label:value columns instead of one long single
         * column list -- read-only properties can pile up (a dozen+ on
         * some drivers), same split xisconf.py's own form_extra_readonly/
         * form_extra_readonly2 does. */
        int half = (o->n_extra_readonly + 1) / 2;
        int n2 = o->n_extra_readonly - half;
        GtkWidget *hbox = gtk_hbox_new(FALSE, 12);
        GtkWidget *t1 = gtk_table_new(half > 0 ? half : 1, 2, FALSE);
        GtkWidget *t2 = gtk_table_new(n2 > 0 ? n2 : 1, 2, FALSE);
        for (int i = 0; i < o->n_extra_readonly; i++) {
            char label[64];
            snprintf(label, sizeof(label), "%s:", o->extra_readonly[i].name);
            GtkWidget *val = gtk_label_new(o->extra_readonly[i].value);
            gtk_misc_set_alignment(GTK_MISC(val), 0.0, 0.5);
            if (i < half) {
                labeled_row(t1, i, label, val);
            } else {
                labeled_row(t2, i - half, label, val);
            }
        }
        gtk_box_pack_start(GTK_BOX(hbox), t1, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(hbox), t2, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(g_screens_extra_readonly_box), hbox, FALSE, FALSE, 0);
        gtk_widget_show_all(g_screens_extra_readonly_box);
        gtk_widget_show(g_screens_extra_readonly_frame);
    } else {
        gtk_widget_hide(g_screens_extra_readonly_frame);
    }
}

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

    rebuild_extra_props_ui();

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
    detect_verbose_props(g_outputs, g_n_outputs);

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

/* Writes the current layout to kiconfd-screens.conf, one line per
 * connected output, in the fixed field order apply_screens_layout() in
 * kiconfd.c expects: NAME ENABLED MODE RATE X Y ROTATION PRIMARY SCALE_X
 * SCALE_Y DPI MIRROR ('-' standing in for an absent MODE/RATE/MIRROR).
 * kiconfd reads this once at session start and replays it via xrandr, so
 * the layout picked here survives a logout/login -- xrandr's own state
 * doesn't. Called right after Aplicar, once g_outputs reflects what was
 * actually just applied (post screens_redetect_preserving_extras()). */
/* The stable identifier to persist for the output currently named
 * `name` -- its edid_id if it has one, else the plain connector name
 * unchanged (a virtual/headless output with no EDID, say). Looks it up
 * among g_outputs rather than taking a ScreenOutput* directly since it's
 * also used for mirror_of, which only ever stores another output's
 * *name*, not a pointer to it. */
static const char *screens_persist_id(const char *name)
{
    for (int i = 0; i < g_n_outputs; i++) {
        if (!strcmp(g_outputs[i].name, name)) {
            return g_outputs[i].edid_id[0] ? g_outputs[i].edid_id : g_outputs[i].name;
        }
    }
    return name;
}

/* Writes the persisted id (edid_id when available -- see ScreenOutput's
 * own comment -- else the plain connector name) rather than always the
 * live connector name: kiconfd's apply_screens_layout() resolves either
 * kind back to a real connector at the next session's start (same
 * xis_resolve_output()-based resolution as xisback/xispanel), but only
 * the edid_id form keeps matching the same physical monitor if RandR
 * happens to enumerate/name it differently next boot. */
static void save_screens_layout(void)
{
    char path[PATH_MAX];
    resolve_path("kiconfd-screens.conf", path, sizeof(path));
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) {
        g_warning("kiconf: could not write '%s': %s", tmp, strerror(errno));
        return;
    }
    fprintf(f, "# kiconfd screens layout -- generated by kiconf's Telas tab,\n");
    fprintf(f, "# applied via xrandr by kiconfd at the start of each session.\n");
    for (int i = 0; i < g_n_outputs; i++) {
        ScreenOutput *o = &g_outputs[i];
        if (!o->connected) {
            continue;
        }
        double sx = fabs(o->scale_x) > 1e-6 ? o->scale_x : 1.0;
        double sy = fabs(o->scale_y) > 1e-6 ? o->scale_y : 1.0;
        char sxbuf[32], sybuf[32];
        fmt_c_double(sxbuf, sizeof(sxbuf), sx);
        fmt_c_double(sybuf, sizeof(sybuf), sy);
        fprintf(f, "%s %d %s %s %d %d %s %d %s %s %d %s\n",
                 o->edid_id[0] ? o->edid_id : o->name, o->enabled,
                 o->current_mode[0] ? o->current_mode : "-",
                 o->current_rate[0] ? o->current_rate : "-",
                 o->x, o->y, o->rotation, o->primary, sxbuf, sybuf, o->dpi,
                 o->mirror_of[0] ? screens_persist_id(o->mirror_of) : "-");
    }
    fclose(f);
    if (rename(tmp, path) != 0) {
        g_warning("kiconf: could not save '%s': %s", path, strerror(errno));
    }
}

static void on_screens_apply(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    for (int i = 0; i < g_n_outputs; i++) {
        apply_output_diff(&g_outputs[i], &g_outputs_baseline[i]);
        apply_extra_prop_diffs(&g_outputs[i], &g_outputs_baseline[i]);
    }
    screens_redetect_preserving_extras();
    save_screens_layout();
    if (g_screens_selected >= g_n_outputs) {
        g_screens_selected = -1;
    }
    char status[64];
    snprintf(status, sizeof(status), "%d saida(s) detectada(s).", g_n_outputs);
    gtk_label_set_text(GTK_LABEL(g_screens_status_label), status);
    if (g_screens_selected >= 0) {
        sync_screens_form();
    } else {
        rebuild_extra_props_ui();
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
    } else {
        rebuild_extra_props_ui();
    }
    gtk_widget_queue_draw(g_screens_canvas);
}

GtkWidget *build_telas_tab(void)
{
    g_n_outputs = detect_outputs(g_outputs, MAX_OUTPUTS);
    detect_verbose_props(g_outputs, g_n_outputs);
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
        "Escala nao sao detectados do hardware (nem xrandr --verbose\n"
        "expoe isso), so escritos ao Aplicar. Aplicar tambem grava o\n"
        "layout pra ser reaplicado automaticamente no inicio da proxima\n"
        "sessao.");
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

    /* The 3 per-output sections side by side in one row instead of
     * stacked -- "Outras propriedades" especially can pile up a dozen+
     * rows on some drivers, which made the tab very tall stacked
     * vertically; each of the 2 property sections scrolls internally
     * (fixed height) instead of growing the tab further. */
    GtkWidget *sections_row = gtk_hbox_new(FALSE, 8);
    gtk_box_pack_start(GTK_BOX(sections_row), frame_with("Saida selecionada", form_table), TRUE, TRUE, 0);

    /* Advanced (editable) driver properties -- TearFree, underscan,
     * scaling mode, PRIME Synchronization, HDCP, max bpc, non-desktop,
     * etc., whatever `xrandr --verbose` exposes for the selected output.
     * Hidden entirely when it has none (see rebuild_extra_props_ui()). */
    g_screens_extra_editable_box = gtk_vbox_new(FALSE, 4);
    GtkWidget *extra_editable_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(extra_editable_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_add_with_viewport(GTK_SCROLLED_WINDOW(extra_editable_scroll), g_screens_extra_editable_box);
    gtk_widget_set_size_request(extra_editable_scroll, -1, 200);
    g_screens_extra_editable_frame = frame_with("Propriedades avancadas", extra_editable_scroll);
    gtk_box_pack_start(GTK_BOX(sections_row), g_screens_extra_editable_frame, TRUE, TRUE, 0);

    /* Other (read-only) properties -- same source, but forced read-only
     * (LUTs, identifiers, timestamps, ...) or with no editable shape at
     * all (no "supported:"/"range:" sub-line). */
    g_screens_extra_readonly_box = gtk_vbox_new(FALSE, 4);
    GtkWidget *extra_readonly_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(extra_readonly_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_add_with_viewport(GTK_SCROLLED_WINDOW(extra_readonly_scroll), g_screens_extra_readonly_box);
    gtk_widget_set_size_request(extra_readonly_scroll, -1, 200);
    g_screens_extra_readonly_frame = frame_with("Outras propriedades", extra_readonly_scroll);
    gtk_box_pack_start(GTK_BOX(sections_row), g_screens_extra_readonly_frame, TRUE, TRUE, 0);

    gtk_box_pack_start(GTK_BOX(outer), sections_row, FALSE, FALSE, 0);

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
    } else {
        rebuild_extra_props_ui();
    }

    return outer;
}
