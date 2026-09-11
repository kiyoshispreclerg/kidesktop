/*
 * GLX platform for the GL renderer (Fase 6): the context, a drawable per
 * output, and windows as textures through GLX_EXT_texture_from_pixmap.
 * The drawing itself is renderer-gl.c, shared with the EGL platform;
 * this file is only what GLX does differently.
 *
 * Why GL exists at all, given XRender already draws the desktop: not
 * speed -- XRender composites a desktop perfectly well -- but *what can be
 * expressed*. XRender's picture transform is affine and its clip is a set
 * of rectangles, which is why a transformed window loses its rounded
 * corners today, why the cube and wobbly are impossible, and why a blur is
 * out of the question. A shader has none of those limits.
 *
 * The shape of it follows the same rule as everything else here: one
 * drawable per output (section 18). Each output gets its own GLX window,
 * a child of the Composite overlay covering exactly that output's
 * scanout, and its own swap -- so outputs still never wait for each
 * other, and the per-output frame clock still drives them independently.
 * presenter-glx.c is the other half: with GL, presenting *is* the buffer
 * swap, so the two come as a pair (renderer_glx_swap() is what it calls).
 *
 * Windows arrive as textures through GLX_EXT_texture_from_pixmap: the
 * pixmap the compositor already names for every window is bound directly
 * as a texture, with no copy and no readback. That extension is the whole
 * reason a GL compositor is practical on X11, and it is checked for at
 * startup rather than assumed (section 17/30) -- without it this backend
 * declines to start and kicomp falls back to XRender.
 *
 * GLX needs an Xlib Display, and kicomp is an XCB program. Rather than
 * mixing event queues -- which is a known way to end up with a
 * hard-to-explain spin -- this opens a second, independent connection
 * used for nothing but GLX. X resources are server-side and their ids are
 * global, so the windows and pixmaps the XCB connection owns are perfectly
 * usable from it.
 *
 * Partial repaint rests on GLX_EXT_buffer_age: the drawing (gl_begin)
 * is told how many swaps old the back buffer is and widens the damage by
 * the frames in between. Without the extension it is told 0 and repaints
 * the whole output every frame, which is slower and always right --
 * drawing only the damaged part of a buffer of unknown age is "parts
 * flashing black, then red, with pieces of windows out of place".
 *
 * Still XRender-only, for the GL renderer as a whole: the X-DENSITY
 * layers and the shade stash. `renderer = glx` is opt-in until they are
 * there, and `auto` still picks xrender.
 */
#include "renderer-gl.h"
#include "window.h"

#include <epoxy/gl.h>
#include <epoxy/glx.h>

#include <X11/Xlib.h>
#include <X11/Xlib-xcb.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The GLX connection, separate from kicomp's XCB one (see above). */
static Display *dpy;

static GLXContext context;
static GLXFBConfig window_config;      /* for the per-output drawables */
static bool have_tfp;

/* A texture-from-pixmap config, chosen for one particular *visual*.
 *
 * Not one config for "24-bit" and one for "32-bit", which is what this
 * used to do: an fbconfig says how the driver reads the bits of a pixmap,
 * and several configs of the same depth disagree about which byte is
 * which. Binding an ARGB pixmap through a config that happens to order
 * its components differently samples the window with its channels
 * rotated -- kiwm's own layers, which are the ARGB windows in the
 * session, came out red on a green theme with the text smeared, because
 * the alpha byte was being read as red.
 *
 * Matching the config to the pixmap's visual removes the guess: same
 * visual, same layout, by construction. */
typedef struct TfpConfig {
    struct TfpConfig *next;
    xcb_visualid_t visual;
    GLXFBConfig config;
    bool rgba;          /* bind with RGBA rather than RGB */
    /* Whether this config hands the pixmap over upside down. Per config,
     * it differs between drivers, and getting it wrong draws every window
     * mirrored vertically -- which looks exactly like "pieces of windows
     * out of place". */
    bool y_inverted;
    bool usable;
} TfpConfig;

static TfpConfig *tfp_configs;
/* GLX_EXT_buffer_age, without which there is no way to know what the back
 * buffer already holds and every frame has to be redrawn whole. */
static bool have_buffer_age;

/* GLX_OML_sync_control: the vblank counter behind the drawable, which is
 * what turns "the swap probably waited for vblank" into a number. */
