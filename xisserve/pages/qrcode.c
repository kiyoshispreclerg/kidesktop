/*
 * qrcode.c - see qrcode.h.
 */
#include "qrcode.h"

#include <dlfcn.h>
#include <string.h>

/* The prefix of libqrencode's QRcode struct, which is all we read. */
typedef struct {
    int version;
    int width;
    unsigned char *data;
} QRcodeHead;

typedef QRcodeHead *(*encode_fn)(const char *string, int version, int level, int hint, int casesensitive);
typedef void (*free_fn)(QRcodeHead *);

static encode_fn p_encode;
static free_fn p_free;

static gboolean load(void)
{
    static gboolean tried;
    if (!tried) {
        tried = TRUE;
        static const char *const libs[] = {"libqrencode.so.4", "libqrencode.so.3", NULL};
        for (int i = 0; libs[i] && !p_encode; i++) {
            void *h = dlopen(libs[i], RTLD_LAZY);
            if (!h) {
                continue;
            }
            p_encode = (encode_fn)dlsym(h, "QRcode_encodeString");
            p_free = (free_fn)dlsym(h, "QRcode_free");
            if (!p_encode || !p_free) {
                p_encode = NULL;
                dlclose(h);
            }
        }
    }
    return p_encode != NULL;
}

QrMatrix *qr_encode(const char *text)
{
    /* version 0 = smallest that fits, level 1 = M, hint 2 = QR_MODE_8
     * (SSIDs/passwords are arbitrary bytes), case-sensitive. */
    if (!load()) {
        return NULL;
    }
    QRcodeHead *q = p_encode(text, 0, 1, 2, 1);
    if (!q) {
        return NULL;
    }
    QrMatrix *m = g_new0(QrMatrix, 1);
    m->width = q->width;
    m->modules = g_memdup2(q->data, (gsize)q->width * q->width);
    p_free(q);
    return m;
}

void qr_free(QrMatrix *m)
{
    if (m) {
        g_free(m->modules);
        g_free(m);
    }
}
