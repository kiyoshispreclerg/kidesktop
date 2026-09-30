/* ks_windows - kistoryd's X source: managed windows opening and closing,
 * focus periods (the active window plus every title it showed while
 * focused), per-output virtual desktop switches and outputs coming and
 * going. Reads EWMH, kiwm's _KIWM_* properties and XRandR monitors.
 *
 * Logged kinds:
 *   win_open / win_close   subject: title             detail: win=0x.. [at_start | open=Ns]
 *   focus                  subject: HH:MM:SS-HH:MM:SS  detail: title | title | ...
 *   desktop                subject: switch            (desktop/output columns)
 *   output                 subject: connected | disconnected */
#ifndef KS_WINDOWS_H
#define KS_WINDOWS_H

#include <X11/Xlib.h>
#include <time.h>
#include "../shared/xis_winident.h"

int  ks_windows_init(Display *dpy);
int  ks_windows_handle_event(XEvent *ev);
/* Ends the running focus period (shutdown, pause). */
void ks_windows_flush(void);

/* Called when a focus period ends (whether or not it is long enough to be
 * logged), before its line is written -- other sources hook in here. */
typedef void (*KsFocusEndCb)(const XisWinIdent *id, time_t start, time_t end, int titles_hidden);
void ks_windows_on_focus_end(KsFocusEndCb cb);

#endif /* KS_WINDOWS_H */