static bool have_oml_sync;
/* Per-output GLX state, hung off CompOutput::render_data: the drawable,
 * and the shared drawing's own bookkeeping embedded first. */
typedef struct {
    GlOutput gl;
    Window window;          /* CRTC-covering child of the overlay */
    GLXWindow drawable;
} GlxOutput;

/* The GLX half of a window (GlWindow::platform): the named pixmap and
 * the GLXPixmap that lets a texture read it. */
typedef struct {
    Pixmap pixmap;          /* the named contents pixmap */
    GLXPixmap glx_pixmap;
    bool bound;             /* the image is currently bound to the texture */
} GlxWindow;

static bool choose_configs(void)
{
    int screen = DefaultScreen(dpy);

    /* The drawable configs have to match the overlay's own visual, since
     * the per-output windows are its children and inherit its depth. */
    int window_attrs[] = {
        GLX_RENDER_TYPE, GLX_RGBA_BIT,
        GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT,
        GLX_DOUBLEBUFFER, True,
        GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8,
        GLX_DEPTH_SIZE, 0, GLX_STENCIL_SIZE, 0,
        None
    };

    int count = 0;
    GLXFBConfig *configs = glXChooseFBConfig(dpy, screen, window_attrs, &count);
    if (!configs || count == 0) {
        fprintf(stderr, "kicomp: glx: no double-buffered RGB framebuffer config\n");
        return false;
    }
    window_config = configs[0];
    XFree(configs);

    /* Texture-from-pixmap has to be there; without it this backend has
     * no way to sample a window at all. The configs themselves are chosen
     * per visual, lazily, when the first window with that visual is
     * bound (tfp_config_for). */
    const char *ext = glXQueryExtensionsString(dpy, screen);
    have_tfp = ext && strstr(ext, "GLX_EXT_texture_from_pixmap");
    /* Asked through epoxy rather than by searching the server's
     * extension string: buffer age is a *client* extension, so it never
     * appears there, and checking the wrong string is a silent "no"
     * -- every frame a full repaint, with nothing to say why. */
    /* Both sides have to have it: the age comes from the client's
     * buffer bookkeeping, but glXQueryDrawable is answered by the
     * server, and a server that doesn't know the attribute (Xephyr, for
     * one) has nothing useful to say about it. */
    const char *client_ext = glXGetClientString(dpy, GLX_EXTENSIONS);
    have_buffer_age = epoxy_has_glx_extension(dpy, screen, "GLX_EXT_buffer_age") &&
                      client_ext && strstr(client_ext, "GLX_EXT_buffer_age");
    /* The frame counter (presenter-glx.c). Same two-sided test: the
     * counter is the server's, the entry point the client's. */
    have_oml_sync = epoxy_has_glx_extension(dpy, screen, "GLX_OML_sync_control") &&
                    client_ext && strstr(client_ext, "GLX_OML_sync_control");

    if (!have_tfp) {
        fprintf(stderr, "kicomp: glx: no GLX_EXT_texture_from_pixmap; "
                        "this backend cannot sample windows without it\n");
        return false;
    }

    return true;
}

/* The texture-from-pixmap config for one visual, found once and kept.
 *
 * Every fbconfig the server offers is walked, and the one whose own
 * visual *is* this visual wins: that is what guarantees the driver reads
 * the pixmap's bytes the way the X server wrote them. Falling back to
 * "any config of the right depth" is what the first version of this file
 * did, and on this machine it picked one that reads the channels in a
 * different order -- the session's ARGB windows, which are kiwm's OSD,
 * its outline rectangles and its decoration menu, came out red on a green
 * theme with the text smeared.
 *
 * A visual with no config of its own is not an error: the entry is kept
 * with usable = false so the walk is not repeated for every frame of
 * every window that has it, and that window simply isn't drawn by this
 * backend. */
