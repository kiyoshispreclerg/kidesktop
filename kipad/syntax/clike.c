/* clike.so - the C family, one engine driven by a table per language: C,
 * C++, JavaScript/TypeScript, Java, Go, Rust.
 *
 * State: 0 plain, ST_BLOCK inside a block comment, ST_BACKTICK inside a
 * `...` string (JS template, Go raw string), ST_PREPROC on a preprocessor
 * line continued with a backslash. */
#include "common.h"

#define ST_BLOCK 1
#define ST_BACKTICK 2
#define ST_PREPROC 3

typedef struct {
    const char *const *keywords, *const *types, *const *constants;
    int preproc;   /* '#' directives (C, C++) */
    int backtick;  /* `...` strings, multi-line */
    int caps_types; /* Name starting with a capital is a type */
    int char_quote; /* '...' is a short char/string literal (not a Rust lifetime) */
    int bang_macros; /* name!(...) is a macro call (Rust) */
} Lang;

/* ---- the engine ------------------------------------------------------ */

static int run(const Lang *L, const char *t, int n, int state, KipadSpanFn span, void *ctx)
{
    int i = 0, closed;
    if (state == ST_BLOCK) {
        const char *e = NULL;
        for (int k = 0; k + 1 < n; k++)
            if (t[k] == '*' && t[k + 1] == '/') {
                e = t + k;
                break;
            }
        i = e ? (int)(e - t) + 2 : n;
        span(ctx, 0, i, KS_COMMENT);
        if (!e)
            return ST_BLOCK;
    } else if (state == ST_BACKTICK) {
        i = scan_quoted(t, n, 0, '`', 1, &closed, span, ctx);
        span(ctx, 0, i, KS_STRING);
        if (!closed)
            return ST_BACKTICK;
    } else if (state == ST_PREPROC) {
        span(ctx, 0, n, KS_PREPROC);
        return n > 0 && t[n - 1] == '\\' ? ST_PREPROC : 0;
    }
    if (L->preproc && i == 0) {
        int k = 0;
        while (k < n && (t[k] == ' ' || t[k] == '\t'))
            k++;
        if (k < n && t[k] == '#') {
            /* the directive, and a <file> after #include */
            int e = k + 1;
            while (e < n && (t[e] == ' ' || is_word(t[e])))
                e++;
            span(ctx, k, e, KS_PREPROC);
            i = e;
            if (i < n && t[i] == '<') {
                const char *g = memchr(t + i, '>', (size_t)(n - i));
                int ge = g ? (int)(g - t) + 1 : n;
                span(ctx, i, ge, KS_STRING);
                i = ge;
            }
            if (n > 0 && t[n - 1] == '\\') {
                span(ctx, i, n, KS_PREPROC);
                return ST_PREPROC;
            }
        }
    }
    while (i < n) {
        char c = t[i];
        if (c == '/' && i + 1 < n && t[i + 1] == '/') {
            span(ctx, i, n, KS_COMMENT);
            break;
        }
        if (c == '/' && i + 1 < n && t[i + 1] == '*') {
            int e = -1;
            for (int k = i + 2; k + 1 < n; k++)
                if (t[k] == '*' && t[k + 1] == '/') {
                    e = k + 2;
                    break;
                }
            span(ctx, i, e < 0 ? n : e, KS_COMMENT);
            if (e < 0)
                return ST_BLOCK;
            i = e;
            continue;
        }
        if (c == '"' || (c == '\'' && L->char_quote)) {
            int e = scan_quoted(t, n, i + 1, c, 1, &closed, span, ctx);
            span(ctx, i, e, KS_STRING);
            i = e;
            continue;
        }
        if (c == '`' && L->backtick) {
            int e = scan_quoted(t, n, i + 1, '`', 1, &closed, span, ctx);
            span(ctx, i, e, KS_STRING);
            if (!closed)
                return ST_BACKTICK;
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
            int k = e;
            while (k < n && t[k] == ' ')
                k++;
            if (in_list(t + i, e - i, L->keywords))
                span(ctx, i, e, KS_KEYWORD);
            else if (in_list(t + i, e - i, L->types) || (L->caps_types && isupper((unsigned char)c) && e - i > 1 &&
                                                         !isupper((unsigned char)t[i + 1])))
                span(ctx, i, e, KS_TYPE);
            else if (L->constants && in_list(t + i, e - i, L->constants))
                span(ctx, i, e, KS_NUMBER);
            else if (k < n && (t[k] == '(' || (t[k] == '!' && L->bang_macros)))
                span(ctx, i, e, KS_FUNCTION);
            i = e;
            continue;
        }
        if (strchr("+-*/%=<>!&|^~?:", c))
            span(ctx, i, i + 1, KS_OPERATOR);
        i++;
    }
    return 0;
}

