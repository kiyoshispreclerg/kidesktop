/* common.h - small helpers shared by kipad's own syntax modules (each .so
 * gets its own copy; nothing here is exported). */
#ifndef KIPAD_SYNTAX_COMMON_H
#define KIPAD_SYNTAX_COMMON_H

#include "../kipad_syntax.h"

#include <ctype.h>
#include <fnmatch.h>
#include <string.h>

static inline int is_word(char c)
{
    return isalnum((unsigned char)c) || c == '_' || (unsigned char)c >= 0x80;
}

/* Score `score` when `base` matches one of the NULL-terminated patterns. */
static inline int match_name(const char *base, const char *const *pats, int score)
{
    for (int i = 0; pats[i]; i++)
        if (fnmatch(pats[i], base, 0) == 0)
            return score;
    return 0;
}

/* Score 80 when the #! line runs one of `progs` (directly or via env). */
static inline int match_shebang(const char *first, const char *const *progs)
{
    if (strncmp(first, "#!", 2))
        return 0;
    const char *p = first + 2, *end;
    while (*p == ' ')
        p++;
    for (end = p; *end && *end != ' '; end++)
        ;
    const char *prog = p;
    for (const char *q = p; q < end; q++)
        if (*q == '/')
            prog = q + 1;
    if ((size_t)(end - prog) == 3 && !strncmp(prog, "env", 3)) {
        for (prog = end; *prog == ' '; prog++)
            ;
        if (!strncmp(prog, "-S ", 3))
            for (prog += 3; *prog == ' '; prog++)
                ;
        for (end = prog; *end && *end != ' '; end++)
            ;
    }
    for (int i = 0; progs[i]; i++) {
        size_t n = strlen(progs[i]);
        /* python3, python3.11 count as python */
        if ((size_t)(end - prog) >= n && !strncmp(prog, progs[i], n) &&
            (prog + n == end || isdigit((unsigned char)prog[n]) || prog[n] == '.'))
            return 80;
    }
    return 0;
}

static inline int in_list(const char *s, int n, const char *const *words)
{
    for (int i = 0; words[i]; i++)
        if ((int)strlen(words[i]) == n && !memcmp(words[i], s, (size_t)n))
            return 1;
    return 0;
}

/* End of a number starting at i (decimal, hex, binary, float, suffixes). */
static inline int scan_number(const char *t, int n, int i)
{
    if (i + 1 < n && t[i] == '0' && (t[i + 1] == 'x' || t[i + 1] == 'X' || t[i + 1] == 'b' || t[i + 1] == 'B' ||
                                     t[i + 1] == 'o' || t[i + 1] == 'O')) {
        i += 2;
        while (i < n && (isxdigit((unsigned char)t[i]) || t[i] == '_'))
            i++;
    } else {
        while (i < n && (isdigit((unsigned char)t[i]) || t[i] == '_' || t[i] == '.'))
            i++;
        if (i < n && (t[i] == 'e' || t[i] == 'E')) {
            int j = i + 1;
            if (j < n && (t[j] == '+' || t[j] == '-'))
                j++;
            if (j < n && isdigit((unsigned char)t[j]))
                for (i = j; i < n && isdigit((unsigned char)t[i]); i++)
                    ;
        }
    }
    while (i < n && isalpha((unsigned char)t[i])) /* 10u, 1.5f, 3i64 */
        i++;
    return i;
}

/* A number starts here (not in the middle of a word). */
static inline int number_starts(const char *t, int i)
{
    return isdigit((unsigned char)t[i]) && (i == 0 || !is_word(t[i - 1]));
}

/* End of a quoted string starting after the opening quote at i; *closed tells
 * whether the closing quote was on this line. Escapes are reported. */
static inline int scan_quoted(const char *t, int n, int i, char q, int escapes, int *closed, KipadSpanFn span,
                              void *ctx)
{
    *closed = 0;
    while (i < n) {
        if (escapes && t[i] == '\\' && i + 1 < n) {
            span(ctx, i, i + 2, KS_ESCAPE);
            i += 2;
            continue;
        }
        if (t[i] == q) {
            *closed = 1;
            return i + 1;
        }
        i++;
    }
    return n;
}

#define SYNTAX_LIST(...)                                                                                            \
    const KipadSyntax *const *kipad_syntax_list(void)                                                              \
    {                                                                                                               \
        static const KipadSyntax *const list[] = { __VA_ARGS__, NULL };                                             \
        return list;                                                                                                \
    }

#endif
