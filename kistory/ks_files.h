/* ks_files - kistoryd's file source. Third-party apps don't report what
 * they open or save, so three partial sources are combined:
 *
 *   xbel  ~/.local/share/recently-used.xbel (GTK file chooser, GIMP,
 *         Inkscape, LibreOffice, ...): each new or re-used entry, with the
 *         app that used it.
 *   kde   ~/.local/share/RecentDocuments/ .desktop files (KDE apps).
 *   fd    when a window loses focus, the regular files under $HOME its
 *         process holds open (players, viewers, editors that keep the
 *         file open); caches, databases, fonts and config are skipped.
 *
 * All logged as kind `file`: subject is the path (or URL), detail
 * `src=xbel|kde|fd`. Apps with hidden titles get no file lines at all:
 * a path says as much as a title. Watched with inotify; the first read of
 * each list at start is only a baseline, nothing is logged for it. */
#ifndef KS_FILES_H
#define KS_FILES_H

#include "../shared/xis_winident.h"
#include <time.h>

int  ks_files_init(void);
int  ks_files_fd(void);        /* inotify fd for the main poll(), -1 if none */
void ks_files_handle(void);    /* call when it is readable */

/* Focus-period end: samples the window's process's open files. */
void ks_files_focus_end(const XisWinIdent *id, time_t start, time_t end, int titles_hidden);

#endif /* KS_FILES_H */
