/*
 * xispanel - minimal desktop panel/taskbar daemon.
 *
 * One process per session (guarded by an flock'd lock file under
 * XDG_RUNTIME_DIR, same pattern as xisback/xisguard). Any further
 * invocation talks to the already running instance over a Unix socket
 * (see PROTOCOL.md) instead of spawning a second process.
 *
 * A "panel" is a bar anchored to one edge (top/bottom/left/right) of one
 * XRandR output, sized by a percentage of that edge plus a fixed
 * thickness, holding an ordered list of "widgets" (spacer, clock,
 * tasklist, ...). Multiple panels (e.g. one per output, or two on the
 * same output) are multiple entries in one config file served by one
 * daemon process -- same relationship xisback has between its process
 * and its per-(output, desktop) wallpaper layers.
 *
 * Panel windows are override-redirect: this keeps xispanel independent of
 * whatever window manager happens to be running (no reparenting, no
 * negotiating "always on top" state with the WM) while still reserving
 * screen space via _NET_WM_STRUT_PARTIAL in "dock" mode -- every WM that
 * matters here reads that property from any top-level window, managed or
 * not. The tradeoff is that xispanel itself is fully responsible for
 * raising/positioning/clipping its own windows, which is why the autohide
 * state machine below does its own slide animation instead of asking the
 * WM for one.
 *
 * This file owns the panel/window/layout/config/IPC machinery. Widget
 * types live in their own widgets/ directory (see xispanel.h for the
 * PanelWidgetOps vtable + core API they're built against) and are wired
 * in as a compile-time registry below -- no dlopen, in the same "flat
 * files, no abstraction beyond what's needed" spirit as the rest of this
 * kit. EWMH/ICCCM helpers live in ewmh.c, the context-menu popup in
 * menu.c. Widget add/remove/reorder is done by editing the config file
 * and sending RELOAD (or restarting) -- there is no live IPC mutation of
 * a panel's widget list yet, see PROTOCOL.md.
 *
 * Rendering: one ARGB32 (if available) cairo_xlib_surface per panel
 * window. Imlib2 is pulled in for future bitmap-theme decoding (nothing
 * uses it yet); window icons come from _NET_WM_ICON via Xlib directly
 * (ewmh.c), not Imlib2 -- it's already raw ARGB pixel data, no
 * image-format decoding needed. Text goes through
 * cairo_ft_font_face_create_for_ft_face with a Fontconfig-resolved font
 * -- no Pango, no GLib.
 *
 * Config lives at $XDG_CONFIG_HOME/xispanel.conf and is *not* written by
 * this program in general: PANEL/WIDGET/THEME lines are hand-edited (or
 * written by a future xisconf tab) and picked up on startup or via the
 * IPC RELOAD command. Persisting live-mutated state (SET_WIDGET etc. over
 * the IPC socket) is a later phase once there is any IPC command that
 * actually mutates a panel. See PROTOCOL.md.
 *
 * One narrow, deliberate exception: tasklist.c's pinned-apps list writes
 * its own fixed_list= key onto its own WIDGET line the first time the
 * user pins something (see config_widget_set_key() and
 * tasklist_persist_pinned() in widgets/tasklist.c) -- not a general
 * config-mutation mechanism, just enough for one widget's own sidecar
 * file path to survive a restart without requiring the user to hand-edit
 * it in first.
 */

#include "xispanel.h"

#include "../shared/xis_outputs.h"

#include <Imlib2.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrandr.h>
#include <X11/extensions/shape.h>

#include <cairo/cairo-ft.h>
#include <cairo/cairo-xlib.h>
#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define XISPANEL_VERSION "0.6.47"
#define MAX_PANELS 8
#define LINE_MAX_LEN 2048
/* 64KB, not 4KB: GET_NOTIFICATIONS can hand back up to NOTIFD_MAX (50)
 * full-size entries (app_name/summary/body near their NOTIFD_*_MAX caps,
 * JSON-escaped to up to 2x) in one response -- worst case runs past 50KB.
 * Every other command's request/response stays tiny; this only costs a
 * bigger stack buffer per accepted connection, never held long-term. */
#define IPC_MAX_LEN 65536
#define AUTOHIDE_ANIM_MS 150
#define AUTOHIDE_DELAY_MS 400

/* ------------------------------------------------------------------ */
/* globals                                                              */
/* ------------------------------------------------------------------ */

Display *g_dpy;
Window g_root;
int g_screen;
cairo_font_face_t *g_font_face;
char g_font_family[128]; /* filled once at startup, see config_scan_globals() */
char g_icon_theme[128];  /* same -- THEME's icon_theme=, read by ewmh.c's resolve_icon_theme_name() */

static int g_rr_event_base;
static volatile sig_atomic_t g_quit = 0;
static Panel g_panels[MAX_PANELS];
static char g_configpath[PATH_MAX];
/* The one container popup currently open (at most one at a time, same
 * as menus) -- see the "container popups" section. */
static Panel *g_open_container = NULL;

static FT_Library g_ft_lib;
static FT_Face g_ft_face;

/* Only needed for dock-mode windows, which (unlike overlay/autohide) are
 * managed rather than override-redirect -- see panel_create_window(). */
static Atom g_atom_net_wm_state;
static Atom g_atom_net_wm_state_skip_taskbar;
static Atom g_atom_net_wm_state_skip_pager;
static Atom g_atom_net_wm_desktop;

/* ------------------------------------------------------------------ */
/* small helpers exposed to widgets                                     */
/* ------------------------------------------------------------------ */

uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000L);
}

/* Looks up "key=value" inside a whitespace-separated token line (the tail
 * of a PANEL/WIDGET/THEME config record, or a widget's own config_kv). A
 * value may be wrapped in double quotes to embed spaces (e.g. launcher's
 * `cmd="xterm -e htop"`) -- everything between the quotes, unparsed, is
 * treated as one token, and the surrounding quotes themselves are
 * stripped from the returned value. No escaping inside quotes (a value
 * can't contain a literal `"`); not needed by anything so far. Returns 1
 * if found. */
int kv_get(const char *kvline, const char *key, char *out, size_t outsz)
{
    if (!kvline) {
        return 0;
    }
    size_t keylen = strlen(key);
    const char *p = kvline;
    while (*p) {
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (!*p) {
            break;
        }
        const char *tok_start = p;
        int in_quotes = 0;
        while (*p && (in_quotes || (*p != ' ' && *p != '\t'))) {
            if (*p == '"') {
                in_quotes = !in_quotes;
            }
            p++;
        }
        size_t tok_len = (size_t)(p - tok_start);
        if (tok_len > keylen && tok_start[keylen] == '=' && strncmp(tok_start, key, keylen) == 0) {
            const char *val_start = tok_start + keylen + 1;
            size_t vlen = tok_len - keylen - 1;
            if (vlen >= 2 && val_start[0] == '"' && val_start[vlen - 1] == '"') {
                val_start++;
                vlen -= 2;
            }
            if (vlen >= outsz) {
                vlen = outsz - 1;
            }
            memcpy(out, val_start, vlen);
            out[vlen] = 0;
            return 1;
        }
    }
    return 0;
}

int kv_get_int(const char *kvline, const char *key, int defval)
{
    char buf[32];
    if (kv_get(kvline, key, buf, sizeof(buf))) {
        return atoi(buf);
    }
    return defval;
}

/* Local-frame rectangle for a widget to paint into: origin always at
 * (0,0), since panel_repaint() already translates+rotates the cairo_t
 * before calling paint() -- a widget never needs to know its own on-panel
 * position, the panel's edge, or its rotate angle.
 *
 * The *shape* reported still depends on both: at rotate 0/180 a widget
 * gets its panel's natural physical shape (wide/short for top/bottom,
 * narrow/tall for left/right); at 90/270 that shape is transposed, since
 * panel_repaint() rotates a quarter turn to fit the *same* physical
 * footprint with width and height swapped. Every widget (text, icons,
 * buttons, everything) therefore renders "rotated" for free with zero
 * orientation-specific code -- see the comment above panel_repaint(). */
void widget_get_rect(const PanelWidget *w, int *x, int *y, int *width, int *height)
{
    Panel *p = w->panel;
    int natural_horizontal = (p->edge == EDGE_TOP || p->edge == EDGE_BOTTOM);
    int transposed = (p->rotate == 90 || p->rotate == 270);
    *x = 0;
    *y = 0;
    if (natural_horizontal != transposed) {
        *width = w->len;
        *height = w->thickness;
    } else {
        *width = w->thickness;
        *height = w->len;
    }
}

PanelWidget *panel_widget_at(Panel *p, int axis_pos, int cross_pos)
{
    for (int i = 0; i < p->n_layout; i++) {
        PanelWidget *w = p->layout[i];
        if (axis_pos >= w->x && axis_pos < w->x + w->len && cross_pos >= w->y && cross_pos < w->y + w->thickness) {
            return w;
        }
    }
    return NULL;
}

int panel_widget_hover_local_x(const PanelWidget *w, int *out_local_x)
{
    if (w->panel->hover_widget != w) {
        return 0;
    }
    if (out_local_x) {
        *out_local_x = w->panel->hover_local_x;
    }
    return 1;
}

int panel_widget_hover_local_y(const PanelWidget *w, int *out_local_y)
{
    if (w->panel->hover_widget != w) {
        return 0;
    }
    if (out_local_y) {
        *out_local_y = w->panel->hover_local_y;
    }
    return 1;
}

void widget_paint_hover_cell(PanelWidget *w, cairo_t *cr, int x, int y, int width, int height)
{
    Panel *p = w->panel;
    /* A theme's button.png replaces the translucent wash entirely -- this
     * is the one place every widget's generic hover feedback goes
     * through, so skinning it here covers launcher/folder/clock/volume/
     * notif/xisserve/tray at once. */
    if (panel_draw_skin(&p->button_skin, cr, SKIN_HOVER, x, y, width, height)) {
        return;
    }
    if (p->has_h_color) {
        cairo_set_source_rgba(cr, p->h_r, p->h_g, p->h_b, p->h_a);
    } else {
        cairo_set_source_rgba(cr, p->fg_r, p->fg_g, p->fg_b, 0.12);
    }
    cairo_rectangle(cr, x, y, width, height);
    cairo_fill(cr);
}

void widget_paint_hover_rect(PanelWidget *w, cairo_t *cr, int x, int width)
{
    widget_paint_hover_cell(w, cr, x, 0, width, w->thickness);
}

void widget_paint_hover_bg(PanelWidget *w, cairo_t *cr)
{
    if (panel_widget_hover_local_x(w, NULL)) {
        widget_paint_hover_rect(w, cr, 0, w->len);
    }
}

double panel_text_size(const Panel *p)
{
    return p->font_size_px > 0 ? p->font_size_px : p->thickness * 0.45;
}

/* ------------------------------------------------------------------ */
/* config-only string helpers (not part of the widget-facing API)       */
/* ------------------------------------------------------------------ */

static void join_fields(char **fields, int start, int nf, char *out, size_t outsz)
{
    out[0] = 0;
    size_t used = 0;
    for (int i = start; i < nf; i++) {
        size_t len = strlen(fields[i]);
        if (used + len + 2 >= outsz) {
            break;
        }
        if (used > 0) {
            out[used++] = ' ';
        }
        memcpy(out + used, fields[i], len);
        used += len;
        out[used] = 0;
    }
}

int parse_hex_color(const char *hex, double *r, double *g, double *b, double *a)
{
    if (!hex || hex[0] != '#') {
        return 0;
    }
    size_t len = strlen(hex);
    if (len != 7 && len != 9) {
        return 0;
    }
    unsigned int ri, gi, bi, ai = 255;
    if (sscanf(hex + 1, "%2x%2x%2x", &ri, &gi, &bi) != 3) {
        return 0;
    }
    if (len == 9) {
        sscanf(hex + 7, "%2x", &ai);
    }
    *r = ri / 255.0;
    *g = gi / 255.0;
    *b = bi / 255.0;
    *a = ai / 255.0;
    return 1;
}

/* ------------------------------------------------------------------ */
/* widget registry                                                      */
/* ------------------------------------------------------------------ */

static const PanelWidgetOps *g_widget_registry[] = {
    &spacer_ops,
    &clock_ops,
    &tasklist_ops,
    &winctl_ops,
    &tray_ops,
    &launcher_ops,
    &volume_ops,
    &globalmenu_ops,
    &folder_ops,
    &xisserve_ops,
    &notif_ops,
    &pager_ops,
    &monitor_ops,
    &energy_ops,
    &container_ops,
    &network_ops,
    &storage_ops,
    NULL,
};

