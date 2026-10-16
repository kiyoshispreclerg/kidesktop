/* ks_av.c - kishot-rec-av.so: encodes recordings with libavcodec/libavformat.
 *
 * Video always goes through a small libavfilter graph, which keeps the
 * three input/encoder combinations in one code path:
 *   dma-buf, VAAPI encoder     hwmap=derive_device=vaapi,scale_vaapi=format=nv12
 *                              (zero-copy: the frame never leaves the GPU)
 *   dma-buf, software encoder  ... then hwdownload,format=nv12 (the GPU does
 *                              the colour conversion; NV12 is read back)
 *   memory, VAAPI encoder      format=nv12,hwupload
 *   memory, software encoder   format=<encoder's>
 * The dma-buf is the pixmap kishot's capture thread keeps CopyArea()ing the
 * screen into, described once as a DRM PRIME frame and mapped into VAAPI on
 * every frame.
 *
 * Audio arrives as S16 interleaved and is converted by hand to whatever the
 * encoder takes (aac wants planar float, opus takes S16). */
#include "../ks_plugin.h"
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NEW_CH_LAYOUT (LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 24, 100))
#define FOURCC(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define DRM_XRGB8888 FOURCC('X', 'R', '2', '4')
#define DRM_MOD_INVALID ((1ULL << 56) - 1)

static const char *const kCandidates[] = {
    "h264_vaapi", "hevc_vaapi", "vp9_vaapi", "av1_vaapi",
    "libx264", "libx265", "libvpx-vp9", "libvpx", "libsvtav1", "libaom-av1",
    "mpeg4", "ffv1", NULL
};
static const char *usable[16];
static int probed;
static char *device_path;
static AVBufferRef *drm_device;  /* the render node, for importing dma-bufs */
static AVBufferRef *va_device;   /* derived from it: shared by every recording */

static int is_vaapi(const char *n) { size_t l = strlen(n); return l > 6 && !strcmp(n + l - 6, "_vaapi"); }

/* Some VAAPI drivers list an encoder they can't run (no entrypoint for
 * that codec on this GPU): open each one on a tiny surface pool. */
static int vaapi_encoder_works(const AVCodec *c)
{
    AVBufferRef *frames = av_hwframe_ctx_alloc(va_device);
    if (!frames)
        return 0;
    AVHWFramesContext *fc = (AVHWFramesContext *)frames->data;
    fc->format = AV_PIX_FMT_VAAPI;
    fc->sw_format = AV_PIX_FMT_NV12;
    fc->width = 256;
    fc->height = 256;
    int ok = 0;
    if (av_hwframe_ctx_init(frames) >= 0) {
        AVCodecContext *ctx = avcodec_alloc_context3(c);
        ctx->width = 256;
        ctx->height = 256;
        ctx->time_base = (AVRational){ 1, 30 };
        ctx->pix_fmt = AV_PIX_FMT_VAAPI;
        ctx->hw_frames_ctx = av_buffer_ref(frames);
        ok = avcodec_open2(ctx, c, NULL) >= 0;
        avcodec_free_context(&ctx);
    }
    av_buffer_unref(&frames);
    return ok;
}

static void probe(void)
{
    if (probed)
        return;
    probed = 1;
    /* trying encoders the GPU lacks prints errors; nothing to report */
    av_log_set_level(AV_LOG_QUIET);
    /* DRM first and VAAPI derived from it: FFmpeg 4.4 can derive VAAPI from
     * DRM but not the other way round, and importing needs both */
    if (device_path && av_hwdevice_ctx_create(&drm_device, AV_HWDEVICE_TYPE_DRM, device_path, NULL, 0) >= 0 &&
        av_hwdevice_ctx_create_derived(&va_device, AV_HWDEVICE_TYPE_VAAPI, drm_device, 0) < 0)
        va_device = NULL;
    if (!va_device && av_hwdevice_ctx_create(&va_device, AV_HWDEVICE_TYPE_VAAPI, device_path, NULL, 0) < 0)
        va_device = NULL;
    int n = 0;
    for (int i = 0; kCandidates[i]; i++) {
        const AVCodec *c = avcodec_find_encoder_by_name(kCandidates[i]);
        if (!c)
            continue;
        if (is_vaapi(kCandidates[i]) && (!va_device || !vaapi_encoder_works(c)))
            continue;
        usable[n++] = kCandidates[i];
    }
    usable[n] = NULL;
    /* KISHOT_AV_LOG=verbose|debug|info for diagnosing a failing recording */
    const char *lvl = getenv("KISHOT_AV_LOG");
    av_log_set_level(!lvl ? AV_LOG_ERROR : !strcmp(lvl, "debug") ? AV_LOG_DEBUG
                     : !strcmp(lvl, "verbose") ? AV_LOG_VERBOSE : AV_LOG_INFO);
}

