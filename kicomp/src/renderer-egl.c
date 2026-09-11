/*
 * EGL platform for the GL renderer: the GPU the server scans out from,
 * frames in buffers the server can flip, windows sampled through their
 * dma-bufs. The drawing is renderer-gl.c, shared with GLX.
 *
 * What this is *not*: EGL on an X window. That would be the same thing
 * as GLX with different spelling -- on Mesa both swap through DRI3 and
 * Present inside the driver, in a drawable the driver owns and at a
 * moment the driver picks. The point here is to own those two things:
 *
 *   - the frame is drawn into a GBM buffer allocated by the compositor
 *     with scanout in mind, named to the server as a pixmap through
 *     DRI3 (dri3.h), and handed to the *Present presenter*, which
 *     already aims each output's frames at its own CRTC. An XRender
 *     pixmap on that path is copied at vblank; this pixmap, when the
 *     window covers exactly what one CRTC scans out, can be flipped
 *     onto it instead -- no copy, the buffer just becomes the screen.
 *     A stock server only flips a window that covers the whole screen;
 *     per-CRTC flipping is what XiS adds (Fase 8), and nothing here
 *     changes for it;
 *
 *   - a window's contents are sampled straight from the dma-buf behind
 *     its pixmap (BuffersFromPixmap + EGL_EXT_image_dma_buf_import),
 *     imported once and read every frame. GLX's texture-from-pixmap
 *     does the same under the hood on DRI3; here it is explicit, with
 *     no fbconfig to match to the pixmap's visual and no per-frame
 *     rebind.
 *
 * Contexts: there is no window and no surface. The context is created
 * on the GBM platform (the device from DRI3Open, so it is the GPU the
 * server drives this screen with, never a guess at /dev/dri) and made
 * current with no surface at all (EGL_KHR_surfaceless_context); every
 * frame goes to a framebuffer object backed by the buffer being drawn.
 *
 * The swapchain is the compositor's own -- three buffers per output,
 * used in turn -- which means the buffer age that makes partial
 * repaint safe is not a question for the driver: a buffer drawn n
 * presents ago is n presents old, exactly. Three rather than two
 * because a flipped buffer is still on the screen until the *next*
 * flip lands, so the one presented last frame is not free when this
 * frame is drawn; the output's presenter never paints a frame while
 * one is in flight (busy), so by the time a buffer comes round again
 * the one after it has replaced it on the scanout.
 *
 * Synchronisation with the server is implicit for now: the GPU work is
 * flushed at the end of the frame, and the kernel's per-buffer fence
 * (the dma-buf reservation) makes the server's copy or flip wait for
 * it -- which is what every X11 compositor on Mesa relies on. Explicit
 * fences are a follow-up.
 */
#include "renderer-gl.h"
#include "dri3.h"
#include "window.h"

#include <epoxy/egl.h>
#include <gbm.h>
#include <drm_fourcc.h>

#include <xcb/composite.h>
#include <xcb/render.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The device (dri3_open_device), GBM on it, EGL on that. */
static int drm_fd = -1;
static struct gbm_device *gbm;
static EGLDisplay egl_display = EGL_NO_DISPLAY;
static EGLContext context = EGL_NO_CONTEXT;
static bool started, start_failed;

static bool have_modifiers;         /* EGL_EXT_image_dma_buf_import_modifiers */

static const GlPlatform egl_platform;

/* How many buffers each output cycles through. See the header. */
#define SWAPCHAIN 3

/* One buffer of an output's swapchain: the GBM allocation, the image
 * and framebuffer GL draws it through, and the pixmap the server knows
 * it as. */
typedef struct {
    struct gbm_bo *bo;
    EGLImageKHR image;
    GLuint texture;
    GLuint fbo;
    xcb_pixmap_t pixmap;
    xcb_render_picture_t picture;   /* for the COPY presenter, which reads one */

    /* The frame counter's value when this buffer was last presented;
     * 0 if never. Its age at the next draw is the frames since. */
    uint64_t presented_at;
} EglBuffer;