static const PanelWidgetOps *find_widget_ops(const char *type_name)
{
    for (int i = 0; g_widget_registry[i]; i++) {
        if (strcmp(g_widget_registry[i]->type_name, type_name) == 0) {
            return g_widget_registry[i];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* font setup                                                           */
/* ------------------------------------------------------------------ */

/* xispanel.conf is the only source of truth for how the panel looks --
 * nothing is read out of any other desktop's configuration (kdeglobals,
 * gtk-3.0/settings.ini, ...) any more. Font family and icon theme are
 * process-global (one FT face, one icon search root for every panel),
 * but they are still written on a THEME line, since that is where every
 * other appearance key already lives; the first THEME line that sets
 * each key wins. Read here by a plain pre-scan of the config file rather
 * than in apply_theme_kv(), because init_font()/pango_text_init() have
 * to run before any panel exists to be themed.
 *
 * Unset font= keeps fontconfig's generic "sans-serif" (init_font()'s own
 * fallback); unset icon_theme= leaves resolve_icon_theme_name() on its
 * hardcoded breeze/Adwaita/hicolor roots. Colors work the same way one
 * level down: no bg=/fg= means alloc_panel()'s built-in dark defaults. */
static void config_scan_globals(void)
{
    g_font_family[0] = 0;
    g_icon_theme[0] = 0;
    char theme_dir[PATH_MAX];
    theme_dir[0] = 0;
    FILE *f = fopen(g_configpath, "r");
    if (!f) {
        return;
    }
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = 0;
        }
        if (strncmp(line, "THEME", 5) != 0) {
            continue;
        }
        if (!g_font_family[0]) {
            kv_get(line, "font", g_font_family, sizeof(g_font_family));
        }
        if (!g_icon_theme[0]) {
            kv_get(line, "icon_theme", g_icon_theme, sizeof(g_icon_theme));
        }
        if (!theme_dir[0]) {
            kv_get(line, "theme", theme_dir, sizeof(theme_dir));
        }
        if (g_font_family[0] && g_icon_theme[0]) {
            break;
        }
    }
    fclose(f);

    /* Last resort for the font: the theme folder's own `colors` file,
     * whose font=/font_size= keys kiwm already reads for its titlebars --
     * so pointing both programs at one theme gives the whole desktop one
     * font without repeating it in either config. An explicit THEME
     * font= still wins, since it was read above. */
    if (!g_font_family[0] && theme_dir[0]) {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/colors", theme_dir);
        FILE *cf = fopen(path, "r");
        if (cf) {
            while (fgets(line, sizeof(line), cf)) {
                if (line[0] == '#') {
                    continue;
                }
                if (!strncmp(line, "font=", 5)) {
                    snprintf(g_font_family, sizeof(g_font_family), "%s", line + 5);
                    size_t l = strlen(g_font_family);
                    while (l && (g_font_family[l - 1] == '\n' || g_font_family[l - 1] == '\r' ||
                                 g_font_family[l - 1] == ' ')) {
                        g_font_family[--l] = 0;
                    }
                    break;
                }
            }
            fclose(cf);
        }
    }
}

static int init_font(const char *family_hint)
{
    if (FT_Init_FreeType(&g_ft_lib) != 0) {
        return -1;
    }
    if (!FcInit()) {
        return -1;
    }
    FcPattern *pat = FcNameParse((const FcChar8 *)(family_hint && *family_hint ? family_hint : "sans-serif"));
    FcConfigSubstitute(NULL, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);
    FcResult result;
    FcPattern *match = FcFontMatch(NULL, pat, &result);
    FcPatternDestroy(pat);
    if (!match) {
        return -1;
    }
    FcChar8 *file = NULL;
    if (FcPatternGetString(match, FC_FILE, 0, &file) != FcResultMatch) {
        FcPatternDestroy(match);
        return -1;
    }
    if (FT_New_Face(g_ft_lib, (const char *)file, 0, &g_ft_face) != 0) {
        FcPatternDestroy(match);
        return -1;
    }
    FcPatternDestroy(match);
    g_font_face = cairo_ft_font_face_create_for_ft_face(g_ft_face, 0);
    return 0;
}

/* ------------------------------------------------------------------ */
/* RandR output geometry (same pattern as xisback's resolve_output_geometry)*/
/* ------------------------------------------------------------------ */

/* Resolves `name` -- a plain RandR connector name, an "edid:..."
 * stable-monitor id (see shared/xis_outputs.h), or "*" -- to the plain
 * connector name it currently refers to, via the same shared, canonical
 * xis_resolve_output() every other kidesktop program (xisback, kiconfd)
 * already uses for this. Unlike resolve_output_geometry() below (a hot
 * path that special-cases only the edid: prefix, to avoid this same
 * XRRGetScreenResourcesCurrent() round trip twice on every single
 * geometry resolve -- see its own comment), this one is for callers that
 * need a *name*, not a geometry, and aren't called anywhere near that
 * often: pager.c's same_output_only, and ewmh_kiwm_current_desktop_for_
 * output() (and everything built on it -- ewmh_resolve_active_for_
 * output(), tasklist.c/winctl.c, and globalmenu.c's own same_output_only
 * filter), all of which need to compare a panel's configured output
 * against kiwm's own _KIWM_OUTPUTS (always plain connector names, never
 * edid: ids) in the same plain-name terms. Without this, comparing an
 * unresolved edid: id (the common case once a panel's output has ever
 * been set via kiconf's own output combo box) against that list would
 * silently never match anything, and every one of those "restrict to
 * this panel's own output" features would fall back to treating every
 * output as this one's own -- looking exactly as if the feature were
 * off despite the config asking for it. Returns 0 (leaving `out`
 * untouched) for "*", or a name that doesn't currently resolve to a
 * connected output (edid: or plain -- unlike the hot path below, a
 * stale/disconnected plain name is caught here too, not just edid:
 * ids); 1 otherwise. */
int panel_resolve_output_name(const char *name, char *out, size_t outsz)
{
    return xis_resolve_output(g_dpy, name, out, outsz, 0);
}

/* *out_hz is left at 0 if the CRTC's current mode has no usable timing
 * info to compute one from -- callers should treat that as "unknown",
 * not "the output truly refreshes at 0Hz".
 *
 * `name` can also be an "edid:..." stable-monitor id (see
 * shared/xis_outputs.h) instead of a literal connector name -- resolved
 * fresh, live, right here, every call, so a connector rename between two
 * calls never needs any reconcile step for these the way a plain saved
 * name does (see build_output_rename_map() below, which only ever
 * touches plain-name panels). Deliberately doesn't just call
 * panel_resolve_output_name() for every name unconditionally: this runs
 * on every geometry resolve (panel move, RandR event, the periodic
 * reconcile poll), a hot path, and that would mean two full
 * XRRGetScreenResourcesCurrent()-plus-output-loop passes (one inside
 * xis_resolve_output(), one right here) for the common plain-name case
 * that needs only one. */
static int resolve_output_geometry(const char *name, int *ox, int *oy, int *ow, int *oh, double *out_hz)
{
    char resolved[XIS_OUTPUT_STR_LEN];
    if (strncmp(name, "edid:", 5) == 0) {
        /* forced=0: see xis_list_outputs()'s own doc comment on
         * `forced`; correct without ever forcing here because main()
         * forces one poll at startup before the first load_config(),
         * enough for every later cached read this session to see fresh
         * EDID -- RandR-change events keep the cache itself current
         * after that. */
        if (!xis_resolve_output(g_dpy, name, resolved, sizeof(resolved), 0)) {
            return 0;
        }
        name = resolved;
    }
    XRRScreenResources *res = XRRGetScreenResourcesCurrent(g_dpy, g_root);
    if (!res) {
        return 0;
    }
    int found = 0;
    for (int i = 0; i < res->noutput && !found; i++) {
        XRROutputInfo *oi = XRRGetOutputInfo(g_dpy, res, res->outputs[i]);
        if (oi && oi->connection == RR_Connected && oi->crtc && strcmp(oi->name, name) == 0) {
            XRRCrtcInfo *ci = XRRGetCrtcInfo(g_dpy, res, oi->crtc);
            if (ci) {
                *ox = ci->x;
                *oy = ci->y;
                *ow = (int)ci->width;
                *oh = (int)ci->height;
                /* X-INPUT-SCALE (see inputscale.c's doc comment): a
                 * compositor doing per-output HiDPI downscaling can
                 * confine this CRTC's cursor (and, by convention, its
                 * actual usable desktop-space area) to a box smaller than
                 * the raw physical scanout -- use that instead when
                 * active, so the panel doesn't oversize itself relative
                 * to everything else drawn at that output's logical
                 * resolution. */
                inputscale_get_confine((unsigned long)oi->crtc, ox, oy, ow, oh);
                *out_hz = 0;
                for (int m = 0; m < res->nmode; m++) {
                    if (res->modes[m].id != ci->mode) {
                        continue;
                    }
                    XRRModeInfo *mi = &res->modes[m];
                    if (mi->hTotal == 0 || mi->vTotal == 0) {
                        break;
                    }
                    /* Same formula xrandr itself uses to print "60.00*" etc. */
                    double vtotal = mi->vTotal;
                    if (mi->modeFlags & RR_DoubleScan) {
                        vtotal *= 2;
                    }
                    if (mi->modeFlags & RR_Interlace) {
                        vtotal /= 2;
                    }
                    *out_hz = (double)mi->dotClock / ((double)mi->hTotal * vtotal);
                    break;
                }
                found = 1;
                XRRFreeCrtcInfo(ci);
            }
        }
        if (oi) {
            XRRFreeOutputInfo(oi);
        }
    }
    XRRFreeScreenResources(res);
    return found;
}

int panel_resolve_own_output(const Panel *p, char *out, size_t outsz)
{
    return panel_resolve_output_name(p->output, out, outsz);
}

/* Public wrapper around resolve_output_geometry() for widgets that only
 * need an output's real pixel size by name (pager.c's proportional-square
 * mode) -- 0 if the output isn't currently connected. */
int panel_lookup_output_size(const char *name, int *out_w, int *out_h)
{
    int ox, oy, ow, oh;
    double hz;
    if (!resolve_output_geometry(name, &ox, &oy, &ow, &oh, &hz)) {
        return 0;
    }
    *out_w = ow;
    *out_h = oh;
    return 1;
}

/* Same, but also reporting the output's origin in root coordinates --
 * pager.c's window-outline mode needs it to map a window's root-relative
 * position into the miniature of the output it lives on. */
int panel_lookup_output_rect(const char *name, int *out_x, int *out_y, int *out_w, int *out_h)
{
    double hz;
    return resolve_output_geometry(name, out_x, out_y, out_w, out_h, &hz);
}

/* Name of the RandR-designated primary output (xrandr --output X --primary),
 * or 0 if none is set/RandR is unavailable -- used only when writing a
 * first-run default config (see write_default_config_if_missing()), so a
 * multi-monitor session's default panel lands on the right screen instead
 * of spanning every monitor combined ("*"). */
static int get_primary_output_name(char *out, size_t outsz)
{
    RROutput primary = XRRGetOutputPrimary(g_dpy, g_root);
    if (!primary) {
        return 0;
    }
    XRRScreenResources *res = XRRGetScreenResourcesCurrent(g_dpy, g_root);
    if (!res) {
        return 0;
    }
    int found = 0;
    XRROutputInfo *oi = XRRGetOutputInfo(g_dpy, res, primary);
    if (oi && oi->connection == RR_Connected) {
        snprintf(out, outsz, "%s", oi->name);
        found = 1;
    }
    if (oi) {
        XRRFreeOutputInfo(oi);
    }
    XRRFreeScreenResources(res);
    return found;
}

/* First run (no $XDG_CONFIG_HOME/xispanel.conf yet): rather than the
 * daemon silently running with zero panels -- which looks exactly like
 * xispanel isn't working at all -- write a minimal but usable default:
 * one panel at the bottom edge of the primary output (or "*", the whole
 * virtual screen, if RandR has no primary set -- equivalent on a
 * single-monitor session anyway), with launcher+tasklist on the left and
 * tray+clock pushed to the right edge by a spacer in between.
 * Deliberately no THEME line here: without one, alloc_panel()'s built-in
 * dark colors and fontconfig's default font are used. xispanel reads no
 * other desktop's configuration to fill those in -- xispanel.conf is the
 * only source of truth (a first-run session-import pass, seeding this
 * file from whatever KDE/GTK config the user already has, belongs to
 * kiconfd, not here). */
static void write_default_config_if_missing(void)
{
    if (access(g_configpath, F_OK) == 0) {
        return;
    }
    char output[64];
    if (!get_primary_output_name(output, sizeof(output))) {
        snprintf(output, sizeof(output), "*");
    }
    FILE *f = fopen(g_configpath, "w");
    if (!f) {
        fprintf(stderr, "xispanel: could not write default config to %s: %s\n", g_configpath, strerror(errno));
        return;
    }
    fprintf(f,
            "PANEL\tmain\t%s\tedge=bottom\tpct=100\tthickness=32\tmode=dock\n"
            "WIDGET\tmain\t0\txisserve\n"
            "WIDGET\tmain\t1\tpager	show_windows=yes\n"
            "WIDGET\tmain\t2\ttasklist\tmode=wide group=yes same_output=yes icon_padding=6 show_thumbs=yes\n"
            "WIDGET\tmain\t3\tspacer\n"
            "WIDGET\tmain\t4\tvolume\n"
            "WIDGET\tmain\t5\tenergy\n"
            "WIDGET\tmain\t6\ttray icon_padding=5\n"
            "WIDGET\tmain\t7\tclock\n",
            output);
    fclose(f);
    fprintf(stderr, "xispanel: no config found, wrote a default panel to %s\n", g_configpath);
}

static void panel_resolve_geometry(Panel *p)
{
    /* A container popup lives on whatever output its owner widget's
     * panel is on -- its own PANEL line's output field only matters for
     * the unlinked fallback (see link_containers()). */
    const char *output = (p->mode == MODE_CONTAINER && p->owner) ? p->owner->panel->output : p->output;
    if (strcmp(output, "*") != 0 &&
        resolve_output_geometry(output, &p->out_x, &p->out_y, &p->out_w, &p->out_h, &p->out_refresh_hz)) {
        /* matched */
    } else {
        if (strcmp(output, "*") != 0) {
            fprintf(stderr, "xispanel: panel '%s': output '%s' not found, falling back to full screen\n", p->name, output);
        }
        p->out_x = 0;
        p->out_y = 0;
        p->out_w = DisplayWidth(g_dpy, g_screen);
        p->out_h = DisplayHeight(g_dpy, g_screen);
        p->out_refresh_hz = 0; /* spans every output ("*") or none matched -- no single rate applies */
    }

    p->thickness = p->thickness_cfg;

    if (p->mode == MODE_CONTAINER) {
        /* Widgets run along the owner panel's own axis (a horizontal
         * panel opens a horizontal popup, rows stacking away from the
         * panel; a vertical one opens columns) -- so the popup's layout
         * and its widgets' orientation match the bar it hangs off of.
         * Size comes from the content (panel_layout()) and the position
         * from the owner widget at open time (container_place()); 1x1 is
         * just so the window can be created now. */
        if (p->owner) {
            p->edge = p->owner->panel->edge;
        }
        p->w = 1;
        p->h = 1;
        p->x = p->out_x;
        p->y = p->out_y;
        p->hidden_x = p->x;
        p->hidden_y = p->y;
        return;
    }

    if (p->edge == EDGE_TOP || p->edge == EDGE_BOTTOM) {
        p->w = p->out_w * p->pct / 100;
        p->h = p->thickness;
        p->x = p->out_x + (p->out_w - p->w) / 2;
        p->y = (p->edge == EDGE_TOP) ? p->out_y : (p->out_y + p->out_h - p->thickness);
        p->hidden_x = p->x;
        p->hidden_y = (p->edge == EDGE_TOP) ? (p->out_y - p->thickness) : (p->out_y + p->out_h);
    } else {
        p->h = p->out_h * p->pct / 100;
        p->w = p->thickness;
        p->y = p->out_y + (p->out_h - p->h) / 2;
        p->x = (p->edge == EDGE_LEFT) ? p->out_x : (p->out_x + p->out_w - p->thickness);
        p->hidden_y = p->y;
        p->hidden_x = (p->edge == EDGE_LEFT) ? (p->out_x - p->thickness) : (p->out_x + p->out_w);
    }
}

/* ------------------------------------------------------------------ */
/* strut (dock mode)                                                    */
/* ------------------------------------------------------------------ */

static void panel_apply_strut(Panel *p)
{
    if (p->mode != MODE_DOCK) {
        return;
    }
    /* _NET_WM_STRUT_PARTIAL: left, right, top, bottom, left_start_y,
     * left_end_y, right_start_y, right_end_y, top_start_x, top_end_x,
     * bottom_start_x, bottom_end_x */
    long strut[12] = {0};
    long strut4[4] = {0};
    switch (p->edge) {
    case EDGE_TOP:
        strut[2] = p->y + p->h; /* distance from top of screen */
        strut[8] = p->x;
        strut[9] = p->x + p->w - 1;
        strut4[2] = strut[2];
        break;
    case EDGE_BOTTOM:
        strut[3] = DisplayHeight(g_dpy, g_screen) - p->y;
        strut[10] = p->x;
        strut[11] = p->x + p->w - 1;
        strut4[3] = strut[3];
        break;
    case EDGE_LEFT:
        strut[0] = p->x + p->w;
        strut[4] = p->y;
        strut[5] = p->y + p->h - 1;
        strut4[0] = strut[0];
        break;
    case EDGE_RIGHT:
        strut[1] = DisplayWidth(g_dpy, g_screen) - p->x;
        strut[6] = p->y;
        strut[7] = p->y + p->h - 1;
        strut4[1] = strut[1];
        break;
    }
    XChangeProperty(g_dpy, p->win, g_atom_wm_strut_partial, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)strut, 12);
    XChangeProperty(g_dpy, p->win, g_atom_wm_strut, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)strut4, 4);
}

/* ------------------------------------------------------------------ */
/* window / cairo surface creation                                      */
/* ------------------------------------------------------------------ */

static void panel_pick_visual(Panel *p)
{
    XVisualInfo vinfo;
    if (XMatchVisualInfo(g_dpy, g_screen, 32, TrueColor, &vinfo)) {
        p->visual = vinfo.visual;
        p->depth = vinfo.depth;
        p->cmap = XCreateColormap(g_dpy, g_root, p->visual, AllocNone);
    } else {
        /* No ARGB visual available (no compositor advertising one): fall
         * back to the default visual. Backgrounds configured with alpha
         * < 1 will just render fully opaque. */
        p->visual = DefaultVisual(g_dpy, g_screen);
        p->depth = DefaultDepth(g_dpy, g_screen);
        p->cmap = DefaultColormap(g_dpy, g_screen);
    }
}

static Window panel_create_window(Panel *p, int x, int y, int w, int h)
{
    /* dock mode needs to be a WM-managed window: this KWin fork's strut
     * handling (workspace.cpp's updateClientArea()) only walks its list of
     * managed clients, never the separate unmanaged/override-redirect list
     * -- so an override-redirect panel can set _NET_WM_STRUT_PARTIAL all it
     * wants and no screen space will ever actually be reserved. overlay/
     * autohide don't need struts at all, so they stay override-redirect to
     * avoid the WM ever touching their position (matters for the autohide
     * slide animation). */
    int managed = (p->mode == MODE_DOCK);

    XSetWindowAttributes attrs;
    memset(&attrs, 0, sizeof(attrs));
    attrs.override_redirect = managed ? False : True;
    attrs.colormap = p->cmap;
    attrs.border_pixel = 0;
    attrs.background_pixel = 0;
    /* PropertyChangeMask: needed for _X_DENSITY_REQUESTED (see density.c) --
     * a compositor writes that directly onto this window. */
    attrs.event_mask =
        ButtonPressMask | EnterWindowMask | LeaveWindowMask | PointerMotionMask | ExposureMask | PropertyChangeMask;

    Window win = XCreateWindow(g_dpy, g_root, x, y, (unsigned)w, (unsigned)h, 0, p->depth, InputOutput, p->visual,
                                CWOverrideRedirect | CWColormap | CWBorderPixel | CWBackPixel | CWEventMask, &attrs);

    char title[64];
    snprintf(title, sizeof(title), "xispanel:%s", p->name);
    XStoreName(g_dpy, win, title);
    XClassHint ch = {(char *)"xispanel", (char *)"xispanel"};
    XSetClassHint(g_dpy, win, &ch);

    /* A container popup is announced as a popup menu, not a dock: it's
     * transient and sits beside the bar, and a compositor's per-type
     * effects (menu fade/slide vs. dock treatment) should see it that way. */
    Atom type = (p->mode == MODE_CONTAINER) ? g_atom_wm_window_type_popup_menu : g_atom_wm_window_type_dock;
    XChangeProperty(g_dpy, win, g_atom_wm_window_type, XA_ATOM, 32, PropModeReplace, (unsigned char *)&type, 1);

    if (managed) {
        /* ICCCM input=False: never wants keyboard focus, so click-to-focus
         * policies must never give it focus (mouse clicks still work --
         * ButtonPress delivery doesn't require focus). */
        XWMHints hints;
        memset(&hints, 0, sizeof(hints));
        hints.flags = InputHint;
        hints.input = False;
        XSetWMHints(g_dpy, win, &hints);

        /* ICCCM PPosition: tells the WM the position was chosen by the
         * application, not left to the placement policy -- otherwise a
         * managed window's initial x/y is only a hint some WMs ignore. */
        XSizeHints sh;
        memset(&sh, 0, sizeof(sh));
        sh.flags = PPosition | PSize;
        sh.x = x;
        sh.y = y;
        sh.width = w;
        sh.height = h;
        XSetWMNormalHints(g_dpy, win, &sh);

        /* Initial _NET_WM_STATE: read by the WM at manage() time, same as
         * if these had been requested via a _NET_WM_STATE client message
         * after mapping. Keeps the panel out of the taskbar/pager despite
         * now being a real managed window. */
        Atom states[2] = {g_atom_net_wm_state_skip_taskbar, g_atom_net_wm_state_skip_pager};
        XChangeProperty(g_dpy, win, g_atom_net_wm_state, XA_ATOM, 32, PropModeReplace, (unsigned char *)states, 2);

        /* All desktops. */
        long all_desktops = -1;
        XChangeProperty(g_dpy, win, g_atom_net_wm_desktop, XA_CARDINAL, 32, PropModeReplace,
                         (unsigned char *)&all_desktops, 1);
    }

    return win;
}

static Window panel_create_sensor(Panel *p)
{
    XSetWindowAttributes attrs;
    memset(&attrs, 0, sizeof(attrs));
    attrs.override_redirect = True;
    attrs.background_pixel = BlackPixel(g_dpy, g_screen);
    attrs.event_mask = EnterWindowMask;

    int sx, sy, sw, sh;
    if (p->edge == EDGE_TOP || p->edge == EDGE_BOTTOM) {
        sx = p->x;
        sy = (p->edge == EDGE_TOP) ? p->out_y : (p->out_y + p->out_h - 1);
        sw = p->w;
        sh = 1;
    } else {
        sx = (p->edge == EDGE_LEFT) ? p->out_x : (p->out_x + p->out_w - 1);
        sy = p->y;
        sw = 1;
        sh = p->h;
    }

    Window win = XCreateWindow(g_dpy, g_root, sx, sy, (unsigned)sw, (unsigned)sh, 0, CopyFromParent, InputOutput,
                                DefaultVisual(g_dpy, g_screen), CWOverrideRedirect | CWBackPixel | CWEventMask, &attrs);
    XMapWindow(g_dpy, win);
    /* Must stay on top (not lowered) or it would sit behind whatever else
     * occupies that screen edge and never receive the EnterNotify that
     * triggers the show animation -- X only delivers pointer-crossing
     * events to the topmost window under the pointer. */
    XRaiseWindow(g_dpy, win);
    return win;
}

/* ------------------------------------------------------------------ */
/* 9-slice PNG background theme                                        */
/* ------------------------------------------------------------------ */

/* Runs `cmd` via `sh -c`, detached (see xispanel.h's doc comment). */
void run_detached(const char *cmd)
{
    if (!cmd || !cmd[0]) {
        return;
    }
    pid_t pid = fork();
    if (pid < 0) {
        perror("xispanel: fork");
        return;
    }
    if (pid == 0) {
        setsid();
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
}

/* Decodes `path` via Imlib2 into a premultiplied-alpha cairo ARGB32
 * surface (Imlib2's DATA32 pixels are straight, not premultiplied --same
 * conversion ewmh_get_icon_surface() does for _NET_WM_ICON). NULL on any
 * failure (missing file, unreadable, decode error) -- callers treat that
 * as "no image", not a fatal error: a THEME's path=<folder>/bg.png is
 * meant to gracefully fall back to the plain bg_r/g/b/a color whenever it
 * can't be loaded. */
cairo_surface_t *load_png_argb(const char *path)
{
    Imlib_Image img = imlib_load_image(path);
    if (!img) {
        return NULL;
    }
    imlib_context_set_image(img);
    int iw = imlib_image_get_width();
    int ih = imlib_image_get_height();
    if (iw <= 0 || ih <= 0 || iw > 4096 || ih > 4096) {
        imlib_free_image();
        return NULL;
    }
    DATA32 *src = imlib_image_get_data_for_reading_only();
    if (!src) {
        imlib_free_image();
        return NULL;
    }

    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, iw, ih);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        imlib_free_image();
        return NULL;
    }
    unsigned char *dst = cairo_image_surface_get_data(surf);
    int stride = cairo_image_surface_get_stride(surf);
    for (int y = 0; y < ih; y++) {
        uint32_t *row = (uint32_t *)(void *)(dst + y * stride);
        for (int x = 0; x < iw; x++) {
            uint32_t argb = src[y * iw + x];
            uint8_t a = (uint8_t)((argb >> 24) & 0xff);
            uint8_t r = (uint8_t)((argb >> 16) & 0xff);
            uint8_t g = (uint8_t)((argb >> 8) & 0xff);
            uint8_t b = (uint8_t)(argb & 0xff);
            r = (uint8_t)((r * a) / 255);
            g = (uint8_t)((g * a) / 255);
            b = (uint8_t)((b * a) / 255);
            row[x] = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        }
    }
    cairo_surface_mark_dirty(surf);
    imlib_free_image();
    return surf;
}

/* ---- librsvg, dlopen'd on first use --------------------------------
 * Only 4 symbols needed, own struct/typedefs below instead of
 * <librsvg/rsvg.h> so this builds with no new -dev package and no new
 * Makefile PKGS entry -- same "dlopen the .so, never link it" approach
 * as libdbus-1 in sni.c/mpris.c/dbusmenu.c, just with hand-written
 * declarations instead of a system header because unlike dbus-1 this one
 * isn't otherwise a build-time dependency of this project at all. The
 * struct layout below is librsvg's long-stable public ABI (RsvgHandle is
 * opaque, only ever passed back to librsvg itself; RsvgDimensionData's
 * four fields haven't changed since librsvg 2.x's first release). */
typedef void RsvgHandle;
typedef struct {
    int width;
    int height;
    double em;
    double ex;
} RsvgDimensionData;

static void *g_librsvg = NULL;
static int g_librsvg_attempted = 0;
static RsvgHandle *(*p_rsvg_handle_new_from_file)(const char *, void *);
static void (*p_rsvg_handle_get_dimensions)(RsvgHandle *, RsvgDimensionData *);
static int (*p_rsvg_handle_render_cairo)(RsvgHandle *, cairo_t *);
static void (*p_g_object_unref)(void *);

