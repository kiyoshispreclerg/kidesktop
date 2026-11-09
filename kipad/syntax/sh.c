/* sh.so - POSIX shell and bash.
 *
 * State: 0 plain, ST_DQ inside "...", ST_SQ inside '...', or ST_HEREDOC
 * plus a hash of the here-document's delimiter. */
#include "common.h"

#define ST_DQ 1
#define ST_SQ 2
#define ST_HEREDOC 0x10000 /* | (hash & 0xffff), | ST_HD_TABS for <<- */
#define ST_HD_TABS 0x20000

static const char *const names[] = {
    "*.sh", "*.bash", "*.zsh", "*.ksh", ".bashrc", ".bash_profile", ".bash_logout", ".bash_aliases", ".profile",
    ".zshrc", ".zprofile", ".xinitrc", ".xsession", ".xprofile", "*.ebuild", "PKGBUILD", "configure", NULL,
};
static const char *const progs[] = { "sh", "bash", "dash", "zsh", "ksh", "ash", "busybox", NULL };

static int detect(const char *base, const char *first)
{
    int s = match_shebang(first, progs);
    return s ? s : match_name(base, names, 50);
}

static const char *const keywords[] = {
    "if", "then", "else", "elif", "fi", "for", "while", "until", "do", "done", "case", "esac", "in",
    "function", "select", "return", "local", "export", "readonly", "declare", "typeset", "unset", "shift",
    "exit", "break", "continue", "time", "!", NULL,
};
static const char *const builtins[] = {
    "echo", "printf", "read", "cd", "test", "eval", "exec", "source", "trap", "set", "alias", "unalias", "wait",
    "kill", "pwd", "getopts", "command", "type", "hash", "umask", "ulimit", "let", "mapfile", "true", "false", NULL,
};

static int delim_hash(const char *s, int n)
{
    unsigned h = 5381;
    for (int i = 0; i < n; i++)
        h = h * 33 + (unsigned char)s[i];
    return (int)(h & 0xffff);
}

/* $name, ${...}, $1, $@, $(...) opener; returns the end, or i when none. */
static int scan_var(const char *t, int n, int i)
{
    if (t[i] != '$' || i + 1 >= n)
        return i;
    int e = i + 1;
    if (t[e] == '{') {
        while (e < n && t[e] != '}')
            e++;
        return e < n ? e + 1 : n;
    }
    if (t[e] == '(')
        return e + 1;
    if (strchr("@*#?$!-0123456789", t[e]))
        return e + 1;
    while (e < n && is_word(t[e]))
        e++;
    return e > i + 1 ? e : i;
}

/* Inside "...": escapes and variables until the closing quote. */
static int scan_dq(const char *t, int n, int i, int *closed, KipadSpanFn span, void *ctx)
{
    int start = i;
    *closed = 0;
    while (i < n) {
        if (t[i] == '\\' && i + 1 < n) {
            span(ctx, i, i + 2, KS_ESCAPE);
            i += 2;
        } else if (t[i] == '$') {
            int e = scan_var(t, n, i);
            if (e > i)
                span(ctx, i, e, KS_VARIABLE);
            i = e > i ? e : i + 1;
        } else if (t[i] == '"') {
            *closed = 1;
            i++;
            break;
        } else
            i++;
    }
    span(ctx, start, i, KS_STRING);
    return i;
}