static void api_set_device(const char *render_node)
{
    if (probed)
        return;
    free(device_path);
    device_path = render_node ? strdup(render_node) : NULL;
}

static const char *const *api_encoders(void)
{
    probe();
    return usable;
}

static int api_vaapi_available(void)
{
    probe();
    return va_device != NULL;
}

typedef struct {
    AVFormatContext *fmt;
    AVCodecContext *venc, *aenc;
    AVStream *vst, *ast;
    AVFilterGraph *graph;
    AVFilterContext *src, *sink;
    AVBufferRef *drm_device, *drm_frames;
    AVDRMFrameDescriptor desc;
    AVFrame *filt;
    AVPacket *pkt;
    pthread_mutex_t lock;
    int64_t last_vpts;
    int width, height, fps;
    /* audio */
    int16_t *abuf;
    int afill, acap;
    int64_t anext;              /* in samples */
    int astarted;
    int channels;
    AVFrame *aframe;
    char what[160];
    int opened;                 /* api_open() succeeded: flush on close */
    int header_written;
    AVPacket *held[64];         /* audio packets that came before the header */
    int nheld;
} Enc;

static void errf(char *err, size_t len, const char *what, int ret)
{
    if (!err || !len)
        return;
    char buf[AV_ERROR_MAX_STRING_SIZE] = "";
    if (ret < 0)
        av_strerror(ret, buf, sizeof(buf));
    snprintf(err, len, "%s%s%s", what, ret < 0 ? ": " : "", buf);
}

/* Encoders without packed-header support (VAAPI on older Mesa) give no
 * extradata, which Matroska and MP4 need in the header: take the parameter
 * sets (H.264 SPS/PPS, HEVC VPS/SPS/PPS) from the first keyframe instead --
 * everything before its first slice. The ffmpeg command does the same by
 * deferring the header too. */
static void extradata_from_packet(AVCodecParameters *par, const AVPacket *pkt)
{
    int hevc = par->codec_id == AV_CODEC_ID_HEVC;
    if (par->extradata_size || (par->codec_id != AV_CODEC_ID_H264 && !hevc))
        return;
    const uint8_t *d = pkt->data;
    int n = pkt->size, end = 0;
    for (int i = 0; i + 3 < n; i++) {
        if (d[i] || d[i + 1] || d[i + 2] != 1)
            continue;
        int type = hevc ? (d[i + 3] >> 1) & 0x3f : d[i + 3] & 0x1f;
        int vcl = hevc ? type < 32 : (type >= 1 && type <= 5);
        if (vcl) {
            end = (i > 0 && !d[i - 1]) ? i - 1 : i;     /* 4-byte start code */
            break;
        }
    }
    if (end <= 0)
        return;
    par->extradata = av_mallocz(end + AV_INPUT_BUFFER_PADDING_SIZE);
    memcpy(par->extradata, d, end);
    par->extradata_size = end;
}

static int write_rescaled(Enc *e, AVPacket *pkt)
{
    AVCodecContext *enc = pkt->stream_index == e->vst->index ? e->venc : e->aenc;
    av_packet_rescale_ts(pkt, enc->time_base, e->fmt->streams[pkt->stream_index]->time_base);
    return av_interleaved_write_frame(e->fmt, pkt);
}

/* Call with e->lock held; pkt is in its encoder's time base (the stream's
 * is only final once the header is written). */