static int svg_ensure_loaded(void)
{
    if (g_librsvg) {
        return 1;
    }
    if (g_librsvg_attempted) {
        return 0;
    }
    g_librsvg_attempted = 1;
    g_librsvg = dlopen("librsvg-2.so.2", RTLD_NOW | RTLD_GLOBAL);
    if (!g_librsvg) {
        g_librsvg = dlopen("librsvg-2.so", RTLD_NOW | RTLD_GLOBAL);
    }
    if (!g_librsvg) {
        return 0;
    }
    *(void **)(&p_rsvg_handle_new_from_file) = dlsym(g_librsvg, "rsvg_handle_new_from_file");
    *(void **)(&p_rsvg_handle_get_dimensions) = dlsym(g_librsvg, "rsvg_handle_get_dimensions");
    *(void **)(&p_rsvg_handle_render_cairo) = dlsym(g_librsvg, "rsvg_handle_render_cairo");
    *(void **)(&p_g_object_unref) = dlsym(g_librsvg, "g_object_unref");
    if (!p_rsvg_handle_new_from_file || !p_rsvg_handle_get_dimensions || !p_rsvg_handle_render_cairo ||
        !p_g_object_unref) {
        dlclose(g_librsvg);
        g_librsvg = NULL;
        return 0;
    }
    return 1;
}

/* See xispanel.h's doc comment. */
cairo_surface_t *load_svg_argb(const char *path, int target_size)
{
    if (!path || !path[0] || target_size <= 0 || !svg_ensure_loaded()) {
        return NULL;
    }
    RsvgHandle *h = p_rsvg_handle_new_from_file(path, NULL);
    if (!h) {
        return NULL;
    }
    RsvgDimensionData dim = {0};
    p_rsvg_handle_get_dimensions(h, &dim);
    if (dim.width <= 0 || dim.height <= 0) {
        p_g_object_unref(h);
        return NULL;
    }
    double scale = (double)target_size / (double)(dim.width > dim.height ? dim.width : dim.height);
    int nw = (int)(dim.width * scale + 0.5);
    int nh = (int)(dim.height * scale + 0.5);
    if (nw < 1) {
        nw = 1;
    }
    if (nh < 1) {
        nh = 1;
    }
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, nw, nh);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        p_g_object_unref(h);
        return NULL;
    }
    cairo_t *cr = cairo_create(surf);
    cairo_scale(cr, scale, scale);
    /* Cairo composites RSVG's drawing straight into an ARGB32 surface
     * already premultiplied -- unlike load_png_argb()'s manual Imlib2
     * premultiply above, there's nothing to convert here. */
    int ok = p_rsvg_handle_render_cairo(h, cr);
    cairo_destroy(cr);
    p_g_object_unref(h);
    if (!ok) {
        cairo_surface_destroy(surf);
        return NULL;
    }
    cairo_surface_mark_dirty(surf);
    return surf;
}

/* See xispanel.h's doc comment. */
cairo_surface_t *shrink_icon_surface(cairo_surface_t *src, int target_size)
{
    if (!src || cairo_surface_status(src) != CAIRO_STATUS_SUCCESS || target_size <= 0) {
        return src;
    }
    int iw = cairo_image_surface_get_width(src);
    int ih = cairo_image_surface_get_height(src);
    if (iw <= 0 || ih <= 0 || (iw <= target_size && ih <= target_size)) {
        return src; /* already small enough -- never upscale */
    }
    double scale = (double)target_size / (double)(iw > ih ? iw : ih);
    int nw = (int)(iw * scale + 0.5);
    int nh = (int)(ih * scale + 0.5);
    if (nw < 1) {
        nw = 1;
    }
    if (nh < 1) {
        nh = 1;
    }
    cairo_surface_t *dst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, nw, nh);
    if (cairo_surface_status(dst) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(dst);
        return src;
    }
    cairo_t *cr = cairo_create(dst);
    cairo_scale(cr, scale, scale);
    cairo_set_source_surface(cr, src, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
    cairo_paint(cr);
    cairo_destroy(cr);
    cairo_surface_destroy(src);
    return dst;
}

/* See xispanel.h's doc comment. */
cairo_surface_t *load_icon_argb(const char *path, int target_size)
{
    return shrink_icon_surface(load_png_argb(path), target_size);
}

/* Sidecar "measurements" file for a 9-slice bg_image: plain key=value
 * lines (left/top/right/bottom, pixels in the *source image*), same
 * spirit as the rest of this program's config format. Missing file or
 * missing keys just default that inset to 0 -- a 0-everywhere slice
 * degrades to a plain full-image stretch, not an error, so a theme author
 * can start simple. */
static void load_slice_file_quiet(const char *path, int *l, int *t, int *r, int *b)
{
    *l = *t = *r = *b = 0;
    if (!path[0]) {
        return;
    }
    FILE *f = fopen(path, "r");
    if (!f) {
        return;
    }
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        int v;
        if (sscanf(line, "left=%d", &v) == 1) {
            *l = v;
        } else if (sscanf(line, "top=%d", &v) == 1) {
            *t = v;
        } else if (sscanf(line, "right=%d", &v) == 1) {
            *r = v;
        } else if (sscanf(line, "bottom=%d", &v) == 1) {
            *b = v;
        }
    }
    fclose(f);
}

/* Same, but warning when the file is missing -- bg.png's own sidecar is
 * the one case where its absence is more likely a mistake than a choice
 * (a stretched panel background rarely looks intentional). */
static void load_slice_file(const char *path, int *l, int *t, int *r, int *b)
{
    if (path[0] && access(path, R_OK) != 0) {
        fprintf(stderr, "xispanel: could not open theme slice file '%s', using 0-inset (full stretch)\n", path);
    }
    load_slice_file_quiet(path, l, t, r, b);
}

/* KiDesktop's central theme (kiconfd.conf's theme=, saved by kiconf's
 * Aparencia tab) -- resolved once, lazily, the first time a panel's own
 * `theme=` doesn't have a file this program is looking for, and cached
 * here for the rest of the run (xispanel never watches kiconfd.conf, same
 * as it never watches its own xispanel.conf outside of RELOAD). Empty
 * after resolution means there isn't one, so every lookup falls through
 * to this program's plain bg=/fg= colors, unchanged from before this
 * existed. */
static char g_central_theme_dir[PATH_MAX];
static int g_central_theme_resolved = 0;

static void read_central_theme_name(char *out, size_t outsz)
{
    out[0] = '\0';
    char path[PATH_MAX];
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) {
        snprintf(path, sizeof(path), "%s/kiconfd.conf", xdg);
    } else {
        snprintf(path, sizeof(path), "%s/.config/kiconfd.conf", getenv("HOME") ? getenv("HOME") : "/tmp");
    }
    FILE *f = fopen(path, "r");
    if (!f) {
        return;
    }
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        char *key = line;
        while (*key == ' ' || *key == '\t') {
            key++;
        }
        char *kend = key + strlen(key);
        while (kend > key && (kend[-1] == ' ' || kend[-1] == '\t')) {
            *--kend = '\0';
        }
        if (strcmp(key, "theme") != 0) {
            continue;
        }
        char *val = eq + 1;
        while (*val == ' ' || *val == '\t') {
            val++;
        }
        val[strcspn(val, "\r\n")] = '\0';
        snprintf(out, outsz, "%s", val);
        break;
    }
    fclose(f);
}

/* Resolves a bare theme name to an actual folder, trying the two real
 * install locations and the source-tree `themes/` folder relative to the
 * current directory (same spots kiwm's own find_theme_file() tries, see
 * kiwm/decoration.c) -- first existing directory wins. */
static int resolve_theme_dir(const char *name, char *out, size_t outsz)
{
    char home_base[PATH_MAX];
    const char *home = getenv("HOME");
    snprintf(home_base, sizeof(home_base), "%s/.local/share/kidesktop/themes", home ? home : "");
    const char *bases[] = {
        "/usr/share/kidesktop/themes", home_base, "../themes", "./themes", "themes",
    };
    for (size_t i = 0; i < sizeof(bases) / sizeof(bases[0]); i++) {
        snprintf(out, outsz, "%s/%s", bases[i], name);
        struct stat st;
        if (stat(out, &st) == 0 && S_ISDIR(st.st_mode)) {
            return 1;
        }
    }
    return 0;
}

static const char *get_central_theme_dir(void)
{
    if (!g_central_theme_resolved) {
        g_central_theme_resolved = 1;
        char name[NAME_MAX];
        read_central_theme_name(name, sizeof(name));
        if (!name[0] || !resolve_theme_dir(name, g_central_theme_dir, sizeof(g_central_theme_dir))) {
            g_central_theme_dir[0] = '\0';
        }
    }
    return g_central_theme_dir[0] ? g_central_theme_dir : NULL;
}

/* Resolves "<theme>/<relname>" against p's own theme_path first, falling
 * back to KiDesktop's central theme (see get_central_theme_dir() above)
 * only when p's own theme doesn't have this particular file -- a panel
 * theme missing a file (or with none configured at all) still gets it
 * from the central theme if there is one, per file, exactly like kiwm's
 * own find_theme_file(). Returns true and fills `out` with the winning
 * path, or false if neither has it. */
static int panel_find_theme_file(Panel *p, const char *relname, char *out, size_t outsz)
{
    if (p->theme_path[0]) {
        snprintf(out, outsz, "%s/%s", p->theme_path, relname);
        if (access(out, R_OK) == 0) {
            return 1;
        }
    }
    const char *central = get_central_theme_dir();
    if (central) {
        snprintf(out, outsz, "%s/%s", central, relname);
        if (access(out, R_OK) == 0) {
            return 1;
        }
    }
    return 0;
}

/* Loads (or reloads) p's bg_image_surface + slice insets from its
 * currently configured theme_path, falling back to KiDesktop's central
 * theme -- a folder containing fixed-named files (bg.png, bg.slice)
 * rather than separately-pointed-to files, so a theme can grow more files
 * later without new config keys. Called once from panel_activate() --
 * config paths don't change without a full RELOAD, which tears down and
 * re-activates every panel anyway. */
static void panel_load_bg_image(Panel *p)
{
    if (p->bg_image_surface) {
        cairo_surface_destroy(p->bg_image_surface);
        p->bg_image_surface = NULL;
    }
    char path[PATH_MAX];
    if (!panel_find_theme_file(p, "bg.png", path, sizeof(path))) {
        return;
    }
    p->bg_image_surface = load_png_argb(path);
    if (!p->bg_image_surface) {
        fprintf(stderr, "xispanel: panel '%s': could not load theme background '%s', falling back to bg color\n",
                p->name, path);
        return;
    }
    if (panel_find_theme_file(p, "bg.slice", path, sizeof(path))) {
        load_slice_file(path, &p->bg_slice_l, &p->bg_slice_t, &p->bg_slice_r, &p->bg_slice_b);
    }
}

/* Sidecar grid measurements for btns.png -- unlike bg.png/bg.slice's 9-slice
 * insets, this is a plain fixed cell size (no stretching), so it only has
 * two keys. Defaults match kiwm's fallback (its own titlebar button size)
 * closely enough to look reasonable before any real theme is applied;
 * winctl.c scales whatever cell size is loaded to fit its own button slot
 * anyway (see winctl_paint()), so an exact match isn't required. */
static void load_btns_slice_file(const char *path, int *cell_w, int *cell_h)
{
    *cell_w = 24;
    *cell_h = 24;
    FILE *f = fopen(path, "r");
    if (!f) {
        return;
    }
    char line[128];
    int v;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "cell_width=%d", &v) == 1) {
            *cell_w = v;
        } else if (sscanf(line, "cell_height=%d", &v) == 1) {
            *cell_h = v;
        }
    }
    fclose(f);
}

/* Loads (or reloads) p's btns_image_surface + cell size from the same
 * theme_path bg.png/bg.slice already come from -- btns.png/btns.slice, the
 * window-control button sprite sheet (see the Panel struct's doc comment
 * for the fixed column/row grid it follows). Independent of whether
 * bg.png loaded: a theme missing one file doesn't take the other down.
 * Called from panel_activate() right after panel_load_bg_image(). */
static void panel_load_btns_image(Panel *p)
{
    if (p->btns_image_surface) {
        cairo_surface_destroy(p->btns_image_surface);
        p->btns_image_surface = NULL;
    }
    char path[PATH_MAX];
    if (!panel_find_theme_file(p, "btns.png", path, sizeof(path))) {
        /* Not a warning like bg.png's: btns.png is the newer, optional
         * half of a theme -- plenty of valid themes (e.g. one authored
         * before winctl.c consumed this) only ship bg.png/bg.slice. */
        return;
    }
    p->btns_image_surface = load_png_argb(path);
    if (!p->btns_image_surface) {
        return;
    }
    if (!panel_find_theme_file(p, "btns.slice", path, sizeof(path))) {
        path[0] = '\0';
    }
    load_btns_slice_file(path, &p->btns_cell_w, &p->btns_cell_h);
}

/* Loads one bitmap skin (<theme>/<name>.png + <name>.slice) into `skin`.
 * The sidecar carries the 9-slice insets (the same left/top/right/bottom
 * keys bg.png's does -- they apply to every row alike) plus
 * cell_width=/cell_height=, whose defaults are the image's own full width
 * and height, so a single-row skin needs only the insets. The row count
 * is the image height divided by cell_height, capped at the four states
 * the SKIN_* enum defines. A missing file is silent and leaves the skin
 * unloaded: every skin is optional, and its absence just means the
 * caller's own Cairo drawing stays in charge. */
static void panel_load_skin(Panel *p, const char *name, PanelSkin *skin)
{
    if (skin->surface) {
        cairo_surface_destroy(skin->surface);
    }
    memset(skin, 0, sizeof(*skin));
    char relname[NAME_MAX], path[PATH_MAX];
    snprintf(relname, sizeof(relname), "%s.png", name);
    if (!panel_find_theme_file(p, relname, path, sizeof(path))) {
        return;
    }
    skin->surface = load_png_argb(path);
    if (!skin->surface) {
        return;
    }
    int img_w = cairo_image_surface_get_width(skin->surface);
    int img_h = cairo_image_surface_get_height(skin->surface);

    snprintf(relname, sizeof(relname), "%s.slice", name);
    if (!panel_find_theme_file(p, relname, path, sizeof(path))) {
        path[0] = '\0';
    }
    load_slice_file_quiet(path, &skin->l, &skin->t, &skin->r, &skin->b);
    skin->cell_w = img_w;
    skin->cell_h = img_h;
    FILE *f = fopen(path, "r");
    if (f) {
        char line[128];
        int v;
        while (fgets(line, sizeof(line), f)) {
            if (sscanf(line, "cell_width=%d", &v) == 1 && v > 0) {
                skin->cell_w = v;
            } else if (sscanf(line, "cell_height=%d", &v) == 1 && v > 0) {
                skin->cell_h = v;
            }
        }
        fclose(f);
    }
    if (skin->cell_w > img_w) {
        skin->cell_w = img_w;
    }
    if (skin->cell_h > img_h) {
        skin->cell_h = img_h;
    }
    skin->rows = skin->cell_h > 0 ? img_h / skin->cell_h : 0;
    if (skin->rows > SKIN_ATTENTION + 1) {
        skin->rows = SKIN_ATTENTION + 1;
    }
    if (skin->rows < 1) {
        cairo_surface_destroy(skin->surface);
        memset(skin, 0, sizeof(*skin));
        fprintf(stderr, "xispanel: panel '%s': theme's %s.png has no usable rows, ignoring it\n", p->name, name);
    }
}

static void panel_free_skin(PanelSkin *skin)
{
    if (skin->surface) {
        cairo_surface_destroy(skin->surface);
    }
    memset(skin, 0, sizeof(*skin));
}

/* Loads every skin file a theme may ship. Each is independent: one
 * missing (or broken) file never affects the others. */
static void panel_load_skins(Panel *p)
{
    panel_load_skin(p, "tasks", &p->tasks_skin);
    panel_load_skin(p, "button", &p->button_skin);
    panel_load_skin(p, "menu", &p->menu_skin);
    panel_load_skin(p, "menuitem", &p->menuitem_skin);
    panel_load_skin(p, "pager", &p->pager_skin);
    panel_load_skin(p, "bar", &p->bar_skin);
}

/* Reads the theme folder's `colors` file -- the same one kiwm reads for
 * its titlebar (see kiwm/README.md's "Theming"). xispanel takes only the
 * handful of keys that mean something for a panel:
 *
 *   bg_active / fg_active  -> this panel's bg/fg, but ONLY when the THEME
 *                             line didn't set bg=/fg= itself (an explicit
 *                             config value always wins over the theme
 *                             folder, same layering bg.png already has
 *                             against bg=).
 *   font_size              -> same rule, against THEME's font_size=.
 *   border_radius          -> rounded panel corners, via the SHAPE
 *                             extension (no compositor needed), the same
 *                             1/2/4-number form kiwm accepts.
 *
 * `font=` is deliberately not read here: the font face is process-global
 * and resolved before any panel exists, so config_scan_globals() reads it
 * from the theme instead (see there). Every other key in the file is
 * kiwm's business and ignored, not an error -- one folder, two programs,
 * each taking what applies to it. */
static void panel_load_theme_colors(Panel *p)
{
    p->border_radius = 0;
    char path[PATH_MAX];
    if (!panel_find_theme_file(p, "colors", path, sizeof(path))) {
        return;
    }
    FILE *f = fopen(path, "r");
    if (!f) {
        return;
    }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len && (line[len - 1] == '\n' || line[len - 1] == '\r' || line[len - 1] == ' ')) {
            line[--len] = 0;
        }
        if (line[0] == '#' || !line[0]) {
            continue;
        }
        char value[64];
        if (!p->cfg_has_bg && sscanf(line, "bg_active=%63s", value) == 1) {
            parse_hex_color(value, &p->bg_r, &p->bg_g, &p->bg_b, &p->bg_a);
        } else if (!p->cfg_has_fg && sscanf(line, "fg_active=%63s", value) == 1) {
            parse_hex_color(value, &p->fg_r, &p->fg_g, &p->fg_b, &p->fg_a);
        } else if (!p->cfg_has_font_size && sscanf(line, "font_size=%63s", value) == 1) {
            double v = atof(value);
            if (v > 0) {
                p->font_size_px = v;
            }
        } else if (!strncmp(line, "border_radius=", 14)) {
            /* kiwm takes 1, 2 or 4 numbers (CSS corner order); a panel is
             * one flat rectangle against a screen edge, so only the first
             * is used -- rounding its four corners differently would need
             * a per-corner shape mask for no visible gain on a bar. */
            int v = atoi(line + 14);
            p->border_radius = v > 0 ? v : 0;
        }
    }
    fclose(f);
}

/* Traces the same rounded-rectangle path panel_apply_shape() masks the
 * window to (see there for why the corners can't just be a SHAPE mask
 * clipping p->win itself). Shared so painting and hit-testing never
 * disagree about where the curve actually falls. */
static void panel_trace_rounded_rect(cairo_t *cr, int w, int h, int r)
{
    double rr = r;
    cairo_new_path(cr);
    cairo_arc(cr, rr, rr, rr, M_PI, 1.5 * M_PI);
    cairo_arc(cr, w - rr, rr, rr, 1.5 * M_PI, 2 * M_PI);
    cairo_arc(cr, w - rr, h - rr, rr, 0, 0.5 * M_PI);
    cairo_arc(cr, rr, h - rr, rr, 0.5 * M_PI, M_PI);
    cairo_close_path(cr);
}

