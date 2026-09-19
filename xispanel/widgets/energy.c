/*
 * energy widget - single icon opening xisserve's --energy page (battery/
 * AC status, other UPower devices' batteries, brightness, night light --
 * see xisserve/PROTOCOL.md's `--energy` entry).
 *
 * The icon itself reads straight from /sys/class/power_supply -- cheap
 * enough for a once-a-second tick with no subprocess, same "read sysfs
 * directly" call monitor.c already makes for CPU/GPU stats, rather than
 * shelling out to `upower` the way xisserve's own --energy page does
 * (that page needs upower's richer per-device parsing for peripherals;
 * this widget only ever needs one number and one status word for the
 * system's main battery).
 *
 * On a system with no battery at all (a desktop, most VMs) there's
 * nothing meaningful to show as "charge level", so the icon falls back
 * to a brightness glyph instead -- the page behind the click still has
 * something useful on a battery-less machine (brightness slider, night
 * light), the icon just can't represent "charge" for something that
 * doesn't have one.
 */
#include "../xispanel.h"

#include <X11/Xlib.h>

#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    ENERGY_UNKNOWN,
    ENERGY_DISCHARGING,
    ENERGY_CHARGING,
    ENERGY_FULL,
} EnergyState;

typedef struct {
    char cmd[192]; /* xisserve binary opened on left click */

    int have_battery;
    int pct;
    EnergyState state;
} EnergyPriv;

static size_t read_file(const char *path, char *buf, size_t bufsz)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        return 0;
    }
    size_t n = fread(buf, 1, bufsz - 1, f);
    fclose(f);
    buf[n] = 0;
    return n;
}

static void rstrip(char *s)
{
    size_t l = strlen(s);
    while (l > 0 && (s[l - 1] == '\n' || s[l - 1] == '\r' || s[l - 1] == ' ')) {
        s[--l] = 0;
    }
}

/* First entry under /sys/class/power_supply whose `type` is "Battery" and
 * whose `scope` isn't "Device" -- the ordinary case is exactly one
 * (BAT0). The scope check matters on any machine with a wireless mouse/
 * keyboard behind a Logitech Unifying-style receiver: the kernel's hidpp
 * driver publishes those as power_supply devices of type "Battery" too
 * (verified live: a receiver with nothing else attached shows up as
 * "hidpp_battery_0"/"_1", `scope` = "Device"), and without this check
 * this widget would show a mouse's charge level as if it were the
 * laptop's -- or claim a battery-less desktop has one. A real system
 * battery either has no `scope` file at all or reports "System"; only
 * device-scoped entries need excluding. A machine with more than one
 * real battery would need summing to be fully correct; not worth it for
 * a panel icon when xisserve's --energy page (upower's own DisplayDevice
 * aggregate, which already makes the same distinction) is one click away
 * for the real breakdown. */
static int find_battery(int *pct, EnergyState *state)
{
    DIR *d = opendir("/sys/class/power_supply");
    if (!d) {
        return 0;
    }
    int found = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') {
            continue;
        }
        char path[320], buf[64];
        snprintf(path, sizeof(path), "/sys/class/power_supply/%s/type", de->d_name);
        if (!read_file(path, buf, sizeof(buf))) {
            continue;
        }
        rstrip(buf);
        if (strcmp(buf, "Battery") != 0) {
            continue;
        }
        snprintf(path, sizeof(path), "/sys/class/power_supply/%s/scope", de->d_name);
        if (read_file(path, buf, sizeof(buf))) {
            rstrip(buf);
            if (!strcmp(buf, "Device")) {
                continue;
            }
        }
        snprintf(path, sizeof(path), "/sys/class/power_supply/%s/capacity", de->d_name);
        if (!read_file(path, buf, sizeof(buf))) {
            continue;
        }
        *pct = atoi(buf);

        snprintf(path, sizeof(path), "/sys/class/power_supply/%s/status", de->d_name);
        *state = ENERGY_UNKNOWN;
        if (read_file(path, buf, sizeof(buf))) {
            rstrip(buf);
            if (!strcmp(buf, "Charging")) {
                *state = ENERGY_CHARGING;
            } else if (!strcmp(buf, "Discharging")) {
                *state = ENERGY_DISCHARGING;
            } else if (!strcmp(buf, "Full")) {
                *state = ENERGY_FULL;
            }
        }
        found = 1;
        break;
    }
    closedir(d);
    return found;
}

