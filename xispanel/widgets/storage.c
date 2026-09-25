/*
 * storage widget - single icon reflecting whether any removable device is
 * currently attached (see ../storage.c for the lsblk-shelling backend).
 * Left-click opens xisserve's --storage page (mount/unmount/eject)
 * anchored to this icon -- same "icon + tooltip + click opens the
 * xisserve page" shape as widgets/volume.c and widgets/network.c.
 *
 * Hotplug/mount toasts are NOT this widget's job -- see storage_events.c,
 * which fires them once regardless of how many `storage` widgets exist
 * across panels (same split audio_events.c already makes from
 * widgets/volume.c). This file only polls for its own icon/tooltip.
 */
#include "../xispanel.h"

#include <X11/Xlib.h>

#include <stdio.h>
#include <string.h>

#define STORAGE_POLL_MS 3000
#define STORAGE_MAX_DEVICES 16
#define STORAGE_TOOLTIP_MAX_LINES 4

typedef struct {
    char cmd[192]; /* xisserve binary opened on left click */

    StorageDevice devices[STORAGE_MAX_DEVICES];
    int count;
} StoragePriv;

static int storage_init(PanelWidget *w)
{
    StoragePriv *sp = w->priv;
    if (!kv_get(w->config_kv, "cmd", sp->cmd, sizeof(sp->cmd)) || !sp->cmd[0]) {
        snprintf(sp->cmd, sizeof(sp->cmd), "xisserve");
    }
    w->next_tick_ms = now_ms();
    return 0;
}

static int storage_on_tick(PanelWidget *w, uint64_t now)
{
    StoragePriv *sp = w->priv;
    w->next_tick_ms = now + STORAGE_POLL_MS;

    int o_count = sp->count;
    StorageDevice old[STORAGE_MAX_DEVICES];
    memcpy(old, sp->devices, sizeof(old));

    /* Never blocks -- the last completed `lsblk` snapshot, refreshed in
     * the background (see ../storage.c). Generation 0 means the first run
     * hasn't landed yet: keep the previous state rather than briefly
     * painting "nothing attached". */
    if (storage_list(STORAGE_POLL_MS, sp->devices, STORAGE_MAX_DEVICES, &sp->count) == 0) {
        sp->count = o_count;
        memcpy(sp->devices, old, sizeof(old));
        return 0;
    }

    if (o_count != sp->count) {
        return 1;
    }
    for (int i = 0; i < sp->count; i++) {
        if (strcmp(old[i].name, sp->devices[i].name) != 0 ||
            strcmp(old[i].mountpoint, sp->devices[i].mountpoint) != 0) {
            return 1;
        }
    }
    return 0;
}

static void storage_measure(PanelWidget *w, int cross_axis, int *out_len, int *out_min_len)
{
    (void)w;
    *out_len = cross_axis;
    *out_min_len = cross_axis;
}

/* A simple USB-stick glyph: a body rectangle plus a narrower connector
 * nub -- filled solid when something is plugged in, drawn faint/outline
 * only when nothing is, so the icon itself hints "nothing to see here"
 * at a glance rather than needing the tooltip. */
static void draw_usb(cairo_t *cr, double cx, double cy, double size, int has_devices, double fg_r, double fg_g,
                      double fg_b)
{
    cairo_save(cr);
    cairo_set_source_rgba(cr, fg_r, fg_g, fg_b, has_devices ? 0.9 : 0.35);
    cairo_set_line_width(cr, 1.4);

    double bw = size * 0.5, bh = size * 0.62;
    double bx = cx - bw / 2, by = cy - bh / 2 + size * 0.06;
    if (has_devices) {
        cairo_rectangle(cr, bx, by, bw, bh);
        cairo_fill(cr);
    } else {
        cairo_rectangle(cr, bx, by, bw, bh);
        cairo_stroke(cr);
    }

    double nw = bw * 0.4, nh = size * 0.16;
    cairo_rectangle(cr, cx - nw / 2, by - nh, nw, nh);
    if (has_devices) {
        cairo_fill(cr);
    } else {
        cairo_stroke(cr);
    }
    cairo_restore(cr);
}

static void storage_paint(PanelWidget *w, cairo_t *cr)
{
    StoragePriv *sp = w->priv;
    Panel *p = w->panel;
    int ox, oy, owidth, oheight;
    widget_get_rect(w, &ox, &oy, &owidth, &oheight);
    (void)owidth;
    (void)oheight;
    widget_paint_hover_bg(w, cr);

    double cx = ox + w->thickness / 2.0;
    double cy = oy + w->thickness / 2.0;

    const char *icon_name = sp->count > 0 ? "drive-removable-media" : "drive-removable-media-symbolic";
    int icon_px = w->thickness > 6 ? w->thickness - 6 : 16;
    cairo_surface_t *themed = panel_theme_icon(p, icon_name, icon_px);
    if (themed) {
        draw_icon_scaled(cr, themed, ox + (w->thickness - icon_px) / 2, oy + (w->thickness - icon_px) / 2, icon_px);
        return;
    }
    draw_usb(cr, cx, cy, w->thickness * 0.8, sp->count > 0, p->fg_r, p->fg_g, p->fg_b);
}

static int storage_get_tooltip(PanelWidget *w, int local_x, char *buf, size_t bufsz, int *anchor_x, int *anchor_w,
                                int *out_closable, void **out_ctx)
{
    (void)local_x;
    (void)out_closable;
    (void)out_ctx;
    StoragePriv *sp = w->priv;
    if (sp->count == 0) {
        snprintf(buf, bufsz, "Nenhum dispositivo remov\xc3\xadvel");
    } else {
        size_t o = 0;
        int shown = sp->count < STORAGE_TOOLTIP_MAX_LINES ? sp->count : STORAGE_TOOLTIP_MAX_LINES;
        for (int i = 0; i < shown && o + 1 < bufsz; i++) {
            const StorageDevice *d = &sp->devices[i];
            const char *label = d->label[0] ? d->label : d->name;
            int n = snprintf(buf + o, bufsz - o, "%s%s: %s", o ? "\n" : "", label,
                              d->mountpoint[0] ? d->mountpoint : "n\xc3\xa3o montado");
            if (n < 0) {
                break;
            }
            o += (size_t)n;
        }
        if (sp->count > shown && o + 1 < bufsz) {
            snprintf(buf + o, bufsz - o, "\n\xe2\x80\xa6 e mais %d", sp->count - shown);
        }
    }
    *anchor_x = 0;
    *anchor_w = w->len;
    return 1;
}

static int storage_on_button(PanelWidget *w, int button, int local_x, int local_y, int root_x, int root_y)
{
    (void)local_x;
    (void)local_y;
    (void)root_x;
    (void)root_y;
    StoragePriv *sp = w->priv;
    if (button != Button1) {
        return 0;
    }
    xisserve_spawn_for_widget(w, sp->cmd, "--storage");
    return 1;
}

const PanelWidgetOps storage_ops = {
    .type_name = "storage",
    .embeddable = 1,
    .priv_size = sizeof(StoragePriv),
    .init = storage_init,
    .measure = storage_measure,
    .paint = storage_paint,
    .on_button = storage_on_button,
    .on_tick = storage_on_tick,
    .get_tooltip = storage_get_tooltip,
};