/* Applies (or clears) the panel window's rounded-corner shape mask, from
 * the theme's border_radius=. Uses the SHAPE extension directly on a
 * 1-bit pixmap -- no compositor involved, so this works on a bare X
 * server exactly like kiwm's own rounded frames used to. A radius of 0
 * (the default, and any theme without the key) resets the window to its
 * plain rectangle, so nothing changes for an unthemed panel.
 *
 * On an ARGB visual this is a no-op on purpose: panel_paint_content()
 * already clips the *content* to the same rounded rect and leaves those
 * pixels transparent, which looks identical once a compositor is
 * painting p->win's alpha -- and unlike SHAPE, it never touches the
 * window's hit region. SHAPE-masking ShapeBounding *always* narrows what
 * XYToWindow() will hit-test into the window, and this server's
 * miSpriteTrace() ANDs that check with ShapeInput rather than letting
 * ShapeInput override it (confirmed empirically: setting ShapeInput back
 * to the full rectangle, as a previous version of this function did,
 * does not stop clicks in the rounded-off corners from falling through
 * to whatever is behind the panel) -- so once ShapeBounding excludes the
 * corners there is no way, from this side of the protocol, to make them
 * clickable again. Only the uncomposited fallback below still needs (and
 * still has) that limitation. */
static void panel_apply_shape(Panel *p)
{
    if (!p->win) {
        return;
    }
    if (p->depth == 32) {
        if (p->shaped) {
            XShapeCombineMask(g_dpy, p->win, ShapeBounding, 0, 0, None, ShapeSet);
            p->shaped = 0;
        }
        return;
    }
    int r = p->border_radius;
    if (r <= 0) {
        if (p->shaped) {
            XShapeCombineMask(g_dpy, p->win, ShapeBounding, 0, 0, None, ShapeSet);
            p->shaped = 0;
        }
        return;
    }
    int max_r = (p->w < p->h ? p->w : p->h) / 2;
    if (r > max_r) {
        r = max_r;
    }
    Pixmap mask = XCreatePixmap(g_dpy, p->win, p->w, p->h, 1);
    cairo_surface_t *ms = cairo_xlib_surface_create_for_bitmap(g_dpy, mask, DefaultScreenOfDisplay(g_dpy), p->w, p->h);
    cairo_t *mcr = cairo_create(ms);
    cairo_set_operator(mcr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(mcr, 0, 0, 0, 0); /* transparent = clipped away */
    cairo_paint(mcr);
    cairo_set_source_rgba(mcr, 1, 1, 1, 1);
    panel_trace_rounded_rect(mcr, p->w, p->h, r);
    cairo_fill(mcr);
    cairo_destroy(mcr);
    cairo_surface_destroy(ms);

    XShapeCombineMask(g_dpy, p->win, ShapeBounding, 0, 0, mask, ShapeSet);
    XFreePixmap(g_dpy, mask);
    p->shaped = 1;
}

/* Drops every decoded theme icon (see panel_theme_icon()) -- called when
 * a panel is torn down or its theme reloaded. */
static void panel_free_theme_icons(Panel *p)
{
    for (int i = 0; i < p->n_icon_cache; i++) {
        if (p->icon_cache[i].surf) {
            cairo_surface_destroy(p->icon_cache[i].surf);
        }
    }
    p->n_icon_cache = 0;
    memset(p->icon_cache, 0, sizeof(p->icon_cache));
}

cairo_surface_t *panel_theme_icon(Panel *p, const char *name, int size)
{
    if (!name || !name[0] || size <= 0) {
        return NULL;
    }
    for (int i = 0; i < p->n_icon_cache; i++) {
        if (p->icon_cache[i].size == size && strcmp(p->icon_cache[i].name, name) == 0) {
            return p->icon_cache[i].surf; /* NULL here means "already looked up, not there" */
        }
    }
    if (p->n_icon_cache >= PANEL_ICON_CACHE_MAX) {
        return NULL;
    }
    char relname[NAME_MAX], path[PATH_MAX];
    snprintf(relname, sizeof(relname), "icons/%s.png", name);
    cairo_surface_t *surf = panel_find_theme_file(p, relname, path, sizeof(path))
                                 ? load_icon_argb(path, icon_fetch_size_for(size))
                                 : NULL;
    /* Negative results are cached too -- a themeless icon name would
     * otherwise re-stat the same missing file on every single repaint. */
    snprintf(p->icon_cache[p->n_icon_cache].name, sizeof(p->icon_cache[0].name), "%s", name);
    p->icon_cache[p->n_icon_cache].size = size;
    p->icon_cache[p->n_icon_cache].surf = surf;
    p->n_icon_cache++;
    return surf;
}

cairo_surface_t *xispanel_first_panel_icon(const char *name, int size)
{
    for (int i = 0; i < MAX_PANELS; i++) {
        if (g_panels[i].in_use) {
            return panel_theme_icon(&g_panels[i], name, size);
        }
    }
    return NULL;
}

/* Paints one source sub-rectangle [sx,sy,sw,sh] of `src` into one
 * destination rectangle [dx,dy,dw,dh] of `cr`, scaling to fit -- the one
 * building block every corner/edge/center region of a 9-slice draw
 * reduces to (corners just happen to have dw==sw, dh==sh, i.e. no
 * scaling). */
void draw_slice_region(cairo_t *cr, cairo_surface_t *src, int sx, int sy, int sw, int sh, double dx, double dy,
                        double dw, double dh)
{
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) {
        return;
    }
    cairo_save(cr);
    cairo_translate(cr, dx, dy);
    cairo_scale(cr, dw / (double)sw, dh / (double)sh);
    cairo_set_source_surface(cr, src, -sx, -sy);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
    cairo_rectangle(cr, 0, 0, sw, sh);
    cairo_clip(cr);
    cairo_paint(cr);
    cairo_restore(cr);
}

/* Draws `src` (sw x sh) into `cr`'s current (0,0)-(dw,dh) rect as a
 * 9-slice: the l/t/r/b-pixel corners are copied unscaled, the four edge
 * strips stretch along one axis, and the center stretches on both --
 * standard border-image technique, letting one theme image cover any
 * panel thickness/length instead of looking stretched-blurry at the
 * corners. Insets are silently clamped if they don't fit inside the
 * source image (a theme author's slice file shouldn't be able to corrupt
 * rendering, just look wrong). */
void panel_draw_9slice(cairo_t *cr, cairo_surface_t *src, int sw, int sh, int l, int t, int r, int b, double dw,
                        double dh)
{
    panel_draw_9slice_at(cr, src, 0, 0, sw, sh, l, t, r, b, dw, dh);
}

void panel_draw_9slice_at(cairo_t *cr, cairo_surface_t *src, int sx, int sy, int sw, int sh, int l, int t, int r,
                           int b, double dw, double dh)
{
    if (l + r > sw) {
        l = r = 0;
    }
    if (t + b > sh) {
        t = b = 0;
    }
    int cw = sw - l - r; /* source center width/height */
    int ch = sh - t - b;
    double dcw = dw - l - r; /* dest center width/height (may go negative on a tiny panel) */
    double dch = dh - t - b;
    if (dcw < 0) {
        dcw = 0;
    }
    if (dch < 0) {
        dch = 0;
    }

    /* corners: unscaled */
    draw_slice_region(cr, src, sx, sy, l, t, 0, 0, l, t);
    draw_slice_region(cr, src, sx + sw - r, sy, r, t, dw - r, 0, r, t);
    draw_slice_region(cr, src, sx, sy + sh - b, l, b, 0, dh - b, l, b);
    draw_slice_region(cr, src, sx + sw - r, sy + sh - b, r, b, dw - r, dh - b, r, b);
    /* edges: stretched along one axis */
    draw_slice_region(cr, src, sx + l, sy, cw, t, l, 0, dcw, t);
    draw_slice_region(cr, src, sx + l, sy + sh - b, cw, b, l, dh - b, dcw, b);
    draw_slice_region(cr, src, sx, sy + t, l, ch, 0, t, l, dch);
    draw_slice_region(cr, src, sx + sw - r, sy + t, r, ch, dw - r, t, r, dch);
    /* center: stretched on both axes */
    draw_slice_region(cr, src, sx + l, sy + t, cw, ch, l, t, dcw, dch);
}

int panel_draw_skin(const PanelSkin *skin, cairo_t *cr, int state, double x, double y, double w, double h)
{
    if (!skin->surface || skin->rows <= 0 || skin->cell_w <= 0 || skin->cell_h <= 0 || w <= 0 || h <= 0) {
        return 0;
    }
    if (state < 0) {
        state = 0;
    }
    if (state >= skin->rows) {
        state = skin->rows - 1;
    }
    cairo_save(cr);
    cairo_translate(cr, x, y);
    panel_draw_9slice_at(cr, skin->surface, 0, state * skin->cell_h, skin->cell_w, skin->cell_h, skin->l, skin->t,
                          skin->r, skin->b, w, h);
    cairo_restore(cr);
    return 1;
}

static void panel_create_buf(Panel *p)
{
    p->buf_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, p->w, p->h);
    p->buf_cr = cairo_create(p->buf_surface);
    if (g_font_face) {
        cairo_set_font_face(p->buf_cr, g_font_face);
    }
    cairo_set_font_size(p->buf_cr, panel_text_size(p));
}

static void panel_create_surface(Panel *p)
{
    p->surface = cairo_xlib_surface_create(g_dpy, p->win, p->visual, p->w, p->h);
    p->cr = cairo_create(p->surface);
    if (g_font_face) {
        cairo_set_font_face(p->cr, g_font_face);
    }
    cairo_set_font_size(p->cr, panel_text_size(p));
    panel_create_buf(p);
}

/* Container popups only: their size follows their content, so a widget
 * appearing/growing (a new tray icon, say) resizes the window in place.
 * Every edge-anchored panel keeps the fixed size panel_resolve_geometry()
 * gave it. */
static void panel_apply_shape(Panel *p);
static void panel_set_size(Panel *p, int w, int h)
{
    p->w = w;
    p->h = h;
    if (!p->win) {
        return;
    }
    XResizeWindow(g_dpy, p->win, (unsigned)w, (unsigned)h);
    cairo_xlib_surface_set_size(p->surface, w, h);
    cairo_destroy(p->buf_cr);
    cairo_surface_destroy(p->buf_surface);
    panel_create_buf(p);
    panel_apply_shape(p);
}

/* ------------------------------------------------------------------ */
/* layout + paint                                                       */
/* ------------------------------------------------------------------ */

/* Layout for a container popup: the window is sized to fit its widgets
 * rather than the widgets squeezed into a fixed window. layout=row (the
 * default) is one line along the owner panel's axis, exactly like a bar;
 * layout=grid wraps that line into rows so the popup comes out roughly
 * square -- the target row length is the side of a square with the same
 * area as the whole one-row strip, never shorter than the widest single
 * widget. The gap between widgets and the popup's own outer margin both
 * come from the owner `container` widget's padding= (WIDGET line), when
 * set -- otherwise p->spacing (the popup panel's own THEME spacing=), so
 * an unthemed container popup still looks like before this key existed. */
static void container_layout(Panel *p)
{
    int n = p->n_layout;
    int lens[MAX_WIDGETS];
    int total = 0, max_len = 0;
    for (int i = 0; i < n; i++) {
        PanelWidget *w = p->layout[i];
        int len = 0, min = 0;
        if (w->ops->measure) {
            w->ops->measure(w, p->thickness, &len, &min);
        }
        if (len < 0) {
            len = p->thickness * 2; /* greedy has nothing to be greedy about here */
        }
        lens[i] = len;
        total += len;
        if (len > max_len) {
            max_len = len;
        }
    }

    int padding_cfg = p->owner ? kv_get_int(p->owner->config_kv, "padding", -1) : -1;
    int gap = padding_cfg >= 0 ? padding_cfg : p->spacing;
    int pad = gap;
    int row_len = total + (n > 1 ? gap * (n - 1) : 0);
    if (p->grid && n > 1) {
        double area = (double)row_len * (p->thickness + gap);
        int target = (int)(sqrt(area) + 0.5);
        row_len = target > max_len ? target : max_len;
    }

    int cursor = 0, row = 0, widest = 0;
    for (int i = 0; i < n; i++) {
        PanelWidget *w = p->layout[i];
        if (cursor > 0 && cursor + lens[i] > row_len) {
            row++;
            cursor = 0;
        }
        w->x = pad + cursor;
        w->y = pad + row * (p->thickness + gap);
        w->len = lens[i];
        w->thickness = p->thickness;
        cursor += lens[i] + gap;
        if (cursor - gap > widest) {
            widest = cursor - gap;
        }
    }
    int rows = n > 0 ? row + 1 : 0;
    int content_len = n > 0 ? widest : p->thickness;
    int content_cross = n > 0 ? rows * p->thickness + (rows - 1) * gap : p->thickness;

    int horizontal = (p->edge == EDGE_TOP || p->edge == EDGE_BOTTOM);
    int want_w = (horizontal ? content_len : content_cross) + 2 * pad;
    int want_h = (horizontal ? content_cross : content_len) + 2 * pad;
    if (want_w != p->w || want_h != p->h) {
        panel_set_size(p, want_w, want_h);
    }
}

/* Rebuilds p->layout[] -- see its doc comment in xispanel.h. */
static void panel_build_layout(Panel *p)
{
    p->n_layout = 0;
    if (p->mode == MODE_CONTAINER) {
        for (int i = 0; i < p->n_widgets; i++) {
            if (!p->widgets[i].inlined) {
                p->layout[p->n_layout++] = &p->widgets[i];
            }
        }
        return;
    }
    for (int i = 0; i < p->n_widgets; i++) {
        PanelWidget *w = &p->widgets[i];
        Panel *q = panel_container_popup(w);
        /* Inlined popup widgets sit just before their container's
         * chevron, so the arrow still reads as "and more in here". */
        for (int j = 0; q && j < q->n_widgets; j++) {
            if (q->widgets[j].inlined && p->n_layout < MAX_WIDGETS * 2) {
                p->layout[p->n_layout++] = &q->widgets[j];
            }
        }
        if (p->n_layout < MAX_WIDGETS * 2) {
            p->layout[p->n_layout++] = w;
        }
    }
}

static void panel_layout(Panel *p)
{
    panel_build_layout(p);
    if (p->mode == MODE_CONTAINER) {
        container_layout(p);
        return;
    }
    int axis_len = (p->edge == EDGE_TOP || p->edge == EDGE_BOTTOM) ? p->w : p->h;
    int lens[MAX_WIDGETS * 2];
    int mins[MAX_WIDGETS * 2];
    int n_greedy = 0;
    int fixed_total = 0;
    int min_total = 0;

    for (int i = 0; i < p->n_layout; i++) {
        PanelWidget *w = p->layout[i];
        int out_len = 0;
        int out_min = 0;
        if (w->ops->measure) {
            w->ops->measure(w, p->thickness, &out_len, &out_min);
        }
        lens[i] = out_len;
        if (out_len < 0) {
            n_greedy++;
            mins[i] = 0;
        } else {
            fixed_total += out_len;
            mins[i] = out_min < 0 ? 0 : (out_min > out_len ? out_len : out_min);
            min_total += mins[i];
        }
    }

    int spacing_total = p->spacing * (p->n_layout > 0 ? p->n_layout - 1 : 0);
    int available = axis_len - spacing_total;
    if (available < 0) {
        available = 0;
    }

    int final_lens[MAX_WIDGETS * 2];

    if (fixed_total <= available) {
        /* Everyone gets their desired size; greedy widgets split whatever
         * is left over. */
        int remaining = available - fixed_total;
        int greedy_each = (n_greedy > 0 && remaining > 0) ? remaining / n_greedy : 0;
        for (int i = 0; i < p->n_layout; i++) {
            final_lens[i] = lens[i] < 0 ? greedy_each : lens[i];
        }
    } else {
        /* Doesn't fit even with every greedy widget at 0: shrink fixed
         * widgets toward their reported minimum, proportionally to how
         * much slack each one has, so no single widget eats the whole
         * squeeze. If even every widget's minimum doesn't fit, this is a
         * panel too small for its content -- everyone just gets their
         * minimum and the last one(s) get slightly clipped, which is the
         * best any layout can do here. */
        int shrinkable = fixed_total - min_total;
        int deficit = fixed_total - available;
        if (deficit > shrinkable) {
            deficit = shrinkable;
        }
        for (int i = 0; i < p->n_layout; i++) {
            if (lens[i] < 0) {
                final_lens[i] = 0;
                continue;
            }
            int slack = lens[i] - mins[i];
            int shrink = (shrinkable > 0) ? (int)((int64_t)deficit * slack / shrinkable) : 0;
            final_lens[i] = lens[i] - shrink;
        }
    }

    int cursor = 0;
    for (int i = 0; i < p->n_layout; i++) {
        PanelWidget *w = p->layout[i];
        int len = final_lens[i] < 0 ? 0 : final_lens[i];
        w->x = cursor;
        w->y = 0;
        w->len = len;
        w->thickness = p->thickness;
        cursor += len + p->spacing;
    }
}

/* Paints p's full content (background + every widget) into `cr`, with an
 * extra cairo_scale(scale, scale) pushed first -- see this function's own
 * doc comment in xispanel.h. Used for the normal on-screen buffer
 * (scale=1, from panel_repaint() below) and by density.c's auxiliary-
 * pixmap render (scale=density, see TESTS/X-DENSITY.md). */
void panel_paint_content(Panel *p, cairo_t *cr, double scale)
{
    cairo_save(cr);
    cairo_scale(cr, scale, scale);

    /* On an ARGB visual (a compositor is running), the rounded corners are
     * cut by clipping the *painted content* to a rounded rect and leaving
     * those pixels transparent -- not by SHAPE-masking p->win (see
     * panel_apply_shape()'s comment: the corners of a SHAPE-bounded window
     * can never be made clickable again, no matter what ShapeInput says).
     * This way p->win keeps its full rectangular hit region and the round
     * look is purely cosmetic, exactly like kiwm's own rounded frames on a
     * composited session. Without ARGB there is no alpha channel to cut
     * into, so panel_apply_shape() falls back to SHAPE for the visual
     * effect and the corners stay unclickable -- an uncomposited session
     * only, and not fixable from here. */
    if (p->border_radius > 0 && p->depth == 32) {
        int r = p->border_radius;
        int max_r = (p->w < p->h ? p->w : p->h) / 2;
        if (r > max_r) {
            r = max_r;
        }
        panel_trace_rounded_rect(cr, p->w, p->h, r);
        cairo_clip(cr);
    }

    /* Only the single cairo_paint() at the end of panel_repaint() (blitting
     * the finished buffer onto the real, on-screen p->cr/surface) touches
     * the window itself -- painting straight onto the xlib surface instead
     * sent each widget's fills/strokes as its own X request, which a
     * compositor could pick up mid-repaint and show as flicker (worst on
     * tasklist, which repaints on every ~800ms poll tick even with nothing
     * visibly different). */
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    if (p->bg_image_surface) {
        /* Clear first: the 9-slice draw below is itself CAIRO_OPERATOR_SOURCE
         * per region, but only actually covers the panel rect once (no
         * overlap/gaps by construction), so no separate clear is needed --
         * kept anyway for a defined background on any 1px seam from
         * floating-point scaling error. */
        cairo_set_source_rgba(cr, 0, 0, 0, 0);
        cairo_paint(cr);
        int sw = cairo_image_surface_get_width(p->bg_image_surface);
        int sh = cairo_image_surface_get_height(p->bg_image_surface);
        panel_draw_9slice(cr, p->bg_image_surface, sw, sh, p->bg_slice_l, p->bg_slice_t, p->bg_slice_r,
                           p->bg_slice_b, p->w, p->h);
    } else {
        cairo_set_source_rgba(cr, p->bg_r, p->bg_g, p->bg_b, p->bg_a);
        cairo_paint(cr);
    }
    cairo_restore(cr);

    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    for (int i = 0; i < p->n_layout; i++) {
        PanelWidget *w = p->layout[i];
        if (w->ops->paint) {
            cairo_save(cr);
            /* This widget's real, physical, un-rotated on-panel rectangle
             * -- same shape widget_get_rect() always used to report
             * before rotation existed. Rotating *about its center* by any
             * multiple of 90 degrees is what lets one formula handle
             * every edge/angle combination: at 0/180 the content shape
             * equals this rect; at 90/270 it's this rect transposed (see
             * widget_get_rect()), and rotating that transposed shape
             * about the same center lands it back on exactly this
             * footprint. Click/tooltip/menu hit-testing never sees any of
             * this -- it works from the panel's real physical layout the
             * whole time, so it's completely unaffected by rotation. */
            int px, py, pw, ph;
            if (p->edge == EDGE_TOP || p->edge == EDGE_BOTTOM) {
                px = w->x;
                py = w->y;
                pw = w->len;
                ph = w->thickness;
            } else {
                px = w->y;
                py = w->x;
                pw = w->thickness;
                ph = w->len;
            }
            double cx = px + pw / 2.0;
            double cy = py + ph / 2.0;
            int transposed = (p->rotate == 90 || p->rotate == 270);
            double content_w = transposed ? ph : pw;
            double content_h = transposed ? pw : ph;
            cairo_translate(cr, cx, cy);
            if (p->rotate) {
                cairo_rotate(cr, p->rotate * M_PI / 180.0);
            }
            cairo_translate(cr, -content_w / 2.0, -content_h / 2.0);
            w->ops->paint(w, cr);
            cairo_restore(cr);
        }
    }
    cairo_restore(cr); /* pops the scale pushed at the top */
}