static int energy_init(PanelWidget *w)
{
    EnergyPriv *ep = w->priv;
    if (!kv_get(w->config_kv, "cmd", ep->cmd, sizeof(ep->cmd)) || !ep->cmd[0]) {
        snprintf(ep->cmd, sizeof(ep->cmd), "xisserve");
    }
    w->next_tick_ms = now_ms();
    return 0;
}

static int energy_on_tick(PanelWidget *w, uint64_t now)
{
    EnergyPriv *ep = w->priv;
    w->next_tick_ms = now + 1000;
    int o_have = ep->have_battery, o_pct = ep->pct;
    EnergyState o_state = ep->state;
    ep->have_battery = find_battery(&ep->pct, &ep->state);
    return o_have != ep->have_battery || o_pct != ep->pct || o_state != ep->state;
}

static void energy_measure(PanelWidget *w, int cross_axis, int *out_len, int *out_min_len)
{
    (void)w;
    *out_len = cross_axis;
    *out_min_len = cross_axis;
}

/* Battery outline + a fill proportional to `pct`, or a plain sun glyph
 * when there's no battery at all -- same drawing weight/style as
 * volume.c's draw_speaker() (plain cairo strokes, no icon theme
 * dependency). A small lightning bolt overlays the battery while
 * charging. */
static void draw_battery(cairo_t *cr, double cx, double cy, double size, int pct, int charging, double fg_r,
                          double fg_g, double fg_b)
{
    cairo_save(cr);
    cairo_set_source_rgba(cr, fg_r, fg_g, fg_b, 0.9);
    cairo_set_line_width(cr, 1.4);

    double bw = size * 0.6, bh = size * 0.85;
    double bx = cx - bw / 2, by = cy - bh / 2;
    double capw = bw * 0.3, caph = bh * 0.12;

    cairo_rectangle(cr, cx - capw / 2, by - caph, capw, caph);
    cairo_fill(cr);

    cairo_rectangle(cr, bx, by, bw, bh);
    cairo_stroke(cr);

    if (pct < 0) {
        pct = 0;
    }
    if (pct > 100) {
        pct = 100;
    }
    double inset = 1.6;
    double fillh = (bh - inset * 2) * (pct / 100.0);
    cairo_rectangle(cr, bx + inset, by + bh - inset - fillh, bw - inset * 2, fillh);
    cairo_fill(cr);

    if (charging) {
        cairo_set_source_rgba(cr, fg_r, fg_g, fg_b, 1.0);
        cairo_move_to(cr, cx + bw * 0.08, by + bh * 0.15);
        cairo_line_to(cr, cx - bw * 0.18, by + bh * 0.55);
        cairo_line_to(cr, cx, by + bh * 0.55);
        cairo_line_to(cr, cx - bw * 0.08, by + bh * 0.9);
        cairo_line_to(cr, cx + bw * 0.22, by + bh * 0.42);
        cairo_line_to(cr, cx, by + bh * 0.42);
        cairo_close_path(cr);
        cairo_fill(cr);
    }
    cairo_restore(cr);
}

static void draw_sun(cairo_t *cr, double cx, double cy, double size, double fg_r, double fg_g, double fg_b)
{
    cairo_save(cr);
    cairo_set_source_rgba(cr, fg_r, fg_g, fg_b, 0.9);
    cairo_set_line_width(cr, 1.4);
    double r = size * 0.22;
    cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
    cairo_fill(cr);
    for (int i = 0; i < 8; i++) {
        double a = i * (M_PI / 4.0);
        double x0 = cx + cos(a) * r * 1.5, y0 = cy + sin(a) * r * 1.5;
        double x1 = cx + cos(a) * size * 0.45, y1 = cy + sin(a) * size * 0.45;
        cairo_move_to(cr, x0, y0);
        cairo_line_to(cr, x1, y1);
        cairo_stroke(cr);
    }
    cairo_restore(cr);
}

