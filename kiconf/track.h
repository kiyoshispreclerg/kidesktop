/* kiconf - pending-changes tracking (the "leave without applying?" question).
 *
 * When a tab has been built, kiconf.c records the value of every editable
 * widget in it (entries, spin buttons, toggles, combos, color/font/file
 * buttons, text views, scales and tree view models); leaving the tab asks
 * for confirmation only if one of them now differs from that baseline. So
 * an edit that is undone by hand isn't pending, and nothing a tab does
 * while loading counts as an edit.
 *
 * Tabs keep the baseline in step with what is really applied:
 *   kiconf_track_applied(w)  after a successful Aplicar/Salvar: the whole
 *                            tab that contains `w` is the new baseline.
 *   kiconf_track_rebase(w)   only `w`'s subtree is: a section with its own
 *                            Aplicar, or a form reloaded from its source
 *                            (a device or item picked in a selector).
 *   kiconf_track_ignore(w)   `w`'s subtree holds no settings: search
 *                            fields, selectors, or controls that apply
 *                            right away.
 *   kiconf_track_state(w, fn, data)
 *                            for state that lives outside the widgets (a
 *                            master/detail tab's records, a canvas layout):
 *                            fn returns a newly allocated string of it, and
 *                            it is compared like a widget value. Still
 *                            consulted inside an ignored subtree.
 * External plugin tabs can call these too (kiconf exports its symbols). */
#ifndef KICONF_TRACK_H
#define KICONF_TRACK_H

#include <gtk/gtk.h>

typedef char *(*KiconfStateFn)(gpointer data);

void kiconf_track_applied(GtkWidget *w);
void kiconf_track_rebase(GtkWidget *w);
void kiconf_track_ignore(GtkWidget *w);
void kiconf_track_state(GtkWidget *w, KiconfStateFn fn, gpointer data);

/* kiconf.c only: marks `root` as a tab and takes its first baseline. */
void kiconf_track_start(GtkWidget *root);
/* 1 if anything under `root` differs from its baseline. */
int kiconf_track_pending(GtkWidget *root);

#endif
