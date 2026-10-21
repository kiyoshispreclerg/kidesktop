/*
 * xisguard widget - protection state at a glance, fed by xisguard's control
 * socket (see ../xisguard_events.c, which also raises the toasts).
 *
 * Icon: shield, solid when Xnotify is enforcing, red when it was switched
 * off at runtime (and the panel marks it urgent, so it stays visible even
 * from inside a container), faint when the guard is not running. A dot on
 * it means a REPORT/REQUEST came in during the last minute. Right-click opens a
 * menu with the toggle (Disable / Re-enable) and the last few reports --
 * the guard keeps no history, this widget's source only remembers what it
 * saw since xispanel started.
 *
 * The guard re-enables Xnotify by itself if it stops, so "disabled" is
 * never a state that outlives the guard.
 */
#include "../xispanel.h"

#include <X11/Xlib.h>

#include <stdio.h>
#include <string.h>
#include <time.h>

#define XG_RECENT_S 60
#define XG_TICK_MS 5000 /* only to age the "recent" dot; events repoll the widget themselves */

typedef struct {
    uint64_t seen_generation;
    int recent;
} XgPriv;

static int xg_recent(const XisguardEvents *ev)
{
    if (ev->history_count == 0) {
        return 0;
    }
    int newest = (ev->history_head + XISGUARD_HISTORY_MAX - 1) % XISGUARD_HISTORY_MAX;
    return time(NULL) - ev->history[newest].ts < XG_RECENT_S;
}

static int xg_init(PanelWidget *w)
{
    w->next_tick_ms = now_ms();
    return 0;
}

static int xg_on_tick(PanelWidget *w, uint64_t now)
{
    XgPriv *xp = w->priv;
    const XisguardEvents *ev = xisguard_events_get();
    w->next_tick_ms = now + XG_TICK_MS;
    int recent = xg_recent(ev);
    int changed = ev->generation != xp->seen_generation || recent != xp->recent;
    xp->seen_generation = ev->generation;
    xp->recent = recent;
    return changed;
}

static void xg_measure(PanelWidget *w, int cross_axis, int *out_len, int *out_min_len)
{
    (void)w;
    *out_len = cross_axis;
    *out_min_len = cross_axis;
}

static void draw_shield(cairo_t *cr, double cx, double cy, double size, double r, double g, double b, double a,
                         int solid)
{
    double hw = size * 0.38, top = cy - size * 0.42, bot = cy + size * 0.46;
    cairo_save(cr);
    cairo_set_source_rgba(cr, r, g, b, a);
    cairo_set_line_width(cr, 1.5);
    cairo_move_to(cr, cx, top);
    cairo_line_to(cr, cx + hw, top + size * 0.12);
    cairo_curve_to(cr, cx + hw, cy + size * 0.2, cx + hw * 0.5, bot - size * 0.1, cx, bot);
    cairo_curve_to(cr, cx - hw * 0.5, bot - size * 0.1, cx - hw, cy + size * 0.2, cx - hw, top + size * 0.12);
    cairo_close_path(cr);
    if (solid) {
        cairo_fill(cr);
    } else {
        cairo_stroke(cr);
    }
    cairo_restore(cr);
}

static void xg_paint(PanelWidget *w, cairo_t *cr)
{
    Panel *p = w->panel;
    const XisguardEvents *ev = xisguard_events_get();
    int ox, oy, owidth, oheight;
    widget_get_rect(w, &ox, &oy, &owidth, &oheight);
    (void)owidth;
    (void)oheight;
    widget_paint_hover_bg(w, cr);

    double cx = ox + w->thickness / 2.0;
    double cy = oy + w->thickness / 2.0;
    double size = w->thickness * 0.78;

    if (!ev->connected) {
        draw_shield(cr, cx, cy, size, p->fg_r, p->fg_g, p->fg_b, 0.35, 0);
    } else if (!ev->enabled) {
        draw_shield(cr, cx, cy, size, 0.86, 0.18, 0.16, 1.0, 1);
        cairo_save(cr);
        cairo_set_source_rgba(cr, 1, 1, 1, 1);
        cairo_set_line_width(cr, 1.6);
        cairo_move_to(cr, cx, cy - size * 0.2);
        cairo_line_to(cr, cx, cy + size * 0.06);
        cairo_stroke(cr);
        cairo_arc(cr, cx, cy + size * 0.2, 1.0, 0, 6.2832);
        cairo_fill(cr);
        cairo_restore(cr);
    } else {
        draw_shield(cr, cx, cy, size, p->fg_r, p->fg_g, p->fg_b, 0.9, 1);
    }
    if (ev->connected && xg_recent(ev)) {
        cairo_save(cr);
        cairo_set_source_rgba(cr, 0.95, 0.65, 0.1, 1);
        cairo_arc(cr, cx + size * 0.36, cy - size * 0.34, w->thickness * 0.12, 0, 6.2832);
        cairo_fill(cr);
        cairo_restore(cr);
    }
}

static void fmt_age(char *out, size_t outsz, time_t ts)
{
    long d = (long)(time(NULL) - ts);
    if (d < 60) {
        snprintf(out, outsz, "%lds", d < 0 ? 0 : d);
    } else if (d < 3600) {
        snprintf(out, outsz, "%ldmin", d / 60);
    } else {
        snprintf(out, outsz, "%ldh", d / 3600);
    }
}

