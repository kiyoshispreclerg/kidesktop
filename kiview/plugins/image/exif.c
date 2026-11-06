/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Just the orientation tag of the first IFD. */
#include <string.h>

#include "image.h"

static guint rd16(const guchar *p, gboolean le)
{
    return le ? (guint)(p[0] | p[1] << 8) : (guint)(p[0] << 8 | p[1]);
}

static guint32 rd32(const guchar *p, gboolean le)
{
    return le ? (guint32)p[0] | (guint32)p[1] << 8 | (guint32)p[2] << 16 | (guint32)p[3] << 24
              : (guint32)p[0] << 24 | (guint32)p[1] << 16 | (guint32)p[2] << 8 | p[3];
}

int kvi_exif_orientation(const guchar *d, gsize len)
{
    if (len < 14 || memcmp(d, "Exif\0\0", 6))
        return 1;
    const guchar *t = d + 6;
    gsize tl = len - 6;
    gboolean le;
    if (t[0] == 'I' && t[1] == 'I')
        le = TRUE;
    else if (t[0] == 'M' && t[1] == 'M')
        le = FALSE;
    else
        return 1;
    if (rd16(t + 2, le) != 42)
        return 1;
    guint32 off = rd32(t + 4, le);
    if ((gsize)off + 2 > tl)
        return 1;
    guint n = rd16(t + off, le);
    for (guint i = 0; i < n; i++) {
        gsize at = (gsize)off + 2 + 12 * (gsize)i;
        if (at + 12 > tl)
            break;
        if (rd16(t + at, le) == 0x0112) {
            guint v = rd16(t + at + 8, le);
            return v >= 1 && v <= 8 ? (int)v : 1;
        }
    }
    return 1;
}
