/* conf.so - INI-style configuration (key = value, [sections]), JSON and
 * YAML. None of them carries state across lines. */
#include "common.h"

/* ---- INI / key=value ------------------------------------------------- */

static const char *const ini_names[] = {
    "*.conf", "*.ini", "*.cfg", "*.desktop", "*.service", "*.socket", "*.timer", "*.mount", "*.target",
    "*.path", "*.theme", "*.directory", "*.properties", "*.env", ".env", "*.inf", "*.repo", "*.list",
    "kdeglobals", "*rc", ".gitconfig", ".editorconfig", "*.kicfg", "*.widget", NULL,
};

static int ini_detect(const char *base, const char *first)
{
    (void)first;
    return match_name(base, ini_names, 40);
}

static const char *const ini_words[] = { "true", "false", "yes", "no", "on", "off", "TRUE", "FALSE", "True", "False", NULL };

static void ini_value(const char *t, int n, int i, KipadSpanFn span, void *ctx)
{
    while (i < n) {
        char c = t[i];
        if ((c == '#' || c == ';') && i > 0 && (t[i - 1] == ' ' || t[i - 1] == '\t')) {
            span(ctx, i, n, KS_COMMENT);
            return;
        }
        if (c == '"' || c == '\'') {
            int closed, e = scan_quoted(t, n, i + 1, c, c == '"', &closed, span, ctx);
            span(ctx, i, e, KS_STRING);
            i = e;
        } else if (c == '$' && i + 1 < n && (t[i + 1] == '{' || is_word(t[i + 1]))) {
            int e = i + 1;
            if (t[e] == '{') {
                while (e < n && t[e] != '}')
                    e++;
                e = e < n ? e + 1 : n;
            } else
                while (e < n && is_word(t[e]))
                    e++;
            span(ctx, i, e, KS_VARIABLE);
            i = e;
        } else if (number_starts(t, i)) {
            int e = scan_number(t, n, i);
            if (e == n || !is_word(t[e]))
                span(ctx, i, e, KS_NUMBER);
            i = e;
        } else if (is_word(c)) {
            int e = i;
            while (e < n && is_word(t[e]))
                e++;
            if (in_list(t + i, e - i, ini_words))
                span(ctx, i, e, KS_KEYWORD);
            i = e;
        } else
            i++;
    }
}

static int ini_line(const char *t, int n, int state, KipadSpanFn span, void *ctx)
{
    int i = 0;
    while (i < n && (t[i] == ' ' || t[i] == '\t'))
        i++;
    if (i == n)
        return state;
    if (t[i] == '#' || t[i] == ';') {
        span(ctx, i, n, KS_COMMENT);
        return state;
    }
    if (t[i] == '[') {
        const char *close = memchr(t + i, ']', (size_t)(n - i));
        span(ctx, i, close ? (int)(close - t) + 1 : n, KS_SECTION);
        return state;
    }
    int eq = i;
    while (eq < n && t[eq] != '=' && t[eq] != ':')
        eq++;
    if (eq == n) {
        ini_value(t, n, i, span, ctx);
        return state;
    }
    int ke = eq;
    while (ke > i && (t[ke - 1] == ' ' || t[ke - 1] == '\t'))
        ke--;
    span(ctx, i, ke, KS_KEY);
    span(ctx, eq, eq + 1, KS_OPERATOR);
    ini_value(t, n, eq + 1, span, ctx);
    return state;
}

static const KipadSyntax ini = { KIPAD_SYNTAX_ABI, "Config", ini_detect, ini_line };

/* ---- JSON ------------------------------------------------------------ */

static const char *const json_names[] = { "*.json", "*.jsonc", "*.json5", "*.geojson", "*.webmanifest", "*.ipynb",
                                          ".babelrc", ".eslintrc", NULL };

static int json_detect(const char *base, const char *first)
{
    (void)first;
    return match_name(base, json_names, 50);
}

static const char *const json_words[] = { "true", "false", "null", NULL };

