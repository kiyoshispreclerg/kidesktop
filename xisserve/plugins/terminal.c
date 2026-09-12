/*
 * terminal.c - first search plugin (see ../xisserve.h). If the typed
 * query's first word resolves via $PATH, offers two results:
 *
 *   - "Executar: <query>" -- runs it directly, the same
 *     fork+setsid+exec-via-sh-c any other launched app gets (see
 *     run_detached()). Right for a GUI program the user just happens to
 *     know the binary name of.
 *   - "Executar no terminal: <query>" -- the original behavior, opening
 *     the session's terminal-exec fallback chain running the whole
 *     query, left open afterwards (`; exec $SHELL`) so a one-off
 *     command's output doesn't just flash and vanish before it can be
 *     read. Right for anything that actually wants a TTY (a CLI tool,
 *     an interactive REPL) or whose output the user wants to see.
 *
 * Both come from resolving the same first word, so they're either both
 * offered or neither.
 */
#include "../xisserve.h"

#include <stdio.h>

/* Tries a few common freedesktop icon names in order --
 * xisserve_resolve_icon() caches per name (including misses), so this
 * costs at most a few no-op theme lookups per keystroke rather than a
 * real cost once the right (or no) name has been found once. */
static GdkPixbuf *resolve_first_icon(const char *const *names, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        GdkPixbuf *p = xisserve_resolve_icon(names[i], XISSERVE_ICON_PX);
        if (p) return p;
    }
    return NULL;
}

static GdkPixbuf *run_icon(void)
{
    static const char *names[] = {"system-run"};
    return resolve_first_icon(names, sizeof(names) / sizeof(names[0]));
}

static GdkPixbuf *terminal_icon(void)
{
    static const char *names[] = {"utilities-terminal", "terminal", "xterm"};
    return resolve_first_icon(names, sizeof(names) / sizeof(names[0]));
}

void plugin_terminal_search(const char *query, GPtrArray *results)
{
    char first_word[256] = "";
    sscanf(query, "%255s", first_word);
    if (!first_word[0]) return;
    gchar *found = g_find_program_in_path(first_word);
    if (!found) return;
    g_free(found);

    ResultEntry *run = g_new0(ResultEntry, 1);
    snprintf(run->name, sizeof(run->name), "Executar: %s", query);
    snprintf(run->exec, sizeof(run->exec), "%s", query);
    snprintf(run->subtitle, sizeof(run->subtitle), "Terminal");
    run->from_desktop = FALSE;
    run->icon = run_icon();
    g_ptr_array_add(results, run);

    ResultEntry *term = g_new0(ResultEntry, 1);
    snprintf(term->name, sizeof(term->name), "Executar no terminal: %s", query);
    char with_shell[1024];
    snprintf(with_shell, sizeof(with_shell), "%s; exec \"${SHELL:-/bin/sh}\"", query);
    build_terminal_exec(with_shell, term->exec, sizeof(term->exec));
    snprintf(term->subtitle, sizeof(term->subtitle), "Terminal");
    term->from_desktop = FALSE;
    term->icon = terminal_icon();
    g_ptr_array_add(results, term);
}
