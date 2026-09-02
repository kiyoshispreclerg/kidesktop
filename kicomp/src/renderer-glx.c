/*
 * GLX renderer (Fase 6): the same scene, drawn by the GPU.
 *
 * Why this exists at all, given XRender already draws the desktop: not
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
 * What this first version deliberately does not do yet, all of which the
 * XRender backend does: shadows, shape clipping (rounded corners), the
 * X-DENSITY layers and the shade stash. Each is a follow-up; `renderer =
 * glx` is opt-in until they are there, and `auto` still picks xrender.
 *
 * One thing it deliberately does *not* do that looks like an optimization
 * and is really a correctness rule: partial repaint. A double-buffered
 * drawable hands back a buffer that holds a frame from two swaps ago, not
 * the last one, so drawing only the damaged part leaves the rest showing
 * an old frame -- or, on a driver that hands over a fresh allocation,
 * whatever was in that memory. That is what "parts flashing black, then
 * red, with pieces of windows out of place" is. Honouring damage here
 * needs GLX_EXT_buffer_age, which says how old the buffer is so the
 * region can be widened by the frames in between; until that exists, this
 * backend repaints the whole output every frame, which is slower and
 * always right.
 */
#include "renderer.h"
#include "region.h"
#include "output.h"
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
static GLXFBConfig tfp_config_rgb;     /* depth 24 windows, as textures */
static GLXFBConfig tfp_config_rgba;    /* depth 32 windows */
/* Whether each of those two hands the pixmap over upside down. Not a
 * detail to guess at: GLX_Y_INVERTED_EXT is per config, it differs
 * between drivers, and getting it wrong draws every window mirrored
 * vertically -- which looks exactly like "pieces of windows out of
 * place". */
static bool tfp_inverted_rgb, tfp_inverted_rgba;
static bool have_tfp;

static GLuint program;
static GLint u_projection, u_transform, u_opacity, u_texture, u_y_flip;
static GLuint quad_vbo;

/* Per-output GL state, hung off CompOutput::render_data. */
typedef struct {
    Window window;          /* CRTC-covering child of the overlay */
    GLXWindow drawable;
} GlxOutput;

/* Per-window GL state. The renderer keeps it in the window's existing
 * backend fields where they fit and in this table where they don't --
 * comp.h's pixmap/picture pair is XRender's vocabulary, and a texture is
 * not a Picture. */
typedef struct GlxWindow {
    struct GlxWindow *next;
    xcb_window_t id;

    Pixmap pixmap;          /* the named contents pixmap */
    GLXPixmap glx_pixmap;
    GLuint texture;
    int width, height;
    bool argb;
    bool bound;             /* the image is currently bound to the texture */
    bool y_inverted;        /* this config hands the pixmap upside down */
} GlxWindow;

static GlxWindow *windows;

/* ------------------------------------------------------------------ */
/* shaders                                                             */
/* ------------------------------------------------------------------ */

/* Deliberately the smallest thing that can draw a window: a textured
 * quad, a 4x4 transform, and one opacity multiplier. Everything the
 * effects do -- move, scale, fade -- is already expressed in those terms
 * (scene.h), which is what makes the effects backend-independent: not one
 * line of effects/ changes for this file to exist. */
static const char *vertex_source =
    "#version 120\n"
    "attribute vec2 position;\n"
    "uniform mat4 projection;\n"
    "uniform mat4 transform;\n"
    "uniform float y_flip;\n"
    "varying vec2 texcoord;\n"
    "void main() {\n"
    "    texcoord = vec2(position.x,\n"
    "                    mix(position.y, 1.0 - position.y, y_flip));\n"
    "    gl_Position = projection * transform * vec4(position, 0.0, 1.0);\n"
    "}\n";

static const char *fragment_source =
    "#version 120\n"
    "uniform sampler2D texture0;\n"
    "uniform float opacity;\n"
    "varying vec2 texcoord;\n"
    "void main() {\n"
    "    vec4 c = texture2D(texture0, texcoord);\n"
    "    gl_FragColor = c * opacity;\n"
    "}\n";

