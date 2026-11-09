/* python.so - Python.
 *
 * State: 0 plain, ST_TDQ inside """...""", ST_TSQ inside '''...'''. */
#include "common.h"

#define ST_TDQ 1
#define ST_TSQ 2

static const char *const names[] = { "*.py", "*.pyw", "*.pyi", "SConstruct", "SConscript", "wscript", NULL };
static const char *const progs[] = { "python", "pypy", NULL };

static int detect(const char *base, const char *first)
{
    int s = match_shebang(first, progs);
    return s ? s : match_name(base, names, 50);
}

static const char *const keywords[] = {
    "and", "as", "assert", "async", "await", "break", "class", "continue", "def", "del", "elif", "else", "except",
    "finally", "for", "from", "global", "if", "import", "in", "is", "lambda", "nonlocal", "not", "or", "pass",
    "raise", "return", "try", "while", "with", "yield", "match", "case", NULL,
};
static const char *const constants[] = { "True", "False", "None", "Ellipsis", "NotImplemented", "__name__",
                                         "__file__", NULL };
static const char *const builtins[] = {
    "print", "len", "range", "open", "str", "int", "float", "bool", "list", "dict", "set", "tuple", "bytes",
    "type", "isinstance", "issubclass", "super", "object", "enumerate", "zip", "map", "filter", "sorted",
    "reversed", "min", "max", "sum", "abs", "any", "all", "iter", "next", "repr", "format", "getattr", "setattr",
    "hasattr", "input", "round", "self", "cls", NULL,
};

/* Searches the closing triple quote q q q from i; returns its end or -1. */
static int find_triple(const char *t, int n, int i, char q, KipadSpanFn span, void *ctx)
{
    while (i < n) {
        if (t[i] == '\\' && i + 1 < n) {
            span(ctx, i, i + 2, KS_ESCAPE);
            i += 2;
        } else if (i + 2 < n && t[i] == q && t[i + 1] == q && t[i + 2] == q)
            return i + 3;
        else
            i++;
    }
    return -1;
}

static int line(const char *t, int n, int state, KipadSpanFn span, void *ctx)
{
    int i = 0;
    if (state) {
        char q = state == ST_TDQ ? '"' : '\'';
        int e = find_triple(t, n, 0, q, span, ctx);
        span(ctx, 0, e < 0 ? n : e, KS_STRING);
        if (e < 0)
            return state;
        i = e;
    }
    int after_def = 0, after_class = 0;
    while (i < n) {
        char c = t[i];
        if (c == '#') {
            span(ctx, i, n, i == 0 && n > 1 && t[1] == '!' ? KS_PREPROC : KS_COMMENT);
            break;
        }
        /* string prefixes: r, b, f, u, rb, fr... */
        int p = i;
        while (p < n && p - i < 2 && strchr("rRbBfFuU", t[p]))
            p++;
        if (p < n && (t[p] == '"' || t[p] == '\'') && (p == i || i == 0 || !is_word(t[i - 1]))) {
            char q = t[p];
            int raw = memchr(t + i, 'r', (size_t)(p - i)) || memchr(t + i, 'R', (size_t)(p - i));
            if (p + 2 < n && t[p + 1] == q && t[p + 2] == q) {
                int e = find_triple(t, n, p + 3, q, span, ctx);
                span(ctx, i, e < 0 ? n : e, KS_STRING);
                if (e < 0)
                    return q == '"' ? ST_TDQ : ST_TSQ;
                i = e;
            } else {
                int closed, e = scan_quoted(t, n, p + 1, q, !raw, &closed, span, ctx);
                span(ctx, i, e, KS_STRING);
                i = e;
            }
            continue;
        }
        if (c == '@' && (i == 0 || t[i - 1] == ' ' || t[i - 1] == '\t')) {
            int e = i + 1;
            while (e < n && (is_word(t[e]) || t[e] == '.'))
                e++;
            span(ctx, i, e, KS_PREPROC);
            i = e;
            continue;
        }
        if (number_starts(t, i)) {
            int e = scan_number(t, n, i);
            span(ctx, i, e, KS_NUMBER);
            i = e;
            continue;
        }
        if (is_word(c)) {
            int e = i;
            while (e < n && is_word(t[e]))
                e++;
            if (after_def)
                span(ctx, i, e, KS_FUNCTION);
            else if (after_class)
                span(ctx, i, e, KS_TYPE);
            else if (in_list(t + i, e - i, keywords))
                span(ctx, i, e, KS_KEYWORD);
            else if (in_list(t + i, e - i, constants))
                span(ctx, i, e, KS_NUMBER);
            else if (in_list(t + i, e - i, builtins))
                span(ctx, i, e, KS_FUNCTION);
            after_def = e - i == 3 && !memcmp(t + i, "def", 3);
            after_class = e - i == 5 && !memcmp(t + i, "class", 5);
            i = e;
            continue;
        }
        if (strchr("+-*/%=<>!&|^~:", c))
            span(ctx, i, i + 1, KS_OPERATOR);
        i++;
    }
    return 0;
}

static const KipadSyntax py = { KIPAD_SYNTAX_ABI, "Python", detect, line };

SYNTAX_LIST(&py)