/* Per-output state, hung off CompOutput::render_data. */
typedef struct {
    GlOutput gl;                    /* first: the drawing's bookkeeping */
    EglBuffer buffers[SWAPCHAIN];
    int current;                    /* the buffer being (or last) drawn */
    uint64_t frames;                /* presents so far, for the ages */
    uint32_t format;                /* DRM fourcc of the buffers */
} EglOutput;

/* The EGL half of a window (GlWindow::platform): the image over its
 * pixmap's dma-buf. The pixmap itself is named here as well, since it
 * has to outlive the import. */
typedef struct {
    xcb_pixmap_t pixmap;
    EGLImageKHR image;
} EglWindow;

/* ------------------------------------------------------------------ */
/* images                                                              */
/* ------------------------------------------------------------------ */

/* The fourcc a pixmap of this depth is stored as. What glamor and the
 * modesetting driver use, which is what matters: the bits are read as
 * this format by the server on the way out, so they have to be drawn
 * as this format on the way in. */
static uint32_t fourcc_for_depth(uint8_t depth)
{
    switch (depth) {
    case 32: return DRM_FORMAT_ARGB8888;
    case 30: return DRM_FORMAT_XRGB2101010;
    case 24: return DRM_FORMAT_XRGB8888;
    case 16: return DRM_FORMAT_RGB565;
    default: return 0;
    }
}

/* An EGLImage over a dma-buf, plane by plane. The fds stay the
 * caller's: the image keeps its own reference. */
static EGLImageKHR image_from_dmabuf(uint32_t fourcc, int width, int height,
                                     int nplanes, const int *fd,
                                     const uint32_t *stride,
                                     const uint32_t *offset,
                                     uint64_t modifier)
{
    static const EGLint plane_fd[] = {
        EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE1_FD_EXT,
        EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE3_FD_EXT };
    static const EGLint plane_offset[] = {
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT,
        EGL_DMA_BUF_PLANE2_OFFSET_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT };
    static const EGLint plane_pitch[] = {
        EGL_DMA_BUF_PLANE0_PITCH_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT,
        EGL_DMA_BUF_PLANE2_PITCH_EXT, EGL_DMA_BUF_PLANE3_PITCH_EXT };
    static const EGLint plane_mod_lo[] = {
        EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,
        EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT };
    static const EGLint plane_mod_hi[] = {
        EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT,
        EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT };

    if (nplanes < 1 || nplanes > DRI3_MAX_PLANES)
        return EGL_NO_IMAGE_KHR;

    EGLint attrs[64];
    int n = 0;
    attrs[n++] = EGL_WIDTH;              attrs[n++] = width;
    attrs[n++] = EGL_HEIGHT;             attrs[n++] = height;
    attrs[n++] = EGL_LINUX_DRM_FOURCC_EXT; attrs[n++] = (EGLint)fourcc;
    for (int i = 0; i < nplanes; i++) {
        attrs[n++] = plane_fd[i];     attrs[n++] = fd[i];
        attrs[n++] = plane_offset[i]; attrs[n++] = (EGLint)offset[i];
        attrs[n++] = plane_pitch[i];  attrs[n++] = (EGLint)stride[i];
        /* A modifier is only spoken of when known: INVALID as an
         * attribute is a refusal, whereas leaving it out lets the driver
         * read the tiling off the buffer itself, which is what it did
         * before modifiers existed. */
        if (have_modifiers && modifier != DRM_FORMAT_MOD_INVALID) {
            attrs[n++] = plane_mod_lo[i]; attrs[n++] = (EGLint)(modifier & 0xffffffffu);
            attrs[n++] = plane_mod_hi[i]; attrs[n++] = (EGLint)(modifier >> 32);
        }
    }
    attrs[n++] = EGL_NONE;

    return eglCreateImageKHR(egl_display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
                             NULL, attrs);
}

/* ------------------------------------------------------------------ */
/* startup                                                             */
/* ------------------------------------------------------------------ */

