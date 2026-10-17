/*
 * launcher widget - a single generic clickable icon: the user supplies an
 * image (icon=<path>, any format Imlib2 can decode), a name (used for the
 * hover tooltip, and as the fallback single-letter icon if icon= is
 * missing/fails to load), and a shell command run detached on click
 * (cmd=<command>, via run_detached() -- same "shell out, don't reimplement
 * a launcher" philosophy xisconf/xisback already use for their own click
 * actions). This is deliberately not a full application launcher (no
 * .desktop parsing, no icon-theme resolution, no search) -- multiple
 * `launcher` widgets on one panel is how the user pins individual
 * shortcuts today; `.desktop`-based pinning is future launcher-phase
 * territory (see tasklist's "pin" TODO).
 *
 * `cmd=` covers left-click; `cmd_middle=`/`cmd_right=`/`cmd_scroll_up=`/
 * `cmd_scroll_down=` are all optional extra actions on the same icon for
 * the other buttons and the scroll wheel (X11 reports wheel motion as
 * ButtonPress with button 4/5, so it falls through the exact same
 * dispatch as a click -- no separate scroll handling needed). This turns
 * a single `launcher` into a small multi-action control -- e.g. a volume
 * icon where scroll adjusts it and a click opens a mixer -- without
 * needing a dedicated widget type for that shape of interaction. Any
 * action left unset is simply a no-op for that button/direction.
 *
 * Optionally the icon, a short text beside it and the tooltip come from
 * commands too, so a script can make the widget a live status indicator
 * (a sync state, a VPN, a mail count) with no widget code at all:
 *
 *   cmd_icon=<cmd>     first stdout line: an icon theme name or a path;
 *                      empty output keeps the static icon=
 *   cmd_text=<cmd>     first stdout line, drawn right of the icon (or
 *                      alone when there is no icon); horizontal panels only
 *   cmd_tooltip=<cmd>  whole stdout, shown as the tooltip; empty keeps name=
 *   interval=<ms>      how often they re-run (default 5000, min 500)
 *
 * They run through asyncmd.c, never blocking the panel; commands that are
 * the same string share one child. Only the keys actually set cost
 * anything: with none of them the widget never ticks. */
#include "../xispanel.h"

#include <X11/Xlib.h>

#include <stdio.h>
#include <string.h>

typedef struct {
    char name[64];
    char cmd[256];         /* left click */
    char cmd_middle[256];  /* middle click */
    char cmd_right[256];   /* right click */
    char cmd_scroll_up[256];
    char cmd_scroll_down[256];
    cairo_surface_t *icon; /* NULL if icon= wasn't set or failed to load */

    /* Command-driven state, see the file comment. */
    char cmd_icon[256];
    char cmd_text[256];
    char cmd_tooltip[256];
    int interval_ms;
    uint64_t gen_icon, gen_text, gen_tooltip; /* asyncmd generation last parsed */
    char dyn_icon_spec[256];                  /* what dyn_icon was resolved from */
    cairo_surface_t *dyn_icon;                /* cmd_icon's icon; NULL = use `icon` */
    char text[128];
    char tooltip[512];
} LauncherPriv;

#define LAUNCHER_DEFAULT_INTERVAL_MS 5000
#define LAUNCHER_MIN_INTERVAL_MS 500

static int launcher_icon_px(const PanelWidget *w)
{
    int thickness = w->thickness > 0 ? w->thickness : w->panel->thickness_cfg;
    return thickness > 6 ? thickness - 6 : 16;
}

/* Text sits beside the icon, so it only makes sense along a horizontal
 * bar -- on a vertical one the widget stays an icon. */
static int launcher_shows_text(const PanelWidget *w)
{
    const LauncherPriv *lp = w->priv;
    return lp->text[0] && w->panel->edge != EDGE_LEFT && w->panel->edge != EDGE_RIGHT;
}

/* No icon of any kind and a text to show instead: draw the text alone
 * rather than a placeholder letter next to it. */
static int launcher_shows_icon(const PanelWidget *w)
{
    const LauncherPriv *lp = w->priv;
    return lp->dyn_icon || lp->icon || !launcher_shows_text(w);
}

/* `text` up to its first newline, trailing whitespace dropped. */
static void launcher_first_line(const char *text, char *out, size_t outsz)
{
    size_t n = strcspn(text, "\n");
    while (n > 0 && (text[n - 1] == ' ' || text[n - 1] == '\t' || text[n - 1] == '\r')) {
        n--;
    }
    if (n >= outsz) {
        n = outsz - 1;
    }
    memcpy(out, text, n);
    out[n] = 0;
    trim_to_utf8_boundary(out);
}

