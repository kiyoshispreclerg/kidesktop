/*
 * kicomp - DRI3: buffers crossing between the GPU and the server.
 *
 * DRI3 is the extension that makes a GL compositor on X11 more than a
 * copy machine. Two requests do all the work:
 *
 *   BuffersFromPixmap  the dma-buf behind a window's pixmap, so its
 *                      contents can be sampled as a texture with nothing
 *                      copied -- the EGL side of what GLX's
 *                      texture-from-pixmap does in the driver;
 *   PixmapFromBuffers  a pixmap the *compositor* allocated (a GBM buffer
 *                      with scanout in mind), named to the server so
 *                      Present can show it -- and, when the window covers
 *                      exactly one CRTC, flip it onto that CRTC instead
 *                      of copying it. An XRender pixmap can never flip;
 *                      this is the pixmap that can.
 *
 * And DRI3Open, which hands over a file descriptor of the device the
 * server is driving this screen with, so the compositor renders on the
 * GPU whose scanout the frame will end up in, and never guesses at
 * /dev/dri/renderD*.
 *
 * Version 1.2 (2018) is what carries the multi-plane, modifier-aware
 * forms; on 1.0 the single-plane forms are used with the modifier left
 * INVALID, which every driver reads as "linear or whatever the tiling
 * default is, you tell me". Both are handled here, so callers see one
 * Dri3Buffer either way.
 */
#ifndef KICOMP_DRI3_H
#define KICOMP_DRI3_H

#include "comp.h"

#include <stdint.h>

/* A buffer as DRI3 describes it: up to four planes, each a dma-buf fd
 * with a stride and an offset, under one format modifier. The fds are
 * owned by whoever holds the struct until dri3_buffer_close(); sending
 * them to the server (dri3_pixmap_from_buffer) consumes them. */
#define DRI3_MAX_PLANES 4

typedef struct {
    int nplanes;
    int fd[DRI3_MAX_PLANES];
    uint32_t stride[DRI3_MAX_PLANES];
    uint32_t offset[DRI3_MAX_PLANES];
    uint64_t modifier;      /* DRM_FORMAT_MOD_INVALID when unknown */
    uint16_t width, height;
    uint8_t depth, bpp;
} Dri3Buffer;

/* Probes the extension on comp.conn; sets comp.caps.dri3. Logs the
 * version like the other extensions. */
void dri3_probe(void);

/* The DRM device behind this screen (DRI3Open on the root window): a
 * file descriptor the caller owns, or -1. */
int dri3_open_device(void);

/* The buffer(s) behind a pixmap. False when the server can't say --
 * the pixmap is in system memory, or a driver without DRI3 backing. */
bool dri3_buffer_from_pixmap(xcb_pixmap_t pixmap, Dri3Buffer *out);

/* Closes every fd still held. Safe on a consumed or empty buffer. */
void dri3_buffer_close(Dri3Buffer *b);

/* A pixmap for `window`'s screen backed by this buffer. The fds are
 * handed to the server and closed here, so `b` is empty afterwards.
 * XCB_NONE on failure, with the fds still closed. */
xcb_pixmap_t dri3_pixmap_from_buffer(xcb_window_t window, Dri3Buffer *b);

/* Format modifiers the server can take for a pixmap of this depth/bpp
 * that will be presented to `window`: the window set is what could be
 * flipped to the CRTC the window sits on, the screen set what could at
 * least be composited by the server's GPU. Both malloc'd, caller frees;
 * counts 0 and NULL on DRI3 < 1.2. */
bool dri3_supported_modifiers(xcb_window_t window, uint8_t depth, uint8_t bpp,
                              uint64_t **window_mods, int *n_window,
                              uint64_t **screen_mods, int *n_screen);

#endif /* KICOMP_DRI3_H */
