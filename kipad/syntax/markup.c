/* markup.so - XML/HTML, Markdown and LaTeX (with its math). */
#include "common.h"

#include <strings.h>

/* ---- XML / HTML ------------------------------------------------------ */

/* State: 0 text, X_COMMENT in <!-- -->, X_TAG inside a tag's attributes,
 * X_DQ / X_SQ inside a quoted attribute value. */
enum { X_TEXT, X_COMMENT, X_TAG, X_DQ, X_SQ };

static const char *const xml_names[] = {
    "*.xml", "*.html", "*.htm", "*.xhtml", "*.svg", "*.xsl", "*.xslt", "*.xsd", "*.rss", "*.atom", "*.ui",
    "*.glade", "*.policy", "*.plist", "*.kml", "*.gpx", "*.vue", "*.metainfo.xml", "*.appdata.xml", "*.menu",
    "*.xbel", "*.qrc", NULL,
};

static int xml_detect(const char *base, const char *first)
{
    if (!strncmp(first, "<?xml", 5) || !strncasecmp(first, "<!DOCTYPE", 9))
        return 80;
    return match_name(base, xml_names, 50);
}

static int xml_line(const char *t, int n, int state, KipadSpanFn span, void *ctx)
{
    int i = 0;
    while (i < n) {
        if (state == X_COMMENT) {
            int end = -1;
            for (int k = i; k + 2 < n; k++)
                if (t[k] == '-' && t[k + 1] == '-' && t[k + 2] == '>') {
                    end = k + 3;
                    break;
                }
            span(ctx, i, end < 0 ? n : end, KS_COMMENT);
            if (end < 0)
                return X_COMMENT;
            i = end;
            state = X_TEXT;
            continue;
        }
        if (state == X_DQ || state == X_SQ) {
            char q = state == X_DQ ? '"' : '\'';
            const char *e = memchr(t + i, q, (size_t)(n - i));
            int end = e ? (int)(e - t) + 1 : n;
            span(ctx, i, end, KS_STRING);
            i = end;
            if (!e)
                return state;
            state = X_TAG;
            continue;
        }
        char c = t[i];
        if (state == X_TAG) {
            if (c == '>' || (c == '/' && i + 1 < n && t[i + 1] == '>') || (c == '?' && i + 1 < n && t[i + 1] == '>')) {
                int e = c == '>' ? i + 1 : i + 2;
                span(ctx, i, e, KS_TAG);
                i = e;
                state = X_TEXT;
            } else if (c == '"' || c == '\'') {
                span(ctx, i, i + 1, KS_STRING);
                i++;
                state = c == '"' ? X_DQ : X_SQ;
            } else if (is_word(c) || c == ':' || c == '-') {
                int e = i;
                while (e < n && (is_word(t[e]) || t[e] == ':' || t[e] == '-' || t[e] == '.'))
                    e++;
                span(ctx, i, e, KS_KEY);
                i = e;
            } else {
                if (c == '=')
                    span(ctx, i, i + 1, KS_OPERATOR);
                i++;
            }
            continue;
        }
        if (c == '<' && i + 3 < n && !strncmp(t + i, "<!--", 4)) {
            state = X_COMMENT;
            continue;
        }
        if (c == '<' && i + 1 < n && (is_word(t[i + 1]) || strchr("/?!", t[i + 1]))) {
            int e = i + 1;
            if (strchr("/?!", t[e]))
                e++;
            while (e < n && (is_word(t[e]) || t[e] == ':' || t[e] == '-' || t[e] == '.'))
                e++;
            span(ctx, i, e, KS_TAG);
            i = e;
            state = X_TAG;
            continue;
        }
        if (c == '&') {
            int e = i + 1;
            while (e < n && (is_word(t[e]) || t[e] == '#'))
                e++;
            if (e < n && t[e] == ';' && e > i + 1) {
                span(ctx, i, e + 1, KS_ESCAPE);
                i = e + 1;
                continue;
            }
        }
        i++;
    }
    return state;
}

static const KipadSyntax xml = { KIPAD_SYNTAX_ABI, "XML/HTML", xml_detect, xml_line };

/* ---- Markdown -------------------------------------------------------- */

#define MD_FENCE 1 /* inside a ``` or ~~~ block */

static const char *const md_names[] = { "*.md", "*.markdown", "*.mkd", "*.mdown", "README", NULL };

static int md_detect(const char *base, const char *first)
{
    (void)first;
    return match_name(base, md_names, 50);
}

static int is_fence(const char *t, int n)
{
    int i = 0;
    while (i < n && i < 3 && t[i] == ' ')
        i++;
    return n - i >= 3 && (!strncmp(t + i, "```", 3) || !strncmp(t + i, "~~~", 3));
}