void panel_foreach(void (*cb)(Panel *p, void *ctx), void *ctx)
{
    for (int i = 0; i < MAX_PANELS; i++) {
        if (g_panels[i].in_use) {
            cb(&g_panels[i], ctx);
        }
    }
}

static void panel_repaint(Panel *p)
{
    if (!p->cr || !p->buf_cr) {
        return;
    }

    /* Once a cairo_t enters an error state (e.g. CAIRO_STATUS_INVALID_STRING
     * from passing malformed UTF-8 to cairo_show_text -- window titles are
     * supposed to be UTF-8 but not every client is well-behaved), every
     * subsequent call on it becomes a silent no-op *permanently*, even
     * cairo_save()/cairo_restore(). Since these cairo_t's are reused across
     * every repaint rather than recreated each time, one bad frame would
     * otherwise blank the panel forever. Recreate here instead of trusting
     * every current and future widget to never make this mistake. */
    if (cairo_status(p->buf_cr) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "xispanel: panel '%s': cairo error (%s), recreating drawing context\n", p->name,
                cairo_status_to_string(cairo_status(p->buf_cr)));
        cairo_destroy(p->buf_cr);
        p->buf_cr = cairo_create(p->buf_surface);
        if (g_font_face) {
            cairo_set_font_face(p->buf_cr, g_font_face);
        }
        cairo_set_font_size(p->buf_cr, panel_text_size(p));
    }

    panel_paint_content(p, p->buf_cr, 1.0);
    cairo_surface_flush(p->buf_surface);

    /* p->cr is also used directly by widgets' measure() (cairo_text_extents
     * needs *some* cairo_t with the right font set, and measure() doesn't
     * get one passed in) -- so it can be poisoned by bad UTF-8 same as
     * buf_cr, even though it's mostly just the final blit target here. */
    if (cairo_status(p->cr) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "xispanel: panel '%s': cairo error (%s), recreating drawing context\n", p->name,
                cairo_status_to_string(cairo_status(p->cr)));
        cairo_destroy(p->cr);
        p->cr = cairo_create(p->surface);
        if (g_font_face) {
            cairo_set_font_face(p->cr, g_font_face);
        }
        cairo_set_font_size(p->cr, panel_text_size(p));
    }

    cairo_save(p->cr);
    cairo_set_operator(p->cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_surface(p->cr, p->buf_surface, 0, 0);
    cairo_paint(p->cr);
    cairo_restore(p->cr);
    cairo_surface_flush(p->surface);
    XFlush(g_dpy);
    p->dirty = 0;

    density_render(p);
}

/* ------------------------------------------------------------------ */
/* autohide state machine                                               */
/* ------------------------------------------------------------------ */

static void panel_autohide_enter(Panel *p)
{
    if (p->mode != MODE_AUTOHIDE) {
        return;
    }
    p->ah_hide_deadline_ms = 0;
    if (p->ah_state == AH_SHOWN || p->ah_state == AH_SHOWING) {
        return;
    }
    p->ah_state = AH_SHOWING;
    p->ah_anim_start_ms = now_ms();
    if (!p->mapped) {
        XMoveWindow(g_dpy, p->win, p->hidden_x, p->hidden_y);
        XMapWindow(g_dpy, p->win);
        XRaiseWindow(g_dpy, p->win);
        p->mapped = 1;
    }
}

static void panel_autohide_leave(Panel *p)
{
    if (p->mode != MODE_AUTOHIDE) {
        return;
    }
    if (p->ah_state == AH_SHOWN || p->ah_state == AH_SHOWING) {
        p->ah_hide_deadline_ms = now_ms() + AUTOHIDE_DELAY_MS;
    }
}

static int lerp_int(int from, int to, double t)
{
    return from + (int)((to - from) * t + 0.5);
}

/* Advances one autohide animation step for `p`. Returns 1 if `p` needs to
 * be woken again soon (mid-animation or waiting out the hide delay). */
static int panel_autohide_tick(Panel *p, uint64_t now)
{
    if (p->mode != MODE_AUTOHIDE) {
        return 0;
    }

    if (p->ah_hide_deadline_ms && now >= p->ah_hide_deadline_ms) {
        if (g_open_container && g_open_container->owner->panel == p) {
            /* A container popup hanging off this bar is open: the pointer
             * left the bar for the popup, not for good. Hold the bar until
             * the popup closes (container_close() restarts the timer). */
            p->ah_hide_deadline_ms = now + AUTOHIDE_DELAY_MS;
            return 1;
        }
        p->ah_hide_deadline_ms = 0;
        p->ah_state = AH_HIDING;
        p->ah_anim_start_ms = now;
    }

    if (p->ah_state == AH_SHOWING || p->ah_state == AH_HIDING) {
        double t = (double)(now - p->ah_anim_start_ms) / AUTOHIDE_ANIM_MS;
        if (t >= 1.0) {
            t = 1.0;
        }
        int from_x = (p->ah_state == AH_SHOWING) ? p->hidden_x : p->x;
        int from_y = (p->ah_state == AH_SHOWING) ? p->hidden_y : p->y;
        int to_x = (p->ah_state == AH_SHOWING) ? p->x : p->hidden_x;
        int to_y = (p->ah_state == AH_SHOWING) ? p->y : p->hidden_y;
        XMoveWindow(g_dpy, p->win, lerp_int(from_x, to_x, t), lerp_int(from_y, to_y, t));
        if (t >= 1.0) {
            if (p->ah_state == AH_SHOWING) {
                p->ah_state = AH_SHOWN;
            } else {
                p->ah_state = AH_HIDDEN;
                XUnmapWindow(g_dpy, p->win);
                p->mapped = 0;
            }
            return p->ah_hide_deadline_ms != 0;
        }
        return 1;
    }

    return p->ah_hide_deadline_ms != 0;
}

/* ------------------------------------------------------------------ */
/* panel lifecycle                                                      */
/* ------------------------------------------------------------------ */

static void panel_destroy_widgets(Panel *p)
{
    for (int i = 0; i < p->n_widgets; i++) {
        PanelWidget *w = &p->widgets[i];
        if (w->ops->destroy) {
            w->ops->destroy(w);
        }
        free(w->priv);
    }
    p->n_widgets = 0;
}

static void panel_deactivate(Panel *p)
{
    panel_destroy_widgets(p);
    density_panel_destroyed(p);
    if (p->bg_image_surface) {
        cairo_surface_destroy(p->bg_image_surface);
        p->bg_image_surface = NULL;
    }
    if (p->btns_image_surface) {
        cairo_surface_destroy(p->btns_image_surface);
        p->btns_image_surface = NULL;
    }
    panel_free_skin(&p->tasks_skin);
    panel_free_skin(&p->button_skin);
    panel_free_skin(&p->menu_skin);
    panel_free_skin(&p->menuitem_skin);
    panel_free_skin(&p->pager_skin);
    panel_free_skin(&p->bar_skin);
    panel_free_theme_icons(p);
    if (p->buf_cr) {
        cairo_destroy(p->buf_cr);
        p->buf_cr = NULL;
    }
    if (p->buf_surface) {
        cairo_surface_destroy(p->buf_surface);
        p->buf_surface = NULL;
    }
    if (p->cr) {
        cairo_destroy(p->cr);
        p->cr = NULL;
    }
    if (p->surface) {
        cairo_surface_destroy(p->surface);
        p->surface = NULL;
    }
    if (p->sensor_win != None) {
        XDestroyWindow(g_dpy, p->sensor_win);
        p->sensor_win = None;
    }
    if (p->win != None) {
        XDestroyWindow(g_dpy, p->win);
        p->win = None;
    }
    if (p->cmap != None && p->cmap != DefaultColormap(g_dpy, g_screen)) {
        XFreeColormap(g_dpy, p->cmap);
        p->cmap = None;
    }
}

static void panel_activate(Panel *p)
{
    panel_resolve_geometry(p);
    panel_pick_visual(p);
    panel_load_bg_image(p);
    panel_load_btns_image(p);
    panel_load_skins(p);
    panel_load_theme_colors(p);

    int start_x = p->x, start_y = p->y;
    p->ah_state = AH_HIDDEN;
    p->ah_hide_deadline_ms = 0;
    if (p->mode == MODE_AUTOHIDE) {
        start_x = p->hidden_x;
        start_y = p->hidden_y;
    }

    p->win = panel_create_window(p, start_x, start_y, p->w, p->h);
    panel_apply_strut(p);
    panel_apply_shape(p); /* theme's border_radius=, no-op without one */
    panel_create_surface(p);

    for (int i = 0; i < p->n_widgets; i++) {
        p->widgets[i].panel = p;
    }
    panel_layout(p);

    if (p->mode == MODE_AUTOHIDE) {
        p->sensor_win = panel_create_sensor(p);
        p->mapped = 0;
    } else if (p->mode == MODE_CONTAINER) {
        p->mapped = 0; /* mapped by container_open() only */
    } else {
        XMapWindow(g_dpy, p->win);
        XRaiseWindow(g_dpy, p->win);
        p->mapped = 1;
    }
    p->dirty = 1;
}

static Panel *find_panel(const char *name)
{
    for (int i = 0; i < MAX_PANELS; i++) {
        if (g_panels[i].in_use && strcmp(g_panels[i].name, name) == 0) {
            return &g_panels[i];
        }
    }
    return NULL;
}

static Panel *alloc_panel(const char *name, const char *output)
{
    for (int i = 0; i < MAX_PANELS; i++) {
        if (!g_panels[i].in_use) {
            Panel *p = &g_panels[i];
            memset(p, 0, sizeof(*p));
            p->in_use = 1;
            snprintf(p->name, sizeof(p->name), "%s", name);
            snprintf(p->output, sizeof(p->output), "%s", output);
            p->edge = EDGE_TOP;
            p->pct = 100;
            p->thickness_cfg = 32;
            p->mode = MODE_DOCK;
            p->tooltip_delay_ms = 500;
            p->tooltip_close_delay_ms = 300;
            p->tooltip_reuse_window = 1;
            p->bg_r = 0.12;
            p->bg_g = 0.12;
            p->bg_b = 0.12;
            p->bg_a = 0.85;
            p->fg_r = p->fg_g = p->fg_b = 0.93;
            p->fg_a = 1.0;
            /* The colors above are the whole fallback: a THEME line's
             * bg=/fg= (applied later, from load_config()) overrides them,
             * and nothing else is consulted -- xispanel.conf is the only
             * source of truth, no other desktop's config is read. */
            /* 0 = "not configured"; every user of font_size_px treats that
             * as "fall back to my own existing size" (see the field's doc
             * comment in xispanel.h). THEME's font_size= sets it. */
            p->font_size_px = 0;
            p->spacing = 4;
            p->density_num = 1;
            p->density_den = 1;
            return p;
        }
    }
    return NULL;
}

static void panel_add_widget(Panel *p, int order, const char *type, const char *kvline)
{
    /* Layout order itself comes from file order (config lines are already
     * emitted in the desired order), not this number -- but it's still
     * stored on the widget as an identifier: it's the third field of this
     * widget's own WIDGET line, so config_widget_set_key() (tasklist.c's
     * pinned-apps persistence) can find that exact line again later by
     * (panel name, order, type), the same triple that uniquely identifies
     * it in the file. */
    if (p->n_widgets >= MAX_WIDGETS) {
        fprintf(stderr, "xispanel: panel '%s': too many widgets, ignoring '%s'\n", p->name, type);
        return;
    }
    const PanelWidgetOps *ops = find_widget_ops(type);
    if (!ops) {
        fprintf(stderr, "xispanel: panel '%s': unknown widget type '%s'\n", p->name, type);
        return;
    }
    if (p->mode == MODE_CONTAINER && !ops->embeddable) {
        fprintf(stderr, "xispanel: container '%s': widget type '%s' can't be placed inside a container, ignoring\n",
                p->name, type);
        return;
    }
    PanelWidget *w = &p->widgets[p->n_widgets++];
    memset(w, 0, sizeof(*w));
    w->ops = ops;
    w->panel = p;
    w->order = order;
    snprintf(w->config_kv, sizeof(w->config_kv), "%s", kvline ? kvline : "");
    if (p->mode == MODE_CONTAINER) {
        char buf[16];
        if (kv_get(w->config_kv, "inline", buf, sizeof(buf))) {
            w->inline_mode = !strcmp(buf, "urgent") ? INLINE_URGENT
                             : (!strcmp(buf, "yes") || !strcmp(buf, "always")) ? INLINE_ALWAYS
                                                                                  : INLINE_NO;
        }
    }
    if (ops->priv_size > 0) {
        w->priv = calloc(1, ops->priv_size);
    }
    if (ops->init) {
        ops->init(w);
    }
}

/* ------------------------------------------------------------------ */
/* config ($XDG_CONFIG_HOME/xispanel.conf)                              */
/* ------------------------------------------------------------------ */

static enum edge parse_edge(const char *s)
{
    if (!strcmp(s, "bottom")) {
        return EDGE_BOTTOM;
    }
    if (!strcmp(s, "left")) {
        return EDGE_LEFT;
    }
    if (!strcmp(s, "right")) {
        return EDGE_RIGHT;
    }
    return EDGE_TOP;
}

static enum panel_mode parse_mode(const char *s)
{
    if (!strcmp(s, "overlay")) {
        return MODE_OVERLAY;
    }
    if (!strcmp(s, "autohide")) {
        return MODE_AUTOHIDE;
    }
    if (!strcmp(s, "container")) {
        return MODE_CONTAINER;
    }
    return MODE_DOCK;
}

static void apply_panel_kv(Panel *p, const char *kvline)
{
    char buf[64];
    if (kv_get(kvline, "edge", buf, sizeof(buf))) {
        p->edge = parse_edge(buf);
    }
    if (kv_get(kvline, "layout", buf, sizeof(buf))) {
        p->grid = strcmp(buf, "grid") == 0;
    }
    p->pct = kv_get_int(kvline, "pct", p->pct);
    if (p->pct < 1) {
        p->pct = 1;
    }
    if (p->pct > 100) {
        p->pct = 100;
    }
    p->thickness_cfg = kv_get_int(kvline, "thickness", p->thickness_cfg);
    if (kv_get(kvline, "mode", buf, sizeof(buf))) {
        p->mode = parse_mode(buf);
    }
    if (kv_get(kvline, "rotate", buf, sizeof(buf))) {
        int r = atoi(buf);
        if (r == 0 || r == 90 || r == 180 || r == 270) {
            p->rotate = r;
        } else {
            fprintf(stderr, "xispanel: panel '%s': invalid rotate=%s (must be 0, 90, 180, or 270), ignoring\n",
                    p->name, buf);
        }
    }
    p->tooltip_delay_ms = kv_get_int(kvline, "tooltip_delay", p->tooltip_delay_ms);
    if (p->tooltip_delay_ms < 0) {
        p->tooltip_delay_ms = 0;
    }
    p->tooltip_close_delay_ms = kv_get_int(kvline, "tooltip_close_delay", p->tooltip_close_delay_ms);
    if (p->tooltip_close_delay_ms < 0) {
        p->tooltip_close_delay_ms = 0;
    }
    p->tooltip_reuse_window = kv_get_int(kvline, "tooltip_reuse", p->tooltip_reuse_window) != 0;
    p->tooltip_toast_padding_extra = kv_get_int(kvline, "padding_extra", p->tooltip_toast_padding_extra);
    if (p->tooltip_toast_padding_extra < 0) {
        p->tooltip_toast_padding_extra = 0;
    }
}

static void apply_theme_kv(Panel *p, const char *kvline)
{
    char buf[32];
    /* cfg_has_* is what makes the theme folder's `colors` file a
     * *default* rather than an override: panel_load_theme_colors() only
     * fills in what the THEME line left unsaid. */
    if (kv_get(kvline, "bg", buf, sizeof(buf))) {
        p->cfg_has_bg = parse_hex_color(buf, &p->bg_r, &p->bg_g, &p->bg_b, &p->bg_a);
    }
    if (kv_get(kvline, "fg", buf, sizeof(buf))) {
        p->cfg_has_fg = parse_hex_color(buf, &p->fg_r, &p->fg_g, &p->fg_b, &p->fg_a);
    }
    if (kv_get(kvline, "h_color", buf, sizeof(buf))) {
        parse_hex_color(buf, &p->h_r, &p->h_g, &p->h_b, &p->h_a);
        p->has_h_color = 1;
    }
    p->spacing = kv_get_int(kvline, "spacing", p->spacing);
    if (kv_get(kvline, "font_size", buf, sizeof(buf))) {
        p->font_size_px = atof(buf);
        p->cfg_has_font_size = p->font_size_px > 0;
    }
    /* Optional bitmap theme: theme=<folder>, shared with kiwm (same file
     * names, see kiwm/README.md's "Theming" section) -- bg.png+slice (the
     * 9-slice background) and btns.png+btns.slice (winctl's button
     * sprites), each independently optional. Actually loaded by
     * panel_load_bg_image()/panel_load_btns_image(), called from
     * panel_activate() -- not here, since that needs Imlib2/an open X
     * display that may not exist yet while just parsing config text. */
    kv_get(kvline, "theme", p->theme_path, sizeof(p->theme_path));
}

/* Directory containing the config file (same place write_default_config_
 * if_missing() creates xispanel.conf in) -- tasklist.c places its
 * pinned-apps sidecar file (fixed_list=, see tasklist_persist_pinned())
 * alongside it when the user hasn't given an absolute path. Returns 0 if
 * g_configpath somehow has no '/' in it (shouldn't happen -- it's always
 * built from an absolute XDG dir in main()), leaving `out` untouched. */
int config_dir(char *out, size_t outsz)
{
    const char *slash = strrchr(g_configpath, '/');
    if (!slash) {
        return 0;
    }
    size_t len = (size_t)(slash - g_configpath);
    if (len >= outsz) {
        len = outsz - 1;
    }
    memcpy(out, g_configpath, len);
    out[len] = 0;
    return 1;
}

/* True if `raw` (one line of the config file, not yet trimmed) is a
 * WIDGET line for exactly (panel_name, order, type_name) -- tokenizes a
 * scratch copy the same whitespace-run-insensitive way load_config()'s
 * own field splitter does, so this matches regardless of whether the
 * line uses tabs, spaces, or a mix. */
static int widget_line_matches(const char *raw, const char *panel_name, int order, const char *type_name)
{
    char buf[LINE_MAX_LEN];
    snprintf(buf, sizeof(buf), "%s", raw);
    char *save = NULL;
    char *tok = strtok_r(buf, " \t\r\n", &save);
    if (!tok || strcmp(tok, "WIDGET") != 0) {
        return 0;
    }
    tok = strtok_r(NULL, " \t\r\n", &save);
    if (!tok || strcmp(tok, panel_name) != 0) {
        return 0;
    }
    tok = strtok_r(NULL, " \t\r\n", &save);
    if (!tok || atoi(tok) != order) {
        return 0;
    }
    tok = strtok_r(NULL, " \t\r\n", &save);
    return tok && strcmp(tok, type_name) == 0;
}