static int launcher_init(PanelWidget *w)
{
    LauncherPriv *lp = w->priv;
    kv_get(w->config_kv, "name", lp->name, sizeof(lp->name));
    kv_get(w->config_kv, "cmd", lp->cmd, sizeof(lp->cmd));
    kv_get(w->config_kv, "cmd_middle", lp->cmd_middle, sizeof(lp->cmd_middle));
    kv_get(w->config_kv, "cmd_right", lp->cmd_right, sizeof(lp->cmd_right));
    kv_get(w->config_kv, "cmd_scroll_up", lp->cmd_scroll_up, sizeof(lp->cmd_scroll_up));
    kv_get(w->config_kv, "cmd_scroll_down", lp->cmd_scroll_down, sizeof(lp->cmd_scroll_down));
    kv_get(w->config_kv, "cmd_icon", lp->cmd_icon, sizeof(lp->cmd_icon));
    kv_get(w->config_kv, "cmd_text", lp->cmd_text, sizeof(lp->cmd_text));
    kv_get(w->config_kv, "cmd_tooltip", lp->cmd_tooltip, sizeof(lp->cmd_tooltip));
    lp->interval_ms = kv_get_int(w->config_kv, "interval", LAUNCHER_DEFAULT_INTERVAL_MS);
    if (lp->interval_ms < LAUNCHER_MIN_INTERVAL_MS) {
        lp->interval_ms = LAUNCHER_MIN_INTERVAL_MS;
    }
    if (lp->cmd_icon[0] || lp->cmd_text[0] || lp->cmd_tooltip[0]) {
        w->next_tick_ms = now_ms();
    }

    char icon_path[PATH_MAX];
    if (kv_get(w->config_kv, "icon", icon_path, sizeof(icon_path)) && icon_path[0]) {
        /* w->thickness isn't resolved yet this early -- see the same fix
         * in folder_init(). */
        int thickness = w->thickness > 0 ? w->thickness : w->panel->thickness_cfg;
        int icon_px = thickness > 6 ? thickness - 6 : 16;
        lp->icon = load_icon_argb(icon_path, icon_fetch_size_for(icon_px));
        if (!lp->icon) {
            fprintf(stderr, "xispanel: launcher: could not load icon '%s', falling back to a placeholder\n",
                    icon_path);
        }
    }
    return 0;
}

static void launcher_destroy(PanelWidget *w)
{
    LauncherPriv *lp = w->priv;
    if (lp->icon) {
        cairo_surface_destroy(lp->icon);
    }
    if (lp->dyn_icon) {
        cairo_surface_destroy(lp->dyn_icon);
    }
}

/* One tick = ask each configured command for its last completed output
 * and re-parse only what has a new generation. asyncmd_get() is also what
 * starts the next run once the cached one is older than interval_ms, and
 * a finished run re-ticks every widget at once (xispanel.c), so a result
 * is painted as soon as it lands. */
static int launcher_on_tick(PanelWidget *w, uint64_t now)
{
    LauncherPriv *lp = w->priv;
    int changed = 0;
    const char *out;
    uint64_t gen;

    if (lp->cmd_icon[0] && (gen = asyncmd_get(lp->cmd_icon, (unsigned)lp->interval_ms, &out)) != lp->gen_icon) {
        lp->gen_icon = gen;
        char spec[sizeof(lp->dyn_icon_spec)];
        launcher_first_line(out, spec, sizeof(spec));
        if (strcmp(spec, lp->dyn_icon_spec) != 0) {
            if (lp->dyn_icon) {
                cairo_surface_destroy(lp->dyn_icon);
                lp->dyn_icon = NULL;
            }
            snprintf(lp->dyn_icon_spec, sizeof(lp->dyn_icon_spec), "%s", spec);
            if (spec[0]) {
                lp->dyn_icon = resolve_icon_theme_name(spec, icon_fetch_size_for(launcher_icon_px(w)));
            }
            changed = 1;
        }
    }
    if (lp->cmd_text[0] && (gen = asyncmd_get(lp->cmd_text, (unsigned)lp->interval_ms, &out)) != lp->gen_text) {
        lp->gen_text = gen;
        char text[sizeof(lp->text)];
        launcher_first_line(out, text, sizeof(text));
        if (strcmp(text, lp->text) != 0) {
            snprintf(lp->text, sizeof(lp->text), "%s", text);
            changed = 1;
        }
    }
    if (lp->cmd_tooltip[0] &&
        (gen = asyncmd_get(lp->cmd_tooltip, (unsigned)lp->interval_ms, &out)) != lp->gen_tooltip) {
        lp->gen_tooltip = gen;
        char tip[sizeof(lp->tooltip)];
        copy_utf8_truncated(tip, sizeof(tip), out);
        size_t n = strlen(tip);
        while (n > 0 && (tip[n - 1] == '\n' || tip[n - 1] == ' ')) {
            tip[--n] = 0;
        }
        /* The tooltip is read when it opens, not painted: no repaint. */
        snprintf(lp->tooltip, sizeof(lp->tooltip), "%s", tip);
    }
    w->next_tick_ms = now + (uint64_t)lp->interval_ms;
    return changed;
}

