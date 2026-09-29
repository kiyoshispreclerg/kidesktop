/* xis_icon_cache - the on-disk cache of already-shrunk icons, shared by
 * xispanel (cairo) and xisserve (GdkPixbuf).
 *
 * Launchers, pinned apps and tray items resolve the same icon files on
 * every start, often to a 512/1024px PNG or an SVG that needs librsvg
 * just to render it. The shrunk result is a few KB, so it's kept as a PNG
 * under $XDG_CACHE_HOME/xispanel/icons/ (one directory for both programs;
 * a PNG written by cairo reads back the same through gdk-pixbuf and vice
 * versa). The name hashes the source path and also carries the target
 * size and the source's mtime+size, so an updated app/theme simply misses
 * and gets a new entry -- nothing ever needs invalidating. This module
 * only names and places files; decoding and encoding stay with each
 * caller's own image library.
 */
#ifndef XIS_ICON_CACHE_H
#define XIS_ICON_CACHE_H

#include <stddef.h>

/* The cache file for `src` shrunk to `size`, into `out`. 0 when `src`
 * shouldn't be cached at all: missing, size <= 0, or a throwaway location
 * (/tmp, $XDG_RUNTIME_DIR) whose files are one-off by nature. */
int xis_icon_cache_path(const char *src, int size, char *out, size_t outsz);

/* Before writing `cpath`: creates the cache directory and gives a
 * per-process temporary name next to it in `tmp`. 0 if the directory
 * can't be made. */
int xis_icon_cache_begin(const char *cpath, char *tmp, size_t tmpsz);

/* After writing `tmp`: moves it into place atomically if `ok`, otherwise
 * removes it. */
void xis_icon_cache_end(const char *tmp, const char *cpath, int ok);

#endif /* XIS_ICON_CACHE_H */