static bool egl_start(void)
{
    if (started)
        return true;
    if (start_failed)
        return false;
    start_failed = true;

    if (!comp.caps.dri3) {
        fprintf(stderr, "kicomp: egl: no DRI3; the egl renderer needs it "
                        "(falling back to xrender)\n");
        return false;
    }

    drm_fd = dri3_open_device();
    if (drm_fd < 0) {
        fprintf(stderr, "kicomp: egl: DRI3Open failed (falling back to xrender)\n");
        return false;
    }

    gbm = gbm_create_device(drm_fd);
    if (!gbm) {
        fprintf(stderr, "kicomp: egl: no GBM on the server's device\n");
        return false;
    }

    /* The EXT form rather than EGL 1.5's core one: which of the two the
     * loader answers is decided before any display exists, and libepoxy
     * aborts outright on a core call it can't resolve at that point. */
    if (!epoxy_has_egl_extension(EGL_NO_DISPLAY, "EGL_EXT_platform_base")) {
        fprintf(stderr, "kicomp: egl: no EGL_EXT_platform_base\n");
        return false;
    }
    egl_display = eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_KHR, gbm, NULL);
    if (egl_display == EGL_NO_DISPLAY) {
        fprintf(stderr, "kicomp: egl: no EGL display on the GBM device\n");
        return false;
    }

    EGLint major, minor;
    if (!eglInitialize(egl_display, &major, &minor)) {
        fprintf(stderr, "kicomp: egl: eglInitialize failed\n");
        return false;
    }

    /* Everything below is what makes this platform *this* platform.
     * Each is checked rather than assumed (section 17/30), and any one
     * missing is a reason to fall back, not to limp. */
    const char *need[] = {
        "EGL_KHR_image_base",
        "EGL_EXT_image_dma_buf_import",
        "EGL_KHR_surfaceless_context",
    };
    for (size_t i = 0; i < sizeof(need) / sizeof(need[0]); i++) {
        if (!epoxy_has_egl_extension(egl_display, need[i])) {
            fprintf(stderr, "kicomp: egl: no %s (falling back to xrender)\n",
                    need[i]);
            return false;
        }
    }
    have_modifiers = epoxy_has_egl_extension(egl_display,
                                             "EGL_EXT_image_dma_buf_import_modifiers");
    bool no_config = epoxy_has_egl_extension(egl_display, "EGL_KHR_no_config_context");

    if (!eglBindAPI(EGL_OPENGL_API)) {
        fprintf(stderr, "kicomp: egl: no desktop GL on this EGL\n");
        return false;
    }

    /* The shaders are GLSL 1.20 (renderer-gl.c), so a legacy-flavoured
     * context: no version or profile asked for, which is how EGL spells
     * "the highest compatibility context you have". */
    EGLConfig config = EGL_NO_CONFIG_KHR;
    if (!no_config) {
        /* Without a configless context a config still has to be named,
         * even though no surface will ever be made from it. */
        static const EGLint attrs[] = {
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
            EGL_NONE
        };
        EGLint count = 0;
        if (!eglChooseConfig(egl_display, attrs, &config, 1, &count) || count < 1) {
            fprintf(stderr, "kicomp: egl: no usable config\n");
            return false;
        }
    }

    context = eglCreateContext(egl_display, config, EGL_NO_CONTEXT, NULL);
    if (context == EGL_NO_CONTEXT) {
        fprintf(stderr, "kicomp: egl: eglCreateContext failed (0x%x)\n",
                eglGetError());
        return false;
    }

    if (!eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, context)) {
        fprintf(stderr, "kicomp: egl: cannot make a surfaceless context current\n");
        return false;
    }

    if (!epoxy_has_gl_extension("GL_OES_EGL_image")) {
        fprintf(stderr, "kicomp: egl: no GL_OES_EGL_image (falling back to xrender)\n");
        return false;
    }
    if (epoxy_gl_version() < 30 &&
        !epoxy_has_gl_extension("GL_ARB_framebuffer_object")) {
        fprintf(stderr, "kicomp: egl: no framebuffer objects (falling back to xrender)\n");
        return false;
    }

    comp_info("egl %d.%d on %s, %s, dma-buf import%s, frames in GBM buffers "
              "presented through DRI3",
              major, minor, eglQueryString(egl_display, EGL_VENDOR),
              (const char *)glGetString(GL_RENDERER),
              have_modifiers ? " with modifiers" : "");

    started = true;
    start_failed = false;
    return true;
}

