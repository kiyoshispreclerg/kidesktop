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
#include <stdio.h>

#define NAME_LEN 128
#define JSON_BUF_LEN 65536

/* ---- generic subprocess helpers (xrandr/xinput/xset/xprop/wmctrl) ---- */

int run_fire(char *const argv[]);
int run_capture(char *const argv[], char *out, size_t outsz);

/* ---- generic JSON-line-over-Unix-socket client (xisguard-ctl/xispanel-ctl) */

int json_line_send(const char *sockpath, const char *req, char *resp, size_t respsz);

/* Sends xispanel-ctl a RELOAD, same as Paineis' own "Salvar" -- for
 * anything that edits xispanel.conf directly (Paineis, and Atalhos'
 * xispanel widget hotkeys) and wants it picked up live, since xispanel
 * (unlike kiwm/kicomp) does have a control socket for this. */
void xispanel_reload(void);

/* ---- xispanel.conf line tokenizing (PANEL/WIDGET/THEME records) ------- */

char *skip_ws(char *p);
/* Splits off the next whitespace-run-separated field from *cursor,
 * NUL-terminating it in place and advancing *cursor past it -- xispanel's
 * config format ("fields separated by any run of spaces and/or tabs",
 * see xispanel/PROTOCOL.md) needs this instead of strtok since a trailing
 * key=value tail must be kept as one raw chunk, not tokenized further. */
char *next_field(char **cursor);

/* ---- config path resolution (mirrors kiconfd.c/xiskeys.c exactly) ---- */

void resolve_path(const char *filename, char *out, size_t outsz);
char *trim(char *s);

/* Sends SIGHUP to every process named `procname` (exact match, not -f). */
void signal_daemon(const char *procname);

/* True iff a process named exactly `procname` is running right now
 * (`pgrep -x`, never `-f` -- an -f pattern can match unrelated processes
 * by their full command line). */
int process_running(const char *procname);

/* Spawns `prog --replace` detached (fire-and-forget, no wait) -- for
 * daemons like kiwm/kicomp that have no config-reload signal at all
 * (SIGHUP means "shut down" for both): the running instance answers
 * losing its ICCCM/EWMH manager selection by exiting on its own, so
 * starting a new one with --replace is the entire "restart" story. */
void spawn_replace(const char *prog);

/* ---- generic GTK2 table-layout helpers -------------------------------- */

GtkWidget *labeled_row(GtkWidget *table, int row, const char *label_text, GtkWidget *widget);
GtkWidget *frame_with(const char *title, GtkWidget *child);

/* "#rrggbb"/"#rrggbbaa" <-> GtkColorButton -- used by every tab that edits
 * a flat color setting (Aparencia, Gerenciamento de janelas, Efeitos do
 * compositor's shadow color). Alpha is accepted on the way in (falls back
 * to black on a parse failure) but never round-tripped back out: GTK2's
 * plain color button has no alpha channel of its own, and every config
 * format this writes into treats a bare #rrggbb as fully opaque anyway. */
GtkWidget *make_color_button(const char *hex);
void color_button_hex(GtkWidget *btn, char *out, size_t outsz);

/* A GtkComboBox over a fixed, NULL-terminated array of plain option
 * strings (mod_key=alt/meta, focus_stealing_prevention=none/low/..., a
 * renderer name, etc.) -- used wherever a config key's value is one of a
 * short closed set, as opposed to make_theme_combo()'s scanned-from-disk
 * lists (which stay local to the Aparencia tab, the only one that scans
 * anything). combo_text() reads back whichever option is selected. */
GtkWidget *make_options_combo(const char *const *options, const char *current);
const char *combo_text(GtkWidget *combo, const char *const *options);

/* Writes "key=value\n" to `f` with `val` formatted to `digits` decimals
 * and always a '.' separator, regardless of LC_NUMERIC -- plain
 * fprintf("%f") prints "0,45" under a comma-decimal locale (pt_BR and
 * others), which every config file these tabs write into parses with a
 * plain C-locale atof/strtod and would silently misread. */
void fprintf_double(FILE *f, const char *key, double val, int digits);

#endif /* KICONF_COMMON_H */
