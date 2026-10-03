/*
 * lockkeys widget - one little keycap per watched lock key (an "A" for
 * Caps Lock, a "1" for Num Lock), filled in while that key is locked and
 * just outlined (dimmed) while it isn't.
 *
 * Meant for a `container` popup with inline=urgent: is_urgent() is 1
 * while any key listed in urgent= is locked, so the widget stays tucked
 * away in the popup until e.g. Caps Lock is on, then surfaces on the bar
 * by itself -- see "Container popups" in PROTOCOL.md.
 *
 * keys=<list> (default caps,num): which lock keys get a keycap, in that
 * order (comma-separated: caps, num). urgent=<list> (default caps): the
 * subset whose being locked counts as urgent; "none" never surfaces on
 * its own. Keys named in urgent= but not in keys= are added to keys=.
 *
 * State comes from XkbGetState()'s locked modifiers (LockMask = Caps,
 * Mod2Mask = Num -- the layout every XKB keymap the distros ship uses),
 * polled every interval= ms (default 250): one cheap round trip, and the
 * widget only reports a change when a key actually flipped.
 */
#include "../xispanel.h"

#include <X11/XKBlib.h>
#include <X11/Xlib.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

typedef enum { LK_CAPS, LK_NUM, LK_COUNT } LockKey;

#define LOCKKEYS_DEFAULT_INTERVAL_MS 250
#define LOCKKEYS_MIN_INTERVAL_MS 50

static const struct {
    const char *name;
    const char *glyph;
    unsigned int mask;
} LK_INFO[LK_COUNT] = {
    [LK_CAPS] = {"caps", "A", LockMask},
    [LK_NUM] = {"num", "1", Mod2Mask},
};

typedef struct {
    int interval_ms;
    unsigned int shown;  /* bitmask of LockKey: which keycaps are drawn */
    unsigned int urgent; /* bitmask of LockKey: which ones surface inline */
    unsigned int locked; /* bitmask of LockKey: state as of the last tick */
} LockKeysPriv;

/* Parses a comma/space separated list of key names into a LockKey bitmask;
 * "none" (or an empty list) gives 0, unknown names are ignored. */
static unsigned int parse_keys(const char *s)
{
    unsigned int mask = 0;
    for (int k = 0; k < LK_COUNT; k++) {
        const char *hit = strstr(s, LK_INFO[k].name);
        if (hit) {
            mask |= 1u << k;
        }
    }
    return mask;
}

static unsigned int read_locked(void)
{
    XkbStateRec st;
    if (XkbGetState(g_dpy, XkbUseCoreKbd, &st) != Success) {
        return 0;
    }
    unsigned int mask = 0;
    for (int k = 0; k < LK_COUNT; k++) {
        if (st.locked_mods & LK_INFO[k].mask) {
            mask |= 1u << k;
        }
    }
    return mask;
}

static int lockkeys_init(PanelWidget *w)
{
    LockKeysPriv *lp = w->priv;
    char buf[64];
    lp->shown = kv_get(w->config_kv, "keys", buf, sizeof(buf)) ? parse_keys(buf) : (1u << LK_CAPS) | (1u << LK_NUM);
    lp->urgent = kv_get(w->config_kv, "urgent", buf, sizeof(buf)) ? parse_keys(buf) : (1u << LK_CAPS);
    lp->shown |= lp->urgent;
    lp->interval_ms = kv_get_int(w->config_kv, "interval", LOCKKEYS_DEFAULT_INTERVAL_MS);
    if (lp->interval_ms < LOCKKEYS_MIN_INTERVAL_MS) {
        lp->interval_ms = LOCKKEYS_MIN_INTERVAL_MS;
    }
    lp->locked = read_locked();
    w->next_tick_ms = now_ms();
    return 0;
}

static int lockkeys_on_tick(PanelWidget *w, uint64_t now)
{
    LockKeysPriv *lp = w->priv;
    w->next_tick_ms = now + (uint64_t)lp->interval_ms;
    unsigned int locked = read_locked();
    if (locked == lp->locked) {
        return 0;
    }
    lp->locked = locked;
    return 1;
}