/* ------------------------------------------------------------------ */
/* the swapchain                                                       */
/* ------------------------------------------------------------------ */

static void buffer_free(EglBuffer *b)
{
    if (b->picture) {
        xcb_render_free_picture(comp.conn, b->picture);
        b->picture = XCB_NONE;
    }
    if (b->pixmap != XCB_NONE) {
        xcb_free_pixmap(comp.conn, b->pixmap);
        b->pixmap = XCB_NONE;
    }
    if (b->fbo) {
        glDeleteFramebuffers(1, &b->fbo);
        b->fbo = 0;
    }
    if (b->texture) {
        glDeleteTextures(1, &b->texture);
        b->texture = 0;
    }
    if (b->image != EGL_NO_IMAGE_KHR) {
        eglDestroyImageKHR(egl_display, b->image);
        b->image = EGL_NO_IMAGE_KHR;
    }
    if (b->bo) {
        gbm_bo_destroy(b->bo);
        b->bo = NULL;
    }
    b->presented_at = 0;
}

/* Allocates one buffer of the output's size: a GBM buffer the display
 * engine could scan out, a framebuffer to draw it through, and the
 * server's name for it. */
static bool buffer_alloc(CompOutput *o, EglOutput *eo, EglBuffer *b)
{
    int w = o->physical.w, h = o->physical.h;
    uint8_t depth = comp.screen->root_depth;
    uint8_t bpp = depth == 16 ? 16 : 32;

    /* With the modifiers the server would flip, if it will say: a buffer
     * in a tiling the display engine can't read is a buffer the server
     * has to copy, which is the thing this platform exists to avoid. */
    uint64_t *window_mods = NULL, *screen_mods = NULL;
    int n_window = 0, n_screen = 0;
    dri3_supported_modifiers(comp.root, depth, bpp,
                             &window_mods, &n_window, &screen_mods, &n_screen);
    if (n_window > 0)
        b->bo = gbm_bo_create_with_modifiers2(gbm, (uint32_t)w, (uint32_t)h,
                                              eo->format, window_mods,
                                              (unsigned)n_window,
                                              GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    free(window_mods);
    free(screen_mods);
    if (!b->bo)
        b->bo = gbm_bo_create(gbm, (uint32_t)w, (uint32_t)h, eo->format,
                              GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    if (!b->bo) {
        fprintf(stderr, "kicomp: egl: cannot allocate a %dx%d scanout buffer\n", w, h);
        return false;
    }

    /* The same dma-buf twice: once into EGL as the image GL draws, once
     * to the server as the pixmap it presents. Both keep their own
     * references; the fds here are closed once each side has them. */
    Dri3Buffer d;
    memset(&d, 0, sizeof(d));
    d.nplanes = gbm_bo_get_plane_count(b->bo);
    if (d.nplanes < 1 || d.nplanes > DRI3_MAX_PLANES)
        d.nplanes = 1;
    for (int i = 0; i < d.nplanes; i++) {
        d.fd[i] = d.nplanes > 1 ? gbm_bo_get_fd_for_plane(b->bo, i)
                                : gbm_bo_get_fd(b->bo);
        d.stride[i] = gbm_bo_get_stride_for_plane(b->bo, i);
        d.offset[i] = gbm_bo_get_offset(b->bo, i);
        if (d.fd[i] < 0) {
            fprintf(stderr, "kicomp: egl: cannot export the scanout buffer\n");
            dri3_buffer_close(&d);
            return false;
        }
    }
    d.modifier = gbm_bo_get_modifier(b->bo);
    d.width = (uint16_t)w;
    d.height = (uint16_t)h;
    d.depth = depth;
    d.bpp = bpp;

    b->image = image_from_dmabuf(eo->format, w, h, d.nplanes, d.fd,
                                 d.stride, d.offset, d.modifier);
    if (b->image == EGL_NO_IMAGE_KHR) {
        fprintf(stderr, "kicomp: egl: cannot import the scanout buffer (0x%x)\n",
                eglGetError());
        dri3_buffer_close(&d);
        return false;
    }

    glGenTextures(1, &b->texture);
    glBindTexture(GL_TEXTURE_2D, b->texture);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, b->image);
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenFramebuffers(1, &b->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, b->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           b->texture, 0);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "kicomp: egl: framebuffer over the scanout buffer "
                        "incomplete (0x%x)\n", status);
        dri3_buffer_close(&d);
        return false;
    }

    /* Consumes the fds. */
    b->pixmap = dri3_pixmap_from_buffer(comp.root, &d);
    if (b->pixmap == XCB_NONE)
        return false;

    /* The COPY presenter reads a Picture off the output (o->target);
     * one per buffer, and end() points the output at the current one. */
    b->picture = xcb_generate_id(comp.conn);
    xcb_render_create_picture(comp.conn, b->picture, b->pixmap,
                              comp_root_pictformat(), 0, NULL);
    return true;
}

