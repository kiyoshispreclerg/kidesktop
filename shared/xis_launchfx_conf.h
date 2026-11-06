/* xis_launchfx_conf - the global launch-feedback settings (zoom and
 * duration), $XDG_CONFIG_HOME/launchfx.conf (fallback ~/.config/launchfx.conf):
 *
 *   zoom=2.0          how large the icon grows (minimum 1.05)
 *   duration_ms=500   how long the animation takes (16..5000)
 *
 * Whether a program animates at all stays that program's own toggle. Read by
 * xis_launchfx_trigger() on every trigger (one stat, re-parsed only when the
 * file changed), so a change in kiconf applies without restarting anything. */
#ifndef XIS_LAUNCHFX_CONF_H
#define XIS_LAUNCHFX_CONF_H

#include <stddef.h>

#define XIS_LAUNCHFX_ZOOM_DEFAULT 2.0
#define XIS_LAUNCHFX_ZOOM_MIN 1.05
#define XIS_LAUNCHFX_ZOOM_MAX 8.0
#define XIS_LAUNCHFX_MS_DEFAULT 500
#define XIS_LAUNCHFX_MS_MIN 16
#define XIS_LAUNCHFX_MS_MAX 5000

void xis_launchfx_conf_path(char *out, size_t outsz);

/* Current settings, clamped; the defaults when the file is missing. */
void xis_launchfx_conf_get(double *zoom, int *duration_ms);

#endif
