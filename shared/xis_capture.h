/* xis_capture - the one rule about excluded windows that every program
 * has to agree on (kicomp/CAPTURE-PROTOCOL.md).
 *
 * A window is excluded from screen captures when its client carries
 * _KIWM_CAPTURE_EXCLUDE, or when it is a popup of one: an override-redirect
 * window (a menu, a tooltip -- kiwm is never offered them, so they cannot
 * be marked) belonging to the same process as a marked window. The
 * compositor and a recorder's own fallback both apply it, and they must
 * not drift apart, so it lives here. */
#ifndef XIS_CAPTURE_H
#define XIS_CAPTURE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define XIS_CAPTURE_MARK_ATOM "_KIWM_CAPTURE_EXCLUDE"

/* `pid` is the window's _NET_WM_PID (0 = unknown); `marked_pids` the PIDs
 * of the marked windows. Unknown PIDs never match. */
static inline bool xis_capture_popup_excluded(bool override_redirect, uint32_t pid,
                                              const uint32_t *marked_pids, size_t n)
{
    if (!override_redirect || pid == 0)
        return false;
    for (size_t i = 0; i < n; i++)
        if (marked_pids[i] == pid)
            return true;
    return false;
}

#endif /* XIS_CAPTURE_H */