/* Appends " key=value" to one WIDGET line's own kv tail, identified by
 * (panel_name, order, type_name) exactly as widget_line_matches() above
 * matches -- this is the ONLY kind of write-back xispanel ever does to
 * its own config (see this file's top-of-file doc comment on config
 * persistence), deliberately narrow: one widget's own fixed_list= key,
 * written once, so a pinned-apps sidecar file survives a restart without
 * needing general "persist any runtime change" config-mutation
 * infrastructure. Rewrites the whole file to a sibling .tmp path (no
 * in-place text insert into a line-oriented file) and renames it over
 * the original, so a crash/power-loss mid-write can't leave a half-
 * written config behind; every line is copied through unchanged except
 * the one WIDGET line that matches. Returns 0 (leaving the original file
 * untouched) if the config can't be read/rewritten, or no matching line
 * was found -- the caller treats that as "this pin just won't survive a
 * restart", not a fatal error. */
int config_widget_set_key(const char *panel_name, int order, const char *type_name, const char *key,
                           const char *value)
{
    FILE *in = fopen(g_configpath, "r");
    if (!in) {
        return 0;
    }
    char tmppath[PATH_MAX];
    snprintf(tmppath, sizeof(tmppath), "%s.tmp", g_configpath);
    FILE *out = fopen(tmppath, "w");
    if (!out) {
        fclose(in);
        return 0;
    }

    char line[LINE_MAX_LEN];
    int found = 0;
    while (fgets(line, sizeof(line), in)) {
        if (!found && widget_line_matches(line, panel_name, order, type_name)) {
            found = 1;
            size_t len = strlen(line);
            while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
                line[--len] = 0;
            }
            fprintf(out, "%s %s=%s\n", line, key, value);
        } else {
            fputs(line, out);
        }
    }
    fclose(in);
    fclose(out);

    if (!found || rename(tmppath, g_configpath) != 0) {
        if (found) {
            fprintf(stderr, "xispanel: could not update config %s: %s\n", g_configpath, strerror(errno));
        }
        remove(tmppath);
        return 0;
    }
    return 1;
}

/* Surveys every "PANEL <name> <output> ..." line's output= token (an
 * edid: id or a literal connector name alike -- xis_build_output_rename_map()
 * sorts out which apply to it) for feeding into that shared rename-map
 * builder. Same reasoning as xisback's own equivalent: outputs get
 * renamed by drivers/re-plugging often enough that a saved "HDMI-1"
 * panel can silently stop matching anything on the next boot, and
 * panel_resolve_geometry() then falls back to full-screen for it,
 * stacking every per-output panel on top of each other on whatever
 * output happens to come first. */
static int collect_saved_output_ids(const char *path, char ids[][XIS_OUTPUT_STR_LEN], int max)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        return 0;
    }
    int n = 0;
    char line[LINE_MAX_LEN];
    while (n < max && fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = 0;
        }
        char *p = line;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (strncmp(p, "PANEL", 5) != 0 || (p[5] != ' ' && p[5] != '\t')) {
            continue;
        }
        p += 5;
        char *tok[3] = {NULL, NULL, NULL};
        for (int i = 0; i < 3; i++) {
            while (*p == ' ' || *p == '\t') {
                p++;
            }
            if (!*p) {
                break;
            }
            tok[i] = p;
            while (*p && *p != ' ' && *p != '\t') {
                p++;
            }
            if (*p) {
                *p++ = 0;
            }
        }
        const char *output = tok[1]; /* PANEL <name> <output> ... */
        if (output) {
            snprintf(ids[n], XIS_OUTPUT_STR_LEN, "%s", output);
            n++;
        }
    }
    fclose(f);
    return n;
}

static void load_config(void)
{
    char saved_ids[MAX_PANELS][XIS_OUTPUT_STR_LEN];
    int n_saved = collect_saved_output_ids(g_configpath, saved_ids, MAX_PANELS);
    const char *saved_ptrs[MAX_PANELS];
    for (int i = 0; i < n_saved; i++) {
        saved_ptrs[i] = saved_ids[i];
    }
    XisOutputRename rename_map[MAX_PANELS];
    int n_rename = xis_build_output_rename_map(g_dpy, saved_ptrs, n_saved, rename_map, MAX_PANELS);

    FILE *f = fopen(g_configpath, "r");
    if (!f) {
        return;
    }
    char line[LINE_MAX_LEN];
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = 0;
        }
        if (len == 0 || line[0] == '#') {
            continue;
        }

        /* Splits on any run of spaces/tabs (not just tab) -- join_fields()
         * below always reassembles the kv-value tail with single spaces
         * between tokens regardless of how many pieces it was split into,
         * so this is a lossless normalization, not a behavior change: a
         * config line can now be hand-edited with plain spaces instead of
         * requiring literal tabs between fields. */
        char *fields[48];
        int nf = 0;
        char *p = line;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p) {
            fields[nf++] = p;
        }
        while (nf < 48) {
            while (*p && *p != ' ' && *p != '\t') {
                p++;
            }
            if (!*p) {
                break;
            }
            *p = 0;
            p++;
            while (*p == ' ' || *p == '\t') {
                p++;
            }
            if (!*p) {
                break;
            }
            fields[nf++] = p;
        }
        if (nf == 0) {
            continue; /* line was whitespace-only after trimming */
        }

        if (strcmp(fields[0], "PANEL") == 0 && nf >= 3) {
            const char *output = xis_apply_output_rename(rename_map, n_rename, fields[2]);
            Panel *pan = alloc_panel(fields[1], output);
            if (!pan) {
                fprintf(stderr, "xispanel: config: too many panels, ignoring '%s'\n", fields[1]);
                continue;
            }
            char kvline[LINE_MAX_LEN];
            join_fields(fields, 3, nf, kvline, sizeof(kvline));
            apply_panel_kv(pan, kvline);
        } else if (strcmp(fields[0], "WIDGET") == 0 && nf >= 4) {
            Panel *pan = find_panel(fields[1]);
            if (!pan) {
                fprintf(stderr, "xispanel: config: WIDGET references unknown panel '%s'\n", fields[1]);
                continue;
            }
            char kvline[LINE_MAX_LEN];
            join_fields(fields, 4, nf, kvline, sizeof(kvline));
            panel_add_widget(pan, atoi(fields[2]), fields[3], kvline);
        } else if (strcmp(fields[0], "THEME") == 0 && nf >= 2) {
            Panel *pan = find_panel(fields[1]);
            if (!pan) {
                fprintf(stderr, "xispanel: config: THEME references unknown panel '%s'\n", fields[1]);
                continue;
            }
            char kvline[LINE_MAX_LEN];
            join_fields(fields, 2, nf, kvline, sizeof(kvline));
            apply_theme_kv(pan, kvline);
        } else {
            fprintf(stderr, "xispanel: config: skipping unknown line: '%s'\n", line);
        }
    }
    fclose(f);
}

static void link_containers(void);
static void schedule_widget_repoll(uint64_t at_ms);

static void reload_all_panels(void)
{
    panel_container_close_all(); /* releases its grab; the Panel is about to go away */
    panel_menu_close(); /* about to invalidate every Panel/PanelWidget it could reference */
    tooltip_close();
    for (int i = 0; i < MAX_PANELS; i++) {
        if (g_panels[i].in_use) {
            panel_deactivate(&g_panels[i]);
        }
    }
    memset(g_panels, 0, sizeof(g_panels));
    load_config();
    link_containers();
    for (int i = 0; i < MAX_PANELS; i++) {
        if (g_panels[i].in_use) {
            panel_activate(&g_panels[i]);
        }
    }
}

/* Re-resolves every panel's output geometry (RandR CRTC box, or
 * X-INPUT-SCALE's confine box when active -- see resolve_output_geometry())
 * and reloads everything from config the instant any of them disagrees
 * with what's currently applied. Shared by the event-driven path
 * (inputscale_fd()/inputscale_poll_change() -- XISConfineNotify, when the
 * running server build supports it) and the polling fallback below (for
 * older server builds with no event support yet). Reuses reload_all_panels() wholesale
 * rather than a bespoke resize path, same tradeoff RandR hotplug already
 * makes. Returns 1 if it reloaded. */
static int check_output_geometry_changed(void)
{
    for (int i = 0; i < MAX_PANELS; i++) {
        Panel *p = &g_panels[i];
        if (!p->in_use) {
            continue;
        }
        int nx, ny, nw, nh;
        double nhz;
        if (!resolve_output_geometry(p->output, &nx, &ny, &nw, &nh, &nhz)) {
            continue;
        }
        if (nx != p->out_x || ny != p->out_y || nw != p->out_w || nh != p->out_h) {
            fprintf(stderr,
                    "xispanel: output '%s' usable area changed (%dx%d+%d+%d -> %dx%d+%d+%d) -- reloading panels\n",
                    p->output, p->out_w, p->out_h, p->out_x, p->out_y, nw, nh, nx, ny);
            reload_all_panels();
            return 1; /* reload_all_panels() already rebuilt every panel from config */
        }
    }
    return 0;
}

/* Slow-poll fallback only, for a server build with no XISConfineNotify
 * event support yet (see inputscale.c's doc comment) -- setting/clearing
 * X-INPUT-SCALE confinement raises no RandR event on its own, so without
 * either this or the live event, a compositor toggling per-output HiDPI
 * confinement while xispanel is already running would go unnoticed until
 * the next unrelated reload. Deliberately slow (confinement toggling is a
 * rare, deliberate compositor action, not something worth reacting to
 * instantly) since this is the fallback path, not the primary one. */
#define CONFINE_POLL_MS 1500
static void poll_output_geometry_changes(uint64_t now)
{
    static uint64_t next_poll_ms = 0;
    if (now < next_poll_ms) {
        return;
    }
    next_poll_ms = now + CONFINE_POLL_MS;
    check_output_geometry_changed();
}

/* ------------------------------------------------------------------ */
/* IPC (line-JSON over $XDG_RUNTIME_DIR/xispanel-ctl.sock)              */
/* ------------------------------------------------------------------ */

/* sockaddr_un.sun_path is only 108 bytes on Linux, well short of PATH_MAX
 * -- an unusually long $XDG_RUNTIME_DIR would otherwise get silently
 * truncated by snprintf, and two differently-long paths could then
 * collide on the same truncated socket name. Fail loudly instead. */
static int build_sockaddr_un(struct sockaddr_un *addr, const char *path)
{
    memset(addr, 0, sizeof(*addr));
    addr->sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(addr->sun_path)) {
        fprintf(stderr, "xispanel: socket path too long (>%zu bytes): %s\n", sizeof(addr->sun_path) - 1, path);
        return -1;
    }
    memcpy(addr->sun_path, path, strlen(path) + 1);
    return 0;
}

/* Same minimal flat-JSON helpers as xisguard-ctl -- good enough for the
 * request shapes this protocol actually needs, no parser dependency. */
static int json_get_int(const char *msg, const char *key, int *out)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(msg, needle);
    if (!p) {
        return 0;
    }
    p += strlen(needle);
    while (*p == ' ') {
        p++;
    }
    char *end;
    long v = strtol(p, &end, 10);
    if (end == p) {
        return 0;
    }
    *out = (int)v;
    return 1;
}

static int json_get_str(const char *msg, const char *key, char *dst, size_t dst_sz)
{
    dst[0] = 0;
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(msg, needle);
    if (!p) {
        return 0;
    }
    p += strlen(needle);
    while (*p == ' ') {
        p++;
    }
    if (*p != '"') {
        return 0;
    }
    p++;
    const char *end = strchr(p, '"');
    if (!end) {
        return 0;
    }
    size_t len = (size_t)(end - p);
    if (len >= dst_sz) {
        len = dst_sz - 1;
    }
    memcpy(dst, p, len);
    dst[len] = 0;
    return 1;
}

/* Escapes `in` for safe embedding inside a JSON string literal -- just
 * enough for text a notification sender controls (summary/body/app_name):
 * '"', '\\', and the control characters JSON forbids literally. Anything
 * else (UTF-8 multibyte sequences included) passes through untouched, so
 * this only ever grows the string, never reinterprets it. Truncates
 * (rather than overflowing) if `out` is too small for the worst case. */
static void json_escape(const char *in, char *out, size_t out_sz)
{
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 2 < out_sz; p++) {
        switch (*p) {
        case '"': out[o++] = '\\'; out[o++] = '"'; break;
        case '\\': out[o++] = '\\'; out[o++] = '\\'; break;
        case '\n': out[o++] = '\\'; out[o++] = 'n'; break;
        case '\r': out[o++] = '\\'; out[o++] = 'r'; break;
        case '\t': out[o++] = '\\'; out[o++] = 't'; break;
        default:
            if (*p < 0x20) {
                break; /* drop other control bytes rather than emit invalid JSON */
            }
            out[o++] = (char)*p;
        }
    }
    out[o] = 0;
}

static void handle_ipc_message(const char *req, char *resp, size_t resp_sz)
{
    char cmd[32];
    json_get_str(req, "cmd", cmd, sizeof(cmd));

    if (strcmp(cmd, "PING") == 0) {
        snprintf(resp, resp_sz, "{\"ok\":true,\"pong\":true}\n");
    } else if (strcmp(cmd, "GET_STATUS") == 0) {
        int n_panels = 0;
        for (int i = 0; i < MAX_PANELS; i++) {
            if (g_panels[i].in_use) {
                n_panels++;
            }
        }
        snprintf(resp, resp_sz, "{\"ok\":true,\"version\":\"%s\",\"panels\":%d}\n", XISPANEL_VERSION, n_panels);
    } else if (strcmp(cmd, "RELOAD") == 0) {
        reload_all_panels();
        snprintf(resp, resp_sz, "{\"ok\":true}\n");
    } else if (strcmp(cmd, "OSD") == 0) {
        /* Lets a process outside xispanel (xiskeys today; kiconfd next --
         * see toast_show_osd()'s own doc comment in xispanel.h) show a
         * toast without linking against xispanel or reimplementing the
         * popup itself. Every field but "summary" is optional. */
        char icon_name[128] = "", summary[NOTIFD_SUMMARY_MAX] = "", body[NOTIFD_BODY_MAX] = "";
        char urgency_str[16] = "", tag[32] = "";
        json_get_str(req, "icon", icon_name, sizeof(icon_name));
        json_get_str(req, "summary", summary, sizeof(summary));
        json_get_str(req, "body", body, sizeof(body));
        json_get_str(req, "urgency", urgency_str, sizeof(urgency_str));
        /* Optional -- see toast_show_osd()'s own doc comment on `tag`.
         * Lets a repeat caller (xiskeys holding a brightness key down,
         * say) update one popup instead of stacking a new one per call. */
        json_get_str(req, "tag", tag, sizeof(tag));
        int level = -1, timeout_ms = 0;
        json_get_int(req, "level", &level);
        json_get_int(req, "timeout_ms", &timeout_ms);

        ToastUrgency urgency = TOAST_URGENCY_NORMAL;
        if (!strcmp(urgency_str, "low")) {
            urgency = TOAST_URGENCY_LOW;
        } else if (!strcmp(urgency_str, "critical")) {
            urgency = TOAST_URGENCY_CRITICAL;
        }

        /* 40px matches toast.c's own TOAST_ICON. xispanel_first_panel_icon()
         * is the "no Panel* of my own" lookup -- see its own doc comment. */
        cairo_surface_t *icon = icon_name[0] ? xispanel_first_panel_icon(icon_name, 40) : NULL;
        toast_show_osd(icon, summary, body, level, urgency, timeout_ms, tag);
        snprintf(resp, resp_sz, "{\"ok\":true}\n");
    } else if (strcmp(cmd, "GET_NOTIFICATIONS") == 0) {
        /* xisserve's --notifications page (see xisserve/PROTOCOL.md): the
         * ring buffer lives here (notifd.c), not in xisserve's process, so
         * this is the one query that hands the whole history out over the
         * socket in one shot -- there's no paging, NOTIFD_MAX (50) is
         * small enough that it never needs one. Newest first, matching
         * the inline panel-menu history the notif widget's right click
         * already shows. */
        int n = notifd_count();
        size_t o = (size_t)snprintf(resp, resp_sz, "{\"ok\":true,\"count\":%d,\"notifications\":[", n);
        for (int i = n - 1; i >= 0 && o < resp_sz; i--) {
            const NotifEntry *e = notifd_get(i);
            if (!e) {
                continue;
            }
            char app_esc[NOTIFD_APP_NAME_MAX * 2], sum_esc[NOTIFD_SUMMARY_MAX * 2], body_esc[NOTIFD_BODY_MAX * 2];
            json_escape(e->app_name, app_esc, sizeof(app_esc));
            json_escape(e->summary, sum_esc, sizeof(sum_esc));
            json_escape(e->body, body_esc, sizeof(body_esc));
            o += (size_t)snprintf(resp + o, o < resp_sz ? resp_sz - o : 0,
                                   "%s{\"id\":%u,\"app_name\":\"%s\",\"summary\":\"%s\",\"body\":\"%s\","
                                   "\"received_ms\":%llu,\"read\":%s}",
                                   i == n - 1 ? "" : ",", e->id, app_esc, sum_esc, body_esc,
                                   (unsigned long long)e->received_ms, e->read ? "true" : "false");
        }
        if (o < resp_sz) {
            o += (size_t)snprintf(resp + o, resp_sz - o, "]}\n");
        }
    } else if (strcmp(cmd, "DELETE_NOTIFICATION") == 0) {
        int id = 0;
        json_get_int(req, "id", &id);
        notifd_remove((unsigned int)id);
        snprintf(resp, resp_sz, "{\"ok\":true}\n");
    } else if (strcmp(cmd, "CLEAR_NOTIFICATIONS") == 0) {
        notifd_clear();
        snprintf(resp, resp_sz, "{\"ok\":true}\n");
    } else if (strcmp(cmd, "QUIT") == 0) {
        g_quit = 1;
        snprintf(resp, resp_sz, "{\"ok\":true}\n");
    } else {
        snprintf(resp, resp_sz, "{\"ok\":false,\"error\":\"unknown command\"}\n");
    }
}

static int ipc_client_request(const char *sockpath, const char *req)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("xispanel: socket");
        return 1;
    }
    struct sockaddr_un addr;
    if (build_sockaddr_un(&addr, sockpath) != 0) {
        close(fd);
        return 1;
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "xispanel: could not connect to daemon (%s)\n", sockpath);
        close(fd);
        return 1;
    }
    if (write(fd, req, strlen(req)) < 0) {
        perror("xispanel: write");
    }
    shutdown(fd, SHUT_WR);

    char buf[IPC_MAX_LEN];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = 0;
        fputs(buf, stdout);
    }
    close(fd);
    return 0;
}

/* ------------------------------------------------------------------ */
/* main loop                                                            */
/* ------------------------------------------------------------------ */

static void handle_signal(int sig)
{
    (void)sig;
    g_quit = 1;
}

/* xispanel constantly queries properties/attributes of *other*
 * processes' windows (tasklist/winctl polling every open window's
 * title/icon/state, thumb.c's live captures, ...), any of which can
 * close between being listed and being queried -- Xlib's default error
 * handler calls exit() on any X protocol error, which would take down
 * the whole panel daemon over what's actually a routine, expected race,
 * not a bug. Every well-behaved WM/panel/taskbar installs a permissive
 * handler for exactly this reason; this just logs and continues. thumb.c
 * additionally swaps in its own temporary handler around composite calls
 * (to detect *its own* failures without logging noise for the common
 * "window isn't redirected" case), but always restores this one
 * afterward -- this is the actual default for the rest of the process. */
static int x_error_handler(Display *dpy, XErrorEvent *ev)
{
    char text[64];
    XGetErrorText(dpy, ev->error_code, text, sizeof(text));
    fprintf(stderr, "xispanel: ignoring X error: %s (request %d.%d, resource 0x%lx)\n", text, ev->request_code,
            ev->minor_code, ev->resourceid);
    return 0;
}

static Panel *find_panel_by_window(Window win, int *is_sensor)
{
    for (int i = 0; i < MAX_PANELS; i++) {
        if (!g_panels[i].in_use) {
            continue;
        }
        if (g_panels[i].win == win) {
            *is_sensor = 0;
            return &g_panels[i];
        }
        if (g_panels[i].sensor_win == win) {
            *is_sensor = 1;
            return &g_panels[i];
        }
    }
    return NULL;
}

