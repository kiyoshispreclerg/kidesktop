/* xis_clock - alarms, countdown timers and the stopwatch, shared by
 * xisserve (the --calendar page edits them, `xisserve --ring` is what
 * rings) and kiconfd (the resident process that notices when one is
 * due and starts that ring popup).
 *
 * Nothing here ever "counts": every running thing is stored as an
 * absolute wall-clock time (a timer's end, the stopwatch's last start),
 * so the state survives xisserve closing, crashing or being restarted --
 * whoever opens the file next just subtracts from now. That is also why
 * only kiconfd has to stay running for anything to *ring*: it's the one
 * process the session always has, and it already wakes on a timer for
 * the night light.
 *
 * File: $XDG_CONFIG_HOME/ki-clock.conf (fallback ~/.config), tab-
 * separated, one record per line, rewritten whole (tmp + rename) on
 * every change. Writers (xisserve's page and its ring popup) always
 * load, change and save in one go rather than saving a copy they held
 * on to, so neither undoes the other's change.
 *
 *   ALARM  id  HH:MM  days  once_at  enabled  snooze_until  label
 *       days: weekday digits, 0 = Sunday ("12345" = weekdays), or "-"
 *       for a one-time alarm that rings at once_at (epoch seconds).
 *       snooze_until: epoch seconds, 0 = not snoozed.
 *   TIMER  id  duration_ms  end_ms  remaining_ms  label
 *       end_ms: epoch ms it ends at while running, 0 otherwise;
 *       remaining_ms: what's left while stopped (== duration when reset).
 *   STOPWATCH  start_ms  accum_ms
 *       start_ms: epoch ms of the last start while running, 0 when not.
 *   LAP  total_ms
 *       elapsed stopwatch time at each lap, oldest first.
 */
#ifndef XIS_CLOCK_H
#define XIS_CLOCK_H

#include <stddef.h>

#define XIS_CLOCK_MAX_ALARMS 32
#define XIS_CLOCK_MAX_TIMERS 16
#define XIS_CLOCK_MAX_LAPS 99
#define XIS_CLOCK_LABEL_LEN 96

typedef struct {
    int id;
    int hour, minute;
    unsigned days;          /* bit 0 = Sunday .. bit 6 = Saturday; 0 = one-time */
    long long once_at;      /* one-time alarms: epoch seconds it rings at */
    int enabled;
    long long snooze_until; /* epoch seconds, 0 = none */
    char label[XIS_CLOCK_LABEL_LEN];
} XisAlarm;

typedef struct {
    int id;
    long long duration_ms;
    long long end_ms;       /* running: epoch ms; 0 = stopped */
    long long remaining_ms; /* stopped: what's left */
    char label[XIS_CLOCK_LABEL_LEN];
} XisTimer;

typedef struct {
    long long start_ms; /* running: epoch ms of the last start; 0 = stopped */
    long long accum_ms; /* elapsed before start_ms */
    int nlaps;
    long long laps[XIS_CLOCK_MAX_LAPS];
} XisStopwatch;

typedef struct {
    XisAlarm alarms[XIS_CLOCK_MAX_ALARMS];
    int nalarms;
    XisTimer timers[XIS_CLOCK_MAX_TIMERS];
    int ntimers;
    XisStopwatch sw;
} XisClock;

void xis_clock_path(char *out, size_t outsz);
/* Zeroes *c first; a missing file is just an empty clock. Returns 1 if
 * the file was there. */
int xis_clock_load(XisClock *c);
/* Returns 0 on success. Creates the config directory if needed. */
int xis_clock_save(const XisClock *c);

long long xis_clock_now_ms(void);

/* The next time (epoch seconds, > after) alarm `a` rings, counting its
 * snooze; 0 if it never will (disabled, or a one-time alarm already
 * past). */
long long xis_alarm_next(const XisAlarm *a, long long after);
/* Next regular occurrence of hour:minute on one of `days` (0 = any day),
 * > after -- what a one-time alarm's once_at is set to when it's armed. */
long long xis_alarm_next_occurrence(int hour, int minute, unsigned days, long long after);

long long xis_stopwatch_elapsed(const XisStopwatch *sw, long long now_ms);
/* Clamped at 0. */
long long xis_timer_remaining(const XisTimer *t, long long now_ms);

XisAlarm *xis_clock_find_alarm(XisClock *c, int id);
XisTimer *xis_clock_find_timer(XisClock *c, int id);
/* Appends a zeroed entry with a fresh id, or returns NULL when full. */
XisAlarm *xis_clock_add_alarm(XisClock *c);
XisTimer *xis_clock_add_timer(XisClock *c);
void xis_clock_remove_alarm(XisClock *c, int id);
void xis_clock_remove_timer(XisClock *c, int id);

#endif /* XIS_CLOCK_H */
