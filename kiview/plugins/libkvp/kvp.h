/* SPDX-License-Identifier: GPL-3.0-or-later */
/* libkvp - what a kiview plugin that shows a bitmap needs: the protocol, its
 * own X connection, the view (fit, zoom, pan, rotate) and drawing. The
 * plugin only supplies the decoder. */
#ifndef KVP_H
#define KVP_H

#include <cairo.h>
#include <glib.h>

#define KVP_PROTOCOL_VERSION 1

typedef enum {
    KVP_ERR_UNSUPPORTED,
    KVP_ERR_CORRUPT,
    KVP_ERR_TOOLARGE,
    KVP_ERR_IO,
} KvpError;

#define KVP_ERROR kvp_error_quark()
GQuark kvp_error_quark(void);

typedef struct {
    const char *caps; /* comma list for the plugin's hello */
    /* Decodes the document `fd` reads (libkvp closes it). Returns an ARGB32
     * or RGB24 image surface, and in *format a description (g_malloc'ed),
     * or NULL with *err set. */
    cairo_surface_t *(*load)(int fd, const char *name, const char *mime, char **format,
                             GError **err);
} KvpBitmapOps;

/* The plugin's main(): runs until kiview goes away. */
int kvp_bitmap_main(const KvpBitmapOps *ops);

#endif
