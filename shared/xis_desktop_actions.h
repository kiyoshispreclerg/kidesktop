/* xis_desktop_actions - the freedesktop.org "Desktop Action" jumplist
 * mechanism (the [Desktop Action <token>] groups behind e.g. Firefox's
 * "Nova aba anonima" context-menu entry) plus the one piece of Exec=
 * handling that goes with it: substituting a specific file back into a
 * %f/%F/%u/%U field code for "open this recent file with this app".
 *
 * Shared by xispanel's tasklist.c (a running/pinned window's right-click
 * menu) and xisserve.c (a launcher result's right-click menu) -- both
 * independently grew the exact same two functions before this was pulled
 * out; each still builds and draws its own menu (Cairo vs GTK) and adds
 * its own extra entries around this (pin/unpin, favorites, ...), only
 * the .desktop parsing underneath was actually identical. Plain C, no
 * toolkit dependency, same as every other file in shared/.
 */
#ifndef XIS_DESKTOP_ACTIONS_H
#define XIS_DESKTOP_ACTIONS_H

#include <stddef.h>

/* Parses desktop_path's own [Desktop Entry] Actions= list and, for each
 * named token, that action's own [Desktop Action <token>] group's
 * Name=/Exec= (field codes stripped, no file substitution -- a jumplist
 * action carries its own fixed command). Fills the parallel
 * out_names[]/out_execs[] arrays (128/512 bytes each) up to `max`
 * entries and returns how many were found (0 if the file declares none,
 * doesn't exist, or none of its named groups turned out usable). Meant
 * to be called at right-click time, re-read rather than cached, same as
 * xis_desktop_build_exec_with_file() below. */
int xis_desktop_load_actions(const char *desktop_path, char out_names[][128], char out_execs[][512], int max);

/* Re-reads desktop_path's own [Desktop Entry] Exec= and substitutes the
 * first %f/%F/%u/%U field code with file_path (shell-quoted) -- "open
 * this file with this app", for a recent-files jumplist entry. Other
 * field codes are dropped; an Exec with no file/uri code at all gets
 * file_path appended as an extra argument. Returns 0 if desktop_path has
 * no Exec= at all. */
int xis_desktop_build_exec_with_file(const char *desktop_path, const char *file_path, char *out, size_t outsz);

/* Same for several files ("drop these files on this app"): a %F/%U
 * field code takes all of them, a %f/%u only the first, per the .desktop
 * spec -- which then wants the app run once per file. Returns how many
 * of files[] went into `out` (0 if desktop_path has no Exec=), so the
 * caller loops from files + that until all are used. */
int xis_desktop_build_exec_with_files(const char *desktop_path, const char *const *files, int n, char *out,
                                      size_t outsz);

#endif /* XIS_DESKTOP_ACTIONS_H */