/* density_handle_property() (density.c) needs the Panel a PropertyNotify's
 * window belongs to, which find_panel_by_window() above is only known to
 * this file -- 0 if the event isn't on any panel's own window (or is its
 * autohide sensor window, which never gets a density request). */
static int density_property_dispatch(const XPropertyEvent *ev)
{
    int is_sensor = 0;
    Panel *p = find_panel_by_window(ev->window, &is_sensor);
    if (!p || is_sensor) {
        return 0;
    }
    return density_handle_property(p, ev);
}

/* ------------------------------------------------------------------ */
/* container popups                                                     */
/* ------------------------------------------------------------------ */

Panel *panel_container_popup(PanelWidget *w)
{
    for (int i = 0; i < MAX_PANELS; i++) {
        if (g_panels[i].in_use && g_panels[i].mode == MODE_CONTAINER && g_panels[i].owner == w) {
            return &g_panels[i];
        }
    }
    return NULL;
}

/* Glues the popup to the owner panel's outer edge, centered on the owner
 * widget (plasmashell centers its system-tray popup on the icon the same
 * way), then clamps it onto the output. */
static void container_place(Panel *q)
{
    Panel *o = q->owner->panel;
    PanelWidget *ow = q->owner;
    int x, y;
    if (o->edge == EDGE_TOP || o->edge == EDGE_BOTTOM) {
        x = o->x + ow->x + (ow->len - q->w) / 2;
        y = (o->edge == EDGE_TOP) ? o->y + o->h : o->y - q->h;
    } else {
        y = o->y + ow->x + (ow->len - q->h) / 2;
        x = (o->edge == EDGE_LEFT) ? o->x + o->w : o->x - q->w;
    }
    if (x + q->w > o->out_x + o->out_w) {
        x = o->out_x + o->out_w - q->w;
    }
    if (y + q->h > o->out_y + o->out_h) {
        y = o->out_y + o->out_h - q->h;
    }
    if (x < o->out_x) {
        x = o->out_x;
    }
    if (y < o->out_y) {
        y = o->out_y;
    }
    q->x = x;
    q->y = y;
    XMoveWindow(g_dpy, q->win, x, y);
}

/* owner_events=True, unlike a menu's grab: events over any of our own
 * windows (the popup itself, every panel, a toast) are delivered to that
 * window normally, so the popup's widgets, the owner bar and everything
 * else keep working while it's open. Only a click that lands on nothing
 * of ours comes back reported against the popup window, with coordinates
 * outside it -- that's the "clicked outside, dismiss" signal (see
 * container_handle_event()). */
static void container_grab(Panel *q)
{
    XGrabPointer(g_dpy, q->win, True, ButtonPressMask, GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
    XGrabKeyboard(g_dpy, q->win, True, GrabModeAsync, GrabModeAsync, CurrentTime);
}

static void container_close(Panel *q)
{
    if (!q->open) {
        return;
    }
    q->open = 0;
    if (g_open_container == q) {
        g_open_container = NULL;
    }
    panel_menu_close(); /* a child widget's menu can't outlive the popup it hangs off of */
    tooltip_close();
    XUngrabKeyboard(g_dpy, CurrentTime);
    XUngrabPointer(g_dpy, CurrentTime);
    XUnmapWindow(g_dpy, q->win);
    q->mapped = 0;
    q->hover_widget = NULL;

    Panel *o = q->owner->panel;
    o->dirty = 1;
    /* The owner bar's hide timer was held off while the popup was up
     * (see panel_autohide_tick()); if the pointer isn't actually over the
     * bar now, start it. No LeaveNotify is coming to do that for us. */
    if (o->mode == MODE_AUTOHIDE && o->mapped) {
        Window rw, cw;
        int rx, ry, wx, wy;
        unsigned int mask;
        if (XQueryPointer(g_dpy, g_root, &rw, &cw, &rx, &ry, &wx, &wy, &mask) &&
            !(rx >= o->x && rx < o->x + o->w && ry >= o->y && ry < o->y + o->h)) {
            panel_autohide_leave(o);
        }
    }
    schedule_widget_repoll(now_ms()); /* notif re-reads panel_free_area() right away */
    XFlush(g_dpy);
}

static void container_open(Panel *q)
{
    if (!q->owner || q->win == None || q->open) {
        return;
    }
    if (g_open_container) {
        container_close(g_open_container);
    }
    panel_menu_close();
    tooltip_close();
    q->open = 1;
    g_open_container = q;
    panel_layout(q); /* sizes the window to its content */
    container_place(q);
    XMapWindow(g_dpy, q->win);
    XRaiseWindow(g_dpy, q->win);
    q->mapped = 1;
    q->dirty = 1;
    panel_repaint(q); /* paint before the first Expose can show a blank frame */
    container_grab(q);
    q->owner->panel->dirty = 1;
    schedule_widget_repoll(now_ms()); /* notif re-reads panel_free_area() right away */
}

void panel_container_toggle(Panel *popup)
{
    if (popup->open) {
        container_close(popup);
    } else {
        container_open(popup);
    }
}

void panel_container_close_all(void)
{
    if (g_open_container) {
        container_close(g_open_container);
    }
}

void panel_free_area(const Panel *p, int *out_x, int *out_y, int *out_w, int *out_h)
{
    int ox = p->out_x, oy = p->out_y, ow = p->out_w, oh = p->out_h;
    int strip[4] = {0}; /* indexed by enum edge */
    for (int i = 0; i < MAX_PANELS; i++) {
        const Panel *b = &g_panels[i];
        if (!b->in_use || b->mode != MODE_DOCK || b->out_x != ox || b->out_y != oy || b->out_w != ow ||
            b->out_h != oh) {
            continue;
        }
        int s = (b->edge == EDGE_TOP)      ? b->y + b->h - oy
                : (b->edge == EDGE_BOTTOM) ? oy + oh - b->y
                : (b->edge == EDGE_LEFT)   ? b->x + b->w - ox
                                           : ox + ow - b->x;
        if (s > strip[b->edge]) {
            strip[b->edge] = s;
        }
    }
    const Panel *q = g_open_container;
    if (q && q->owner) {
        const Panel *b = q->owner->panel;
        if (b->mode == MODE_DOCK && b->out_x == ox && b->out_y == oy && b->out_w == ow && b->out_h == oh) {
            int s = (b->edge == EDGE_TOP)      ? q->y + q->h - oy
                    : (b->edge == EDGE_BOTTOM) ? oy + oh - q->y
                    : (b->edge == EDGE_LEFT)   ? q->x + q->w - ox
                                               : ox + ow - q->x;
            if (s > strip[b->edge]) {
                strip[b->edge] = s;
            }
        }
    }
    *out_x = ox + strip[EDGE_LEFT];
    *out_y = oy + strip[EDGE_TOP];
    *out_w = ow - strip[EDGE_LEFT] - strip[EDGE_RIGHT];
    *out_h = oh - strip[EDGE_TOP] - strip[EDGE_BOTTOM];
    if (*out_w < 1) {
        *out_w = 1;
    }
    if (*out_h < 1) {
        *out_h = 1;
    }
}

void panel_container_menu_closed(void)
{
    if (g_open_container) {
        container_grab(g_open_container);
    }
}

/* Returns 1 if `ev` was consumed on behalf of the open popup: Escape
 * closes it; a click reported outside it (nothing of ours under the
 * pointer, see container_grab()) closes it; a click on any *other* panel
 * closes it too, and is swallowed when it landed on the owner icon
 * itself -- otherwise that icon's on_button() would just reopen what
 * the click meant to close. A click on another panel's other widgets
 * still goes through, so the bar stays usable with the popup up. */
static int container_handle_event(const XEvent *ev)
{
    Panel *q = g_open_container;
    if (!q) {
        return 0;
    }
    if (ev->type == KeyPress) {
        XKeyEvent key = ev->xkey;
        if (XLookupKeysym(&key, 0) == XK_Escape) {
            container_close(q);
            return 1;
        }
        return 0;
    }
    if (ev->type != ButtonPress) {
        return 0;
    }
    if (ev->xbutton.window == q->win) {
        int x = ev->xbutton.x, y = ev->xbutton.y;
        if (x < 0 || y < 0 || x >= q->w || y >= q->h) {
            container_close(q);
            return 1;
        }
        return 0;
    }
    int is_sensor = 0;
    Panel *p = find_panel_by_window(ev->xbutton.window, &is_sensor);
    if (!p || is_sensor) {
        return 0;
    }
    int horiz = (p->edge == EDGE_TOP || p->edge == EDGE_BOTTOM);
    int on_owner = panel_widget_at(p, horiz ? ev->xbutton.x : ev->xbutton.y, horiz ? ev->xbutton.y : ev->xbutton.x) ==
                   q->owner;
    container_close(q);
    return on_owner;
}

/* Resolves every mode=container PANEL to the `container` widget whose
 * name= names it. Runs once per (re)load, after every line has been read,
 * so the file order of the two lines doesn't matter. A container panel
 * nothing links to can't ever open -- rather than exist invisibly it
 * falls back to a plain overlay bar (so the mistake is at least visible
 * and fixable from the config), steered onto an edge of its output no
 * other bar already occupies when its own edge= is taken. */
static void link_containers(void)
{
    for (int i = 0; i < MAX_PANELS; i++) {
        Panel *q = &g_panels[i];
        if (!q->in_use || q->mode != MODE_CONTAINER) {
            continue;
        }
        q->owner = NULL;
        for (int j = 0; j < MAX_PANELS && !q->owner; j++) {
            Panel *p = &g_panels[j];
            if (!p->in_use || p->mode == MODE_CONTAINER) {
                continue;
            }
            for (int k = 0; k < p->n_widgets; k++) {
                PanelWidget *w = &p->widgets[k];
                char name[64];
                if (strcmp(w->ops->type_name, "container") == 0 && kv_get(w->config_kv, "name", name, sizeof(name)) &&
                    strcmp(name, q->name) == 0) {
                    q->owner = w;
                    break;
                }
            }
        }
        if (q->owner) {
            continue;
        }
        fprintf(stderr, "xispanel: container '%s': no container widget has name=%s, showing it as an overlay panel\n",
                q->name, q->name);
        q->mode = MODE_OVERLAY;
        int taken[4] = {0};
        for (int j = 0; j < MAX_PANELS; j++) {
            Panel *p = &g_panels[j];
            if (p->in_use && p != q && p->mode != MODE_CONTAINER && strcmp(p->output, q->output) == 0) {
                taken[p->edge] = 1;
            }
        }
        if (taken[q->edge]) {
            const enum edge order[4] = {EDGE_TOP, EDGE_BOTTOM, EDGE_LEFT, EDGE_RIGHT};
            for (int e = 0; e < 4; e++) {
                if (!taken[order[e]]) {
                    q->edge = order[e];
                    break;
                }
            }
        }
    }
}

/* Moves each container-popup widget between the popup and its owner bar
 * as its inline= asks: inline=yes lives on the bar permanently,
 * inline=urgent only while its is_urgent() says so. Both panels relayout
 * on a change; the popup resizes with it next time it's laid out. Runs
 * once per main-loop pass (after the widgets' own ticks, so is_urgent()
 * sees fresh state). */
static void containers_update_inline(void)
{
    for (int i = 0; i < MAX_PANELS; i++) {
        Panel *q = &g_panels[i];
        if (!q->in_use || q->mode != MODE_CONTAINER || !q->owner) {
            continue;
        }
        Panel *bar = q->owner->panel;
        for (int j = 0; j < q->n_widgets; j++) {
            PanelWidget *w = &q->widgets[j];
            int want = w->inline_mode == INLINE_ALWAYS ||
                       (w->inline_mode == INLINE_URGENT && w->ops->is_urgent && w->ops->is_urgent(w));
            if (want == w->inlined) {
                continue;
            }
            tooltip_close(); /* may be describing this very widget where it no longer is */
            w->inlined = want;
            w->panel = want ? bar : q;
            bar->dirty = 1;
            q->dirty = 1;
            if (bar->hover_widget == w || q->hover_widget == w) {
                bar->hover_widget = NULL;
                q->hover_widget = NULL;
            }
        }
    }
}

static void dispatch_button(Panel *p, int button, int x, int y, int root_x, int root_y)
{
    int axis_pos = (p->edge == EDGE_TOP || p->edge == EDGE_BOTTOM) ? x : y;
    int cross_pos = (p->edge == EDGE_TOP || p->edge == EDGE_BOTTOM) ? y : x;
    PanelWidget *w = panel_widget_at(p, axis_pos, cross_pos);
    if (w && w->ops->on_button) {
        w->ops->on_button(w, button, axis_pos - w->x, cross_pos - w->y, root_x, root_y);
    }
}

/* Updates p's generic hover-highlight state (see panel_widget_hover_local_x()/
 * panel_widget_hover_local_y() in xispanel.h) from a MotionNotify's axis/
 * cross-axis position -- same widget lookup dispatch_button() uses for
 * clicks. Only marks the panel dirty when the hovered widget or its
 * local_x/local_y actually changed, so a stream of MotionNotify events
 * over the *same* spot (X can resend these) isn't a repaint each time. */
static void panel_update_hover(Panel *p, int axis_pos, int cross_pos)
{
    PanelWidget *hit = panel_widget_at(p, axis_pos, cross_pos);
    int local_x = hit ? axis_pos - hit->x : 0;
    int local_y = hit ? cross_pos - hit->y : 0;
    if (hit != p->hover_widget || (hit && (local_x != p->hover_local_x || local_y != p->hover_local_y))) {
        p->hover_widget = hit;
        p->hover_local_x = local_x;
        p->hover_local_y = local_y;
        p->dirty = 1;
    }
}

/* Clears p's hover-highlight state (pointer left the panel entirely) --
 * called from LeaveNotify, alongside tooltip_notice_leave(). */
static void panel_clear_hover(Panel *p)
{
    if (p->hover_widget) {
        p->hover_widget = NULL;
        p->dirty = 1;
    }
}

/* How long to coalesce a stream of ConfigureNotify (window move/resize)
 * events before re-polling the widgets -- a drag emits one per pointer
 * motion, and only the window's *resting* output actually matters, so
 * rebuilding the task list on every motion would be pure waste. See the
 * ConfigureNotify handling in the main loop. */
#define WATCH_GEOMETRY_DEBOUNCE_MS 200

/* Lowers every ticking widget's next tick to at_ms (never raises it), so
 * the existing tick loop re-polls them at that time and each reports
 * whether anything it draws actually changed. Turns a WM property or
 * geometry change into an immediate (at_ms = now) or debounced (at_ms =
 * now + WATCH_GEOMETRY_DEBOUNCE_MS) re-poll without any extra timer, since
 * next_tick_ms already feeds the main loop's soonest-wake computation. */
static void schedule_widget_repoll(uint64_t at_ms)
{
    for (int i = 0; i < MAX_PANELS; i++) {
        if (!g_panels[i].in_use) {
            continue;
        }
        for (int j = 0; j < g_panels[i].n_widgets; j++) {
            PanelWidget *w = &g_panels[i].widgets[j];
            if (w->ops->on_tick && w->next_tick_ms != 0 && w->next_tick_ms > at_ms) {
                w->next_tick_ms = at_ms;
            }
        }
    }
}

/* 1 if envvar `name` is set to anything other than "" or "0" -- used
 * below by a handful of XISPANEL_DISABLE_* debug toggles, one per
 * subsystem that keeps running/polling in the background regardless of
 * which widgets are configured (some, notably ewmh_watch_init()'s
 * property/geometry watching, run even with zero panels at all). Meant
 * for bisecting a CPU-usage report by process of elimination (run with
 * one disabled at a time, see which one it was), not for end-user
 * configuration -- there's no config-file equivalent on purpose. */
static int env_flag(const char *name)
{
    const char *v = getenv(name);
    return v && v[0] && strcmp(v, "0") != 0;
}

static int run_as_daemon(const char *sockpath)
{
    int disable_ewmh_watch = env_flag("XISPANEL_DISABLE_EWMH_WATCH");
    int disable_modtap = env_flag("XISPANEL_DISABLE_MODTAP");
    int disable_sni = env_flag("XISPANEL_DISABLE_SNI");
    int disable_mpris = env_flag("XISPANEL_DISABLE_MPRIS");
    int disable_notifd = env_flag("XISPANEL_DISABLE_NOTIFD");
    if (disable_ewmh_watch || disable_modtap || disable_sni || disable_mpris || disable_notifd) {
        fprintf(stderr,
                "xispanel: debug subsystem disable flags active:%s%s%s%s%s\n",
                disable_ewmh_watch ? " EWMH_WATCH" : "", disable_modtap ? " MODTAP" : "",
                disable_sni ? " SNI" : "", disable_mpris ? " MPRIS" : "", disable_notifd ? " NOTIFD" : "");
    }

    g_dpy = XOpenDisplay(NULL);
    if (!g_dpy) {
        fprintf(stderr, "xispanel: could not open the X display\n");
        return 1;
    }
    XSetErrorHandler(x_error_handler);
    g_screen = DefaultScreen(g_dpy);
    g_root = RootWindow(g_dpy, g_screen);

    imlib_context_set_display(g_dpy);
    imlib_context_set_visual(DefaultVisual(g_dpy, g_screen));
    imlib_context_set_colormap(DefaultColormap(g_dpy, g_screen));
    imlib_context_set_anti_alias(1);
    imlib_context_set_dither(1);

    ewmh_init_atoms();
    density_init();     /* X-DENSITY (see TESTS/X-DENSITY.md) -- must come after g_screen/g_root are set above */
    inputscale_init();  /* X-INPUT-SCALE (see inputscale.c) -- same ordering requirement */
    if (!disable_modtap) {
        modtap_init(); /* bare-modifier ("tap Meta alone") hotkeys, see hotkey.c/modtap.c */
    }
    toast_init(); /* wires notifd.c's arrived callback to the toast popups, see toast.c */
    g_atom_net_wm_state = XInternAtom(g_dpy, "_NET_WM_STATE", False);
    g_atom_net_wm_state_skip_taskbar = XInternAtom(g_dpy, "_NET_WM_STATE_SKIP_TASKBAR", False);
    g_atom_net_wm_state_skip_pager = XInternAtom(g_dpy, "_NET_WM_STATE_SKIP_PAGER", False);
    g_atom_net_wm_desktop = XInternAtom(g_dpy, "_NET_WM_DESKTOP", False);

    int rr_error_base;
    if (!XRRQueryExtension(g_dpy, &g_rr_event_base, &rr_error_base)) {
        fprintf(stderr, "xispanel: RandR extension unavailable, named outputs won't work\n");
        g_rr_event_base = -1;
    } else {
        XRRSelectInput(g_dpy, g_root, RRScreenChangeNotifyMask);
    }

    unlink(sockpath);
    int listenfd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr;
    if (build_sockaddr_un(&addr, sockpath) != 0) {
        return 1;
    }
    if (bind(listenfd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(listenfd, 16) != 0) {
        perror("xispanel: bind/listen");
        return 1;
    }
    chmod(sockpath, 0600);

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGPIPE, SIG_IGN);
    /* run_detached()'s children (launcher widget clicks) are never
     * waitpid()'d -- ignoring SIGCHLD makes the kernel reap them itself
     * instead of leaving zombies, same pattern xisback uses. */
    signal(SIGCHLD, SIG_IGN);
    fcntl(listenfd, F_SETFD, FD_CLOEXEC);
    fcntl(ConnectionNumber(g_dpy), F_SETFD, FD_CLOEXEC);

    write_default_config_if_missing();
    /* Font/icon theme come out of the config file itself, so this has to
     * follow write_default_config_if_missing() (there may not have been a
     * file to scan before it) and precede reload_all_panels() (widgets
     * measure their text at creation time). */
    config_scan_globals();
    if (init_font(g_font_family) != 0) {
        fprintf(stderr, "xispanel: could not resolve a default font via fontconfig\n");
    } else if (g_font_family[0]) {
        fprintf(stderr, "xispanel: using configured font '%s'\n", g_font_family);
    }
    pango_text_init(g_font_family);

    /* One forced poll before the very first resolve_output_geometry()
     * call (inside reload_all_panels() below): the X server's own RandR
     * cache can still be missing/stale EDID data this early in a fresh
     * session, with no CRTC/topology change ever generated to say so --
     * see xis_list_outputs()'s own doc comment on `forced`, and
     * resolve_output_geometry()'s comment on why its own per-call
     * lookup deliberately stays uncached-forcing. One forced read here
     * is enough for the rest of the session -- it updates the
     * server-wide cache (not per-client), and this program's own
     * RandR-change-event reaction (see XRRSelectInput() above) keeps it
     * current after that. */
    {
        XisOutput warm[XIS_MAX_OUTPUTS];
        xis_list_outputs(g_dpy, warm, XIS_MAX_OUTPUTS, 1);
    }

    reload_all_panels();
    /* Watch the root + every client window for the properties the polling
     * widgets (tasklist/winctl/globalmenu) care about, so they re-poll the
     * instant one changes rather than only on their slow fallback tick --
     * see ewmh_watch_init() and the PropertyNotify handling below. */
    if (!disable_ewmh_watch) {
        ewmh_watch_init();
    }
    XFlush(g_dpy);

    int xfd = ConnectionNumber(g_dpy);
    int modtapfd = modtap_fd();
    int xisfd = inputscale_fd();
    while (!g_quit) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(listenfd, &rfds);
        FD_SET(xfd, &rfds);
        int maxfd = listenfd > xfd ? listenfd : xfd;
        if (modtapfd >= 0) {
            FD_SET(modtapfd, &rfds);
            if (modtapfd > maxfd) {
                maxfd = modtapfd;
            }
        }
        if (xisfd >= 0) {
            FD_SET(xisfd, &rfds);
            if (xisfd > maxfd) {
                maxfd = xisfd;
            }
        }
        /* Re-read each iteration, unlike the fds above: the tray's bus
         * connection is established lazily inside the first sni_poll(),
         * so this is -1 for the first few passes and valid afterwards. */
        int snifd = disable_sni ? -1 : sni_fd();
        if (snifd >= 0) {
            FD_SET(snifd, &rfds);
            if (snifd > maxfd) {
                maxfd = snifd;
            }
        }
        /* Same "re-read every iteration" reasoning as snifd above: -1
         * until pactl subscribe is actually running, which only happens
         * lazily (and gets re-tried on its own backoff after a crash --
         * see audio_events_fd()). */
        int audiofd = audio_events_fd();
        if (audiofd >= 0) {
            FD_SET(audiofd, &rfds);
            if (audiofd > maxfd) {
                maxfd = audiofd;
            }
        }

        uint64_t now = now_ms();
        long timeout_ms = -1;
        int any_animating = 0;
        uint64_t soonest_tick = 0;

        for (int i = 0; i < MAX_PANELS; i++) {
            Panel *p = &g_panels[i];
            if (!p->in_use) {
                continue;
            }
            if (p->mode == MODE_AUTOHIDE && (p->ah_state == AH_SHOWING || p->ah_state == AH_HIDING || p->ah_hide_deadline_ms)) {
                any_animating = 1;
            }
            for (int j = 0; j < p->n_widgets; j++) {
                uint64_t t = p->widgets[j].next_tick_ms;
                if (t != 0 && (soonest_tick == 0 || t < soonest_tick)) {
                    soonest_tick = t;
                }
            }
        }
        if (any_animating) {
            timeout_ms = 33;
        }
        if (soonest_tick != 0) {
            long delta = (long)(soonest_tick > now ? soonest_tick - now : 0);
            if (timeout_ms < 0 || delta < timeout_ms) {
                timeout_ms = delta;
            }
        }
        uint64_t tooltip_wake = tooltip_next_wake_ms();
        if (tooltip_wake != 0) {
            long delta = (long)(tooltip_wake > now ? tooltip_wake - now : 0);
            if (timeout_ms < 0 || delta < timeout_ms) {
                timeout_ms = delta;
            }
        }
        uint64_t menu_wake = panel_menu_next_wake_ms();
        if (menu_wake != 0) {
            long delta = (long)(menu_wake > now ? menu_wake - now : 0);
            if (timeout_ms < 0 || delta < timeout_ms) {
                timeout_ms = delta;
            }
        }
        uint64_t toast_wake = toast_next_wake_ms();
        if (toast_wake != 0) {
            long delta = (long)(toast_wake > now ? toast_wake - now : 0);
            if (timeout_ms < 0 || delta < timeout_ms) {
                timeout_ms = delta;
            }
        }
        uint64_t launchfx_wake = launchfx_next_wake_ms();
        if (launchfx_wake != 0) {
            long delta = (long)(launchfx_wake > now ? launchfx_wake - now : 0);
            if (timeout_ms < 0 || delta < timeout_ms) {
                timeout_ms = delta;
            }
        }

        /* mpris_poll()/sni_poll()/notifd_poll() are only actually called
         * once per select() wake, on whatever cadence *this* loop wakes
         * up at -- they don't get their own timer the way widget ticks
         * do. If nothing above scheduled a wake (no widget with its own
         * on_tick, e.g. a panel of just spacer/launcher/folder widgets),
         * timeout_ms is still -1 here and select() would block
         * indefinitely, silently starving those DBus pollers until some
         * unrelated X/IPC event happens to nudge the loop -- confirmed
         * live: a real Notify() call timed out completely against a
         * spacer-only panel until this fallback was added. 1000ms is
         * looser than any of their own poll intervals, so it never
         * tightens the common case (some widget's own tick already
         * bounds the wake tighter than this whenever one exists). */
        if (timeout_ms < 0) {
            timeout_ms = 1000;
        }

        struct timeval tv;
        struct timeval *tvp = NULL;
        if (timeout_ms >= 0) {
            tv.tv_sec = timeout_ms / 1000;
            tv.tv_usec = (timeout_ms % 1000) * 1000;
            tvp = &tv;
        }

        int r = select(maxfd + 1, &rfds, NULL, NULL, tvp);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        if (r > 0 && FD_ISSET(listenfd, &rfds)) {
            int cfd = accept(listenfd, NULL, NULL);
            if (cfd >= 0) {
                struct timeval tvto = {5, 0};
                setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tvto, sizeof(tvto));
                setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &tvto, sizeof(tvto));
                char reqbuf[IPC_MAX_LEN];
                ssize_t n = read(cfd, reqbuf, sizeof(reqbuf) - 1);
                if (n > 0) {
                    reqbuf[n] = 0;
                    char resp[IPC_MAX_LEN];
                    handle_ipc_message(reqbuf, resp, sizeof(resp));
                    /* GET_NOTIFICATIONS can fill tens of KB -- a single
                     * write() to a stream socket is allowed to send less
                     * than asked, so this has to loop until it's all out
                     * (or the client's gone) rather than assuming one call
                     * covers it, unlike every other command's tiny reply. */
                    size_t resp_len = strlen(resp);
                    size_t sent = 0;
                    while (sent < resp_len) {
                        ssize_t w = write(cfd, resp + sent, resp_len - sent);
                        if (w < 0) {
                            if (errno == EINTR) {
                                continue;
                            }
                            perror("xispanel: write");
                            break;
                        }
                        sent += (size_t)w;
                    }
                }
                close(cfd);
            }
        }

        if (r > 0 && FD_ISSET(xfd, &rfds)) {
            while (XPending(g_dpy)) {
                XEvent ev;
                XNextEvent(g_dpy, &ev);
                if (g_rr_event_base >= 0 && ev.type == g_rr_event_base + RRScreenChangeNotify) {
                    XRRUpdateConfiguration(&ev);
                    reload_all_panels();
                    XFlush(g_dpy);
                } else if (density_handle_xfixes_event(&ev)) {
                    /* the _X_DENSITY_MANAGER_S<screen> selection appeared/
                     * disappeared -- see density.c/TESTS/X-DENSITY.md */
                } else if (hotkey_handle_event(&ev)) {
                    /* consumed by a registered global hotkey -- see hotkey.c */
                } else if (thumb_handle_event(&ev)) {
                    /* an XDamage notification for a watched thumbnail window --
                     * see thumb.c/tooltip.c's tooltip_tick() */
                } else if (panel_menu_handle_event(&ev)) {
                    /* consumed by the open context menu */
                } else if (container_handle_event(&ev)) {
                    /* Escape / click-outside / owner-icon click closed the
                     * open container popup -- see container_handle_event() */
                } else if (tooltip_handle_event(&ev)) {
                    /* consumed by the tooltip popup (just Expose -- it
                     * takes no grab and never handles clicks) */
                } else if (toast_handle_event(&ev)) {
                    /* consumed by a toast popup (click-to-dismiss, Expose) */
                } else if (launchfx_handle_event(&ev)) {
                    /* consumed by the launch-feedback zoom+fade popup (Expose only) */
                } else if (ev.type == ButtonPress) {
                    int is_sensor = 0;
                    Panel *p = find_panel_by_window(ev.xbutton.window, &is_sensor);
                    if (p && !is_sensor) {
                        tooltip_close();
                        dispatch_button(p, (int)ev.xbutton.button, ev.xbutton.x, ev.xbutton.y, ev.xbutton.x_root, ev.xbutton.y_root);
                    }
                } else if (ev.type == MotionNotify) {
                    int is_sensor = 0;
                    Panel *p = find_panel_by_window(ev.xmotion.window, &is_sensor);
                    if (p && !is_sensor) {
                        int axis_pos = (p->edge == EDGE_TOP || p->edge == EDGE_BOTTOM) ? ev.xmotion.x : ev.xmotion.y;
                        int cross_pos = (p->edge == EDGE_TOP || p->edge == EDGE_BOTTOM) ? ev.xmotion.y : ev.xmotion.x;
                        tooltip_notice_motion(p, axis_pos, cross_pos);
                        panel_update_hover(p, axis_pos, cross_pos);
                    }
                } else if (ev.type == EnterNotify) {
                    int is_sensor = 0;
                    Panel *p = find_panel_by_window(ev.xcrossing.window, &is_sensor);
                    if (p) {
                        panel_autohide_enter(p);
                    }
                    /* Also (re)establish the hover state from the crossing
                     * event's own coordinates, instead of waiting for the
                     * next MotionNotify: after the implicit grab of a
                     * click ends, X delivers an EnterNotify(NotifyUngrab)
                     * and *no* motion event if the pointer hasn't moved,
                     * so anything drawn only while hovered (winctl's
                     * collapse_buttons=) would otherwise stay hidden until
                     * the pointer was jiggled. */
                    if (p && !is_sensor) {
                        int horiz = (p->edge == EDGE_TOP || p->edge == EDGE_BOTTOM);
                        panel_update_hover(p, horiz ? ev.xcrossing.x : ev.xcrossing.y,
                                           horiz ? ev.xcrossing.y : ev.xcrossing.x);
                    }
                } else if (ev.type == LeaveNotify) {
                    int is_sensor = 0;
                    Panel *p = find_panel_by_window(ev.xcrossing.window, &is_sensor);
                    /* NotifyGrab/NotifyUngrab crossings are the pointer
                     * grab a click implicitly takes and releases, not the
                     * pointer actually leaving the panel -- treating them
                     * as a real leave dropped the hover state on every
                     * click. */
                    if (p && !is_sensor && ev.xcrossing.mode == NotifyNormal) {
                        panel_autohide_leave(p);
                        tooltip_notice_leave(p);
                        panel_clear_hover(p);
                    }
                } else if (ev.type == Expose) {
                    int is_sensor = 0;
                    Panel *p = find_panel_by_window(ev.xexpose.window, &is_sensor);
                    if (p && !is_sensor) {
                        p->dirty = 1;
                    }
                } else if (ev.type == PropertyNotify && density_property_dispatch(&ev.xproperty)) {
                    /* consumed: _X_DENSITY_REQUESTED on one of our own panel
                     * windows -- see density.c/TESTS/X-DENSITY.md */
                } else if (ev.type == PropertyNotify) {
                    /* A WM property the polling widgets depend on changed
                     * (active window, client list, current desktop, or a
                     * window's title/max/min state/desktop) -- re-poll every
                     * ticking widget this iteration instead of waiting out
                     * its fallback interval. Each on_tick still returns
                     * "changed?" so a spurious event that doesn't actually
                     * alter anything drawn costs a cheap re-read, not a
                     * repaint. See ewmh_watch_init(). */
                    int client_list_changed = 0;
                    if (ewmh_property_event_is_relevant(&ev.xproperty, &client_list_changed)) {
                        if (client_list_changed) {
                            ewmh_watch_windows(); /* start watching any newly-mapped windows */
                        }
                        schedule_widget_repoll(now_ms());
                    }
                } else if (ev.type == ConfigureNotify) {
                    /* A top-level window moved/resized (SubstructureNotify on
                     * root) -- the only signal for a window changing output,
                     * which has no property to watch. Debounced: a drag emits
                     * a continuous stream, and only the resting position's
                     * output matters, so coalesce into at most one re-poll
                     * per WATCH_GEOMETRY_DEBOUNCE_MS. */
                    schedule_widget_repoll(now_ms() + WATCH_GEOMETRY_DEBOUNCE_MS);
                }
            }
        }

        if (modtapfd >= 0 && r > 0 && FD_ISSET(modtapfd, &rfds)) {
            modtap_process();
        }

        if (xisfd >= 0 && r > 0 && FD_ISSET(xisfd, &rfds) && inputscale_poll_change()) {
            /* a CRTC's X-INPUT-SCALE confine box was set/reset -- react
             * immediately instead of waiting for
             * poll_output_geometry_changes()'s next tick. */
            check_output_geometry_changed();
        }

        now = now_ms();
        poll_output_geometry_changes(now);
        tooltip_tick(now);
        panel_menu_tick(now);
        toast_tick(now);
        launchfx_tick(now);
        if (!disable_mpris) {
            mpris_poll(now);
        }
        /* mpris feeds tooltips only (no widget paints it), so it doesn't
         * mark panels dirty. sni feeds the tray widget's paint, so a
         * change there does -- and since any panel could host a tray
         * widget, mark them all (tray changes are rare, so the occasional
         * repaint of a tray-less panel is cheaper than tracking which
         * panel owns the tray). notifd's badge latency is covered by the
         * notif widget's own on_tick polling unread count. */
        if (snifd >= 0 && r > 0 && FD_ISSET(snifd, &rfds)) {
            sni_wake();
        }
        if (audiofd >= 0 && r > 0 && FD_ISSET(audiofd, &rfds)) {
            audio_events_poll();
        }
        int tray_changed = disable_sni ? 0 : sni_poll(now);
        if (!disable_notifd) {
            notifd_poll(now);
        }
        storage_events_poll(now); /* rate-limits itself internally, see its own doc comment */
        for (int i = 0; i < MAX_PANELS; i++) {
            Panel *p = &g_panels[i];
            if (!p->in_use) {
                continue;
            }
            if (tray_changed) {
                p->dirty = 1;
            }
            panel_autohide_tick(p, now);
            for (int j = 0; j < p->n_widgets; j++) {
                PanelWidget *w = &p->widgets[j];
                if (w->next_tick_ms != 0 && now >= w->next_tick_ms && w->ops->on_tick) {
                    if (w->ops->on_tick(w, now)) {
                        w->panel->dirty = 1; /* the bar it's inlined on, if it is */
                    }
                }
            }
        }
        containers_update_inline();
        for (int i = 0; i < MAX_PANELS; i++) {
            Panel *p = &g_panels[i];
            if (p->in_use && p->dirty && p->mapped) {
                panel_layout(p);
                panel_repaint(p);
            }
        }
        /* Autohide's XMoveWindow/XMapWindow/XUnmapWindow calls above (and
         * panel_autohide_enter()'s, from the event-handling block earlier
         * in this iteration) are buffered by Xlib until something flushes
         * them. Xlib only auto-flushes from XPending()/XNextEvent(), which
         * we only call when the X fd is already readable -- without this,
         * a pure animation/timeout-driven tick (no incoming X event) could
         * sit in the client buffer indefinitely, waiting for unrelated
         * server traffic to nudge it out. */
        XFlush(g_dpy);
    }

    panel_container_close_all();
    panel_menu_close();
    tooltip_close();
    for (int i = 0; i < MAX_PANELS; i++) {
        if (g_panels[i].in_use) {
            panel_deactivate(&g_panels[i]);
        }
    }
    close(listenfd);
    unlink(sockpath);
    if (g_font_face) {
        cairo_font_face_destroy(g_font_face);
    }
    if (g_ft_face) {
        FT_Done_Face(g_ft_face);
    }
    if (g_ft_lib) {
        FT_Done_FreeType(g_ft_lib);
    }
    XCloseDisplay(g_dpy);
    return 0;
}