static TfpConfig *tfp_config_for(const CompWindow *w)
{
    for (TfpConfig *c = tfp_configs; c; c = c->next)
        if (c->visual == w->visual)
            return c;

    TfpConfig *c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    c->visual = w->visual;
    c->rgba = w->argb;
    c->next = tfp_configs;
    tfp_configs = c;

    int screen = DefaultScreen(dpy);
    int count = 0;
    GLXFBConfig *all = glXGetFBConfigs(dpy, screen, &count);
    if (!all)
        return c;

    for (int i = 0; i < count; i++) {
        int visual_id = 0, drawable = 0, targets = 0, rgb = 0, rgba = 0;
        glXGetFBConfigAttrib(dpy, all[i], GLX_VISUAL_ID, &visual_id);
        if ((xcb_visualid_t)visual_id != w->visual)
            continue;

        glXGetFBConfigAttrib(dpy, all[i], GLX_DRAWABLE_TYPE, &drawable);
        if (!(drawable & GLX_PIXMAP_BIT))
            continue;
        glXGetFBConfigAttrib(dpy, all[i], GLX_BIND_TO_TEXTURE_TARGETS_EXT, &targets);
        if (!(targets & GLX_TEXTURE_2D_BIT_EXT))
            continue;

        glXGetFBConfigAttrib(dpy, all[i], GLX_BIND_TO_TEXTURE_RGB_EXT, &rgb);
        glXGetFBConfigAttrib(dpy, all[i], GLX_BIND_TO_TEXTURE_RGBA_EXT, &rgba);

        /* An ARGB window wants its alpha; anything else is happy with
         * RGB, and asking for RGBA on a visual that has no alpha bits
         * gets undefined values in that channel rather than an error. */
        if (w->argb && rgba)
            c->rgba = true;
        else if (rgb)
            c->rgba = false;
        else if (rgba)
            c->rgba = true;
        else
            continue;

        int inverted = 0;
        glXGetFBConfigAttrib(dpy, all[i], GLX_Y_INVERTED_EXT, &inverted);

        c->config = all[i];
        c->y_inverted = inverted != 0;
        c->usable = true;
        break;
    }

    XFree(all);

    if (!c->usable)
        fprintf(stderr, "kicomp: glx: no texture-from-pixmap config for visual 0x%x\n",
                (unsigned)w->visual);
    return c;
}

/* Opened once, on the first output. Everything here is per-screen rather
 * than per-output: one context, one program, one set of configs. */
static bool glx_window_bind(CompWindow *w, GlWindow *g);
static void glx_window_unbind(GlWindow *g);

static const GlPlatform glx_platform = {
    .window_bind   = glx_window_bind,
    .window_unbind = glx_window_unbind,
};

static bool glx_start(void)
{
    if (dpy)
        return context != NULL;

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "kicomp: glx: cannot open a display connection\n");
        return false;
    }

    int major = 0, minor = 0;
    if (!glXQueryVersion(dpy, &major, &minor) ||
        major < 1 || (major == 1 && minor < 3)) {
        fprintf(stderr, "kicomp: glx: need GLX 1.3 for framebuffer configs\n");
        return false;
    }

    if (!choose_configs())
        return false;

    context = glXCreateNewContext(dpy, window_config, GLX_RGBA_TYPE, NULL, True);
    if (!context) {
        fprintf(stderr, "kicomp: glx: cannot create a rendering context\n");
        return false;
    }

    comp_info("glx %d.%d, %s, texture-from-pixmap per visual, %s, %s",
              major, minor,
              glXIsDirect(dpy, context) ? "direct" : "indirect (software path)",
              have_buffer_age ? "partial repaint (buffer age)"
                              : "full repaint every frame (no buffer age)",
              have_oml_sync ? "vblank counter (OML sync control)"
                            : "no vblank counter");
    return true;
}

/* ------------------------------------------------------------------ */
/* outputs                                                             */
/* ------------------------------------------------------------------ */

