/* SPDX-License-Identifier: GPL-3.0-or-later */
/* kiview-image: still and animated images. The format is told by the file's
 * first bytes (not its name); JPEG, PNG, GIF and WebP have their own
 * decoders, everything else goes through gdk-pixbuf's loaders. */
#include <string.h>

#include "image.h"

static KvpDecoded *load(GBytes *data, const char *name, const char *mime, const KvpTarget *t, GError **err)
{
    (void)name;
    (void)mime;
    gsize len;
    const guchar *p = g_bytes_get_data(data, &len);
    KvpDecoded *(*dec)(GBytes *, const KvpTarget *, GError **) = NULL;
    if (len > 3 && p[0] == 0xFF && p[1] == 0xD8 && p[2] == 0xFF)
        dec = kvi_jpeg;
    else if (len > 8 && !memcmp(p, "\x89PNG\r\n\x1a\n", 8))
        dec = kvi_png;
    else if (len > 6 && !memcmp(p, "GIF8", 4))
        dec = kvi_gif;
    else if (len > 12 && !memcmp(p, "RIFF", 4) && !memcmp(p + 8, "WEBP", 4))
        dec = kvi_webp;
    if (dec) {
        GError *e = NULL;
        KvpDecoded *r = dec(data, t, &e);
        if (r)
            // cppcheck-suppress returnDanglingLifetime
            return r; /* false positive: r is heap-allocated, only &e was passed to dec() */
        if (!g_error_matches(e, KVP_ERROR, KVP_ERR_UNSUPPORTED)) {
            g_propagate_error(err, e);
            return NULL;
        }
        g_clear_error(&e); /* not built in, or a variant it can't do: gdk-pixbuf may */
    }
    return kvi_pixbuf(data, t, err);
}

int main(void)
{
    static const KvpBitmapOps ops = {.caps = "rotate,anim,prefetch", .load = load};
    return kvp_bitmap_main(&ops);
}