static void energy_paint(PanelWidget *w, cairo_t *cr)
{
    EnergyPriv *ep = w->priv;
    Panel *p = w->panel;
    int ox, oy, owidth, oheight;
    widget_get_rect(w, &ox, &oy, &owidth, &oheight);
    (void)owidth;
    (void)oheight;
    widget_paint_hover_bg(w, cr);

    double cx = ox + w->thickness / 2.0;
    double cy = oy + w->thickness / 2.0;
    int icon_px = w->thickness > 6 ? w->thickness - 6 : 16;

    if (!ep->have_battery) {
        cairo_surface_t *themed = panel_theme_icon(p, "brightness", icon_px);
        if (themed) {
            draw_icon_scaled(cr, themed, ox + (w->thickness - icon_px) / 2, oy + (w->thickness - icon_px) / 2,
                              icon_px);
            return;
        }
        draw_sun(cr, cx, cy, w->thickness * 0.8, p->fg_r, p->fg_g, p->fg_b);
        return;
    }

    int charging = ep->state == ENERGY_CHARGING;
    /* freedesktop's own bucketing (battery-full/good/low/caution/empty),
     * same breakpoints a theme's own icon set expects a widget to pick
     * from -- a theme with only some of these names still degrades
     * gracefully since each lookup falls back to the vector battery on
     * its own. */
    const char *level = ep->pct >= 90 || ep->state == ENERGY_FULL ? "full"
                        : ep->pct >= 40                            ? "good"
                        : ep->pct >= 20                            ? "low"
                        : ep->pct >= 10                             ? "caution"
                                                                     : "empty";
    char icon_name[40];
    snprintf(icon_name, sizeof(icon_name), "battery-%s%s", level, charging ? "-charging" : "");
    cairo_surface_t *themed = panel_theme_icon(p, icon_name, icon_px);
    if (themed) {
        draw_icon_scaled(cr, themed, ox + (w->thickness - icon_px) / 2, oy + (w->thickness - icon_px) / 2, icon_px);
        return;
    }
    draw_battery(cr, cx, cy, w->thickness * 0.8, ep->pct, charging, p->fg_r, p->fg_g, p->fg_b);
}

static const char *energy_state_label(EnergyState s)
{
    switch (s) {
    case ENERGY_CHARGING: return "carregando";
    case ENERGY_DISCHARGING: return "descarregando";
    case ENERGY_FULL: return "completa";
    default: return "";
    }
}

static int energy_get_tooltip(PanelWidget *w, int local_x, char *buf, size_t bufsz, int *anchor_x, int *anchor_w,
                               int *out_closable, void **out_ctx)
{
    (void)local_x;
    (void)out_closable;
    (void)out_ctx;
    EnergyPriv *ep = w->priv;
    if (ep->have_battery) {
        const char *st = energy_state_label(ep->state);
        if (st[0]) {
            snprintf(buf, bufsz, "Bateria: %d%% (%s)", ep->pct, st);
        } else {
            snprintf(buf, bufsz, "Bateria: %d%%", ep->pct);
        }
    } else {
        snprintf(buf, bufsz, "Brilho e luz noturna");
    }
    *anchor_x = 0;
    *anchor_w = w->len;
    return 1;
}

static int energy_on_button(PanelWidget *w, int button, int local_x, int local_y, int root_x, int root_y)
{
    (void)local_x;
    (void)local_y;
    (void)root_x;
    (void)root_y;
    EnergyPriv *ep = w->priv;
    if (button != Button1) {
        return 0;
    }
    xisserve_spawn_for_widget(w, ep->cmd, "--energy");
    return 1;
}

const PanelWidgetOps energy_ops = {
    .type_name = "energy",
    .priv_size = sizeof(EnergyPriv),
    .init = energy_init,
    .measure = energy_measure,
    .paint = energy_paint,
    .on_button = energy_on_button,
    .on_tick = energy_on_tick,
    .get_tooltip = energy_get_tooltip,
};
