/* xis_i18n.h - shared gettext setup for every KiDesktop program.
 *
 * `_("...")` marks a user-visible string literal for translation and
 * looks it up in the current locale's catalog at runtime; `N_("...")`
 * marks one for extraction *without* looking it up right there (a string
 * table initialized at compile time, translated only when actually read
 * into a widget -- the table itself stays static const, wrapped in _()
 * only at the point of display, same pattern as kiconf's FixedShortcut).
 *
 * xis_i18n_init(domain) does the three calls every gettext program needs
 * once, at startup, before building any UI: setlocale() so libc/GTK pick
 * up the user's LANG/LC_MESSAGES instead of the C locale, then
 * bindtextdomain()+textdomain() so gettext() knows which catalog
 * (`domain`, matching the program's own binary/po name -- "xisguard",
 * "xispanel", ...) and which directory (LOCALEDIR, baked in by that
 * program's own Makefile from its own PREFIX, defaulting to
 * PREFIX/share/locale) to read .mo files from.
 *
 * Every program keeps its own po/ (see any program's po/README.md for
 * the day-to-day translator workflow) and its own LOCALEDIR -D flag --
 * this header only factors out the three calls themselves, which are
 * identical everywhere, so each program doesn't carry its own copy of
 * the same setlocale()/bindtextdomain()/textdomain() dance.
 */
#ifndef XIS_I18N_H
#define XIS_I18N_H

#include <libintl.h>
#include <locale.h>

#define _(s) gettext(s)
#define N_(s) (s)

#ifndef LOCALEDIR
#define LOCALEDIR "/usr/local/share/locale"
#endif

/* LC_NUMERIC goes back to "C" right after: every config file here writes
 * decimals with a '.', and under a locale whose separator is ',' (pt_BR)
 * atof()/strtod()/sscanf("%f") stop at that '.', so "0.85" read as 0 --
 * which is how kicomp's cube/wobbly settings and kiwm's font_size=12.5
 * broke when gettext came in. Messages, dates and the rest keep the
 * user's locale. GTK programs get LC_ALL reset again by gtk_init() and
 * have to keep handling this themselves (kiconf's fprintf_double()). */
static inline void xis_i18n_init(const char *domain)
{
    setlocale(LC_ALL, "");
    setlocale(LC_NUMERIC, "C");
    bindtextdomain(domain, LOCALEDIR);
    bind_textdomain_codeset(domain, "UTF-8");
    textdomain(domain);
}

#endif /* XIS_I18N_H */