static GLuint compile(GLenum type, const char *source, const char *what)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);

    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        fprintf(stderr, "kicomp: glx: %s shader: %s\n", what, log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static bool program_build(void)
{
    GLuint vs = compile(GL_VERTEX_SHADER, vertex_source, "vertex");
    GLuint fs = compile(GL_FRAGMENT_SHADER, fragment_source, "fragment");
    if (!vs || !fs)
        return false;

    program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glBindAttribLocation(program, 0, "position");
    glLinkProgram(program);

    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(program, sizeof(log), NULL, log);
        fprintf(stderr, "kicomp: glx: link: %s\n", log);
        return false;
    }

    u_projection = glGetUniformLocation(program, "projection");
    u_transform = glGetUniformLocation(program, "transform");
    u_opacity = glGetUniformLocation(program, "opacity");
    u_texture = glGetUniformLocation(program, "texture0");
    u_y_flip = glGetUniformLocation(program, "y_flip");

    /* One unit quad, reused for every window: the transform is what makes
     * it the right size in the right place, which is the same thing the
     * scene node already says. */
    static const float quad[] = { 0, 0, 1, 0, 0, 1, 1, 1 };
    glGenBuffers(1, &quad_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, quad_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);

    return true;
}

/* ------------------------------------------------------------------ */
/* setup                                                               */
/* ------------------------------------------------------------------ */

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

    /* And the two texture-from-pixmap configs: one for ordinary windows,
     * one for the 32-bit ones whose alpha is real. */
    const char *ext = glXQueryExtensionsString(dpy, screen);
    have_tfp = ext && strstr(ext, "GLX_EXT_texture_from_pixmap");
    if (!have_tfp) {
        fprintf(stderr, "kicomp: glx: no GLX_EXT_texture_from_pixmap; "
                        "this backend cannot sample windows without it\n");
        return false;
    }

    for (int argb = 0; argb < 2; argb++) {
        int attrs[] = {
            GLX_RENDER_TYPE, GLX_RGBA_BIT,
            GLX_DRAWABLE_TYPE, GLX_PIXMAP_BIT,
            GLX_BIND_TO_TEXTURE_TARGETS_EXT, GLX_TEXTURE_2D_BIT_EXT,
            argb ? GLX_BIND_TO_TEXTURE_RGBA_EXT : GLX_BIND_TO_TEXTURE_RGB_EXT, True,
            GLX_BUFFER_SIZE, argb ? 32 : 24,
            GLX_ALPHA_SIZE, argb ? 8 : 0,
            GLX_DOUBLEBUFFER, False,
            GLX_Y_INVERTED_EXT, GLX_DONT_CARE,
            None
        };

        count = 0;
        configs = glXChooseFBConfig(dpy, screen, attrs, &count);
        if (!configs || count == 0) {
            fprintf(stderr, "kicomp: glx: no %s texture-from-pixmap config\n",
                    argb ? "RGBA" : "RGB");
            return false;
        }
        int inverted = 0;
        glXGetFBConfigAttrib(dpy, configs[0], GLX_Y_INVERTED_EXT, &inverted);

        if (argb) {
            tfp_config_rgba = configs[0];
            tfp_inverted_rgba = inverted != 0;
        } else {
            tfp_config_rgb = configs[0];
            tfp_inverted_rgb = inverted != 0;
        }
        XFree(configs);
    }

    return true;
}

