/*
 * lidpower.h - what KiDesktop does when the laptop lid closes and when
 * the power button is pressed (kiconf's Energia tab writes the choices
 * into kiconfd-power.conf; kiconfd.c feeds them in here).
 *
 * Actions, per trigger:
 *   "system"   leave it to whoever handles it already (logind, acpid...)
 *   "nothing"  swallow the event
 *   "suspend" / "lock" / "poweroff" run that directly
 *   "ask"      (power button only) open `xisserve --session`
 *
 * Anything but "system" also takes a logind inhibitor lock for that
 * trigger (via systemd-inhibit, when it exists), so logind doesn't act
 * on top of us. Without logind there is nothing to inhibit; acpid
 * scripts, if any, still run.
 */
#ifndef KICONFD_LIDPOWER_H
#define KICONFD_LIDPOWER_H

#include <X11/Xlib.h>

/* Apply (or re-apply) the configuration. NULL means "system". */
void lidpower_configure(Display *dpy, Window root, const char *lid_action, const char *power_action);

/* fd to add to select(), -1 if no lid monitor is running; call
 * lidpower_on_readable() when it is readable. */
int lidpower_fd(void);
void lidpower_on_readable(void);

/* Housekeeping each main-loop turn: restarts a dead lid monitor. */
void lidpower_tick(void);

/* KeyPress for the power button, if it is one. */
void lidpower_handle_key(const XKeyEvent *ev);

void lidpower_shutdown(void);

#endif