static void md_inline(const char *t, int n, int i, KipadSpanFn span, void *ctx)
{
    while (i < n) {
        char c = t[i];
        if (c == '\\' && i + 1 < n && ispunct((unsigned char)t[i + 1])) {
            span(ctx, i, i + 2, KS_ESCAPE);
            i += 2;
        } else if (c == '`') {
            const char *e = memchr(t + i + 1, '`', (size_t)(n - i - 1));
            int end = e ? (int)(e - t) + 1 : n;
            span(ctx, i, end, KS_STRING);
            i = end;
        } else if ((c == '*' || c == '_') && i + 1 < n && t[i + 1] != ' ' &&
                   (c == '*' || i == 0 || !is_word(t[i - 1]))) {
            /* *em*, **strong**, _em_: up to the same run of markers */
            int k = i;
            while (k < n && t[k] == c)
                k++;
            int run = k - i, e = -1;
            for (int j = k + 1; j + run <= n; j++)
                if (t[j] == c && t[j - 1] != ' ') {
                    int r = 0;
                    while (j + r < n && t[j + r] == c)
                        r++;
                    if (r >= run) {
                        e = j + run;
                        break;
                    }
                    j += r;
                }
            if (e > 0) {
                span(ctx, i, e, KS_EMPHASIS);
                md_inline(t, e - run, k, span, ctx); /* links and code inside */
                i = e;
            } else
                i = k;
        } else if (c == '[' || (c == '!' && i + 1 < n && t[i + 1] == '[')) {
            /* [text](url) and ![alt](url) */
            const char *cb = memchr(t + i, ']', (size_t)(n - i));
            int j = cb ? (int)(cb - t) + 1 : n;
            if (j < n && (t[j] == '(' || t[j] == '[')) {
                const char *ce = memchr(t + j, t[j] == '(' ? ')' : ']', (size_t)(n - j));
                int end = ce ? (int)(ce - t) + 1 : n;
                span(ctx, i, j, KS_KEY);
                span(ctx, j, end, KS_LINK);
                i = end;
            } else
                i++;
        } else if (c == '<' && i + 1 < n && (!strncmp(t + i + 1, "http", 4) || !strncmp(t + i + 1, "mailto:", 7))) {
            const char *e = memchr(t + i, '>', (size_t)(n - i));
            int end = e ? (int)(e - t) + 1 : n;
            span(ctx, i, end, KS_LINK);
            i = end;
        } else
            i++;
    }
}

static int md_line(const char *t, int n, int state, KipadSpanFn span, void *ctx)
{
    if (is_fence(t, n)) {
        span(ctx, 0, n, KS_PREPROC);
        return state == MD_FENCE ? 0 : MD_FENCE;
    }
    if (state == MD_FENCE) {
        span(ctx, 0, n, KS_STRING);
        return state;
    }
    int i = 0;
    while (i < n && t[i] == ' ')
        i++;
    if (i >= 4) { /* indented code */
        span(ctx, 0, n, KS_STRING);
        return 0;
    }
    if (i < n && t[i] == '#') {
        int k = i;
        while (k < n && t[k] == '#')
            k++;
        if (k - i <= 6 && (k == n || t[k] == ' ')) {
            span(ctx, i, n, KS_SECTION);
            return 0;
        }
    }
    if (i < n && t[i] == '>') {
        span(ctx, i, i + 1, KS_OPERATOR);
        span(ctx, i + 1, n, KS_COMMENT);
        md_inline(t, n, i + 1, span, ctx);
        return 0;
    }
    /* --- / *** rules and === / --- under a heading */
    int k = i, same = 1;
    while (k < n && (t[k] == t[i] || t[k] == ' '))
        k++;
    same = k == n && n - i >= 3 && strchr("-*_=", t[i]);
    if (same) {
        span(ctx, i, n, KS_OPERATOR);
        return 0;
    }
    if (i + 1 < n && strchr("-*+", t[i]) && t[i + 1] == ' ') {
        span(ctx, i, i + 1, KS_OPERATOR);
        i += 2;
    } else {
        int d = i;
        while (d < n && isdigit((unsigned char)t[d]))
            d++;
        if (d > i && d + 1 < n && (t[d] == '.' || t[d] == ')') && t[d + 1] == ' ') {
            span(ctx, i, d + 1, KS_OPERATOR);
            i = d + 2;
        }
    }
    md_inline(t, n, i, span, ctx);
    return 0;
}

static const KipadSyntax md = { KIPAD_SYNTAX_ABI, "Markdown", md_detect, md_line };

