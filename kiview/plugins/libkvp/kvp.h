/* SPDX-License-Identifier: GPL-3.0-or-later */
/* libkvp - what a kiview plugin that shows a bitmap needs: the protocol, its
 * own X connection, the view (fit, zoom, pan, rotate), a small cache of
 * documents decoded in threads, animation and drawing. The plugin only
 * supplies the decoder. */
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

/* An animation: the decoder composes the frames itself, one at a time, into
 * the image surface the document shows (only the canvas stays in memory). */
typedef struct KvpAnim KvpAnim;
struct KvpAnim {
    int delay_ms; /* how long the frame now on the canvas stays */
    /* Draws the next frame into img (the same surface as before) and sets
     * delay_ms; after the last frame it starts over and sets *looped. False
     * on failure: the animation stops. Runs on the main thread. */
    gboolean (*next)(KvpAnim *a, cairo_surface_t *img, gboolean *looped);
    void (*destroy)(KvpAnim *a);
};

/* What a decoder is asked for: enough pixels for this much zoom. */
typedef struct {
    double min_zoom;    /* device pixels per document pixel needed; 0: fit the area */
    int area_w, area_h; /* the area in device pixels */
} KvpTarget;

typedef struct {
    cairo_surface_t *img; /* ARGB32 (premultiplied) or RGB24, at 1/denom of the document */
    int denom;
    int full_w, full_h;   /* the document's size, as stored (before orientation) */
    int flip, rot;        /* its orientation: see kvp_orientation() */
    char *format;         /* g_malloc'ed, "JPEG 4000×3000" */
    GBytes *keep;         /* set: load() can be called again for a sharper image */
    KvpAnim *anim;        /* animated: img is its canvas */
} KvpDecoded;

typedef struct {
    const char *caps; /* comma list for the plugin's hello */
    /* Decodes a whole file. Runs on a worker thread: no X, no shared state. */
    KvpDecoded *(*load)(GBytes *data, const char *name, const char *mime, const KvpTarget *t,
                        GError **err);
} KvpBitmapOps;

void kvp_decoded_free(KvpDecoded *r);

/* The plugin's main(): runs until kiview goes away. */
int kvp_bitmap_main(const KvpBitmapOps *ops);

#endif
