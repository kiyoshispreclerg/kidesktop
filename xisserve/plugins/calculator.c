/*
 * calculator.c - search plugin: if the entire query parses as a basic
 * arithmetic expression -- the four basic operators plus power (^) and
 * sqrt(...), parentheses for grouping, at the usual sqrt/power > times/
 * divide > plus/minus precedence -- offers one result showing the
 * value. Clicking (or Enter) copies it to the clipboard rather than
 * launching anything.
 */
#include "../xisserve.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct {
    const char *p;
    gboolean ok;
} CalcCursor;

static void calc_skip_ws(CalcCursor *c)
{
    while (*c->p == ' ' || *c->p == '\t') c->p++;
}

static double calc_expr(CalcCursor *c);

static double calc_primary(CalcCursor *c)
{
    calc_skip_ws(c);
    if (*c->p == '-') { c->p++; return -calc_primary(c); }
    if (*c->p == '+') { c->p++; return calc_primary(c); }
    if (*c->p == '(') {
        c->p++;
        double v = calc_expr(c);
        calc_skip_ws(c);
        if (*c->p != ')') { c->ok = FALSE; return 0; }
        c->p++;
        return v;
    }
    if (strncasecmp(c->p, "sqrt", 4) == 0) {
        c->p += 4;
        calc_skip_ws(c);
        if (*c->p != '(') { c->ok = FALSE; return 0; }
        c->p++;
        double v = calc_expr(c);
        calc_skip_ws(c);
        if (*c->p != ')') { c->ok = FALSE; return 0; }
        c->p++;
        if (v < 0) { c->ok = FALSE; return 0; }
        return sqrt(v);
    }
    char *end = NULL;
    double v = strtod(c->p, &end);
    if (end == c->p) { c->ok = FALSE; return 0; }
    c->p = end;
    return v;
}

/* Right-associative, above * / in precedence -- 2^3^2 == 2^(3^2). */
static double calc_power(CalcCursor *c)
{
    double base = calc_primary(c);
    calc_skip_ws(c);
    if (*c->p == '^') {
        c->p++;
        return pow(base, calc_power(c));
    }
    return base;
}

static double calc_term(CalcCursor *c)
{
    double v = calc_power(c);
    for (;;) {
        calc_skip_ws(c);
        if (*c->p == '*') {
            c->p++;
            v *= calc_power(c);
        } else if (*c->p == '/') {
            c->p++;
            double d = calc_power(c);
            if (d == 0) { c->ok = FALSE; return 0; }
            v /= d;
        } else {
            break;
        }
    }
    return v;
}

static double calc_expr(CalcCursor *c)
{
    double v = calc_term(c);
    for (;;) {
        calc_skip_ws(c);
        if (*c->p == '+') { c->p++; v += calc_term(c); }
        else if (*c->p == '-') { c->p++; v -= calc_term(c); }
        else break;
    }
    return v;
}

/* TRUE if `query` could plausibly be a math expression at all -- every
 * character is a digit, whitespace, an operator/paren, or one of the
 * letters in "sqrt" -- cheap enough to run on every keystroke before
 * the real (and only slightly less cheap) recursive-descent parse.
 * Requires at least one digit so a bare "sqrt" or "()" is left alone. */
static gboolean looks_like_math(const char *query)
{
    gboolean has_digit = FALSE;
    for (const char *p = query; *p; p++) {
        if (isdigit((unsigned char)*p)) { has_digit = TRUE; continue; }
        if (strchr(".+-*/^() \t", *p)) continue;
        if (strchr("sqrtSQRT", *p)) continue;
        return FALSE;
    }
    return has_digit;
}

static GdkPixbuf *calculator_icon(void)
{
    return xisserve_resolve_icon("accessories-calculator", XISSERVE_ICON_PX);
}

/* self->exec holds the formatted result (never launched as a shell
 * command -- this activate_fn replaces the default exec-based launch),
 * copied to the clipboard instead of running anything. */
static void calc_copy_activate(ResultEntry *self)
{
    GtkClipboard *cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    gtk_clipboard_set_text(cb, self->exec, -1);
}

void plugin_calculator_search(const char *query, GPtrArray *results)
{
    if (!looks_like_math(query)) return;

    CalcCursor c = { query, TRUE };
    double result = calc_expr(&c);
    calc_skip_ws(&c);
    if (!c.ok || *c.p != 0) return; /* leftover/invalid input -- not a clean expression */

    char formatted[64];
    if (result == (long long)result && fabs(result) < 1e15) {
        snprintf(formatted, sizeof(formatted), "%lld", (long long)result);
    } else {
        snprintf(formatted, sizeof(formatted), "%.10g", result);
    }

    ResultEntry *e = g_new0(ResultEntry, 1);
    snprintf(e->name, sizeof(e->name), "= %s", formatted);
    snprintf(e->subtitle, sizeof(e->subtitle), "Calculadora -- clique para copiar");
    snprintf(e->exec, sizeof(e->exec), "%s", formatted);
    e->from_desktop = FALSE;
    e->icon = calculator_icon();
    e->activate_fn = calc_copy_activate;
    g_ptr_array_add(results, e);
}
