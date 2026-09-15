/* kiconf - gettext setup.
 *
 * `_("...")` marks a user-visible string literal for translation and
 * looks it up in the current locale's catalog at runtime; `N_("...")`
 * marks one for extraction *without* looking it up right there (a string
 * table initialized at compile time, translated only when actually shown
 * -- see tabs/shortcuts.c's FixedShortcut.doc for an example: the table
 * itself is static const, so each .doc is wrapped in _() only where it's
 * read into a label, not in the table literal).
 *
 * kiconf_i18n_init() does the three calls every gettext program needs
 * once, at startup, before building any UI: setlocale() so libc/GTK pick
 * up the user's LANG/LC_MESSAGES instead of the C locale, then
 * bindtextdomain()+textdomain() so gettext() knows which catalog ("kiconf")
 * and which directory (LOCALEDIR, from the Makefile, defaulting to
 * PREFIX/share/locale) to read .mo files from. Translations themselves
 * live in po/ -- see po/README.md.
 */
#ifndef KICONF_I18N_H
#define KICONF_I18N_H

#include <libintl.h>

#define _(s) gettext(s)
#define N_(s) (s)

#ifndef LOCALEDIR
#define LOCALEDIR "/usr/local/share/locale"
#endif

void kiconf_i18n_init(void);

#endif /* KICONF_I18N_H */