/* ---- LaTeX ----------------------------------------------------------- */

/* State: 0 text, or the math mode a line ends in. */
enum { T_TEXT, T_INLINE, T_DISPLAY, T_BRACKET };

static const char *const tex_names[] = { "*.tex", "*.ltx", "*.sty", "*.cls", "*.bib", "*.dtx", "*.ins", NULL };

static int tex_detect(const char *base, const char *first)
{
    (void)first;
    return match_name(base, tex_names, 50);
}

static const char *const tex_sections[] = { "part", "chapter", "section", "subsection", "subsubsection", "paragraph",
                                            "title", "caption", NULL };
static const char *const tex_envs[] = { "begin", "end", NULL };
static const char *const tex_math_envs[] = { "equation", "equation*", "align", "align*", "gather", "gather*",
                                             "multline", "multline*", "math", "displaymath", "eqnarray", NULL };

/* "{arg}" from i, returns its end (or i when there is none) */
static int brace_arg(const char *t, int n, int i, int *as, int *ae)
{
    if (i >= n || t[i] != '{')
        return i;
    const char *e = memchr(t + i, '}', (size_t)(n - i));
    *as = i + 1;
    *ae = e ? (int)(e - t) : n;
    return e ? *ae + 1 : n;
}

static int tex_line(const char *t, int n, int state, KipadSpanFn span, void *ctx)
{
    int i = 0;
    while (i < n) {
        char c = t[i];
        if (c == '%') {
            span(ctx, i, n, KS_COMMENT);
            break;
        }
        int math = state != T_TEXT;
        /* math delimiters: $ $$ \( \) \[ \] */
        if (c == '$') {
            int dbl = i + 1 < n && t[i + 1] == '$';
            span(ctx, i, i + 1 + dbl, KS_OPERATOR);
            state = math ? T_TEXT : dbl ? T_DISPLAY : T_INLINE;
            i += 1 + dbl;
            continue;
        }
        if (c == '\\' && i + 1 < n && strchr("()[]", t[i + 1])) {
            span(ctx, i, i + 2, KS_OPERATOR);
            state = (t[i + 1] == '(' || t[i + 1] == '[') ? (t[i + 1] == '[' ? T_BRACKET : T_INLINE) : T_TEXT;
            i += 2;
            continue;
        }
        if (c == '\\') {
            int e = i + 1;
            if (e < n && !isalpha((unsigned char)t[e])) {
                span(ctx, i, e + 1, KS_ESCAPE); /* \% \{ \\ */
                i = e + 1;
                continue;
            }
            while (e < n && isalpha((unsigned char)t[e]))
                e++;
            if (e < n && t[e] == '*')
                e++;
            int as = 0, ae = 0;
            if (in_list(t + i + 1, e - i - 1, tex_envs)) {
                span(ctx, i, e, KS_KEYWORD);
                int end = brace_arg(t, n, e, &as, &ae);
                if (end > e) {
                    span(ctx, as, ae, KS_TYPE);
                    /* math environments switch the following lines to math */
                    if (in_list(t + as, ae - as, tex_math_envs))
                        state = t[i + 1] == 'b' ? T_BRACKET : T_TEXT;
                }
                i = end;
            } else if (math) {
                span(ctx, i, e, KS_FUNCTION); /* \frac, \alpha, \sum */
                i = e;
            } else if (in_list(t + i + 1, e - i - 1, tex_sections) || (e > i + 1 && t[e - 1] == '*' &&
                                                                       in_list(t + i + 1, e - i - 2, tex_sections))) {
                span(ctx, i, e, KS_KEYWORD);
                int end = brace_arg(t, n, e, &as, &ae);
                if (end > e)
                    span(ctx, as, ae, KS_SECTION);
                i = end;
            } else {
                span(ctx, i, e, KS_KEYWORD);
                i = e;
            }
            continue;
        }
        if (math) {
            if (isdigit((unsigned char)c)) {
                int e = scan_number(t, n, i);
                span(ctx, i, e, KS_NUMBER);
                i = e;
                continue;
            }
            if (isalpha((unsigned char)c))
                span(ctx, i, i + 1, KS_VARIABLE);
            else if (strchr("+-*/=<>^_!|&", c))
                span(ctx, i, i + 1, KS_OPERATOR);
        } else if (c == '{' || c == '}')
            span(ctx, i, i + 1, KS_OPERATOR);
        i++;
    }
    return state;
}

static const KipadSyntax tex = { KIPAD_SYNTAX_ABI, "LaTeX", tex_detect, tex_line };

SYNTAX_LIST(&xml, &md, &tex)
