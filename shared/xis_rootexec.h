/* xis_rootexec - the one place KiDesktop programs gain root, through pkexec.
 * Every call is its own pkexec, so the password is asked each time (nothing
 * is cached by the app). Hardening of the escalation itself belongs here, not
 * in each app: pkexec is run from fixed system paths (never via $PATH),
 * commands are passed as argv (no shell unless the caller supplies a script)
 * and the caller's stdin/stdout are never handed to the root process. */
#ifndef XIS_ROOTEXEC_H
#define XIS_ROOTEXEC_H

#include <stddef.h>

#define XIS_ROOTEXEC_CANCELLED 126      /* pkexec: prompt dismissed */
#define XIS_ROOTEXEC_NOT_RUN   127      /* pkexec: not authorized / error; also pkexec missing */

/* Both return the exit status of the root command (0 = success), the codes
 * above when pkexec itself refused or could not run, or -1 on a local
 * failure (fork/pipe) or when the process died by signal. They block until
 * the command ends, and feed `in` (may be NULL) to its stdin. */

/* `pkexec argv...`, argv NULL-terminated, no shell. */
int xis_rootexec(char *const argv[], const char *in, size_t in_len);

/* `pkexec sh -c script sh args...` with `args` NULL-terminated (may be
 * NULL): `script` can use $1.. and several steps share one password prompt.
 * `script` must be a constant of the program, never built from user input. */
int xis_rootexec_script(const char *script, char *const args[], const char *in);

/* TRUE when pkexec is installed. */
int xis_rootexec_available(void);

#endif
