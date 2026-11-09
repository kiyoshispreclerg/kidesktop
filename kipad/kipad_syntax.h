/* kipad_syntax.h - interface of kipad's syntax highlighting modules.
 *
 * A module is <name>.so in $KIPAD_SYNTAX_PATH (colon-separated),
 * ~/.local/lib/kipad/syntax or $LIBDIR/kipad/syntax; a name found earlier in
 * that order hides the same name later. It exports kipad_syntax_list(), a
 * NULL-terminated list of the languages it knows (one .so may have several).
 *
 * Modules see no kipad or GTK internals: kipad hands them one line of text
 * at a time and paints the spans they report. Multi-line constructs (block
 * comments, here-documents, strings) are carried in an int state the module
 * defines: 0 at the start of the file, and whatever line() returned for the
 * line before. kipad keeps one state per line, so the state must depend only
 * on the text up to the end of that line.
 *
 * Modules are loaded only while highlighting is on (View > Syntax
 * highlighting), and all of them are unloaded when it is turned off. */
#ifndef KIPAD_SYNTAX_H
#define KIPAD_SYNTAX_H

#define KIPAD_SYNTAX_ABI 1

/* What a span is; kipad picks the colours. */
enum {
    KS_NONE,
    KS_COMMENT,
    KS_KEYWORD,
    KS_TYPE,
    KS_STRING,
    KS_NUMBER,
    KS_PREPROC,  /* #include, shebang, directives */
    KS_FUNCTION, /* function names, also math functions */
    KS_VARIABLE, /* $var, @attr */
    KS_OPERATOR,
    KS_ESCAPE,   /* \n inside a string, &amp; */
    KS_KEY,      /* key of key=value, markup attribute name */
    KS_SECTION,  /* [section], heading */
    KS_TAG,      /* markup tag name */
    KS_EMPHASIS,
    KS_LINK,
    KS_ERROR,
    KS_COUNT
};

/* start and end are byte offsets into the line, end exclusive. Spans may come
 * in any order; a later span over the same bytes wins. */
typedef void (*KipadSpanFn)(void *ctx, int start, int end, int style);

typedef struct {
    int abi;          /* KIPAD_SYNTAX_ABI */
    const char *name; /* shown in the status bar, e.g. "Shell" */
    /* How well this language fits a file, 0 = not at all. `basename` is the
     * file's name without directories; `first_line` its first line without
     * the newline (for #! and modelines). Usual scores: 50 for a name
     * pattern, 80 for a #! line. */
    int (*detect)(const char *basename, const char *first_line);
    /* Reports the spans of one line (UTF-8, no newline) and returns the
     * state at its end. `state` is the state at the end of the line before. */
    int (*line)(const char *text, int len, int state, KipadSpanFn span, void *ctx);
} KipadSyntax;

const KipadSyntax *const *kipad_syntax_list(void);

#endif
