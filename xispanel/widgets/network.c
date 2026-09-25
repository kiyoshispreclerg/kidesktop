/*
 * network widget - single icon reflecting connectivity (see ../network.c
 * for the nmcli-shelling backend). Left-click opens xisserve's --network
 * page (device list, wifi scan/connect) anchored to this icon -- same
 * "icon + tooltip + click opens the xisserve page" shape as widgets/
 * volume.c and widgets/energy.c.
 *
 * No toasts from this widget: NetworkManager already fires its own DBus
 * notifications (connected/disconnected) that xispanel's notifd.c/toast
 * pipeline already shows -- duplicating that here would just double up
 * the same information.
 */
#include "../xispanel.h"

#include <X11/Xlib.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#define NETWORK_POLL_MS 3000

typedef struct {
    char cmd[192]; /* xisserve binary opened on left click */

    int connected;
    char type[16];   /* "wifi"/"ethernet" */
    char name[128];  /* connection name */
    int signal_pct;  /* -1 for ethernet/disconnected */
} NetworkPriv;

static int network_init(PanelWidget *w)
{
    NetworkPriv *np = w->priv;
    if (!kv_get(w->config_kv, "cmd", np->cmd, sizeof(np->cmd)) || !np->cmd[0]) {
        snprintf(np->cmd, sizeof(np->cmd), "xisserve");
    }
    np->signal_pct = -1;
    w->next_tick_ms = now_ms();
    return 0;
}

static int network_on_tick(PanelWidget *w, uint64_t now)
{
    NetworkPriv *np = w->priv;
    w->next_tick_ms = now + NETWORK_POLL_MS;

    int o_connected = np->connected, o_signal = np->signal_pct;
    char o_type[16], o_name[128];
    snprintf(o_type, sizeof(o_type), "%s", np->type);
    snprintf(o_name, sizeof(o_name), "%s", np->name);

    /* Never blocks: this reads the last completed `nmcli` snapshot and
     * asks asyncmd.c to refresh it in the background when it has aged
     * past NETWORK_POLL_MS (see ../network.c). Until the very first run
     * finishes it returns generation 0, and the widget simply keeps
     * painting its initial "not connected" state for a tick or two
     * instead of the panel waiting on nmcli to answer. */
    char type[16], name[128];
    int connected = 0, signal_pct = -1;
    if (network_get_summary(NETWORK_POLL_MS, type, sizeof(type), name, sizeof(name), &signal_pct, &connected) == 0) {
        return 0;
    }
    np->connected = connected;
    np->signal_pct = signal_pct;
    snprintf(np->type, sizeof(np->type), "%s", type);
    snprintf(np->name, sizeof(np->name), "%s", name);

    return o_connected != np->connected || o_signal != np->signal_pct || strcmp(o_type, np->type) != 0 ||
           strcmp(o_name, np->name) != 0;
}

static void network_measure(PanelWidget *w, int cross_axis, int *out_len, int *out_min_len)
{
    (void)w;
    *out_len = cross_axis;
    *out_min_len = cross_axis;
}

/* Concentric quarter-arcs, the universal wifi-signal glyph -- `bars` (0-3)
 * of them filled, the rest drawn faint. A short vertical stroke instead of
 * arcs draws the ethernet plug/cable glyph; a single dot with no arcs at
 * all is "no connection". Same drawing weight as volume.c's draw_speaker(). */
static void draw_wifi(cairo_t *cr, double cx, double cy, double size, int bars, double fg_r, double fg_g, double fg_b)
{
    cairo_save(cr);
    cairo_set_line_width(cr, 1.6);
    cairo_set_source_rgba(cr, fg_r, fg_g, fg_b, 0.9);
    cairo_arc(cr, cx, cy + size * 0.35, size * 0.06, 0, 2 * M_PI);
    cairo_fill(cr);
    for (int i = 0; i < 3; i++) {
        double r = size * (0.18 + 0.22 * i);
        cairo_set_source_rgba(cr, fg_r, fg_g, fg_b, i < bars ? 0.9 : 0.25);
        cairo_arc(cr, cx, cy + size * 0.35, r, M_PI * 1.22, M_PI * 1.78);
        cairo_stroke(cr);
    }
    cairo_restore(cr);
}