static void launcher_measure(PanelWidget *w, int cross_axis, int *out_len, int *out_min_len)
{
    LauncherPriv *lp = w->priv;
    *out_len = cross_axis;
    *out_min_len = cross_axis;
    if (!launcher_shows_text(w)) {
        return;
    }
    double tw = 0;
    pango_text_extents_panel(w->panel->cr, lp->text, panel_text_size(w->panel), 0, &tw, NULL, w->panel);
    int text_len = (int)tw + 8;
    *out_len = launcher_shows_icon(w) ? cross_axis + text_len : text_len + 8;
    *out_min_len = *out_len; /* clipped text is worse than none */
}

static void launcher_paint(PanelWidget *w, cairo_t *cr)
{
    LauncherPriv *lp = w->priv;
    Panel *p = w->panel;
    int ox, oy, owidth, oheight;
    widget_get_rect(w, &ox, &oy, &owidth, &oheight);
    (void)owidth;
    (void)oheight;
    widget_paint_hover_bg(w, cr);

    int icon_px = w->thickness > 6 ? w->thickness - 6 : 16;
    int icon_x = ox + (w->thickness - icon_px) / 2;
    int icon_y = oy + (w->thickness - icon_px) / 2;
    cairo_surface_t *icon = lp->dyn_icon ? lp->dyn_icon : lp->icon;
    if (launcher_shows_icon(w)) {
        if (icon) {
            draw_icon_scaled(cr, icon, icon_x, icon_y, icon_px);
        } else {
            draw_fallback_icon(cr, icon_x, icon_y, icon_px, lp->name, p->fg_r, p->fg_g, p->fg_b,
                               panel_text_size(p));
        }
    }
    if (launcher_shows_text(w)) {
        cairo_set_source_rgba(cr, p->fg_r, p->fg_g, p->fg_b, p->fg_a);
        int tx = launcher_shows_icon(w) ? ox + w->thickness : ox + 8;
        pango_show_text_boxed(cr, tx, oy, w->thickness, 0, panel_text_size(p), lp->text, NULL, p);
    }
}

static int launcher_get_tooltip(PanelWidget *w, int local_x, char *buf, size_t bufsz, int *anchor_x, int *anchor_w,
                                 int *out_closable, void **out_ctx)
{
    (void)local_x;
    (void)out_closable;
    (void)out_ctx;
    LauncherPriv *lp = w->priv;
    const char *tip = lp->tooltip[0] ? lp->tooltip : lp->name;
    if (!tip[0]) {
        return 0;
    }
    snprintf(buf, bufsz, "%s", tip);
    *anchor_x = 0;
    *anchor_w = w->len;
    return 1;
}

static int launcher_on_button(PanelWidget *w, int button, int local_x, int local_y, int root_x, int root_y)
{
    (void)local_x;
    (void)local_y;
    (void)root_x;
    (void)root_y;
    LauncherPriv *lp = w->priv;
    const char *cmd;
    switch (button) {
    case Button1:
        cmd = lp->cmd;
        break;
    case Button2:
        cmd = lp->cmd_middle;
        break;
    case Button3:
        cmd = lp->cmd_right;
        break;
    case Button4:
        cmd = lp->cmd_scroll_up;
        break;
    case Button5:
        cmd = lp->cmd_scroll_down;
        break;
    default:
        return 0;
    }
    run_detached(cmd);
    return 1;
}

const PanelWidgetOps launcher_ops = {
    .type_name = "launcher",
    .embeddable = 1,
    .priv_size = sizeof(LauncherPriv),
    .init = launcher_init,
    .destroy = launcher_destroy,
    .measure = launcher_measure,
    .paint = launcher_paint,
    .on_button = launcher_on_button,
    .on_tick = launcher_on_tick,
    .get_tooltip = launcher_get_tooltip,
};