static bool glx_init(CompOutput *o)
{
    if (!glx_start())
        return false;
    if (comp.overlay == XCB_NONE || o->physical.w <= 0 || o->physical.h <= 0)
        return false;

    GlxOutput *go = calloc(1, sizeof(*go));
    if (!go)
        return false;

    int screen = DefaultScreen(dpy);
    XVisualInfo *vi = glXGetVisualFromFBConfig(dpy, window_config);
    if (!vi) {
        free(go);
        return false;
    }

    /* A window per output, exactly covering its scanout -- the same shape
     * the Present presenter uses, and for the same reasons: outputs stay
     * independent, and a window covering exactly one CRTC is what a page
     * flip can eventually take. */
    XSetWindowAttributes swa;
    memset(&swa, 0, sizeof(swa));
    swa.colormap = XCreateColormap(dpy, RootWindow(dpy, screen), vi->visual, AllocNone);
    swa.background_pixmap = None;
    swa.border_pixel = 0;
    swa.event_mask = 0;

    go->window = XCreateWindow(dpy, comp.overlay,
                               o->physical.x, o->physical.y,
                               (unsigned)o->physical.w, (unsigned)o->physical.h,
                               0, vi->depth, InputOutput, vi->visual,
                               CWColormap | CWBackPixmap | CWBorderPixel | CWEventMask,
                               &swa);
    XFree(vi);

    go->drawable = glXCreateWindow(dpy, window_config, go->window, NULL);
    XMapWindow(dpy, go->window);
    XFlush(dpy);

    if (!glXMakeContextCurrent(dpy, go->drawable, go->drawable, context)) {
        fprintf(stderr, "kicomp: glx: cannot make the context current\n");
        glXDestroyWindow(dpy, go->drawable);
        XDestroyWindow(dpy, go->window);
        free(go);
        return false;
    }

    if (!gl_setup(&glx_platform)) {
        glXDestroyWindow(dpy, go->drawable);
        XDestroyWindow(dpy, go->window);
        free(go);
        return false;
    }

    /* Transparent to input, like the overlay itself: the compositor's own
     * windows must never eat a click. Done through XCB, since that is the
     * connection that owns the shape extension state here. */
    if (comp.caps.xfixes) {
        xcb_xfixes_region_t empty = xcb_generate_id(comp.conn);
        xcb_xfixes_create_region(comp.conn, empty, 0, NULL);
        xcb_xfixes_set_window_shape_region(comp.conn, (xcb_window_t)go->window,
                                           XCB_SHAPE_SK_INPUT, 0, 0, empty);
        xcb_xfixes_destroy_region(comp.conn, empty);
        xcb_flush(comp.conn);
    }

    o->render_data = go;
    /* No XRender target: the presenters that read one are not the ones
     * this backend comes with. */
    o->target = 0;
    return true;
}

static void glx_destroy(CompOutput *o)
{
    GlxOutput *go = o->render_data;
    if (!go)
        return;

    if (go->drawable)
        glXDestroyWindow(dpy, go->drawable);
    if (go->window)
        XDestroyWindow(dpy, go->window);
    XFlush(dpy);

    free(go);
    o->render_data = NULL;
}

/* ------------------------------------------------------------------ */
/* windows as textures                                                 */
/* ------------------------------------------------------------------ */

/* Releases the image without touching the pixmap: what has to happen
 * before every rebind. Binding an already-bound image is undefined by the
 * extension, and "undefined" on a real driver is stale or torn contents. */
static void glx_window_release(GlWindow *g, GlxWindow *x)
{
    if (!x->bound || !x->glx_pixmap)
        return;

    glBindTexture(GL_TEXTURE_2D, g->texture);
    glXReleaseTexImageEXT(dpy, x->glx_pixmap, GLX_FRONT_LEFT_EXT);
    glBindTexture(GL_TEXTURE_2D, 0);
    x->bound = false;
}

static void glx_window_unbind(GlWindow *g)
{
    GlxWindow *x = g->platform;
    if (!x)
        return;

    if (x->glx_pixmap) {
        glx_window_release(g, x);
        glXDestroyPixmap(dpy, x->glx_pixmap);
        x->glx_pixmap = 0;
    }
    if (x->pixmap) {
        xcb_free_pixmap(comp.conn, (xcb_pixmap_t)x->pixmap);
        x->pixmap = 0;
    }
    g->content = false;
}

/* Names the window's contents and binds them as a texture. The pixmap is
 * named once and kept until the window is resized or unmapped -- the same
 * lifetime the XRender backend gives it -- while the *binding* is redone
 * every frame the window is drawn, which is what picks up new contents on
 * drivers that don't update a bound texture in place. */