static int shown_count(const LockKeysPriv *lp)
{
    int n = 0;
    for (int k = 0; k < LK_COUNT; k++) {
        n += (lp->shown >> k) & 1;
    }
    return n;
}

static void lockkeys_measure(PanelWidget *w, int cross_axis, int *out_len, int *out_min_len)
{
    int n = shown_count(w->priv);
    *out_len = cross_axis * (n ? n : 1);
    *out_min_len = *out_len;
}

static void rounded_rect(cairo_t *cr, double x, double y, double wd, double ht, double r)
{
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + wd - r, y + r, r, -M_PI / 2, 0);
    cairo_arc(cr, x + wd - r, y + ht - r, r, 0, M_PI / 2);
    cairo_arc(cr, x + r, y + ht - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
}

static void lockkeys_paint(PanelWidget *w, cairo_t *cr)
{
    LockKeysPriv *lp = w->priv;
    Panel *p = w->panel;
    int ox, oy, owidth, oheight;
    widget_get_rect(w, &ox, &oy, &owidth, &oheight);
    int horizontal = owidth >= oheight;
    int cell = w->thickness;
    double key = cell * 0.62;
    double size = key * 0.7;
    int slot = 0;

    for (int k = 0; k < LK_COUNT; k++) {
        if (!((lp->shown >> k) & 1)) {
            continue;
        }
        double cx = ox + (horizontal ? slot * cell : 0) + cell / 2.0;
        double cy = oy + (horizontal ? 0 : slot * cell) + cell / 2.0;
        slot++;

        int on = (lp->locked >> k) & 1;
        cairo_save(cr);
        rounded_rect(cr, cx - key / 2, cy - key / 2, key, key, key * 0.2);
        if (on) {
            cairo_set_source_rgba(cr, p->fg_r, p->fg_g, p->fg_b, 0.95);
            cairo_fill(cr);
        } else {
            cairo_set_source_rgba(cr, p->fg_r, p->fg_g, p->fg_b, 0.4);
            cairo_set_line_width(cr, 1.2);
            cairo_stroke(cr);
        }
        cairo_restore(cr);

        double tw, th;
        pango_text_extents_ellipsized(cr, LK_INFO[k].glyph, size, 0, &tw, &th);
        if (on) {
            cairo_set_source_rgba(cr, p->bg_r, p->bg_g, p->bg_b, 1.0);
        } else {
            cairo_set_source_rgba(cr, p->fg_r, p->fg_g, p->fg_b, 0.4);
        }
        pango_show_text_boxed_bold(cr, cx - tw / 2.0, cy - key / 2, key, key, size, LK_INFO[k].glyph, 1, NULL, p);
    }
}

static int lockkeys_get_tooltip(PanelWidget *w, int local_x, char *buf, size_t bufsz, int *anchor_x, int *anchor_w,
                                 int *out_closable, void **out_ctx)
{
    (void)out_closable;
    (void)out_ctx;
    LockKeysPriv *lp = w->priv;
    buf[0] = 0;
    for (int k = 0; k < LK_COUNT; k++) {
        if (!((lp->shown >> k) & 1)) {
            continue;
        }
        const char *label = k == LK_CAPS ? _("Caps Lock") : _("Num Lock");
        const char *state = (lp->locked >> k) & 1 ? _("ligado") : _("desligado");
        size_t o = strlen(buf);
        snprintf(buf + o, bufsz - o, "%s%s: %s", o ? "\n" : "", label, state);
    }
    (void)local_x;
    *anchor_x = 0;
    *anchor_w = w->len;
    return buf[0] != 0;
}

/* Any watched-for-urgency key currently locked. */
static int lockkeys_is_urgent(PanelWidget *w)
{
    LockKeysPriv *lp = w->priv;
    return (lp->locked & lp->urgent) != 0;
}

const PanelWidgetOps lockkeys_ops = {
    .type_name = "lockkeys",
    .embeddable = 1,
    .priv_size = sizeof(LockKeysPriv),
    .init = lockkeys_init,
    .measure = lockkeys_measure,
    .paint = lockkeys_paint,
    .on_tick = lockkeys_on_tick,
    .get_tooltip = lockkeys_get_tooltip,
    .is_urgent = lockkeys_is_urgent,
};
