/* ks_audio.c - system sound and microphone capture for recordings.
 *
 * libpulse-simple is dlopen()ed, never linked: without it kishot still
 * records, just silently. It also serves PipeWire through pipewire-pulse.
 * "System sound" is the monitor of the default sink (@DEFAULT_MONITOR@),
 * "microphone" the default source. With both, the monitor stream paces the
 * output and the microphone is mixed in from a small queue. */
#include "ks.h"
#include <dlfcn.h>
#include <string.h>

typedef struct { int format; uint32_t rate; uint8_t channels; } PaSampleSpec;
typedef struct { uint32_t maxlength, tlength, prebuf, minreq, fragsize; } PaBufferAttr;
#define PA_SAMPLE_S16LE 3
#define PA_STREAM_RECORD 2

static void *(*p_new)(const char *, const char *, int, const char *, const char *,
                      const PaSampleSpec *, const void *, const PaBufferAttr *, int *);
static int (*p_read)(void *, void *, size_t, int *);
static void (*p_free)(void *);
static int loaded = -1;

gboolean ks_audio_available(void)
{
    if (loaded < 0) {
        void *h = dlopen("libpulse-simple.so.0", RTLD_NOW | RTLD_LOCAL);
        loaded = 0;
        if (h) {
            p_new = (void *)dlsym(h, "pa_simple_new");
            p_read = (void *)dlsym(h, "pa_simple_read");
            p_free = (void *)dlsym(h, "pa_simple_free");
            loaded = p_new && p_read && p_free;
        }
    }
    return loaded;
}

#define CHUNK 480   /* 10 ms */

static GThread *main_thread, *mic_thread;
static void *s_main, *s_mic;
static volatile gint stopping;
static KsAudioSink sink;
static void *sink_data;
static GMutex q_lock;
static GByteArray *mic_q;

static void *open_stream(const char *dev, const char *what)
{
    PaSampleSpec ss = { PA_SAMPLE_S16LE, KS_AUDIO_RATE, KS_AUDIO_CHANNELS };
    PaBufferAttr ba = { (uint32_t)-1, (uint32_t)-1, (uint32_t)-1, (uint32_t)-1,
                        CHUNK * KS_AUDIO_CHANNELS * 2 };
    int err = 0;
    void *s = p_new(NULL, "kishot", PA_STREAM_RECORD, dev, what, &ss, NULL, &ba, &err);
    if (!s)
        g_printerr("kishot: could not open %s (%s): error %d\n", what, dev ? dev : "default", err);
    return s;
}

static gpointer mic_reader(gpointer data)
{
    (void)data;
    int16_t buf[CHUNK * KS_AUDIO_CHANNELS];
    while (!g_atomic_int_get(&stopping)) {
        int err;
        if (p_read(s_mic, buf, sizeof(buf), &err) < 0)
            break;
        g_mutex_lock(&q_lock);
        /* keep at most ~200 ms so a stalled mixer never grows it */
        if (mic_q->len > sizeof(buf) * 20)
            g_byte_array_remove_range(mic_q, 0, sizeof(buf));
        g_byte_array_append(mic_q, (guint8 *)buf, sizeof(buf));
        g_mutex_unlock(&q_lock);
    }
    return NULL;
}

static gpointer main_reader(gpointer data)
{
    (void)data;
    int16_t buf[CHUNK * KS_AUDIO_CHANNELS], mic[CHUNK * KS_AUDIO_CHANNELS];
    while (!g_atomic_int_get(&stopping)) {
        int err;
        if (p_read(s_main, buf, sizeof(buf), &err) < 0)
            break;
        if (s_mic) {
            g_mutex_lock(&q_lock);
            guint n = MIN(mic_q->len, sizeof(mic));
            memcpy(mic, mic_q->data, n);
            g_byte_array_remove_range(mic_q, 0, n);
            g_mutex_unlock(&q_lock);
            for (guint i = 0; i < n / 2; i++) {
                int v = buf[i] + mic[i];
                buf[i] = CLAMP(v, -32768, 32767);
            }
        }
        sink(buf, CHUNK, sink_data);
    }
    return NULL;
}

gboolean ks_audio_start(gboolean system, gboolean mic, KsAudioSink cb, void *data)
{
    if (!ks_audio_available() || (!system && !mic) || main_thread)
        return FALSE;
    sink = cb;
    sink_data = data;
    g_atomic_int_set(&stopping, 0);
    if (system) {
        s_main = open_stream("@DEFAULT_MONITOR@", "system sound");
        if (mic)
            s_mic = open_stream(NULL, "microphone");
    } else {
        s_main = open_stream(NULL, "microphone");
    }
    if (!s_main) {
        if (s_mic)
            p_free(s_mic);
        s_mic = NULL;
        return FALSE;
    }
    if (s_mic) {
        mic_q = g_byte_array_new();
        mic_thread = g_thread_new("kishot-mic", mic_reader, NULL);
    }
    main_thread = g_thread_new("kishot-audio", main_reader, NULL);
    return TRUE;
}

void ks_audio_stop(void)
{
    if (!main_thread)
        return;
    g_atomic_int_set(&stopping, 1);
    /* each read returns within one 10 ms chunk */
    g_thread_join(main_thread);
    main_thread = NULL;
    if (mic_thread) {
        g_thread_join(mic_thread);
        mic_thread = NULL;
    }
    p_free(s_main);
    s_main = NULL;
    if (s_mic)
        p_free(s_mic);
    s_mic = NULL;
    if (mic_q)
        g_byte_array_free(mic_q, TRUE);
    mic_q = NULL;
}
