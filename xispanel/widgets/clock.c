/*
 * clock widget - shows the current time, reformatted once a second via
 * strftime(). Displays the system's configured timezone by default;
 * `tz=<IANA zone>` overrides just this widget's displayed time (e.g. a
 * secondary panel showing a coworker's timezone). `tooltip_tz=<zone,...>`
 * lists additional zones whose date+time are shown one per block in the
 * tooltip, instead of just the single current-zone line.
 *
 * The format may produce more than one line -- either with strftime's own
 * `%n` (a newline, POSIX) or with a literal `\n` in the config value,
 * unescaped here since the config file itself is line-based and can't
 * carry a real newline inside a value. Each line is drawn centered in its
 * own horizontal band of the widget, at a size shrunk to fit them all
 * (see clock_line_size()), so `format=%H:%M%n%a %d %b` gives the usual
 * time-over-date panel clock.
 *
 * `capitalize=yes|no` (default yes): uppercase the first letter of each
 * line. strftime's locale-provided day/month names are lowercase in
 * pt_BR and most other locales ("sáb. 29 ago"), which reads wrong as a
 * label; this makes it "Sáb. 29 ago" without needing a locale-specific
 * format string. Lines starting with a digit (any plain %H:%M) are
 * unaffected either way.
 */
#include "../xispanel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CLOCK_MAX_TOOLTIP_TZ 8

#define CLOCK_MAX_LINES 4

typedef struct {
    char format[64];
    char text[96]; /* may hold several '\n'-separated lines */
    char tz[64];   /* empty = system default */
    char tooltip_tz[CLOCK_MAX_TOOLTIP_TZ][64];
    int n_tooltip_tz;
    int capitalize;
} ClockPriv;

/* Rewrites "\n" (backslash + n) in place into a real newline -- the config
 * file is one record per line, so that's the only way a value can ask for
 * a line break. strftime's own %n does the same thing without this, and
 * both are documented; this exists because `format=%H:%M\n%a %d %b` is
 * what everyone tries first. A trailing lone backslash is left alone. */
static void unescape_newlines(char *s)
{
    char *r = s, *w = s;
    while (*r) {
        if (r[0] == '\\' && r[1] == 'n') {
            *w++ = '\n';
            r += 2;
        } else {
            *w++ = *r++;
        }
    }
    *w = 0;
}

/* Uppercases the first letter of every line, in place and without
 * changing the byte count: ASCII plus the two-byte UTF-8 range that
 * covers the accented Latin letters locale day/month names actually
 * start with (á, é, ç, ...). Anything else (a digit, a CJK codepoint) is
 * left exactly as it was. */
static void capitalize_lines(char *s)
{
    int at_line_start = 1;
    for (unsigned char *p = (unsigned char *)s; *p; p++) {
        if (at_line_start) {
            if (*p >= 'a' && *p <= 'z') {
                *p -= 32;
            } else if (p[0] == 0xC3 && p[1] >= 0xA0 && p[1] <= 0xBE && p[1] != 0xB7) {
                /* U+00E0..U+00FE minus ÷ -- the lowercase half of Latin-1
                 * Supplement, whose uppercase is exactly 0x20 lower. */
                p[1] -= 0x20;
            }
            at_line_start = 0;
        }
        if (*p == '\n') {
            at_line_start = 1;
        }
    }
}

/* Splits cp->text into at most CLOCK_MAX_LINES pointers into `copy` (which
 * must be a writable duplicate of it -- the separators are overwritten
 * with NULs). Returns the line count, always >= 1. */
static int clock_split_lines(const char *text, char *copy, size_t copysz, const char *lines[CLOCK_MAX_LINES])
{
    snprintf(copy, copysz, "%s", text);
    int n = 0;
    lines[n++] = copy;
    for (char *p = copy; *p && n < CLOCK_MAX_LINES; p++) {
        if (*p == '\n') {
            *p = 0;
            lines[n++] = p + 1;
        }
    }
    /* Any further newlines past the cap stay inside the last line -- a
     * clock with five lines isn't a case worth failing over. */
    return n;
}

/* Text size for one line: the panel's normal size, shrunk when there's
 * more than one line so the whole stack still fits the widget's
 * thickness. */
static double clock_line_size(const Panel *p, int thickness, int n_lines)
{
    double size = panel_text_size(p);
    if (n_lines > 1) {
        double fit = (double)thickness / n_lines * 0.78;
        if (fit < size) {
            size = fit;
        }
    }
    return size < 6 ? 6 : size;
}

/* localtime_r() has no "in this zone" variant in POSIX/glibc -- the
 * standard workaround is to temporarily point the TZ env var at the zone
 * of interest, call tzset() so libc re-reads it, then put TZ back exactly
 * as it was. Not signal-safe/reentrant, but xispanel is single-threaded
 * and this is only ever called from the main loop. tz=NULL/"" means "use
 * whatever TZ already says", i.e. the system default. */
static void localtime_in_tz(const char *tz, struct tm *out)
{
    char old[128];
    int had_old = 0;
    const char *cur = getenv("TZ");
    if (cur) {
        snprintf(old, sizeof(old), "%s", cur);
        had_old = 1;
    }
    if (tz && tz[0]) {
        setenv("TZ", tz, 1);
        tzset();
    }
    time_t t = time(NULL);
    localtime_r(&t, out);
    if (tz && tz[0]) {
        if (had_old) {
            setenv("TZ", old, 1);
        } else {
            unsetenv("TZ");
        }
        tzset();
    }
}

