/* xis_launchfx_conf - see xis_launchfx_conf.h. */
#include "xis_launchfx_conf.h"
#include "xis_safefile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

void xis_launchfx_conf_path(char *out, size_t outsz)
{
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) {
        snprintf(out, outsz, "%s/launchfx.conf", xdg);
        return;
    }
    const char *home = getenv("HOME");
    snprintf(out, outsz, "%s/.config/launchfx.conf", home && *home ? home : "/tmp");
}

static double clampd(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }

void xis_launchfx_conf_get(double *zoom, int *duration_ms)
{
    static double c_zoom = XIS_LAUNCHFX_ZOOM_DEFAULT;
    static int c_ms = XIS_LAUNCHFX_MS_DEFAULT;
    static struct timespec c_mtime;
    static off_t c_size;
    static int c_have;   /* c_mtime is meaningful (the file existed at the last look) */

    char path[4096];
    xis_launchfx_conf_path(path, sizeof(path));
    struct stat st;
    if (stat(path, &st) != 0) {
        c_zoom = XIS_LAUNCHFX_ZOOM_DEFAULT;
        c_ms = XIS_LAUNCHFX_MS_DEFAULT;
        c_have = 0;
    } else if (!c_have || st.st_mtim.tv_sec != c_mtime.tv_sec || st.st_mtim.tv_nsec != c_mtime.tv_nsec ||
               st.st_size != c_size) {
        c_mtime = st.st_mtim;
        c_size = st.st_size;
        c_have = 1;
        c_zoom = XIS_LAUNCHFX_ZOOM_DEFAULT;
        c_ms = XIS_LAUNCHFX_MS_DEFAULT;
        FILE *f = xis_fopen_read(path, xis_check_usable);
        if (f) {
            char line[256];
            while (fgets(line, sizeof(line), f)) {
                char *eq = strchr(line, '=');
                if (!eq)
                    continue;
                *eq = '\0';
                const char *k = line, *v = eq + 1;
                /* strtod/atoi: LC_NUMERIC is pinned to C by xis_i18n_init in the programs that call it;
                 * the files are always written with '.'. */
                if (!strcmp(k, "zoom"))
                    c_zoom = clampd(strtod(v, NULL), XIS_LAUNCHFX_ZOOM_MIN, XIS_LAUNCHFX_ZOOM_MAX);
                else if (!strcmp(k, "duration_ms"))
                    c_ms = (int)clampd(atoi(v), XIS_LAUNCHFX_MS_MIN, XIS_LAUNCHFX_MS_MAX);
            }
            fclose(f);
        }
    }
    if (zoom)
        *zoom = c_zoom;
    if (duration_ms)
        *duration_ms = c_ms;
}
