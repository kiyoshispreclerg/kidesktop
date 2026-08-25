/*
 * terminal.c - first search plugin (see ../xisserve.h). If the typed
 * query's first word resolves via $PATH, offers "Executar: <query>" as
 * a result -- launching it opens the session's terminal-exec fallback
 * chain running the whole query, left open afterwards (`; exec $SHELL`)
 * so a one-off command doesn't just flash and vanish before it can be
 * read.
 */
#include "../xisserve.h"

#include <stdio.h>

/* Tries a few common freedesktop icon names for "a terminal" in order --
 * xisserve_resolve_icon() caches per name (including misses), so this
 * costs at most a few no-op theme lookups per keystroke rather than a
 * real cost once the right (or no) name has been found once. */
static GdkPixbuf *terminal_icon(void)
{
    static const char *names[] = {"utilities-terminal", "terminal", "xterm"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        GdkPixbuf *p = xisserve_resolve_icon(names[i], XISSERVE_ICON_PX);
        if (p) return p;
    }
    return NULL;
}

void plugin_terminal_search(const char *query, GPtrArray *results)
{
    char first_word[256] = "";
    sscanf(query, "%255s", first_word);
    if (!first_word[0]) return;
    gchar *found = g_find_program_in_path(first_word);
    if (!found) return;
    g_free(found);

    ResultEntry *e = g_new0(ResultEntry, 1);
    snprintf(e->name, sizeof(e->name), "Executar: %s", query);
    char with_shell[1024];
    snprintf(with_shell, sizeof(with_shell), "%s; exec \"${SHELL:-/bin/sh}\"", query);
    build_terminal_exec(with_shell, e->exec, sizeof(e->exec));
    snprintf(e->subtitle, sizeof(e->subtitle), "Terminal");
    e->from_desktop = FALSE;
    e->icon = terminal_icon();
    g_ptr_array_add(results, e);
}
