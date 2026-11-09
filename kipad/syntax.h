/* syntax.h - kipad's side of syntax highlighting (modules in kipad_syntax.h). */
#ifndef KIPAD_SYNTAX_HOST_H
#define KIPAD_SYNTAX_HOST_H

#include <gtk/gtk.h>

typedef struct SynDoc SynDoc;

/* On: modules load when the first file needs them. Off: every view loses its
 * colours and every module is unloaded. */
void syn_set_enabled(gboolean on);

/* Highlighting for the text view inside `scroll`; follows edits and
 * scrolling by itself. */
SynDoc *syn_doc_new(GtkWidget *scroll, GtkTextView *view);
void syn_doc_free(SynDoc *sd);
/* Picks the language for `path` (NULL: none) from its name and first line. */
void syn_doc_set_file(SynDoc *sd, const char *path);
/* The language's name, or NULL. */
const char *syn_doc_language(const SynDoc *sd);

/* Called when a document's language changes (to update the status bar). */
void syn_set_changed_cb(void (*cb)(void));

#endif