/* ------------------------------------------------------------------ */
/* entry point                                                          */
/* ------------------------------------------------------------------ */

static void usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s [options]\n"
            "\n"
            "  --reload    tell the running daemon to reload its config\n"
            "  --quit      stop the running daemon\n"
            "  --version   print version and exit\n"
            "\n"
            "With no options, runs as the daemon (or does nothing but report\n"
            "'already running' if one is active). Panels/widgets/theme are\n"
            "configured in $XDG_CONFIG_HOME/xispanel.conf -- see PROTOCOL.md.\n",
            prog);
}

int main(int argc, char **argv)
{
    /* LC_TIME affects strftime()'s %A/%B (full weekday/month names) --
     * clock's short "%H:%M" panel format is locale-independent, but its
     * tooltip's full "weekday, day de month de year" isn't. */
    setlocale(LC_TIME, "");

    const char *rundir = getenv("XDG_RUNTIME_DIR");
    if (!rundir || !*rundir) {
        rundir = "/tmp";
    }
    char sockpath[PATH_MAX];
    char lockpath[PATH_MAX];
    snprintf(sockpath, sizeof(sockpath), "%s/xispanel-ctl.sock", rundir);
    snprintf(lockpath, sizeof(lockpath), "%s/xispanel.lock", rundir);

    const char *xdg_config = getenv("XDG_CONFIG_HOME");
    if (xdg_config && *xdg_config) {
        mkdir(xdg_config, 0700);
        snprintf(g_configpath, sizeof(g_configpath), "%s/xispanel.conf", xdg_config);
    } else {
        const char *home = getenv("HOME");
        if (!home || !*home) {
            home = "/tmp";
        }
        char configdir[PATH_MAX];
        snprintf(configdir, sizeof(configdir), "%s/.config", home);
        mkdir(configdir, 0700);
        snprintf(g_configpath, sizeof(g_configpath), "%s/xispanel.conf", configdir);
    }

    if (argc > 1) {
        if (!strcmp(argv[1], "--version")) {
            printf("xispanel %s\n", XISPANEL_VERSION);
            return 0;
        }
        if (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
            usage(argv[0]);
            return 0;
        }
        if (!strcmp(argv[1], "--quit")) {
            return ipc_client_request(sockpath, "{\"cmd\":\"QUIT\"}\n");
        }
        if (!strcmp(argv[1], "--reload")) {
            return ipc_client_request(sockpath, "{\"cmd\":\"RELOAD\"}\n");
        }
        fprintf(stderr, "xispanel: unknown option '%s'\n", argv[1]);
        usage(argv[0]);
        return 1;
    }

    /* O_CLOEXEC matters here specifically: without it, this fd leaks into
     * every child run_detached() forks (launcher/tasklist/xisserve clicks)
     * and, via exec(), into whatever program they launch -- and since an
     * flock() is held by the open file description, not by a process, a
     * launched GUI app that never closes an inherited fd (most don't
     * bother) keeps this lock held even after xispanel itself has been
     * killed, making a relaunch report "already running" until every
     * program it ever launched also exits. */
    int lockfd = open(lockpath, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (lockfd < 0) {
        perror("xispanel: open lock");
        return 1;
    }
    if (flock(lockfd, LOCK_EX | LOCK_NB) != 0) {
        close(lockfd);
        fprintf(stderr, "xispanel: already running\n");
        return 1;
    }

    return run_as_daemon(sockpath);
}
