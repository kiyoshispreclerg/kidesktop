/* clock.h - the --calendar page's Android-clock-style tabs (alarms,
 * stopwatch, countdown timers), kept out of calendar.c. See clock.c. */
#ifndef XISSERVE_PAGES_CLOCK_H
#define XISSERVE_PAGES_CLOCK_H

#include <gtk/gtk.h>

typedef enum { CLOCK_TAB_NONE, CLOCK_TAB_ALARMS, CLOCK_TAB_STOPWATCH, CLOCK_TAB_TIMERS } ClockTab;

GtkWidget *clock_alarms_build(void);
GtkWidget *clock_stopwatch_build(void);
GtkWidget *clock_timers_build(void);

/* Which of the three is on screen (CLOCK_TAB_NONE: another tab, or the
 * page is hidden) -- drives how often the display ticks. */
void clock_set_visible_tab(ClockTab tab);
void clock_on_show(void);
void clock_on_hide(void);

#endif