/* ---- C / C++ ------------------------------------------------------------ */

static const char *const c_kw[] = {
    "if", "else", "for", "while", "do", "switch", "case", "default", "break", "continue", "return", "goto",
    "sizeof", "typedef", "struct", "union", "enum", "static", "extern", "const", "volatile", "register", "inline",
    "restrict", "auto", "_Alignof", "_Static_assert", "_Generic", "_Noreturn", "_Thread_local", NULL,
};
static const char *const c_types[] = {
    "void", "char", "short", "int", "long", "float", "double", "signed", "unsigned", "_Bool", "bool", "size_t",
    "ssize_t", "off_t", "ptrdiff_t", "intptr_t", "uintptr_t", "int8_t", "int16_t", "int32_t", "int64_t", "uint8_t",
    "uint16_t", "uint32_t", "uint64_t", "FILE", "gboolean", "gint", "guint", "gchar", "gpointer", "gsize", NULL,
};
static const char *const c_consts[] = { "NULL", "true", "false", "TRUE", "FALSE", "EOF", "stdin", "stdout", "stderr",
                                        NULL };
static const char *const cpp_kw[] = {
    "if", "else", "for", "while", "do", "switch", "case", "default", "break", "continue", "return", "goto",
    "sizeof", "typedef", "struct", "union", "enum", "static", "extern", "const", "volatile", "register", "inline",
    "class", "namespace", "template", "typename", "public", "private", "protected", "virtual", "override", "final",
    "friend", "operator", "new", "delete", "this", "try", "catch", "throw", "using", "constexpr", "consteval",
    "decltype", "explicit", "mutable", "noexcept", "static_cast", "dynamic_cast", "reinterpret_cast", "const_cast",
    "co_await", "co_return", "co_yield", "concept", "requires", "static_assert", "auto", NULL,
};
static const char *const cpp_types[] = {
    "void", "char", "short", "int", "long", "float", "double", "signed", "unsigned", "bool", "size_t", "wchar_t",
    "char8_t", "char16_t", "char32_t", "int8_t", "int16_t", "int32_t", "int64_t", "uint8_t", "uint16_t",
    "uint32_t", "uint64_t", "string", "vector", "map", "set", "unique_ptr", "shared_ptr", NULL,
};
static const char *const cpp_consts[] = { "nullptr", "NULL", "true", "false", NULL };

static const Lang c_lang = { c_kw, c_types, c_consts, 1, 0, 0, 1, 0 };
static const Lang cpp_lang = { cpp_kw, cpp_types, cpp_consts, 1, 0, 0, 1, 0 };

static const char *const c_names[] = { "*.c", "*.h", "*.xpm", NULL };
static const char *const cpp_names[] = { "*.cpp", "*.cc", "*.cxx", "*.c++", "*.hpp", "*.hh", "*.hxx", "*.ino",
                                         "*.moc", NULL };

static int c_detect(const char *b, const char *f) { (void)f; return match_name(b, c_names, 50); }
static int cpp_detect(const char *b, const char *f) { (void)f; return match_name(b, cpp_names, 50); }
static int c_line(const char *t, int n, int s, KipadSpanFn sp, void *c) { return run(&c_lang, t, n, s, sp, c); }
static int cpp_line(const char *t, int n, int s, KipadSpanFn sp, void *c) { return run(&cpp_lang, t, n, s, sp, c); }

static const KipadSyntax c_syn = { KIPAD_SYNTAX_ABI, "C", c_detect, c_line };
static const KipadSyntax cpp_syn = { KIPAD_SYNTAX_ABI, "C++", cpp_detect, cpp_line };

/* ---- JavaScript / TypeScript ---------------------------------------------- */

static const char *const js_kw[] = {
    "if", "else", "for", "while", "do", "switch", "case", "default", "break", "continue", "return", "function",
    "var", "let", "const", "new", "delete", "typeof", "instanceof", "in", "of", "class", "extends", "super", "this",
    "import", "export", "from", "as", "async", "await", "yield", "try", "catch", "finally", "throw", "static", "get",
    "set", "void", "with", "debugger", "interface", "type", "enum", "implements", "public", "private", "protected",
    "readonly", "declare", "namespace", "abstract", "keyof", NULL,
};
static const char *const js_types[] = { "string", "number", "boolean", "any", "unknown", "never", "object", "symbol",
                                        "bigint", NULL };
static const char *const js_consts[] = { "true", "false", "null", "undefined", "NaN", "Infinity", NULL };
static const Lang js_lang = { js_kw, js_types, js_consts, 0, 1, 1, 1, 0 };
static const char *const js_names[] = { "*.js", "*.mjs", "*.cjs", "*.jsx", "*.ts", "*.tsx", "*.mts", "*.qml", NULL };
static const char *const js_progs[] = { "node", "deno", "bun", "gjs", NULL };

