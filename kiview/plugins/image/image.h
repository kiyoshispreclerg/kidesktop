/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KVI_IMAGE_H
#define KVI_IMAGE_H

#include <glib.h>

#include <stdbool.h>

#include "../libkvp/kvp.h"
#include "../libkvp/kvp_view.h"

#define KVI_MAX_PIXELS (256u * 1024 * 1024)

/* Each returns NULL with *err set; KVP_ERR_UNSUPPORTED means "try another
 * decoder". Decoders borrow the bytes and ref them into ->keep or an
 * animation when they need them later. */
KvpDecoded *kvi_jpeg(GBytes *data, const KvpTarget *t, GError **err);
KvpDecoded *kvi_png(GBytes *data, const KvpTarget *t, GError **err);
KvpDecoded *kvi_gif(GBytes *data, const KvpTarget *t, GError **err);
KvpDecoded *kvi_webp(GBytes *data, const KvpTarget *t, GError **err);
KvpDecoded *kvi_pixbuf(GBytes *data, const KvpTarget *t, GError **err);

/* An image surface of the right format, or NULL (and *err) when it is too
 * large to allocate. */
cairo_surface_t *kvi_surface(int w, int h, gboolean alpha, GError **err);

/* The EXIF orientation (1..8) from an APP1 payload ("Exif\0\0" + TIFF). */
int kvi_exif_orientation(const guchar *exif, gsize len);

#endif
