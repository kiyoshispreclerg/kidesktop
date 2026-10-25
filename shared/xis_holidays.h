/* xis_holidays - public holidays by country, read from data files so the
 * lists can be updated or extended without touching any code.
 *
 * Shared by xisserve (the calendar page marks them) and kiconf (the
 * "Ver feriados" list). One file per country, <CC>.conf (ISO 3166 alpha-2,
 * upper case), looked up in $XDG_DATA_HOME/xisserve/holidays (fallback
 * ~/.local/share) before the system directory XIS_HOLIDAYS_DIR, so a user
 * file replaces the installed one of the same country and a new file adds a
 * country. Lines (blank lines and '#' comments ignored; the name is the rest
 * of the line):
 *
 *   FIXED  MM-DD  name         same date every year
 *   EASTER offset name         days from Easter Sunday (Gregorian), may be < 0
 *   NTH    MM n DOW name       n-th weekday of the month: n = 1..5, or -1 for
 *                              the last; DOW = SUN MON TUE WED THU FRI SAT
 *
 * The country is the one the system timezone belongs to (zone.tab), falling
 * back to the country of the locale. */
#ifndef XIS_HOLIDAYS_H
#define XIS_HOLIDAYS_H

#include <stddef.h>

#ifndef XIS_HOLIDAYS_DIR
#define XIS_HOLIDAYS_DIR "/usr/share/xisserve/holidays"
#endif

typedef struct {
    int month, day; /* 1-based */
    char name[128];
} XisHoliday;

/* Writes the ISO country code of this machine ("BR"), "" when unknown. */
void xis_holidays_country(char *out, size_t out_sz);

/* The holidays of `country` in `year`, sorted by date, at most `max`.
 * Returns how many (0 when the country has no file). */
int xis_holidays_for_year(const char *country, int year, XisHoliday *out, int max);

#endif