static int line(const char *t, int n, int state, KipadSpanFn span, void *ctx)
{
    int i = 0, closed;
    if (state & ST_HEREDOC) {
        int s = 0;
        if (state & ST_HD_TABS)
            while (s < n && t[s] == '\t')
                s++;
        span(ctx, 0, n, KS_STRING);
        return delim_hash(t + s, n - s) == (state & 0xffff) ? 0 : state;
    }
    if (state == ST_SQ) {
        const char *q = memchr(t, '\'', (size_t)n);
        i = q ? (int)(q - t) + 1 : n;
        span(ctx, 0, i, KS_STRING);
        if (!q)
            return ST_SQ;
    } else if (state == ST_DQ) {
        i = scan_dq(t, n, 0, &closed, span, ctx);
        if (!closed)
            return ST_DQ;
    }
    int next = 0; /* a here-document starts on the next line */
    int cmd_pos = 1; /* at the start of a command: a keyword or name() may follow */
    while (i < n) {
        char c = t[i];
        if (c == '#' && (i == 0 || t[i - 1] == ' ' || t[i - 1] == '\t' || t[i - 1] == ';')) {
            span(ctx, i, n, i == 0 && n > 1 && t[1] == '!' ? KS_PREPROC : KS_COMMENT);
            break;
        }
        if (c == '\\' && i + 1 < n) {
            i += 2;
            continue;
        }
        if (c == '\'') {
            const char *q = memchr(t + i + 1, '\'', (size_t)(n - i - 1));
            int e = q ? (int)(q - t) + 1 : n;
            span(ctx, i, e, KS_STRING);
            if (!q)
                return next ? next : ST_SQ;
            i = e;
            cmd_pos = 0;
            continue;
        }
        if (c == '"') {
            span(ctx, i, i + 1, KS_STRING); /* the opening quote */
            i = scan_dq(t, n, i + 1, &closed, span, ctx);
            if (!closed)
                return next ? next : ST_DQ;
            cmd_pos = 0;
            continue;
        }
        if (c == '$') {
            int e = scan_var(t, n, i);
            if (e > i) {
                span(ctx, i, e, KS_VARIABLE);
                i = e;
                cmd_pos = t[e - 1] == '(';
                continue;
            }
        }
        if (c == '<' && i + 1 < n && t[i + 1] == '<' && (i + 2 >= n || t[i + 2] != '<')) {
            int j = i + 2, tabs = 0;
            if (j < n && t[j] == '-') {
                tabs = 1;
                j++;
            }
            while (j < n && t[j] == ' ')
                j++;
            int q = j < n && (t[j] == '\'' || t[j] == '"');
            int ds = j + q, de = ds;
            while (de < n && (is_word(t[de]) || t[de] == '-' || t[de] == '.'))
                de++;
            span(ctx, i, de + (q && de < n), KS_OPERATOR);
            if (de > ds)
                next = ST_HEREDOC | delim_hash(t + ds, de - ds) | (tabs ? ST_HD_TABS : 0);
            i = de + (q && de < n);
            continue;
        }
        if (strchr("|&;()<>", c)) {
            span(ctx, i, i + 1, KS_OPERATOR);
            cmd_pos = c != '>' && c != '<';
            i++;
            continue;
        }
        if (number_starts(t, i)) {
            int e = scan_number(t, n, i);
            if (e == n || !is_word(t[e]))
                span(ctx, i, e, KS_NUMBER);
            i = e;
            cmd_pos = 0;
            continue;
        }
        if (is_word(c) || c == '!') {
            int e = i + 1;
            while (e < n && (is_word(t[e]) || t[e] == '-' || t[e] == '.'))
                e++;
            if (cmd_pos && in_list(t + i, e - i, keywords)) {
                span(ctx, i, e, KS_KEYWORD);
                cmd_pos = 1; /* if/then/do... are followed by a command */
            } else if (cmd_pos && e + 1 < n && t[e] == '(' && t[e + 1] == ')') {
                span(ctx, i, e, KS_FUNCTION);
                cmd_pos = 0;
            } else {
                if (cmd_pos && in_list(t + i, e - i, builtins))
                    span(ctx, i, e, KS_FUNCTION);
                else if (e < n && t[e] == '=' && cmd_pos)
                    span(ctx, i, e, KS_VARIABLE); /* name=value */
                cmd_pos = cmd_pos && e < n && t[e] == '=';
            }
            i = e;
            continue;
        }
        if (c != ' ' && c != '\t')
            cmd_pos = 0;
        i++;
    }
    return next;
}

static const KipadSyntax sh = { KIPAD_SYNTAX_ABI, "Shell", detect, line };

SYNTAX_LIST(&sh)
