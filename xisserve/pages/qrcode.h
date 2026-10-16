/*
 * qrcode.h - QR code encoding through libqrencode, loaded with dlopen()
 * so it stays a runtime-only, optional dependency (same approach as the
 * other soft dependencies here): without the library qr_encode()
 * returns NULL and the caller just leaves the QR out.
 */
#ifndef XISSERVE_QRCODE_H
#define XISSERVE_QRCODE_H

#include <glib.h>

typedef struct {
    int width;          /* modules per side */
    guint8 *modules;    /* width*width, bit 0 set = dark */
} QrMatrix;

/* NULL if libqrencode is missing or the text doesn't fit. */
QrMatrix *qr_encode(const char *text);
void qr_free(QrMatrix *m);

#endif