static bool glx_window_bind(CompWindow *w, GlWindow *g)
{
    GlxWindow *x = g->platform;
    if (!x) {
        x = calloc(1, sizeof(*x));
        if (!x)
            return false;
        g->platform = x;
    }

    CompRect r = window_rect(w);

    if (x->pixmap && (g->width != r.w || g->height != r.h))
        glx_window_unbind(g);

    if (!x->pixmap) {
        xcb_pixmap_t pixmap = xcb_generate_id(comp.conn);
        xcb_generic_error_t *err = xcb_request_check(comp.conn,
            xcb_composite_name_window_pixmap_checked(comp.conn, w->id, pixmap));
        if (err) {
            free(err);
            return false;
        }

        x->pixmap = pixmap;
        g->content = true;
        g->width = r.w;
        g->height = r.h;
        g->argb = w->argb;

        TfpConfig *cfg = tfp_config_for(w);
        if (!cfg || !cfg->usable) {
            glx_window_unbind(g);
            return false;
        }

        const int attrs[] = {
            GLX_TEXTURE_TARGET_EXT, GLX_TEXTURE_2D_EXT,
            GLX_TEXTURE_FORMAT_EXT, cfg->rgba ? GLX_TEXTURE_FORMAT_RGBA_EXT
                                              : GLX_TEXTURE_FORMAT_RGB_EXT,
            None
        };
        x->glx_pixmap = glXCreatePixmap(dpy, cfg->config, x->pixmap, attrs);
        if (!x->glx_pixmap) {
            glx_window_unbind(g);
            return false;
        }
        g->y_inverted = cfg->y_inverted;
    }

    /* Released first, then bound again: that pair is how the contents of
     * a window that has drawn since the last frame actually reach the
     * texture on most drivers. */
    glx_window_release(g, x);
    glBindTexture(GL_TEXTURE_2D, gl_window_texture(g));
    glXBindTexImageEXT(dpy, x->glx_pixmap, GLX_FRONT_LEFT_EXT, NULL);
    x->bound = true;
    return true;
}


/* What presenter-glx.c calls: with GL, presenting is the swap, and the
 * drawable belongs to the renderer. */
void renderer_glx_swap(CompOutput *o)
{
    GlxOutput *go = o->render_data;
    if (!go)
        return;

    glXMakeContextCurrent(dpy, go->drawable, go->drawable, context);
    glXSwapBuffers(dpy, go->drawable);
}

/* The drawable's counters right now (GLX_OML_sync_control): ust is the
 * time of the last vblank in microseconds, msc how many there have been,
 * sbc how many swaps have completed. False without the extension, and
 * the presenter goes back to believing rather than measuring. */
bool renderer_glx_sync_values(CompOutput *o, int64_t *ust, int64_t *msc,
                              int64_t *sbc)
{
    GlxOutput *go = o->render_data;
    if (!go || !have_oml_sync)
        return false;
    return glXGetSyncValuesOML(dpy, go->drawable, ust, msc, sbc);
}

bool renderer_glx_ready(void)
{
    return dpy != NULL && context != NULL;
}


/* ------------------------------------------------------------------ */
/* the renderer                                                        */
/* ------------------------------------------------------------------ */

static void glx_begin(CompOutput *o, const CompRegion *damage)
{
    GlxOutput *go = o->render_data;
    if (!go)
        return;

    glXMakeContextCurrent(dpy, go->drawable, go->drawable, context);

    /* How many swaps old the back buffer is, when the driver will say
     * (GLX_EXT_buffer_age); 0 otherwise, and the frame is drawn whole. */
    unsigned age = 0;
    if (have_buffer_age)
        glXQueryDrawable(dpy, go->drawable, GLX_BACK_BUFFER_AGE_EXT, &age);

    gl_begin(o, &go->gl, damage, age);
}

static void glx_draw_scene(CompOutput *o, CompScene *s, const CompRegion *damage)
{
    GlxOutput *go = o->render_data;
    if (!go)
        return;
    (void)damage;
    gl_draw_scene(o, &go->gl, s);
}

static void glx_end(CompOutput *o)
{
    GlxOutput *go = o->render_data;
    if (!go)
        return;
    gl_end(&go->gl);
}

static void glx_shutdown(void)
{
    gl_teardown();

    while (tfp_configs) {
        TfpConfig *c = tfp_configs;
        tfp_configs = c->next;
        free(c);
    }

    if (context) {
        glXMakeContextCurrent(dpy, None, None, NULL);
        glXDestroyContext(dpy, context);
        context = NULL;
    }
    if (dpy) {
        XCloseDisplay(dpy);
        dpy = NULL;
    }
}

static const CompRenderer glx_renderer = {
    .name       = "glx",
    .init       = glx_init,
    .destroy    = glx_destroy,
    .begin      = glx_begin,
    .draw_scene = glx_draw_scene,
    .end        = glx_end,

    .window_invalidate  = gl_window_invalidate,
    .window_shape_invalidate = gl_window_shape_invalidate,
    .window_free        = gl_window_free,
    .window_has_content = gl_window_has_content,
    .shutdown           = glx_shutdown,
};

const CompRenderer *renderer_glx(void)
{
    return &glx_renderer;
}
