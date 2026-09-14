/* kiconf - shared helpers used by 2+ tabs.
 *
 * See kiconf.c's top doc comment for the overall design. This header
 * only declares the genuinely cross-tab bits (subprocess runners, the
 * JSON-line-over-Unix-socket client transport, config path resolution,
 * daemon signalling, and the two generic GTK table-layout helpers);
 * anything used by exactly one tab lives static in that tab's own
 * tabs/ C file instead.
 */
#ifndef KICONF_COMMON_H
#define KICONF_COMMON_H

#include <gtk/gtk.h>
#include <stddef.h>

#define NAME_LEN 128
#define JSON_BUF_LEN 65536

/* ---- generic subprocess helpers (xrandr/xinput/xset/xprop/wmctrl) ---- */

int run_fire(char *const argv[]);
int run_capture(char *const argv[], char *out, size_t outsz);

/* ---- generic JSON-line-over-Unix-socket client (xisguard-ctl/xispanel-ctl) */

int json_line_send(const char *sockpath, const char *req, char *resp, size_t respsz);

/* ---- config path resolution (mirrors kiconfd.c/xiskeys.c exactly) ---- */

void resolve_path(const char *filename, char *out, size_t outsz);
char *trim(char *s);

/* Sends SIGHUP to every process named `procname` (exact match, not -f). */
void signal_daemon(const char *procname);

/* ---- generic GTK2 table-layout helpers -------------------------------- */

GtkWidget *labeled_row(GtkWidget *table, int row, const char *label_text, GtkWidget *widget);
GtkWidget *frame_with(const char *title, GtkWidget *child);

#endif /* KICONF_COMMON_H */