static void draw_ethernet(cairo_t *cr, double cx, double cy, double size, double fg_r, double fg_g, double fg_b)
{
    cairo_save(cr);
    cairo_set_source_rgba(cr, fg_r, fg_g, fg_b, 0.9);
    cairo_set_line_width(cr, 1.6);
    double hw = size * 0.32, hh = size * 0.22;
    cairo_rectangle(cr, cx - hw, cy - hh * 0.3, hw * 2, hh);
    cairo_stroke(cr);
    for (int i = 0; i < 3; i++) {
        double x = cx - hw * 0.6 + i * hw * 0.6;
        cairo_move_to(cr, x, cy - hh * 0.3);
        cairo_line_to(cr, x, cy - hh * 0.9);
        cairo_stroke(cr);
    }
    cairo_restore(cr);
}

static void network_paint(PanelWidget *w, cairo_t *cr)
{
    NetworkPriv *np = w->priv;
    Panel *p = w->panel;
    int ox, oy, owidth, oheight;
    widget_get_rect(w, &ox, &oy, &owidth, &oheight);
    (void)owidth;
    (void)oheight;
    widget_paint_hover_bg(w, cr);

    double cx = ox + w->thickness / 2.0;
    double cy = oy + w->thickness / 2.0;

    const char *icon_name;
    if (!np->connected) {
        icon_name = "network-offline";
    } else if (strcmp(np->type, "ethernet") == 0) {
        icon_name = "network-wired";
    } else {
        int bars = np->signal_pct < 0 ? 3 : (np->signal_pct < 34 ? 1 : (np->signal_pct < 67 ? 2 : 3));
        icon_name = bars <= 1 ? "network-wireless-signal-weak"
                    : bars == 2 ? "network-wireless-signal-ok"
                                : "network-wireless-signal-excellent";
    }
    int icon_px = w->thickness > 6 ? w->thickness - 6 : 16;
    cairo_surface_t *themed = panel_theme_icon(p, icon_name, icon_px);
    if (themed) {
        draw_icon_scaled(cr, themed, ox + (w->thickness - icon_px) / 2, oy + (w->thickness - icon_px) / 2, icon_px);
        return;
    }

    if (!np->connected) {
        draw_wifi(cr, cx, cy, w->thickness * 0.8, 0, p->fg_r, p->fg_g, p->fg_b);
    } else if (strcmp(np->type, "ethernet") == 0) {
        draw_ethernet(cr, cx, cy, w->thickness * 0.8, p->fg_r, p->fg_g, p->fg_b);
    } else {
        int bars = np->signal_pct < 0 ? 3 : (np->signal_pct < 34 ? 1 : (np->signal_pct < 67 ? 2 : 3));
        draw_wifi(cr, cx, cy, w->thickness * 0.8, bars, p->fg_r, p->fg_g, p->fg_b);
    }
}

static int network_get_tooltip(PanelWidget *w, int local_x, char *buf, size_t bufsz, int *anchor_x, int *anchor_w,
                                int *out_closable, void **out_ctx)
{
    (void)local_x;
    (void)out_closable;
    (void)out_ctx;
    NetworkPriv *np = w->priv;
    if (!np->connected) {
        snprintf(buf, bufsz, "Sem conex\xc3\xa3o de rede");
    } else if (strcmp(np->type, "ethernet") == 0) {
        snprintf(buf, bufsz, "Rede: %s\nCabo", np->name);
    } else if (np->signal_pct >= 0) {
        snprintf(buf, bufsz, "Rede: %s\nWi-Fi \xc2\xb7 %d%%", np->name, np->signal_pct);
    } else {
        snprintf(buf, bufsz, "Rede: %s\nWi-Fi", np->name);
    }
    *anchor_x = 0;
    *anchor_w = w->len;
    return 1;
}

static int network_on_button(PanelWidget *w, int button, int local_x, int local_y, int root_x, int root_y)
{
    (void)local_x;
    (void)local_y;
    (void)root_x;
    (void)root_y;
    NetworkPriv *np = w->priv;
    if (button != Button1) {
        return 0;
    }
    xisserve_spawn_for_widget(w, np->cmd, "--network");
    return 1;
}

const PanelWidgetOps network_ops = {
    .type_name = "network",
    .embeddable = 1,
    .priv_size = sizeof(NetworkPriv),
    .init = network_init,
    .measure = network_measure,
    .paint = network_paint,
    .on_button = network_on_button,
    .on_tick = network_on_tick,
    .get_tooltip = network_get_tooltip,
};