static int js_detect(const char *b, const char *f)
{
    int s = match_shebang(f, js_progs);
    return s ? s : match_name(b, js_names, 50);
}
static int js_line(const char *t, int n, int s, KipadSpanFn sp, void *c) { return run(&js_lang, t, n, s, sp, c); }
static const KipadSyntax js_syn = { KIPAD_SYNTAX_ABI, "JavaScript", js_detect, js_line };

/* ---- Java -------------------------------------------------------------- */

static const char *const java_kw[] = {
    "if", "else", "for", "while", "do", "switch", "case", "default", "break", "continue", "return", "class",
    "interface", "enum", "record", "extends", "implements", "new", "this", "super", "import", "package", "public",
    "private", "protected", "static", "final", "abstract", "native", "synchronized", "transient", "volatile",
    "strictfp", "throws", "throw", "try", "catch", "finally", "instanceof", "assert", "var", "yield", "sealed",
    "permits", NULL,
};
static const char *const java_types[] = { "void", "boolean", "byte", "char", "short", "int", "long", "float",
                                          "double", NULL };
static const char *const java_consts[] = { "true", "false", "null", NULL };
static const Lang java_lang = { java_kw, java_types, java_consts, 0, 0, 1, 1, 0 };
static const char *const java_names[] = { "*.java", "*.kt", "*.kts", "*.scala", "*.groovy", "*.gradle", "*.cs",
                                          NULL };
static int java_detect(const char *b, const char *f) { (void)f; return match_name(b, java_names, 50); }
static int java_line(const char *t, int n, int s, KipadSpanFn sp, void *c) { return run(&java_lang, t, n, s, sp, c); }
static const KipadSyntax java_syn = { KIPAD_SYNTAX_ABI, "Java", java_detect, java_line };

/* ---- Go -------------------------------------------------------------- */

static const char *const go_kw[] = {
    "break", "case", "chan", "const", "continue", "default", "defer", "else", "fallthrough", "for", "func", "go",
    "goto", "if", "import", "interface", "map", "package", "range", "return", "select", "struct", "switch", "type",
    "var", NULL,
};
static const char *const go_types[] = {
    "bool", "byte", "complex64", "complex128", "error", "float32", "float64", "int", "int8", "int16", "int32",
    "int64", "rune", "string", "uint", "uint8", "uint16", "uint32", "uint64", "uintptr", "any", NULL,
};
static const char *const go_consts[] = { "true", "false", "nil", "iota", NULL };
static const Lang go_lang = { go_kw, go_types, go_consts, 0, 1, 0, 1, 0 };
static const char *const go_names[] = { "*.go", NULL };
static int go_detect(const char *b, const char *f) { (void)f; return match_name(b, go_names, 50); }
static int go_line(const char *t, int n, int s, KipadSpanFn sp, void *c) { return run(&go_lang, t, n, s, sp, c); }
static const KipadSyntax go_syn = { KIPAD_SYNTAX_ABI, "Go", go_detect, go_line };

/* ---- Rust ------------------------------------------------------------ */

static const char *const rust_kw[] = {
    "as", "async", "await", "break", "const", "continue", "crate", "dyn", "else", "enum", "extern", "fn", "for",
    "if", "impl", "in", "let", "loop", "match", "mod", "move", "mut", "pub", "ref", "return", "self", "Self",
    "static", "struct", "super", "trait", "type", "unsafe", "use", "where", "while", "macro_rules", NULL,
};
static const char *const rust_types[] = {
    "i8", "i16", "i32", "i64", "i128", "isize", "u8", "u16", "u32", "u64", "u128", "usize", "f32", "f64", "bool",
    "char", "str", NULL,
};
static const char *const rust_consts[] = { "true", "false", "None", "Some", "Ok", "Err", NULL };
/* no char_quote: 'a is a lifetime as often as 'a' is a char */
static const Lang rust_lang = { rust_kw, rust_types, rust_consts, 0, 0, 1, 0, 1 };
static const char *const rust_names[] = { "*.rs", NULL };
static int rust_detect(const char *b, const char *f) { (void)f; return match_name(b, rust_names, 50); }
static int rust_line(const char *t, int n, int s, KipadSpanFn sp, void *c) { return run(&rust_lang, t, n, s, sp, c); }
static const KipadSyntax rust_syn = { KIPAD_SYNTAX_ABI, "Rust", rust_detect, rust_line };

SYNTAX_LIST(&c_syn, &cpp_syn, &js_syn, &java_syn, &go_syn, &rust_syn)