static int clock_init(PanelWidget *w)
{
    ClockPriv *cp = w->priv;
    if (!kv_get(w->config_kv, "format", cp->format, sizeof(cp->format))) {
        snprintf(cp->format, sizeof(cp->format), "%%H:%%M");
    }
    unescape_newlines(cp->format);
    kv_get(w->config_kv, "tz", cp->tz, sizeof(cp->tz));
    char capbuf[8];
    cp->capitalize = !(kv_get(w->config_kv, "capitalize", capbuf, sizeof(capbuf)) && !strcmp(capbuf, "no"));

    char list[512];
    cp->n_tooltip_tz = 0;
    if (kv_get(w->config_kv, "tooltip_tz", list, sizeof(list))) {
        char *save = NULL;
        for (char *tok = strtok_r(list, ",", &save); tok && cp->n_tooltip_tz < CLOCK_MAX_TOOLTIP_TZ;
             tok = strtok_r(NULL, ",", &save)) {
            snprintf(cp->tooltip_tz[cp->n_tooltip_tz], sizeof(cp->tooltip_tz[0]), "%s", tok);
            cp->n_tooltip_tz++;
        }
    }

    cp->text[0] = 0;
    w->next_tick_ms = now_ms(); /* paint something immediately */
    return 0;
}

static int clock_on_tick(PanelWidget *w, uint64_t now)
{
    ClockPriv *cp = w->priv;
    w->next_tick_ms = now + 1000;
    char old[sizeof(cp->text)];
    snprintf(old, sizeof(old), "%s", cp->text);
    struct tm tmv;
    localtime_in_tz(cp->tz, &tmv);
    strftime(cp->text, sizeof(cp->text), cp->format, &tmv);
    if (cp->capitalize) {
        capitalize_lines(cp->text);
    }
    /* Only repaint when the rendered string actually changed -- a
     * %H:%M clock ticks every second but its text changes once a
     * minute, so 59 of every 60 ticks now cost nothing. */
    return strcmp(old, cp->text) != 0;
}

static void clock_measure(PanelWidget *w, int cross_axis, int *out_len, int *out_min_len)
{
    ClockPriv *cp = w->priv;
    Panel *p = w->panel;
    const char *sample = cp->text[0] ? cp->text : "00:00";

    char copy[sizeof(cp->text)];
    const char *lines[CLOCK_MAX_LINES];
    int n_lines = clock_split_lines(sample, copy, sizeof(copy), lines);
    double size = clock_line_size(p, cross_axis, n_lines);

    double widest = 0;
    for (int i = 0; i < n_lines; i++) {
        double tw;
        pango_text_extents_ellipsized(p->cr, lines[i], size, 0, &tw, NULL);
        if (tw > widest) {
            widest = tw;
        }
    }
    *out_len = (int)widest + 16;
    *out_min_len = *out_len; /* a clipped clock is worse than useless -- don't shrink it */
}

static int clock_get_tooltip(PanelWidget *w, int local_x, char *buf, size_t bufsz, int *anchor_x, int *anchor_w,
                              int *out_closable, void **out_ctx)
{
    (void)local_x;
    (void)out_closable;
    (void)out_ctx;
    ClockPriv *cp = w->priv;

    if (cp->n_tooltip_tz == 0) {
        /* No extra zones configured -- same single weekday/date + time
         * pair as before, just following this widget's own tz= override
         * (if any) instead of always the system zone, so the tooltip
         * agrees with what the panel itself is showing. */
        struct tm tmv;
        localtime_in_tz(cp->tz, &tmv);
        char line1[96], line2[32];
        strftime(line1, sizeof(line1), "%A, %d de %B de %Y", &tmv);
        strftime(line2, sizeof(line2), "%H:%M:%S", &tmv);
        snprintf(buf, bufsz, "%s\n%s", line1, line2);
    } else {
        /* One block per configured zone: zone name, then date+time,
         * separated by a blank line from the next block -- the "padding"
         * asked for so a wall of stacked times doesn't run together. */
        size_t used = 0;
        for (int i = 0; i < cp->n_tooltip_tz && used < bufsz; i++) {
            struct tm tmv;
            localtime_in_tz(cp->tooltip_tz[i], &tmv);
            char line[128];
            strftime(line, sizeof(line), "%d de %B de %Y, %H:%M:%S", &tmv);
            int n = snprintf(buf + used, bufsz - used, "%s%s\n%s\n", i > 0 ? "\n" : "", cp->tooltip_tz[i], line);
            if (n < 0) {
                break;
            }
            used += (size_t)n;
        }
    }
    *anchor_x = 0;
    *anchor_w = w->len;
    return 1;
}

static void clock_paint(PanelWidget *w, cairo_t *cr)
{
    ClockPriv *cp = w->priv;
    Panel *p = w->panel;
    int x, y, width, height;
    widget_get_rect(w, &x, &y, &width, &height);
    widget_paint_hover_bg(w, cr);

    cairo_set_source_rgba(cr, p->fg_r, p->fg_g, p->fg_b, p->fg_a);

    char copy[sizeof(cp->text)];
    const char *lines[CLOCK_MAX_LINES];
    int n_lines = clock_split_lines(cp->text, copy, sizeof(copy), lines);
    double size = clock_line_size(p, height, n_lines);
    double band = (double)height / n_lines;

    for (int i = 0; i < n_lines; i++) {
        double tw;
        pango_text_extents_ellipsized(cr, lines[i], size, 0, &tw, NULL);
        pango_show_text_boxed(cr, x + (width - tw) / 2.0, y + i * band, band, 0, size, lines[i], NULL);
    }
}

const PanelWidgetOps clock_ops = {
    .type_name = "clock",
    .priv_size = sizeof(ClockPriv),
    .init = clock_init,
    .destroy = NULL,
    .measure = clock_measure,
    .paint = clock_paint,
    .on_button = NULL,
    .on_tick = clock_on_tick,
    .get_tooltip = clock_get_tooltip,
};