static bool egl_init(CompOutput *o)
{
    if (!egl_start())
        return false;
    if (o->physical.w <= 0 || o->physical.h <= 0)
        return false;

    EglOutput *eo = calloc(1, sizeof(*eo));
    if (!eo)
        return false;
    for (int i = 0; i < SWAPCHAIN; i++)
        eo->buffers[i].image = EGL_NO_IMAGE_KHR;

    eo->format = fourcc_for_depth(comp.screen->root_depth);
    if (!eo->format) {
        fprintf(stderr, "kicomp: egl: no scanout format for depth %d\n",
                comp.screen->root_depth);
        free(eo);
        return false;
    }

    /* Rows top-first in a buffer the server reads as a pixmap. */
    eo->gl.y_down = true;

    if (!gl_setup(&egl_platform)) {
        free(eo);
        return false;
    }

    for (int i = 0; i < SWAPCHAIN; i++) {
        if (!buffer_alloc(o, eo, &eo->buffers[i])) {
            for (int k = 0; k <= i; k++)
                buffer_free(&eo->buffers[k]);
            free(eo);
            return false;
        }
    }
    eo->current = -1;

    o->render_data = eo;
    o->target = XCB_NONE;
    return true;
}

static void egl_destroy(CompOutput *o)
{
    EglOutput *eo = o->render_data;
    if (!eo)
        return;

    for (int i = 0; i < SWAPCHAIN; i++)
        buffer_free(&eo->buffers[i]);
    free(eo);
    o->render_data = NULL;
    o->target = XCB_NONE;
}

/* ------------------------------------------------------------------ */
/* windows as textures                                                 */
/* ------------------------------------------------------------------ */

static void egl_window_unbind(GlWindow *g)
{
    EglWindow *x = g->platform;
    if (!x)
        return;

    if (x->image != EGL_NO_IMAGE_KHR) {
        eglDestroyImageKHR(egl_display, x->image);
        x->image = EGL_NO_IMAGE_KHR;
    }
    if (x->pixmap != XCB_NONE) {
        xcb_free_pixmap(comp.conn, x->pixmap);
        x->pixmap = XCB_NONE;
    }
    g->content = false;
}

/* Names the window's contents and imports the buffer behind them, once
 * per size. There is no per-frame rebind: the texture reads the
 * buffer's memory, and what the client draws into it is what the next
 * frame samples. */
static bool egl_window_bind(CompWindow *w, GlWindow *g)
{
    EglWindow *x = g->platform;
    if (!x) {
        x = calloc(1, sizeof(*x));
        if (!x)
            return false;
        x->image = EGL_NO_IMAGE_KHR;
        g->platform = x;
    }

    CompRect r = window_rect(w);
    if (x->pixmap != XCB_NONE && (g->width != r.w || g->height != r.h))
        egl_window_unbind(g);

    if (x->pixmap == XCB_NONE) {
        xcb_pixmap_t pixmap = xcb_generate_id(comp.conn);
        xcb_generic_error_t *err = xcb_request_check(comp.conn,
            xcb_composite_name_window_pixmap_checked(comp.conn, w->id, pixmap));
        if (err) {
            free(err);
            return false;
        }
        x->pixmap = pixmap;

        Dri3Buffer d;
        if (!dri3_buffer_from_pixmap(pixmap, &d)) {
            egl_window_unbind(g);
            return false;
        }
        uint32_t fourcc = fourcc_for_depth(d.depth);
        if (fourcc)
            x->image = image_from_dmabuf(fourcc, d.width, d.height, d.nplanes,
                                         d.fd, d.stride, d.offset, d.modifier);
        /* The image holds its own reference to the buffer. */
        dri3_buffer_close(&d);
        if (x->image == EGL_NO_IMAGE_KHR) {
            egl_window_unbind(g);
            return false;
        }

        glBindTexture(GL_TEXTURE_2D, gl_window_texture(g));
        glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, x->image);
        glBindTexture(GL_TEXTURE_2D, 0);

        g->content = true;
        g->width = r.w;
        g->height = r.h;
        g->argb = w->argb;
        /* Row 0 of the buffer is the top of the window: the texture is
         * already the way the drawing's coordinates run. */
        g->y_inverted = true;
    }

    glBindTexture(GL_TEXTURE_2D, g->texture);
    return true;
}

