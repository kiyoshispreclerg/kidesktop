/*
 * xis_fmt.h - the two string checks every program here needs for paths.
 *
 * Header-only (static inline) so a program just includes it, with no
 * extra source file in its Makefile.
 *
 *   xis_fmt_fits()  snprintf() that says whether everything fit: 1 if it
 *                   did, 0 if `out` holds a truncated string -- for paths,
 *                   where a cut-off one must not be used.
 *   xis_fmt_trunc() / xis_strlcpy()  the same for display text, where a
 *                   cut-off label is fine -- saying so at the call site.
 *   xis_sun_path()  fills a sockaddr_un from a path, or returns 0 when the
 *                   path is too long for sun_path (108 bytes on Linux)
 *                   instead of silently connecting to a truncated one.
 */
#ifndef XIS_FMT_H
#define XIS_FMT_H

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>

__attribute__((format(printf, 3, 4))) static inline int xis_fmt_fits(char *out, size_t outsz, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out, outsz, fmt, ap);
    va_end(ap);
    return n >= 0 && (size_t)n < outsz;
}

__attribute__((format(printf, 3, 4))) static inline void xis_fmt_trunc(char *out, size_t outsz, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out, outsz, fmt, ap);
    va_end(ap);
}

static inline void xis_strlcpy(char *dst, const char *src, size_t dstsz)
{
    if (dstsz == 0) {
        return;
    }
    size_t n = strlen(src);
    if (n >= dstsz) {
        n = dstsz - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static inline int xis_sun_path(struct sockaddr_un *addr, const char *path)
{
    size_t len = strlen(path);
    memset(addr, 0, sizeof(*addr));
    addr->sun_family = AF_UNIX;
    if (len >= sizeof(addr->sun_path)) {
        return 0;
    }
    memcpy(addr->sun_path, path, len + 1);
    return 1;
}

#endif /* XIS_FMT_H */
