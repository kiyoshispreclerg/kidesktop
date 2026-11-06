/* SPDX-License-Identifier: GPL-3.0-or-later */
/* WebP through libwebp: animated files by WebPAnimDecoder (which composes
 * the frames on its canvas; we copy one frame at a time into the image). */
#include "image.h"

#ifdef HAVE_WEBP
#include <string.h>
#include <webp/decode.h>
#include <webp/demux.h>

typedef struct {
    KvpAnim base;
    GBytes *data;
    WebPAnimDecoder *dec;
    int prev_ts;
    int w, h;
} Webp;

static gboolean copy_frame(WebPAnimDecoder *dec, cairo_surface_t *img, int w, int h, int *ts)
{
    guint8 *buf;
    if (!WebPAnimDecoderGetNext(dec, &buf, ts))
        return FALSE;
    cairo_surface_flush(img);
    guchar *dst = cairo_image_surface_get_data(img);
    int stride = cairo_image_surface_get_stride(img);
    for (int y = 0; y < h; y++)
        memcpy(dst + (gsize)y * stride, buf + (gsize)y * w * 4, (gsize)w * 4);
    cairo_surface_mark_dirty(img);
    return TRUE;
}

static gboolean anim_next(KvpAnim *a, cairo_surface_t *img, gboolean *looped)
{
    Webp *w = (Webp *)a;
    if (!WebPAnimDecoderHasMoreFrames(w->dec)) {
        WebPAnimDecoderReset(w->dec);
        w->prev_ts = 0;
        *looped = TRUE;
    }
    int ts;
    if (!copy_frame(w->dec, img, w->w, w->h, &ts))
        return FALSE;
    a->delay_ms = ts - w->prev_ts;
    w->prev_ts = ts;
    return TRUE;
}

static void anim_destroy(KvpAnim *a)
{
    Webp *w = (Webp *)a;
    WebPAnimDecoderDelete(w->dec);
    g_bytes_unref(w->data);
    g_free(w);
}

KvpDecoded *kvi_webp(GBytes *data, const KvpTarget *t, GError **err)
{
    (void)t;
    gsize len;
    const guchar *p = g_bytes_get_data(data, &len);
    WebPBitstreamFeatures feat;
    if (WebPGetFeatures(p, len, &feat) != VP8_STATUS_OK) {
        g_set_error(err, KVP_ERROR, KVP_ERR_CORRUPT, "not a valid WebP image");
        return NULL;
    }
    int w = feat.width, h = feat.height;
    if ((guint64)w * h > KVI_MAX_PIXELS) {
        g_set_error(err, KVP_ERROR, KVP_ERR_TOOLARGE, "%d×%d is too large", w, h);
        return NULL;
    }
    WebPData wd = {p, len};
    WebPAnimDecoderOptions opt;
    WebPAnimDecoderOptionsInit(&opt);
    opt.color_mode = MODE_bgrA; /* premultiplied, as cairo wants it */
    opt.use_threads = 0;
    WebPAnimDecoder *dec = WebPAnimDecoderNew(&wd, &opt);
    if (!dec) {
        g_set_error(err, KVP_ERROR, KVP_ERR_CORRUPT, "can't decode the WebP image");
        return NULL;
    }
    WebPAnimInfo info;
    WebPAnimDecoderGetInfo(dec, &info);
    cairo_surface_t *s = kvi_surface(w, h, feat.has_alpha, err);
    int ts;
    if (!s) {
        WebPAnimDecoderDelete(dec);
        return NULL;
    }
    if (!copy_frame(dec, s, w, h, &ts)) {
        WebPAnimDecoderDelete(dec);
        cairo_surface_destroy(s);
        g_set_error(err, KVP_ERROR, KVP_ERR_CORRUPT, "can't decode the WebP image");
        return NULL;
    }
    KvpDecoded *d = g_new0(KvpDecoded, 1);
    d->img = s;
    d->denom = 1;
    d->full_w = w;
    d->full_h = h;
    gboolean animated = info.frame_count > 1;
    d->format = g_strdup_printf("WebP %d×%d%s", w, h, animated ? " (animated)" : "");
    if (animated) {
        Webp *wp = g_new0(Webp, 1);
        wp->data = g_bytes_ref(data);
        wp->dec = dec;
        wp->prev_ts = ts;
        wp->w = w;
        wp->h = h;
        wp->base.delay_ms = ts;
        wp->base.next = anim_next;
        wp->base.destroy = anim_destroy;
        d->anim = &wp->base;
    } else {
        WebPAnimDecoderDelete(dec);
    }
    return d;
}
#else
KvpDecoded *kvi_webp(GBytes *data, const KvpTarget *t, GError **err)
{
    (void)data;
    (void)t;
    g_set_error(err, KVP_ERROR, KVP_ERR_UNSUPPORTED, "built without libwebp");
    return NULL;
}
#endif
