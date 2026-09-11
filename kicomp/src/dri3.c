/*
 * DRI3 requests, and nothing else (dri3.h). What the buffers are *for*
 * is the EGL platform's business.
 */
#include "dri3.h"

#include <xcb/dri3.h>

#include <drm_fourcc.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static uint32_t dri3_major, dri3_minor;

/* The multi-plane, modifier-aware requests arrived in 1.2. */
static bool have_modifiers(void)
{
    return dri3_major > 1 || (dri3_major == 1 && dri3_minor >= 2);
}

void dri3_probe(void)
{
    const xcb_query_extension_reply_t *ext =
        xcb_get_extension_data(comp.conn, &xcb_dri3_id);
    if (!ext || !ext->present)
        return;

    xcb_dri3_query_version_reply_t *v = xcb_dri3_query_version_reply(comp.conn,
        xcb_dri3_query_version(comp.conn, 1, 2), NULL);
    if (!v)
        return;

    dri3_major = v->major_version;
    dri3_minor = v->minor_version;
    comp.caps.dri3 = true;
    comp_log("DRI3 %u.%u", dri3_major, dri3_minor);
    free(v);
}

int dri3_open_device(void)
{
    if (!comp.caps.dri3)
        return -1;

    xcb_dri3_open_reply_t *r = xcb_dri3_open_reply(comp.conn,
        xcb_dri3_open(comp.conn, comp.root, 0), NULL);
    if (!r)
        return -1;

    int fd = -1;
    if (r->nfd == 1) {
        int *fds = xcb_dri3_open_reply_fds(comp.conn, r);
        if (fds)
            fd = fds[0];
    }
    free(r);
    return fd;
}

void dri3_buffer_close(Dri3Buffer *b)
{
    for (int i = 0; i < b->nplanes; i++) {
        if (b->fd[i] >= 0)
            close(b->fd[i]);
        b->fd[i] = -1;
    }
    b->nplanes = 0;
}

bool dri3_buffer_from_pixmap(xcb_pixmap_t pixmap, Dri3Buffer *out)
{
    memset(out, 0, sizeof(*out));
    for (int i = 0; i < DRI3_MAX_PLANES; i++)
        out->fd[i] = -1;
    out->modifier = DRM_FORMAT_MOD_INVALID;

    if (!comp.caps.dri3)
        return false;

    if (have_modifiers()) {
        xcb_dri3_buffers_from_pixmap_reply_t *r =
            xcb_dri3_buffers_from_pixmap_reply(comp.conn,
                xcb_dri3_buffers_from_pixmap(comp.conn, pixmap), NULL);
        if (!r)
            return false;

        int n = r->nfd;
        int *fds = xcb_dri3_buffers_from_pixmap_reply_fds(comp.conn, r);
        const uint32_t *strides = xcb_dri3_buffers_from_pixmap_strides(r);
        const uint32_t *offsets = xcb_dri3_buffers_from_pixmap_offsets(r);
        if (!fds || n < 1 || n > DRI3_MAX_PLANES ||
            xcb_dri3_buffers_from_pixmap_strides_length(r) < n) {
            /* Whatever came, close it: an fd the server sent is ours to
             * leak. */
            for (int i = 0; fds && i < n; i++)
                close(fds[i]);
            free(r);
            return false;
        }

        out->nplanes = n;
        for (int i = 0; i < n; i++) {
            out->fd[i] = fds[i];
            out->stride[i] = strides[i];
            out->offset[i] = offsets[i];
        }
        out->modifier = r->modifier;
        out->width = r->width;
        out->height = r->height;
        out->depth = r->depth;
        out->bpp = r->bpp;
        free(r);
        return true;
    }

    xcb_dri3_buffer_from_pixmap_reply_t *r =
        xcb_dri3_buffer_from_pixmap_reply(comp.conn,
            xcb_dri3_buffer_from_pixmap(comp.conn, pixmap), NULL);
    if (!r)
        return false;

    int *fds = xcb_dri3_buffer_from_pixmap_reply_fds(comp.conn, r);
    if (!fds || r->nfd != 1) {
        free(r);
        return false;
    }
    out->nplanes = 1;
    out->fd[0] = fds[0];
    out->stride[0] = r->stride;
    out->offset[0] = 0;
    out->width = r->width;
    out->height = r->height;
    out->depth = r->depth;
    out->bpp = r->bpp;
    free(r);
    return true;
}

xcb_pixmap_t dri3_pixmap_from_buffer(xcb_window_t window, Dri3Buffer *b)
{
    if (!comp.caps.dri3 || b->nplanes < 1)
        return XCB_NONE;

    xcb_pixmap_t pixmap = xcb_generate_id(comp.conn);
    xcb_void_cookie_t ck;

    /* libxcb sends the fds and closes them once they are on the wire;
     * from here on they are the server's. */
    if (have_modifiers()) {
        int32_t fds[DRI3_MAX_PLANES];
        for (int i = 0; i < b->nplanes; i++)
            fds[i] = b->fd[i];
        ck = xcb_dri3_pixmap_from_buffers_checked(comp.conn, pixmap, window,
                (uint8_t)b->nplanes, b->width, b->height,
                b->stride[0], b->offset[0], b->stride[1], b->offset[1],
                b->stride[2], b->offset[2], b->stride[3], b->offset[3],
                b->depth, b->bpp, b->modifier, fds);
    } else {
        if (b->nplanes != 1) {
            dri3_buffer_close(b);
            return XCB_NONE;
        }
        ck = xcb_dri3_pixmap_from_buffer_checked(comp.conn, pixmap, comp.root,
                b->stride[0] * b->height, b->width, b->height,
                b->stride[0], b->depth, b->bpp, b->fd[0]);
    }
    for (int i = 0; i < b->nplanes; i++)
        b->fd[i] = -1;
    b->nplanes = 0;

    xcb_generic_error_t *err = xcb_request_check(comp.conn, ck);
    if (err) {
        fprintf(stderr, "kicomp: dri3: PixmapFromBuffers failed (error %d)\n",
                err->error_code);
        free(err);
        return XCB_NONE;
    }
    return pixmap;
}

bool dri3_supported_modifiers(xcb_window_t window, uint8_t depth, uint8_t bpp,
                              uint64_t **window_mods, int *n_window,
                              uint64_t **screen_mods, int *n_screen)
{
    *window_mods = *screen_mods = NULL;
    *n_window = *n_screen = 0;

    if (!comp.caps.dri3 || !have_modifiers())
        return false;

    xcb_dri3_get_supported_modifiers_reply_t *r =
        xcb_dri3_get_supported_modifiers_reply(comp.conn,
            xcb_dri3_get_supported_modifiers(comp.conn, window, depth, bpp), NULL);
    if (!r)
        return false;

    int nw = xcb_dri3_get_supported_modifiers_window_modifiers_length(r);
    int ns = xcb_dri3_get_supported_modifiers_screen_modifiers_length(r);
    if (nw > 0) {
        *window_mods = malloc(sizeof(uint64_t) * (size_t)nw);
        if (*window_mods) {
            memcpy(*window_mods, xcb_dri3_get_supported_modifiers_window_modifiers(r),
                   sizeof(uint64_t) * (size_t)nw);
            *n_window = nw;
        }
    }
    if (ns > 0) {
        *screen_mods = malloc(sizeof(uint64_t) * (size_t)ns);
        if (*screen_mods) {
            memcpy(*screen_mods, xcb_dri3_get_supported_modifiers_screen_modifiers(r),
                   sizeof(uint64_t) * (size_t)ns);
            *n_screen = ns;
        }
    }
    free(r);
    return true;
}