static const GlPlatform egl_platform = {
    .window_bind   = egl_window_bind,
    .window_unbind = egl_window_unbind,
};

/* For main.c to decide before any output exists: can this platform
 * start at all? Failing here falls back to XRender cleanly; failing
 * at the first output's init would leave the outputs without a
 * renderer. */
bool renderer_egl_available(void)
{
    return egl_start();
}

/* ------------------------------------------------------------------ */
/* the renderer                                                        */
/* ------------------------------------------------------------------ */

static void egl_begin(CompOutput *o, const CompRegion *damage)
{
    EglOutput *eo = o->render_data;
    if (!eo)
        return;

    eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, context);

    /* The next buffer round. Its age is exact: the presents since it
     * was last shown, and 0 if it never was. */
    eo->current = (eo->current + 1) % SWAPCHAIN;
    EglBuffer *b = &eo->buffers[eo->current];
    unsigned age = 0;
    if (b->presented_at > 0)
        age = (unsigned)(eo->frames - b->presented_at + 1);

    glBindFramebuffer(GL_FRAMEBUFFER, b->fbo);
    gl_begin(o, &eo->gl, damage, age);
}

static void egl_draw_scene(CompOutput *o, CompScene *s, const CompRegion *damage)
{
    EglOutput *eo = o->render_data;
    if (!eo)
        return;
    (void)damage;
    gl_draw_scene(o, &eo->gl, s);
}

static void egl_end(CompOutput *o)
{
    EglOutput *eo = o->render_data;
    if (!eo)
        return;

    gl_end(&eo->gl);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    /* Presented next, by whichever presenter: counted now so the ages
     * of the other buffers move on, and the output's target is this
     * buffer for a presenter that reads one. */
    EglBuffer *b = &eo->buffers[eo->current];
    eo->frames++;
    b->presented_at = eo->frames;
    o->target = b->picture;
}

static xcb_pixmap_t egl_output_pixmap(const CompOutput *o)
{
    const EglOutput *eo = o->render_data;
    if (!eo || eo->current < 0)
        return XCB_NONE;
    return eo->buffers[eo->current].pixmap;
}

static void egl_shutdown(void)
{
    if (context != EGL_NO_CONTEXT) {
        eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, context);
        gl_teardown();
        eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(egl_display, context);
        context = EGL_NO_CONTEXT;
    }
    if (egl_display != EGL_NO_DISPLAY) {
        eglTerminate(egl_display);
        egl_display = EGL_NO_DISPLAY;
    }
    if (gbm) {
        gbm_device_destroy(gbm);
        gbm = NULL;
    }
    if (drm_fd >= 0) {
        close(drm_fd);
        drm_fd = -1;
    }
    started = false;
}

static const CompRenderer egl_renderer = {
    .name       = "egl",
    .init       = egl_init,
    .destroy    = egl_destroy,
    .begin      = egl_begin,
    .draw_scene = egl_draw_scene,
    .end        = egl_end,

    .window_invalidate  = gl_window_invalidate,
    .window_shape_invalidate = gl_window_shape_invalidate,
    .window_free        = gl_window_free,
    .window_has_content = gl_window_has_content,
    .output_pixmap      = egl_output_pixmap,
    .shutdown           = egl_shutdown,
};

const CompRenderer *renderer_egl(void)
{
    return &egl_renderer;
}