static const char *xg_state_text(const XisguardEvents *ev)
{
    if (!ev->connected) {
        return _("XisGuard n\xc3\xa3o est\xc3\xa1 em execu\xc3\xa7\xc3\xa3o");
    }
    return ev->enabled ? _("Prote\xc3\xa7\xc3\xa3o ativa") : _("Prote\xc3\xa7\xc3\xa3o DESLIGADA");
}

/* Newest first; i = 0 is the newest. */
static const XisguardHistoryEntry *xg_hist(const XisguardEvents *ev, int i)
{
    return &ev->history[(ev->history_head + XISGUARD_HISTORY_MAX - 1 - i) % XISGUARD_HISTORY_MAX];
}

/* "firefox: captura a tela (12s)"; a REQUEST (asked, not yet using) says so. */
static void xg_hist_label(char *out, size_t outsz, const XisguardHistoryEntry *h)
{
    char age[32];
    fmt_age(age, sizeof(age), h->ts);
    const char *exe = h->exe[0] ? h->exe : "?";
    if (strcmp(h->ev, "REQUEST") == 0) {
        snprintf(out, outsz, _("%s pediu: %s (%s)"), exe, xisguard_action_text(h->action), age);
    } else {
        snprintf(out, outsz, "%s: %s (%s)", exe, xisguard_action_text(h->action), age);
    }
}

/* Newest first, skipping an entry already shown for the same program+action
 * (a program polling the clipboard would otherwise fill the whole list). */
static int xg_next_unique(const XisguardEvents *ev, int from, const XisguardHistoryEntry **out)
{
    for (int i = from; i < ev->history_count; i++) {
        const XisguardHistoryEntry *h = xg_hist(ev, i);
        int dup = 0;
        for (int j = 0; j < i && !dup; j++) {
            const XisguardHistoryEntry *o = xg_hist(ev, j);
            dup = !strcmp(o->exe, h->exe) && !strcmp(o->action, h->action);
        }
        if (!dup) {
            *out = h;
            return i;
        }
    }
    return -1;
}

static int xg_get_tooltip(PanelWidget *w, int local_x, char *buf, size_t bufsz, int *anchor_x, int *anchor_w,
                           int *out_closable, void **out_ctx)
{
    (void)local_x;
    (void)out_closable;
    (void)out_ctx;
    const XisguardEvents *ev = xisguard_events_get();
    size_t o = (size_t)snprintf(buf, bufsz, "%s", xg_state_text(ev));
    const XisguardHistoryEntry *h;
    int shown = 0;
    for (int i = xg_next_unique(ev, 0, &h); i >= 0 && shown < 5 && o + 1 < bufsz;
         i = xg_next_unique(ev, i + 1, &h), shown++) {
        char line[240];
        xg_hist_label(line, sizeof(line), h);
        int n = snprintf(buf + o, bufsz - o, "\n%s", line);
        if (n < 0) {
            break;
        }
        o += (size_t)n;
    }
    *anchor_x = 0;
    *anchor_w = w->len;
    return 1;
}

static void xg_menu_select(Panel *panel, PanelWidget *widget, void *ctx, int index)
{
    (void)panel;
    (void)widget;
    (void)ctx;
    if (index != 0) {
        return;
    }
    const XisguardEvents *ev = xisguard_events_get();
    xisguard_send_command(ev->enabled ? "DISABLE" : "ENABLE");
}

static int xg_on_button(PanelWidget *w, int button, int local_x, int local_y, int root_x, int root_y)
{
    (void)local_x;
    (void)local_y;
    (void)root_x;
    (void)root_y;
    if (button != Button3) { /* context menu only */
        return 0;
    }
    const XisguardEvents *ev = xisguard_events_get();
    MenuItem items[2 + XISGUARD_HISTORY_MAX];
    memset(items, 0, sizeof(items));
    int n = 0;

    snprintf(items[n].label, sizeof(items[n].label), "%s",
             ev->enabled ? _("Desativar temporariamente") : _("Reativar prote\xc3\xa7\xc3\xa3o"));
    items[n].enabled = ev->connected;
    n++;
    if (ev->history_count > 0) {
        items[n].is_separator = 1;
        n++;
    }
    const XisguardHistoryEntry *h;
    for (int i = xg_next_unique(ev, 0, &h); i >= 0 && n < 2 + XISGUARD_HISTORY_MAX;
         i = xg_next_unique(ev, i + 1, &h)) {
        xg_hist_label(items[n].label, sizeof(items[n].label), h);
        items[n].enabled = 0; /* informational only */
        n++;
    }
    panel_menu_open(w->panel, w, 0, w->len, items, n, NULL, xg_menu_select);
    w->panel->dirty = 1;
    return 1;
}

static int xg_is_urgent(PanelWidget *w)
{
    (void)w;
    const XisguardEvents *ev = xisguard_events_get();
    return ev->connected && !ev->enabled;
}

const PanelWidgetOps xisguard_ops = {
    .type_name = "xisguard",
    .embeddable = 1,
    .priv_size = sizeof(XgPriv),
    .init = xg_init,
    .measure = xg_measure,
    .paint = xg_paint,
    .on_button = xg_on_button,
    .on_tick = xg_on_tick,
    .get_tooltip = xg_get_tooltip,
    .is_urgent = xg_is_urgent,
};