static int json_line(const char *t, int n, int state, KipadSpanFn span, void *ctx)
{
    int i = 0;
    while (i < n) {
        char c = t[i];
        if (c == '"') {
            int closed, e = scan_quoted(t, n, i + 1, '"', 1, &closed, span, ctx);
            int k = e;
            while (k < n && (t[k] == ' ' || t[k] == '\t'))
                k++;
            span(ctx, i, e, k < n && t[k] == ':' ? KS_KEY : KS_STRING);
            i = e;
        } else if (c == '/' && i + 1 < n && t[i + 1] == '/') { /* jsonc */
            span(ctx, i, n, KS_COMMENT);
            break;
        } else if (isdigit((unsigned char)c) || (c == '-' && i + 1 < n && isdigit((unsigned char)t[i + 1]))) {
            int e = scan_number(t, n, c == '-' ? i + 1 : i);
            span(ctx, i, e, KS_NUMBER);
            i = e;
        } else if (is_word(c)) {
            int e = i;
            while (e < n && is_word(t[e]))
                e++;
            span(ctx, i, e, in_list(t + i, e - i, json_words) ? KS_KEYWORD : KS_ERROR);
            i = e;
        } else
            i++;
    }
    return state;
}

static const KipadSyntax json = { KIPAD_SYNTAX_ABI, "JSON", json_detect, json_line };

/* ---- YAML ------------------------------------------------------------ */

static const char *const yaml_names[] = { "*.yaml", "*.yml", ".clang-format", "*.kicompose", NULL };

static int yaml_detect(const char *base, const char *first)
{
    if (!strcmp(first, "---") || !strncmp(first, "%YAML", 5))
        return 30;
    return match_name(base, yaml_names, 50);
}

static const char *const yaml_words[] = { "true", "false", "null", "yes", "no", "on", "off", "True", "False", "~", NULL };

static int yaml_line(const char *t, int n, int state, KipadSpanFn span, void *ctx)
{
    int i = 0;
    while (i < n && (t[i] == ' ' || t[i] == '\t'))
        i++;
    if (i < n && t[i] == '#') {
        span(ctx, i, n, KS_COMMENT);
        return state;
    }
    if (n - i >= 3 && (!strncmp(t + i, "---", 3) || !strncmp(t + i, "...", 3))) {
        span(ctx, i, i + 3, KS_PREPROC);
        return state;
    }
    if (i < n && t[i] == '-' && (i + 1 == n || t[i + 1] == ' ')) {
        span(ctx, i, i + 1, KS_OPERATOR);
        for (i++; i < n && t[i] == ' '; i++)
            ;
    }
    /* key: (a colon followed by a space or the line end, outside quotes) */
    int k = i;
    while (k < n && t[k] != ':' && t[k] != '"' && t[k] != '\'' && t[k] != '#')
        k++;
    if (k < n && t[k] == ':' && (k + 1 == n || t[k + 1] == ' ')) {
        span(ctx, i, k, KS_KEY);
        i = k + 1;
    }
    while (i < n) {
        char c = t[i];
        if (c == '#' && (t[i - 1] == ' ' || t[i - 1] == '\t')) {
            span(ctx, i, n, KS_COMMENT);
            break;
        }
        if (c == '"' || c == '\'') {
            int closed, e = scan_quoted(t, n, i + 1, c, c == '"', &closed, span, ctx);
            span(ctx, i, e, KS_STRING);
            i = e;
        } else if ((c == '&' || c == '*') && i + 1 < n && is_word(t[i + 1])) {
            int e = i + 1;
            while (e < n && (is_word(t[e]) || t[e] == '-'))
                e++;
            span(ctx, i, e, KS_VARIABLE);
            i = e;
        } else if (c == '|' || c == '>') {
            span(ctx, i, i + 1, KS_OPERATOR);
            i++;
        } else if (number_starts(t, i) || is_word(c) || c == '~') {
            int e = i;
            while (e < n && t[e] != ' ' && t[e] != ',' && t[e] != ']' && t[e] != '}')
                e++;
            if (in_list(t + i, e - i, yaml_words))
                span(ctx, i, e, KS_KEYWORD);
            else if (number_starts(t, i) && scan_number(t, n, i) == e)
                span(ctx, i, e, KS_NUMBER);
            i = e;
        } else
            i++;
    }
    return state;
}

static const KipadSyntax yaml = { KIPAD_SYNTAX_ABI, "YAML", yaml_detect, yaml_line };

SYNTAX_LIST(&ini, &json, &yaml)
