/* SPDX-License-Identifier: GPL-3.0-or-later */
/* GIF through giflib, one frame at a time: only the composed canvas (the
 * image the document shows) lives in memory, not every frame. A still GIF
 * is decoded once and the decoder dropped. */
#include "image.h"

#ifdef HAVE_GIF
#include <gif_lib.h>
#include <string.h>

typedef struct {
    const guchar *p;
    gsize len, pos;
} Mem;

typedef struct {
    KvpAnim base;
    GBytes *data;
    Mem mem;
    GifFileType *gif;
    gboolean have_rt;
    GifRecordType rt;
    int prev_disposal;
    int px, py, pw, ph; /* the previous frame's rectangle */
    guint32 *saved;     /* canvas before a "restore previous" frame */
} Gif;

static int read_fn(GifFileType *gif, GifByteType *buf, int n)
{
    Mem *m = gif->UserData;
    int k = (int)MIN((gsize)n, m->len - m->pos);
    memcpy(buf, m->p + m->pos, (gsize)k);
    m->pos += (gsize)k;
    return k;
}

static GifFileType *open_gif(Mem *m)
{
    int e;
    m->pos = 0;
    return DGifOpen(m, read_fn, &e);
}

/* Composes the next frame on the canvas. 1: done, 0: no more frames, -1:
 * error. */
static int next_frame(Gif *g, guint32 *canvas, int cw, int ch, int stride_px)
{
    static const int ioff[] = {0, 4, 2, 1}, istep[] = {8, 8, 4, 2};
    int transparent = NO_TRANSPARENT_COLOR, disposal = DISPOSAL_UNSPECIFIED, delay = 100;
    for (;;) {
        GifRecordType rt;
        if (g->have_rt) {
            rt = g->rt;
            g->have_rt = FALSE;
        } else if (DGifGetRecordType(g->gif, &rt) == GIF_ERROR) {
            return -1;
        }
        if (rt == TERMINATE_RECORD_TYPE)
            return 0;
        if (rt == EXTENSION_RECORD_TYPE) {
            int code;
            GifByteType *ext;
            if (DGifGetExtension(g->gif, &code, &ext) == GIF_ERROR)
                return -1;
            while (ext) {
                if (code == GRAPHICS_EXT_FUNC_CODE && ext[0] >= 4) {
                    GraphicsControlBlock gcb;
                    if (DGifExtensionToGCB(ext[0], ext + 1, &gcb) == GIF_OK) {
                        transparent = gcb.TransparentColor;
                        disposal = gcb.DisposalMode;
                        delay = gcb.DelayTime * 10;
                    }
                }
                if (DGifGetExtensionNext(g->gif, &ext) == GIF_ERROR)
                    return -1;
            }
            continue;
        }
        if (rt != IMAGE_DESC_RECORD_TYPE)
            continue;
        if (DGifGetImageDesc(g->gif) == GIF_ERROR)
            return -1;
        GifImageDesc *d = &g->gif->Image;
        ColorMapObject *cmap = d->ColorMap ? d->ColorMap : g->gif->SColorMap;

        /* what the previous frame asked for happens now */
        if (g->prev_disposal == DISPOSE_BACKGROUND) {
            for (int y = MAX(g->py, 0); y < MIN(g->py + g->ph, ch); y++)
                for (int x = MAX(g->px, 0); x < MIN(g->px + g->pw, cw); x++)
                    canvas[(gsize)y * stride_px + x] = 0;
        } else if (g->prev_disposal == DISPOSE_PREVIOUS && g->saved) {
            for (int y = 0; y < ch; y++)
                memcpy(canvas + (gsize)y * stride_px, g->saved + (gsize)y * cw, (gsize)cw * 4);
        }
        if (disposal == DISPOSE_PREVIOUS) {
            if (!g->saved)
                g->saved = g_malloc((gsize)cw * ch * 4);
            for (int y = 0; y < ch; y++)
                memcpy(g->saved + (gsize)y * cw, canvas + (gsize)y * stride_px, (gsize)cw * 4);
        }

        GifByteType *line = g_malloc((gsize)MAX(d->Width, 1));
        int rows = d->Height, passes = d->Interlace ? 4 : 1;
        gboolean ok = TRUE;
        int row = 0;
        for (int pass = 0; pass < passes && ok; pass++) {
            int y0 = d->Interlace ? ioff[pass] : 0, step = d->Interlace ? istep[pass] : 1;
            for (int r = y0; r < rows && ok; r += step, row++) {
                if (DGifGetLine(g->gif, line, d->Width) == GIF_ERROR) {
                    ok = FALSE;
                    break;
                }
                int y = d->Top + r;
                if (y < 0 || y >= ch || !cmap)
                    continue;
                for (int x = 0; x < d->Width; x++) {
                    int cx = d->Left + x, idx = line[x];
                    if (cx < 0 || cx >= cw || idx == transparent || idx >= cmap->ColorCount)
                        continue;
                    GifColorType c = cmap->Colors[idx];
                    canvas[(gsize)y * stride_px + cx] = 0xff000000u | (guint32)c.Red << 16 | (guint32)c.Green << 8 | c.Blue;
                }
            }
        }
        g_free(line);
        if (!ok)
            return -1;
        g->px = d->Left;
        g->py = d->Top;
        g->pw = d->Width;
        g->ph = d->Height;
        g->prev_disposal = disposal;
        g->base.delay_ms = delay <= 10 ? 100 : delay; /* what browsers do */
        return 1;
    }
}

