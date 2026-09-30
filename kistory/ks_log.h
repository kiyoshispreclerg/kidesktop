/* ks_log - kistoryd's event log: one tab-separated line per event in
 * $XDG_DATA_HOME/kistory/events/YYYY-MM-DD.tsv (local date of the event),
 * files 0600 in a 0700 directory:
 *
 *   ts  kind  app  exe  desktop  output  subject  detail
 *
 * ts is local ISO time (2026-09-30T14:02:11); `\t` `\n` `\\` are escaped
 * inside fields; empty fields stay empty. Each write opens, flocks,
 * appends and closes the day's file: nothing is buffered across a crash,
 * and `kistory purge` (same lock) can rewrite a file between two writes.
 *
 * Pausing: while $XDG_RUNTIME_DIR/kistory-paused.<display> exists and
 * holds 0 or an epoch in the future, nothing is written (the kistory CLI
 * creates/removes it). */
#ifndef KS_LOG_H
#define KS_LOG_H

#include <time.h>

int  ks_log_init(const char *data_dir, int display);
void ks_log_event(time_t ts, const char *kind, const char *app, const char *exe, long desktop,
                  const char *output, const char *subject, const char *detail);
int  ks_log_paused(void);
/* Deletes day files older than `days` (0: keep forever). */
void ks_log_prune(int days);

/* "14:02:11-14:31:40" (or with dates when the range crosses midnight). */
void ks_log_range(char *out, unsigned long outsz, time_t start, time_t end);

#endif /* KS_LOG_H */
