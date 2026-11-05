/* xis_appchooser.h - small "open with" window shared by the GTK2 programs.
 *
 * Lists the installed applications (recommended ones for the content
 * type first) with an entry that both filters the list and, when
 * nothing in it fits, is taken as a command line to run instead --
 * the same dual-purpose input as the xisserve launcher.
 */
#ifndef XIS_APPCHOOSER_H
#define XIS_APPCHOOSER_H

#include <gtk/gtk.h>
#include <gio/gio.h>

/* Runs the chooser modally over `parent` (may be NULL). `content_type`
 * (may be NULL) only decides which applications are listed first.
 * Returns a new reference to the chosen GAppInfo -- an installed
 * application or one built from the typed command, with %F appended
 * when the command has no field code -- or NULL when cancelled. */
GAppInfo *xis_app_chooser_run(GtkWindow *parent, const char *content_type);

#endif