static gboolean anim_next(KvpAnim *a, cairo_surface_t *img, gboolean *looped)
{
    Gif *g = (Gif *)a;
    int cw = cairo_image_surface_get_width(img), ch = cairo_image_surface_get_height(img);
    cairo_surface_flush(img);
    guint32 *canvas = (guint32 *)cairo_image_surface_get_data(img);
    int stride_px = cairo_image_surface_get_stride(img) / 4;
    int r = next_frame(g, canvas, cw, ch, stride_px);
    if (r == 0) {
        /* the end: from the start again, on an empty canvas */
        int e;
        DGifCloseFile(g->gif, &e);
        g->gif = open_gif(&g->mem);
        if (!g->gif)
            return FALSE;
        for (int y = 0; y < ch; y++)
            memset(canvas + (gsize)y * stride_px, 0, (gsize)cw * 4);
        g->prev_disposal = DISPOSAL_UNSPECIFIED;
        *looped = TRUE;
        r = next_frame(g, canvas, cw, ch, stride_px);
    }
    cairo_surface_mark_dirty(img);
    return r == 1;
}

static void anim_destroy(KvpAnim *a)
{
    Gif *g = (Gif *)a;
    int e;
    if (g->gif)
        DGifCloseFile(g->gif, &e);
    g_free(g->saved);
    g_bytes_unref(g->data);
    g_free(g);
}

KvpDecoded *kvi_gif(GBytes *data, const KvpTarget *t, GError **err)
{
    (void)t;
    Gif *g = g_new0(Gif, 1);
    g->mem.p = g_bytes_get_data(data, &g->mem.len);
    g->gif = open_gif(&g->mem);
    if (!g->gif) {
        g_free(g);
        g_set_error(err, KVP_ERROR, KVP_ERR_CORRUPT, "not a valid GIF image");
        return NULL;
    }
    int w = g->gif->SWidth, h = g->gif->SHeight;
    if (w <= 0 || h <= 0 || (guint64)w * h > KVI_MAX_PIXELS) {
        int e;
        DGifCloseFile(g->gif, &e);
        g_free(g);
        g_set_error(err, KVP_ERROR, w <= 0 || h <= 0 ? KVP_ERR_CORRUPT : KVP_ERR_TOOLARGE, "bad GIF size %d×%d", w, h);
        return NULL;
    }
    cairo_surface_t *s = kvi_surface(w, h, TRUE, err);
    if (!s) {
        int e;
        DGifCloseFile(g->gif, &e);
        g_free(g);
        return NULL;
    }
    cairo_surface_flush(s);
    memset(cairo_image_surface_get_data(s), 0, (gsize)cairo_image_surface_get_stride(s) * h);
    int r = next_frame(g, (guint32 *)cairo_image_surface_get_data(s), w, h, cairo_image_surface_get_stride(s) / 4);
    cairo_surface_mark_dirty(s);
    if (r != 1) {
        int e;
        DGifCloseFile(g->gif, &e);
        g_free(g);
        cairo_surface_destroy(s);
        g_set_error(err, KVP_ERROR, KVP_ERR_CORRUPT, "the GIF image has no frames");
        return NULL;
    }

    /* more frames? */
    GifRecordType rt;
    gboolean animated = FALSE;
    if (DGifGetRecordType(g->gif, &rt) == GIF_OK && rt != TERMINATE_RECORD_TYPE) {
        g->have_rt = TRUE;
        g->rt = rt;
        animated = TRUE;
    }
    KvpDecoded *d = g_new0(KvpDecoded, 1);
    d->img = s;
    d->denom = 1;
    d->full_w = w;
    d->full_h = h;
    d->format = g_strdup_printf("GIF %d×%d%s", w, h, animated ? " (animated)" : "");
    if (animated) {
        g->data = g_bytes_ref(data);
        g->base.next = anim_next;
        g->base.destroy = anim_destroy;
        d->anim = &g->base;
    } else {
        anim_destroy(&g->base);
        g->data = NULL;
    }
    return d;
}
#else
KvpDecoded *kvi_gif(GBytes *data, const KvpTarget *t, GError **err)
{
    (void)data;
    (void)t;
    g_set_error(err, KVP_ERROR, KVP_ERR_UNSUPPORTED, "built without giflib");
    return NULL;
}
#endif