static int mux(Enc *e, AVPacket *pkt)
{
    if (!e->header_written) {
        if (pkt->stream_index != e->vst->index) {
            if (e->nheld < (int)(sizeof(e->held) / sizeof(e->held[0])))
                e->held[e->nheld++] = av_packet_clone(pkt);
            return 0;
        }
        extradata_from_packet(e->vst->codecpar, pkt);
        int ret = avformat_write_header(e->fmt, NULL);
        if (ret < 0)
            return ret;
        e->header_written = 1;
        ret = write_rescaled(e, pkt);
        for (int i = 0; i < e->nheld; i++) {
            if (ret >= 0)
                ret = write_rescaled(e, e->held[i]);
            av_packet_free(&e->held[i]);
        }
        e->nheld = 0;
        return ret;
    }
    return write_rescaled(e, pkt);
}

static int write_packets(Enc *e, AVCodecContext *enc, AVStream *st)
{
    for (;;) {
        int ret = avcodec_receive_packet(enc, e->pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return 0;
        if (ret < 0)
            return ret;
        e->pkt->stream_index = st->index;
        pthread_mutex_lock(&e->lock);
        ret = mux(e, e->pkt);
        pthread_mutex_unlock(&e->lock);
        av_packet_unref(e->pkt);
        if (ret < 0)
            return ret;
    }
}

static void noop_free(void *opaque, uint8_t *data) { (void)opaque; (void)data; }

static int build_graph(Enc *e, const KsEncParams *p, const AVCodec *codec, char *err, size_t errlen)
{
    int vaapi_enc = is_vaapi(codec->name);
    int dmabuf = p->input == KS_INPUT_DMABUF;
    char chain[256];
    enum AVPixelFormat sw_fmt = AV_PIX_FMT_YUV420P;
    if (!vaapi_enc && codec->pix_fmts) {
        int has420 = 0;
        for (const enum AVPixelFormat *f = codec->pix_fmts; *f != AV_PIX_FMT_NONE; f++)
            has420 |= *f == AV_PIX_FMT_YUV420P;
        if (!has420)
            sw_fmt = codec->pix_fmts[0];
    }
    if (dmabuf && vaapi_enc)
        snprintf(chain, sizeof(chain), "hwmap=derive_device=vaapi,scale_vaapi=format=nv12");
    else if (dmabuf)
        snprintf(chain, sizeof(chain),
                 "hwmap=derive_device=vaapi,scale_vaapi=format=nv12,hwdownload,format=nv12%s%s",
                 sw_fmt == AV_PIX_FMT_YUV420P ? "" : ",format=", sw_fmt == AV_PIX_FMT_YUV420P ? "" : av_get_pix_fmt_name(sw_fmt));
    else if (vaapi_enc)
        snprintf(chain, sizeof(chain), "format=nv12,hwupload");
    else
        snprintf(chain, sizeof(chain), "format=%s", av_get_pix_fmt_name(sw_fmt));

    e->graph = avfilter_graph_alloc();
    char args[256];
    snprintf(args, sizeof(args), "video_size=%dx%d:pix_fmt=%d:time_base=1/1000000:pixel_aspect=1/1",
             p->width, p->height, dmabuf ? AV_PIX_FMT_DRM_PRIME : AV_PIX_FMT_BGR0);
    int ret = avfilter_graph_create_filter(&e->src, avfilter_get_by_name("buffer"), "in", args, NULL, e->graph);
    if (ret < 0) {
        errf(err, errlen, "buffer source", ret);
        return ret;
    }
    if (dmabuf) {
        AVBufferSrcParameters *par = av_buffersrc_parameters_alloc();
        par->format = AV_PIX_FMT_DRM_PRIME;
        par->hw_frames_ctx = e->drm_frames;
        ret = av_buffersrc_parameters_set(e->src, par);
        av_free(par);
        if (ret < 0) {
            errf(err, errlen, "buffer source parameters", ret);
            return ret;
        }
    }
    ret = avfilter_graph_create_filter(&e->sink, avfilter_get_by_name("buffersink"), "out", NULL, NULL, e->graph);
    if (ret < 0) {
        errf(err, errlen, "buffer sink", ret);
        return ret;
    }
    AVFilterInOut *outs = avfilter_inout_alloc(), *ins = avfilter_inout_alloc();
    outs->name = av_strdup("in");
    outs->filter_ctx = e->src;
    ins->name = av_strdup("out");
    ins->filter_ctx = e->sink;
    ret = avfilter_graph_parse_ptr(e->graph, chain, &ins, &outs, NULL);
    avfilter_inout_free(&ins);
    avfilter_inout_free(&outs);
    if (ret < 0) {
        errf(err, errlen, chain, ret);
        return ret;
    }
    for (unsigned i = 0; i < e->graph->nb_filters; i++)
        if (va_device && !strcmp(e->graph->filters[i]->filter->name, "hwupload"))
            e->graph->filters[i]->hw_device_ctx = av_buffer_ref(va_device);
    ret = avfilter_graph_config(e->graph, NULL);
    if (ret < 0) {
        errf(err, errlen, chain, ret);
        return ret;
    }
    if (dmabuf && vaapi_enc)
        snprintf(e->what, sizeof(e->what), "VAAPI %s (zero-copy)", codec->name);
    else if (dmabuf)
        snprintf(e->what, sizeof(e->what), "VAAPI → %s", codec->name);
    else if (vaapi_enc)
        snprintf(e->what, sizeof(e->what), "%s (upload)", codec->name);
    else
        snprintf(e->what, sizeof(e->what), "%s", codec->name);
    return 0;
}

static void set_quality(AVCodecContext *c, const char *name, int q)
{
    if (is_vaapi(name)) {
        c->global_quality = q > 0 ? q : 1;
    } else if (!strcmp(name, "libx264") || !strcmp(name, "libx265")) {
        av_opt_set(c->priv_data, "preset", "veryfast", 0);
        av_opt_set_int(c->priv_data, "crf", q, 0);
    } else if (!strcmp(name, "libvpx-vp9") || !strcmp(name, "libvpx")) {
        av_opt_set(c->priv_data, "deadline", "realtime", 0);
        av_opt_set_int(c->priv_data, "cpu-used", 8, 0);
        av_opt_set_int(c->priv_data, "crf", q < 4 ? 4 : q, 0);
        c->bit_rate = 0;
    } else if (!strcmp(name, "libsvtav1")) {
        av_opt_set_int(c->priv_data, "preset", 10, 0);
        av_opt_set_int(c->priv_data, "crf", q, 0);
    } else if (!strcmp(name, "libaom-av1")) {
        av_opt_set_int(c->priv_data, "cpu-used", 8, 0);
        av_opt_set_int(c->priv_data, "crf", q, 0);
        av_opt_set(c->priv_data, "usage", "realtime", 0);
    } else if (!strcmp(name, "mpeg4")) {
        c->flags |= AV_CODEC_FLAG_QSCALE;
        c->global_quality = FF_QP2LAMBDA * (1 + q * 30 / 51);
    }
}

static const char *pick_audio_codec(const char *container)
{
    const char *opts_webm[] = { "libopus", "libvorbis", NULL };
    const char *opts_other[] = { "aac", "libopus", "libmp3lame", NULL };
    const char **list = !strcmp(container, "webm") ? opts_webm : opts_other;
    for (int i = 0; list[i]; i++)
        if (avcodec_find_encoder_by_name(list[i]))
            return list[i];
    return NULL;
}

static enum AVSampleFormat pick_sample_fmt(const AVCodec *c)
{
    static const enum AVSampleFormat pref[] = { AV_SAMPLE_FMT_S16, AV_SAMPLE_FMT_FLTP, AV_SAMPLE_FMT_FLT, AV_SAMPLE_FMT_S16P };
    if (!c->sample_fmts)
        return AV_SAMPLE_FMT_S16;
    for (unsigned k = 0; k < sizeof(pref) / sizeof(pref[0]); k++)
        for (const enum AVSampleFormat *f = c->sample_fmts; *f != AV_SAMPLE_FMT_NONE; f++)
            if (*f == pref[k])
                return *f;
    return AV_SAMPLE_FMT_NONE;
}

static int open_audio(Enc *e, const KsEncParams *p, char *err, size_t errlen)
{
    const char *name = pick_audio_codec(p->container);
    const AVCodec *c = name ? avcodec_find_encoder_by_name(name) : NULL;
    if (!c) {
        errf(err, errlen, "no audio encoder for this container", 0);
        return -1;
    }
    e->aenc = avcodec_alloc_context3(c);
    e->aenc->sample_rate = p->sample_rate;
    e->aenc->sample_fmt = pick_sample_fmt(c);
    if (e->aenc->sample_fmt == AV_SAMPLE_FMT_NONE) {
        errf(err, errlen, "audio encoder takes no supported sample format", 0);
        return -1;
    }
#if NEW_CH_LAYOUT
    av_channel_layout_default(&e->aenc->ch_layout, p->channels);
#else
    e->aenc->channels = p->channels;
    e->aenc->channel_layout = av_get_default_channel_layout(p->channels);
#endif
    e->aenc->bit_rate = 160000;
    e->aenc->time_base = (AVRational){ 1, p->sample_rate };
    if (e->fmt->oformat->flags & AVFMT_GLOBALHEADER)
        e->aenc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    int ret = avcodec_open2(e->aenc, c, NULL);
    if (ret < 0) {
        errf(err, errlen, name, ret);
        return ret;
    }
    e->ast = avformat_new_stream(e->fmt, NULL);
    e->ast->time_base = e->aenc->time_base;
    avcodec_parameters_from_context(e->ast->codecpar, e->aenc);
    e->channels = p->channels;
    e->aframe = av_frame_alloc();
    return 0;
}

static int api_close(void *h, char *err, size_t errlen);

static void *api_open(const KsEncParams *p, char *err, size_t errlen)
{
    probe();
    const AVCodec *codec = avcodec_find_encoder_by_name(p->vcodec);
    if (!codec) {
        errf(err, errlen, "video encoder not found", 0);
        return NULL;
    }
    if ((is_vaapi(p->vcodec) || p->input == KS_INPUT_DMABUF) && !va_device) {
        errf(err, errlen, "no VAAPI device", 0);
        return NULL;
    }
    if (p->input == KS_INPUT_DMABUF && !drm_device) {
        errf(err, errlen, "no DRM device for the X server's GPU", 0);
        return NULL;
    }
    Enc *e = calloc(1, sizeof(*e));
    pthread_mutex_init(&e->lock, NULL);
    e->width = p->width;
    e->height = p->height;
    e->fps = p->fps;
    e->last_vpts = -1;
    e->pkt = av_packet_alloc();
    e->filt = av_frame_alloc();

    const char *fmtname = !strcmp(p->container, "mkv") ? "matroska" : p->container;
    int ret = avformat_alloc_output_context2(&e->fmt, NULL, fmtname, p->path);
    if (ret < 0 || !e->fmt) {
        errf(err, errlen, "container", ret);
        goto fail;
    }

    if (p->input == KS_INPUT_DMABUF) {
        e->drm_device = av_buffer_ref(drm_device);
        e->drm_frames = av_hwframe_ctx_alloc(e->drm_device);
        AVHWFramesContext *fc = (AVHWFramesContext *)e->drm_frames->data;
        fc->format = AV_PIX_FMT_DRM_PRIME;
        fc->sw_format = AV_PIX_FMT_BGR0;
        fc->width = p->width;
        fc->height = p->height;
        ret = av_hwframe_ctx_init(e->drm_frames);
        if (ret < 0) {
            errf(err, errlen, "DRM frames", ret);
            goto fail;
        }
        e->desc.nb_objects = 1;
        e->desc.objects[0].fd = dup(p->dmabuf_fd);
        e->desc.objects[0].size = p->dmabuf_size;
        e->desc.objects[0].format_modifier = DRM_MOD_INVALID;
        e->desc.nb_layers = 1;
        e->desc.layers[0].format = DRM_XRGB8888;
        e->desc.layers[0].nb_planes = 1;
        e->desc.layers[0].planes[0].object_index = 0;
        e->desc.layers[0].planes[0].offset = 0;
        e->desc.layers[0].planes[0].pitch = p->dmabuf_stride;
    }

    if (build_graph(e, p, codec, err, errlen) < 0)
        goto fail;

    e->venc = avcodec_alloc_context3(codec);
    e->venc->width = p->width;
    e->venc->height = p->height;
    e->venc->time_base = (AVRational){ 1, p->fps };
    e->venc->framerate = (AVRational){ p->fps, 1 };
    e->venc->gop_size = p->fps * 2;
    e->venc->max_b_frames = 0;
    e->venc->pix_fmt = av_buffersink_get_format(e->sink);
    e->venc->sample_aspect_ratio = (AVRational){ 1, 1 };
    AVBufferRef *hwf = av_buffersink_get_hw_frames_ctx(e->sink);
    if (hwf)
        e->venc->hw_frames_ctx = av_buffer_ref(hwf);
    if (e->fmt->oformat->flags & AVFMT_GLOBALHEADER)
        e->venc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    set_quality(e->venc, codec->name, p->quality);
    ret = avcodec_open2(e->venc, codec, NULL);
    if (ret < 0) {
        errf(err, errlen, codec->name, ret);
        goto fail;
    }
    e->vst = avformat_new_stream(e->fmt, NULL);
    e->vst->time_base = e->venc->time_base;
    avcodec_parameters_from_context(e->vst->codecpar, e->venc);

    if (p->audio && open_audio(e, p, err, errlen) < 0)
        goto fail;

    ret = avio_open(&e->fmt->pb, p->path, AVIO_FLAG_WRITE);
    if (ret < 0) {
        errf(err, errlen, p->path, ret);
        goto fail;
    }
    /* the header is written with the first video packet, see mux() */
    e->opened = 1;
    return e;

fail:
    api_close(e, NULL, 0);
    return NULL;
}

static const char *api_describe(void *h)
{
    return ((Enc *)h)->what;
}

static int push_video(Enc *e, AVFrame *in)
{
    int ret = av_buffersrc_add_frame_flags(e->src, in, AV_BUFFERSRC_FLAG_KEEP_REF);
    if (ret < 0)
        return ret;
    for (;;) {
        ret = av_buffersink_get_frame(e->sink, e->filt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return 0;
        if (ret < 0)
            return ret;
        e->filt->pts = av_rescale_q(e->filt->pts, (AVRational){ 1, 1000000 }, e->venc->time_base);
        if (e->filt->pts <= e->last_vpts) {
            av_frame_unref(e->filt);
            continue;
        }
        e->last_vpts = e->filt->pts;
        ret = avcodec_send_frame(e->venc, e->filt);
        av_frame_unref(e->filt);
        if (ret < 0)
            return ret;
        ret = write_packets(e, e->venc, e->vst);
        if (ret < 0)
            return ret;
    }
}

static int api_video_cpu(void *h, const uint8_t *bgrx, int stride, int64_t pts_us)
{
    Enc *e = h;
    AVFrame *f = av_frame_alloc();
    f->format = AV_PIX_FMT_BGR0;
    f->width = e->width;
    f->height = e->height;
    f->data[0] = (uint8_t *)bgrx;
    f->linesize[0] = stride;
    f->buf[0] = av_buffer_create((uint8_t *)bgrx, (size_t)stride * e->height, noop_free, NULL, AV_BUFFER_FLAG_READONLY);
    f->pts = pts_us;
    /* the graph converts (or uploads) synchronously inside push_video(), so
     * wrapping the SHM buffer without a copy is safe */
    int ret = push_video(e, f);
    av_frame_free(&f);
    return ret < 0 ? -1 : 0;
}

static int api_video_dmabuf(void *h, int64_t pts_us)
{
    Enc *e = h;
    AVFrame *f = av_frame_alloc();
    f->format = AV_PIX_FMT_DRM_PRIME;
    f->width = e->width;
    f->height = e->height;
    f->data[0] = (uint8_t *)&e->desc;
    f->buf[0] = av_buffer_create((uint8_t *)&e->desc, sizeof(e->desc), noop_free, NULL, AV_BUFFER_FLAG_READONLY);
    f->hw_frames_ctx = av_buffer_ref(e->drm_frames);
    f->pts = pts_us;
    int ret = push_video(e, f);
    av_frame_free(&f);
    return ret < 0 ? -1 : 0;
}

static int encode_audio_frame(Enc *e, int nsamples)
{
    AVFrame *f = e->aframe;
    av_frame_unref(f);
    f->format = e->aenc->sample_fmt;
    f->nb_samples = nsamples;
    f->sample_rate = e->aenc->sample_rate;
#if NEW_CH_LAYOUT
    av_channel_layout_copy(&f->ch_layout, &e->aenc->ch_layout);
#else
    f->channel_layout = e->aenc->channel_layout;
    f->channels = e->aenc->channels;
#endif
    if (av_frame_get_buffer(f, 0) < 0)
        return -1;
    int ch = e->channels;
    const int16_t *in = e->abuf;
    switch (e->aenc->sample_fmt) {
    case AV_SAMPLE_FMT_S16:
        memcpy(f->data[0], in, (size_t)nsamples * ch * 2);
        break;
    case AV_SAMPLE_FMT_S16P:
        for (int c = 0; c < ch; c++)
            for (int i = 0; i < nsamples; i++)
                ((int16_t *)f->data[c])[i] = in[i * ch + c];
        break;
    case AV_SAMPLE_FMT_FLT:
        for (int i = 0; i < nsamples * ch; i++)
            ((float *)f->data[0])[i] = in[i] / 32768.0f;
        break;
    case AV_SAMPLE_FMT_FLTP:
        for (int c = 0; c < ch; c++)
            for (int i = 0; i < nsamples; i++)
                ((float *)f->data[c])[i] = in[i * ch + c] / 32768.0f;
        break;
    default:
        return -1;
    }
    f->pts = e->anext;
    e->anext += nsamples;
    int ret = avcodec_send_frame(e->aenc, f);
    if (ret < 0)
        return ret;
    return write_packets(e, e->aenc, e->ast);
}

static int api_audio(void *h, const int16_t *pcm, int frames, int64_t pts_us)
{
    Enc *e = h;
    if (!e->aenc)
        return 0;
    if (!e->astarted) {
        e->astarted = 1;
        e->anext = av_rescale(pts_us, e->aenc->sample_rate, 1000000);
    }
    int ch = e->channels;
    if (e->afill + frames > e->acap) {
        e->acap = (e->afill + frames) * 2;
        e->abuf = realloc(e->abuf, (size_t)e->acap * ch * 2);
    }
    memcpy(e->abuf + (size_t)e->afill * ch, pcm, (size_t)frames * ch * 2);
    e->afill += frames;
    int fs = e->aenc->frame_size > 0 ? e->aenc->frame_size : 1024;
    while (e->afill >= fs) {
        if (encode_audio_frame(e, fs) < 0)
            return -1;
        e->afill -= fs;
        memmove(e->abuf, e->abuf + (size_t)fs * ch, (size_t)e->afill * ch * 2);
    }
    return 0;
}

static int api_close(void *h, char *err, size_t errlen)
{
    Enc *e = h;
    int ret = 0;
    if (e->opened) {
        int wret = 0;
        if (e->venc) {
            avcodec_send_frame(e->venc, NULL);
            wret = write_packets(e, e->venc, e->vst);
        }
        if (e->aenc) {
            avcodec_send_frame(e->aenc, NULL);
            write_packets(e, e->aenc, e->ast);
        }
        if (!e->header_written) {
            errf(err, errlen, wret < 0 ? "header" : "no video was encoded", wret);
            ret = -1;
        } else if ((ret = av_write_trailer(e->fmt)) < 0) {
            errf(err, errlen, "trailer", ret);
        }
    }
    for (int i = 0; i < e->nheld; i++)
        av_packet_free(&e->held[i]);
    if (e->fmt && e->fmt->pb)
        avio_closep(&e->fmt->pb);
    avformat_free_context(e->fmt);
    avcodec_free_context(&e->venc);
    avcodec_free_context(&e->aenc);
    avfilter_graph_free(&e->graph);
    av_buffer_unref(&e->drm_frames);
    av_buffer_unref(&e->drm_device);
    if (e->desc.nb_objects && e->desc.objects[0].fd >= 0)
        close(e->desc.objects[0].fd);
    av_frame_free(&e->filt);
    av_frame_free(&e->aframe);
    av_packet_free(&e->pkt);
    free(e->abuf);
    pthread_mutex_destroy(&e->lock);
    free(e);
    return ret < 0 ? -1 : 0;
}

static const KsPluginApi api = {
    .abi = KS_PLUGIN_ABI,
    .set_device = api_set_device,
    .encoders = api_encoders,
    .vaapi_available = api_vaapi_available,
    .open = api_open,
    .describe = api_describe,
    .video_cpu = api_video_cpu,
    .video_dmabuf = api_video_dmabuf,
    .audio = api_audio,
    .close = api_close,
};

__attribute__((visibility("default"))) const KsPluginApi *ks_plugin_entry(void)
{
    return &api;
}
