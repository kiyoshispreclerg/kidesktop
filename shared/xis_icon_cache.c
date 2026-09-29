/* See xis_icon_cache.h. */
#include "xis_icon_cache.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int cache_dir(char *out, size_t outsz)
{
    const char *xdg = getenv("XDG_CACHE_HOME");
    const char *home = getenv("HOME");
    if (xdg && xdg[0] == '/') {
        snprintf(out, outsz, "%s/xispanel/icons", xdg);
    } else if (home && home[0]) {
        snprintf(out, outsz, "%s/.cache/xispanel/icons", home);
    } else {
        return 0;
    }
    return 1;
}

int xis_icon_cache_path(const char *src, int size, char *out, size_t outsz)
{
    const char *rundir = getenv("XDG_RUNTIME_DIR");
    if (!src || size <= 0 || !strncmp(src, "/tmp/", 5) ||
        (rundir && rundir[0] && !strncmp(src, rundir, strlen(rundir)))) {
        return 0;
    }
    struct stat st;
    char dir[PATH_MAX];
    if (stat(src, &st) != 0 || !cache_dir(dir, sizeof(dir))) {
        return 0;
    }
    uint64_t h = 1469598103934665603ULL; /* FNV-1a */
    for (const unsigned char *c = (const unsigned char *)src; *c; c++) {
        h = (h ^ *c) * 1099511628211ULL;
    }
    int n = snprintf(out, outsz, "%s/%016llx-%d-%lld-%lld.png", dir, (unsigned long long)h, size,
                     (long long)st.st_mtime, (long long)st.st_size);
    return n > 0 && (size_t)n < outsz;
}

int xis_icon_cache_begin(const char *cpath, char *tmp, size_t tmpsz)
{
    char dir[PATH_MAX];
    if (!cache_dir(dir, sizeof(dir))) {
        return 0;
    }
    /* mkdir -p: every component past the first '/' */
    for (char *s = strchr(dir + 1, '/');; s = strchr(s + 1, '/')) {
        if (s) {
            *s = '\0';
        }
        if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
            return 0;
        }
        if (!s) {
            break;
        }
        *s = '/';
    }
    int n = snprintf(tmp, tmpsz, "%s.%d.tmp", cpath, (int)getpid());
    return n > 0 && (size_t)n < tmpsz;
}

void xis_icon_cache_end(const char *tmp, const char *cpath, int ok)
{
    if (ok) {
        rename(tmp, cpath); /* atomic against a concurrent reader */
    } else {
        unlink(tmp);
    }
}
