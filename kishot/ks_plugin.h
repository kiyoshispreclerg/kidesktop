/* ks_plugin.h - interface of kishot-rec-av.so, the libav recording plugin.
 *
 * kishot itself links no FFmpeg library: it dlopen()s the plugin, and when
 * that fails (plugin not built, or the libav* it was built against are gone
 * or replaced by another major version) recording falls back to piping
 * frames into the ffmpeg command, or is turned off. Bump KS_PLUGIN_ABI on
 * any change to these structs. */
#ifndef KS_PLUGIN_H
#define KS_PLUGIN_H

#include <stdint.h>
#include <stddef.h>

#define KS_PLUGIN_ABI 1
#define KS_PLUGIN_ENTRY "ks_plugin_entry"

typedef enum {
    KS_INPUT_CPU,       /* BGRX frames in memory */
    KS_INPUT_DMABUF,    /* one dma-buf (XRGB8888, implicit modifier) reused for every frame */
} KsInput;

typedef struct {
    int width, height, fps;
    const char *path;
    const char *container;      /* mkv, mp4, webm */
    const char *vcodec;         /* encoder name, "auto" picks */
    int quality;                /* 0 best .. 51 worst */
    int audio;                  /* expect ks_plugin_api.audio() calls */
    int sample_rate, channels;  /* S16 interleaved */
    KsInput input;
    /* KS_INPUT_DMABUF only; the plugin dups the fd */
    int dmabuf_fd;
    int dmabuf_stride;
    uint64_t dmabuf_size;
} KsEncParams;

typedef struct {
    int abi;
    /* The render node of the X server's GPU (e.g. /dev/dri/renderD128), so
     * dma-bufs and the encoder are on the same device on hybrid machines.
     * Call before anything else; NULL lets VAAPI pick. */
    void (*set_device)(const char *render_node);
    /* NULL-terminated list of usable encoder names (static storage);
     * VAAPI ones only when a VAAPI device opens. */
    const char *const *(*encoders)(void);
    int (*vaapi_available)(void);
    /* NULL on failure, with the reason in err */
    void *(*open)(const KsEncParams *p, char *err, size_t errlen);
    /* human-readable path taken, e.g. "dma-buf → VAAPI h264_vaapi" */
    const char *(*describe)(void *h);
    /* pts in microseconds since the start of the recording */
    int (*video_cpu)(void *h, const uint8_t *bgrx, int stride, int64_t pts_us);
    int (*video_dmabuf)(void *h, int64_t pts_us);
    int (*audio)(void *h, const int16_t *pcm, int frames, int64_t pts_us);
    /* flushes, writes the trailer and frees; 0 on success */
    int (*close)(void *h, char *err, size_t errlen);
} KsPluginApi;

typedef const KsPluginApi *(*KsPluginEntry)(void);

#endif /* KS_PLUGIN_H */
