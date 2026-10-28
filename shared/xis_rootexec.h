/* xis_rootexec - run one command as root through pkexec, for GUI apps whose
 * action was refused for lack of permission. Every call is its own pkexec,
 * so the password is asked each time (nothing is cached by the app). */
#ifndef XIS_ROOTEXEC_H
#define XIS_ROOTEXEC_H

#include <stddef.h>

#define XIS_ROOTEXEC_CANCELLED 126      /* prompt dismissed / not authorized */

/* Runs `pkexec argv...` (argv NULL-terminated, no shell) feeding `in` (may be
 * NULL) to its stdin; stdout and stderr are discarded. Blocks until it ends.
 * Returns the exit status, XIS_ROOTEXEC_CANCELLED when the prompt was
 * dismissed, or -1 when pkexec could not be run. */
int xis_rootexec(char *const argv[], const char *in, size_t in_len);

#endif