/* Opened once, on the first output. Everything here is per-screen rather
 * than per-output: one context, one program, one set of configs. */
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

    comp_info("glx %d.%d, %s, texture-from-pixmap y-inverted=%d/%d (rgb/rgba)",
              major, minor,
              glXIsDirect(dpy, context) ? "direct" : "indirect (software path)",
              tfp_inverted_rgb, tfp_inverted_rgba);
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

    if (!program && !program_build()) {
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

static GlxWindow *glx_window_find(xcb_window_t id)
{
    for (GlxWindow *g = windows; g; g = g->next)
        if (g->id == id)
            return g;
    return NULL;
}

static GlxWindow *glx_window_get(CompWindow *w)
{
    GlxWindow *g = glx_window_find(w->id);
    if (g)
        return g;

    g = calloc(1, sizeof(*g));
    if (!g)
        return NULL;
    g->id = w->id;
    g->next = windows;
    windows = g;
    return g;
}

/* Releases the image without touching the pixmap: what has to happen
 * before every rebind. Binding an already-bound image is undefined by the
 * extension, and "undefined" on a real driver is stale or torn contents. */
static void glx_window_release(GlxWindow *g)
{
    if (!g->bound || !g->glx_pixmap)
        return;

    glBindTexture(GL_TEXTURE_2D, g->texture);
    glXReleaseTexImageEXT(dpy, g->glx_pixmap, GLX_FRONT_LEFT_EXT);
    glBindTexture(GL_TEXTURE_2D, 0);
    g->bound = false;
}

static void glx_window_unbind(GlxWindow *g)
{
    if (g->glx_pixmap) {
        glx_window_release(g);
        glXDestroyPixmap(dpy, g->glx_pixmap);
        g->glx_pixmap = 0;
    }
    if (g->pixmap) {
        xcb_free_pixmap(comp.conn, (xcb_pixmap_t)g->pixmap);
        g->pixmap = 0;
    }
}

/* Names the window's contents and binds them as a texture. The pixmap is
 * named once and kept until the window is resized or unmapped -- the same
 * lifetime the XRender backend gives it -- while the *binding* is redone
 * every frame the window is drawn, which is what picks up new contents on
 * drivers that don't update a bound texture in place. */
static bool glx_window_bind(CompWindow *w, GlxWindow *g)
{
    CompRect r = window_rect(w);

    if (g->pixmap && (g->width != r.w || g->height != r.h))
        glx_window_unbind(g);

    if (!g->pixmap) {
        xcb_pixmap_t pixmap = xcb_generate_id(comp.conn);
        xcb_generic_error_t *err = xcb_request_check(comp.conn,
            xcb_composite_name_window_pixmap_checked(comp.conn, w->id, pixmap));
        if (err) {
            free(err);
            return false;
        }

        g->pixmap = pixmap;
        g->width = r.w;
        g->height = r.h;
        g->argb = w->argb;

        const int attrs[] = {
            GLX_TEXTURE_TARGET_EXT, GLX_TEXTURE_2D_EXT,
            GLX_TEXTURE_FORMAT_EXT, w->argb ? GLX_TEXTURE_FORMAT_RGBA_EXT
                                            : GLX_TEXTURE_FORMAT_RGB_EXT,
            None
        };
        g->glx_pixmap = glXCreatePixmap(dpy,
            w->argb ? tfp_config_rgba : tfp_config_rgb, g->pixmap, attrs);
        if (!g->glx_pixmap) {
            glx_window_unbind(g);
            return false;
        }
        g->y_inverted = w->argb ? tfp_inverted_rgba : tfp_inverted_rgb;

        if (!g->texture) {
            glGenTextures(1, &g->texture);
            glBindTexture(GL_TEXTURE_2D, g->texture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glBindTexture(GL_TEXTURE_2D, 0);
        }
    }

    /* Released first, then bound again: that pair is how the contents of
     * a window that has drawn since the last frame actually reach the
     * texture on most drivers. */
    glx_window_release(g);
    glBindTexture(GL_TEXTURE_2D, g->texture);
    glXBindTexImageEXT(dpy, g->glx_pixmap, GLX_FRONT_LEFT_EXT, NULL);
    g->bound = true;
    return true;
}

static void glx_window_invalidate(CompWindow *w)
{
    GlxWindow *g = glx_window_find(w->id);
    if (g)
        glx_window_unbind(g);
}

static void glx_window_free(CompWindow *w)
{
    GlxWindow **pp = &windows;
    while (*pp) {
        GlxWindow *g = *pp;
        if (g->id != w->id) {
            pp = &g->next;
            continue;
        }
        *pp = g->next;
        glx_window_unbind(g);
        if (g->texture)
            glDeleteTextures(1, &g->texture);
        free(g);
        return;
    }
}

/* ------------------------------------------------------------------ */
/* drawing                                                             */
/* ------------------------------------------------------------------ */

/* Logical root coordinates to clip space, for this output: the scale and
 * the output's origin folded into one matrix, so the rest of the drawing
 * works in the same coordinates everything above the renderer uses.
 * Column-major, as GL wants it. */
static void projection_for(const CompOutput *o, float m[16])
{
    float w = (float)o->rect.w;
    float h = (float)o->rect.h;
    if (w <= 0.0f) w = 1.0f;
    if (h <= 0.0f) h = 1.0f;

    memset(m, 0, sizeof(float) * 16);
    m[0] = 2.0f / w;
    m[5] = -2.0f / h;          /* y grows downwards in X, upwards in GL */
    m[10] = 1.0f;
    m[12] = -1.0f - 2.0f * (float)o->rect.x / w;
    m[13] = 1.0f + 2.0f * (float)o->rect.y / h;
    m[15] = 1.0f;
}

/* A scene node's placement as a matrix: the unit quad scaled to the
 * window's rectangle, then whatever the effects did to it. */
static void node_matrix(const CompSceneNode *n, float m[16])
{
    /* quad (0..1) -> the window's logical rectangle */
    float place[16] = {
        (float)n->geometry.w, 0, 0, 0,
        0, (float)n->geometry.h, 0, 0,
        0, 0, 1, 0,
        (float)n->geometry.x, (float)n->geometry.y, 0, 1
    };

    /* The node's own transform (transform.h) is row-major 4x4 in root
     * coordinates; GL wants column-major, hence the transpose. */
    float t[16];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            t[c * 4 + r] = n->transform.m[r][c];

    /* m = t * place */
    for (int c = 0; c < 4; c++) {
        for (int r = 0; r < 4; r++) {
            float sum = 0.0f;
            for (int k = 0; k < 4; k++)
                sum += t[k * 4 + r] * place[c * 4 + k];
            m[c * 4 + r] = sum;
        }
    }
}

static void glx_begin(CompOutput *o, const CompRegion *damage)
{
    GlxOutput *go = o->render_data;
    if (!go)
        return;

    glXMakeContextCurrent(dpy, go->drawable, go->drawable, context);

    glViewport(0, 0, o->physical.w, o->physical.h);

    /* The damage is deliberately ignored, and the whole output is redrawn
     * every frame (see this file's header). Scissoring to it would leave
     * the rest of the *back* buffer showing a frame from two swaps ago,
     * or uninitialised memory -- which is what the flashing and the
     * misplaced pieces were. GLX_EXT_buffer_age is what makes honouring
     * it correct, and until then this is the honest trade. */
    (void)damage;
    glDisable(GL_SCISSOR_TEST);

    glClearColor(0.109f, 0.109f, 0.109f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
}

static void glx_draw_scene(CompOutput *o, CompScene *s, const CompRegion *damage)
{
    GlxOutput *go = o->render_data;
    if (!go || !program)
        return;

    /* Every node, every frame: the whole buffer is being rebuilt (see
     * glx_begin), so skipping the ones damage didn't touch would leave
     * holes rather than save work. */
    (void)damage;

    float projection[16];
    projection_for(o, projection);

    glUseProgram(program);
    glUniformMatrix4fv(u_projection, 1, GL_FALSE, projection);
    glUniform1i(u_texture, 0);

    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glActiveTexture(GL_TEXTURE0);

    glBindBuffer(GL_ARRAY_BUFFER, quad_vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);

    for (int i = 0; i < s->count; i++) {
        CompSceneNode *n = &s->nodes[i];
        CompWindow *w = n->win;

        if (n->visible_rect.w <= 0 || n->visible_rect.h <= 0)
            continue;

        GlxWindow *g = glx_window_get(w);
        if (!g || !glx_window_bind(w, g))
            continue;

        float m[16];
        node_matrix(n, m);
        glUniformMatrix4fv(u_transform, 1, GL_FALSE, m);
        glUniform1f(u_opacity, n->opacity);
        /* GLX_Y_INVERTED_EXT *true* means the pixmap's first texture row
         * is its top one, which is already what the texture coordinates
         * here assume (they run downwards, like X's own y). It is the
         * *false* case -- GL's usual bottom-first convention -- that has
         * to be mirrored. Getting this backwards draws every window
         * upside down, which is worth stating plainly because the two
         * mistakes look identical until you try the other driver. */
        glUniform1f(u_y_flip, g->y_inverted ? 0.0f : 1.0f);

        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    glDisableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
    glDisable(GL_BLEND);
}

static void glx_end(CompOutput *o)
{
    (void)o;
    glDisable(GL_SCISSOR_TEST);
    glFlush();
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

bool renderer_glx_ready(void)
{
    return dpy != NULL && context != NULL;
}

/* ------------------------------------------------------------------ */
/* the parts this backend doesn't have yet                             */
/* ------------------------------------------------------------------ */

/* Shape clipping, the stash, the X-DENSITY layers and the root background
 * are XRender-only for now (see this file's header). The vtable simply
 * leaves those ops NULL, which is what the wrappers in renderer.c are for
 * -- each is a follow-up here, and none of them is a change anywhere
 * else. */
static bool glx_window_has_content(const CompWindow *w)
{
    GlxWindow *g = glx_window_find(w->id);
    return g && g->pixmap != 0;
}

static void glx_shutdown(void)
{
    while (windows) {
        GlxWindow *g = windows;
        windows = g->next;
        glx_window_unbind(g);
        if (g->texture)
            glDeleteTextures(1, &g->texture);
        free(g);
    }

    if (program) {
        glDeleteProgram(program);
        program = 0;
    }
    if (quad_vbo) {
        glDeleteBuffers(1, &quad_vbo);
        quad_vbo = 0;
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

    .window_invalidate  = glx_window_invalidate,
    .window_free        = glx_window_free,
    .window_has_content = glx_window_has_content,
    .shutdown           = glx_shutdown,
};

const CompRenderer *renderer_glx(void)
{
    return &glx_renderer;
}
