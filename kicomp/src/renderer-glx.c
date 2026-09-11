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
#include "shadow.h"
#include "text.h"
#include "transform.h"

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

static GLuint program;
static GLint u_projection, u_transform, u_opacity, u_texture, u_y_flip;
static GLuint quad_vbo;

/* The shadow program, and the profile texture it reads (see shadow.h:
 * one dimension, 2*radius alpha texels, the same numbers XRender builds
 * its tiles from). */
static GLuint shadow_program;
static GLint su_projection, su_transform, su_color, su_size, su_span,
             su_hole, su_profile;
static GLuint shadow_texture;
static int shadow_texture_radius;

/* GLX_EXT_buffer_age, without which there is no way to know what the back
 * buffer already holds and every frame has to be redrawn whole. */
static bool have_buffer_age;

/* GLX_OML_sync_control: the vblank counter behind the drawable, which is
 * what turns "the swap probably waited for vblank" into a number. */
static bool have_oml_sync;

/* The rectangle currently being repainted, in root coordinates.
 * Everything drawn is scissored to it (and to whatever else it is
 * already clipped by), so one pass over the scene per rectangle covers
 * exactly the damage and nothing else. */
static CompRect repaint_rect;

/* Per-output GL state, hung off CompOutput::render_data. */
/* How many past frames' damage to remember. A back buffer this old or
 * older cannot be trusted at all, so anything beyond this is a full
 * repaint -- and drivers in practice hand back ages of 1..3. */
#define AGE_HISTORY 4

typedef struct {
    Window window;          /* CRTC-covering child of the overlay */
    GLXWindow drawable;

    /* What was repainted in each of the last few frames, newest first.
     * A back buffer that is `age` swaps old already holds everything
     * except what has changed since -- so repainting the union of the
     * last `age` frames' damage brings it up to date, and repainting the
     * whole screen every frame to be safe is what this replaces. */
    CompRegion history[AGE_HISTORY];
    int history_len;

    /* This frame's answer, kept from begin() to end(): the renderer draws
     * and clears through it, and everything is scissored to one of its
     * rectangles at a time. */
    CompRegion repaint;

    /* And this frame's *damage*, which is what goes into the history --
     * not the repaint. The two differ whenever a frame redraws more than
     * changed, and the history has to mean "what changed then", because
     * that is what a buffer from before it is missing. Recording the
     * repaint instead makes a full frame poison every frame after it: it
     * lands in the history as "everything", which forces the next frame
     * full, which records "everything" again, for ever. */
    CompRegion frame_damage;
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

    /* The window's silhouette, in window-local pixels, as the scissor
     * rectangles the quad is drawn through. XRender hands its region to
     * the server and forgets about it; GL has no such thing, so the
     * rectangles are fetched once and kept until the shape changes --
     * kiwm reshapes a frame on every resize step, so fetching them per
     * frame would be a round trip in the middle of the paint. */
    xcb_rectangle_t *shape_rects;
    int shape_count;
    bool shape_known;
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

/* A shadow is a blurred rectangle, and the blur of a rectangle is
 * separable: the alpha at any point is the horizontal profile times the
 * vertical one. So instead of XRender's nine composited tiles this is one
 * quad and two texture lookups per fragment -- the same shape, expressed
 * the way the hardware here is happy to draw it.
 *
 * `span` is 2*radius, the width of the fade. For a rectangle narrower
 * than two fades the two ends overlap, and P(d) + P(size-d) - 1 is what
 * the convolution actually gives there -- the naive min() of the two
 * would leave a shadow that is too dark down the middle of a thin window.
 *
 * `hole` is the window's own rectangle in the same local pixels: the
 * shadow is not drawn behind it, because a translucent window would
 * otherwise have its own shadow showing through it. */
static const char *shadow_vertex_source =
    "#version 120\n"
    "attribute vec2 position;\n"
    "uniform mat4 projection;\n"
    "uniform mat4 transform;\n"
    "uniform vec2 size;\n"
    "varying vec2 local;\n"
    "void main() {\n"
    "    local = position * size;\n"
    "    gl_Position = projection * transform * vec4(position, 0.0, 1.0);\n"
    "}\n";

static const char *shadow_fragment_source =
    "#version 120\n"
    "uniform sampler2D profile;\n"
    "uniform vec4 color;\n"
    "uniform vec2 size;\n"
    "uniform float span;\n"
    "uniform vec4 hole;\n"
    "varying vec2 local;\n"
    "float edge(float d) {\n"
    "    return texture2D(profile, vec2(clamp(d / span, 0.0, 1.0), 0.5)).a;\n"
    "}\n"
    "float band(float p, float len) {\n"
    "    return clamp(edge(p) + edge(len - p) - 1.0, 0.0, 1.0);\n"
    "}\n"
    "void main() {\n"
    "    if (local.x >= hole.x && local.x < hole.x + hole.z &&\n"
    "        local.y >= hole.y && local.y < hole.y + hole.w)\n"
    "        discard;\n"
    "    float a = band(local.x, size.x) * band(local.y, size.y);\n"
    "    gl_FragColor = vec4(color.rgb * color.a * a, color.a * a);\n"
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

    /* The shadow program is optional in the sense that failing to build
     * it costs shadows, not the session: everything else still draws. */
    vs = compile(GL_VERTEX_SHADER, shadow_vertex_source, "shadow vertex");
    fs = compile(GL_FRAGMENT_SHADER, shadow_fragment_source, "shadow fragment");
    if (vs && fs) {
        shadow_program = glCreateProgram();
        glAttachShader(shadow_program, vs);
        glAttachShader(shadow_program, fs);
        glBindAttribLocation(shadow_program, 0, "position");
        glLinkProgram(shadow_program);
        glDeleteShader(vs);
        glDeleteShader(fs);

        ok = 0;
        glGetProgramiv(shadow_program, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[512];
            glGetProgramInfoLog(shadow_program, sizeof(log), NULL, log);
            fprintf(stderr, "kicomp: glx: shadow link: %s\n", log);
            glDeleteProgram(shadow_program);
            shadow_program = 0;
        } else {
            su_projection = glGetUniformLocation(shadow_program, "projection");
            su_transform = glGetUniformLocation(shadow_program, "transform");
            su_color = glGetUniformLocation(shadow_program, "color");
            su_size = glGetUniformLocation(shadow_program, "size");
            su_span = glGetUniformLocation(shadow_program, "span");
            su_hole = glGetUniformLocation(shadow_program, "hole");
            su_profile = glGetUniformLocation(shadow_program, "profile");
        }
    }

    return true;
}

/* The blur profile as a 1-D texture, rebuilt when a different radius is
 * asked for. Two radii alternate in practice (focused and unfocused), and
 * a 2*radius byte upload is cheap enough that keeping one is enough. */
static bool shadow_profile_texture(int radius)
{
    if (shadow_texture && shadow_texture_radius == radius)
        return true;

    int n = radius * 2;
    uint8_t *profile = malloc((size_t)n);
    if (!profile)
        return false;
    shadow_profile(radius, profile);

    if (!shadow_texture)
        glGenTextures(1, &shadow_texture);
    glBindTexture(GL_TEXTURE_2D, shadow_texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, n, 1, 0, GL_ALPHA,
                 GL_UNSIGNED_BYTE, profile);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    free(profile);
    shadow_texture_radius = radius;
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
        g->glx_pixmap = glXCreatePixmap(dpy, cfg->config, g->pixmap, attrs);
        if (!g->glx_pixmap) {
            glx_window_unbind(g);
            return false;
        }
        g->y_inverted = cfg->y_inverted;

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

static void glx_shape_forget(GlxWindow *g)
{
    free(g->shape_rects);
    g->shape_rects = NULL;
    g->shape_count = 0;
    g->shape_known = false;
}

static void glx_window_invalidate(CompWindow *w)
{
    GlxWindow *g = glx_window_find(w->id);
    if (g) {
        glx_window_unbind(g);
        glx_shape_forget(g);
    }
}

static void glx_window_shape_invalidate(CompWindow *w)
{
    GlxWindow *g = glx_window_find(w->id);
    if (g)
        glx_shape_forget(g);
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
        free(g->shape_rects);
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
/* The output's lens (comp.h's view) as a magnification and an origin.
 * Always a scale about a point, which is all a zoom asks for. */
static float lens_of(const CompOutput *o, float *ox, float *oy)
{
    float k = o->view.m[0][0];
    if (k <= 0.0f)
        k = 1.0f;
    *ox = o->view.m[0][3];
    *oy = o->view.m[1][3];
    return k;
}

static void projection_for(const CompOutput *o, float m[16])
{
    float w = (float)o->rect.w;
    float h = (float)o->rect.h;
    if (w <= 0.0f) w = 1.0f;
    if (h <= 0.0f) h = 1.0f;

    /* The lens folds in here, once, and everything drawn through this
     * projection follows it: the windows, their shadows, the mesh and
     * the slices. A vertex arrives in root coordinates and is magnified
     * on its way to the screen, which is what a lens is. */
    float ox, oy;
    float k = lens_of(o, &ox, &oy);

    memset(m, 0, sizeof(float) * 16);
    m[0] = 2.0f * k / w;
    m[5] = -2.0f * k / h;      /* y grows downwards in X, upwards in GL */
    m[10] = 1.0f;
    m[12] = -1.0f + 2.0f * (ox - (float)o->rect.x) / w;
    m[13] = 1.0f - 2.0f * (oy - (float)o->rect.y) / h;
    m[15] = 1.0f;
}

/* The unit quad placed on a rectangle in root coordinates, then put
 * through a node's transform. Two callers: the window itself, and its
 * shadow -- which is a different rectangle carried by the same transform,
 * so that a window being slid or scaled by an effect takes its shadow
 * with it. */
static void rect_matrix(const CompRect *rect, const CompTransform *transform,
                        float m[16])
{
    /* quad (0..1) -> the rectangle */
    float place[16] = {
        (float)rect->w, 0, 0, 0,
        0, (float)rect->h, 0, 0,
        0, 0, 1, 0,
        (float)rect->x, (float)rect->y, 0, 1
    };

    /* The node's own transform (transform.h) is row-major 4x4 in root
     * coordinates; GL wants column-major, hence the transpose. */
    float t[16];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            t[c * 4 + r] = transform->m[r][c];

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

static void node_matrix(const CompSceneNode *n, float m[16])
{
    rect_matrix(&n->geometry, &n->transform, m);
}

/* The window's bounding shape, in window-local pixels. An unshaped
 * window answers with its whole rectangle, which is exactly the answer
 * that needs no clipping at all -- so shape_count == 0 means "draw it
 * whole" and costs nothing per frame. */
static void shape_fetch(CompWindow *w, GlxWindow *g);

static void shape_fetch(CompWindow *w, GlxWindow *g)
{
    if (g->shape_known)
        return;
    g->shape_known = true;

    free(g->shape_rects);
    g->shape_rects = NULL;
    g->shape_count = 0;

    if (!comp.caps.shape)
        return;

    /* Whether the window is shaped at all, and where that shape is, has
     * to be asked here rather than read off the window: CompWindow::shaped
     * and ::shape_extents are filled in by whichever backend looks, and
     * the XRender one is not running. Trusting a flag nobody set is why
     * the first version of this drew every frame with square corners
     * while insisting it was clipping them.
     *
     * The extents are worth the same round trip: the shadow is cast
     * around them (draw_shadow), and a window whose rectangle is far
     * bigger than what it draws -- VirtualBox's mini-toolbar -- would
     * otherwise get a shadow the size of the screen. */
    xcb_shape_query_extents_reply_t *ext = xcb_shape_query_extents_reply(comp.conn,
        xcb_shape_query_extents(comp.conn, w->id), NULL);
    if (ext) {
        w->shaped = ext->bounding_shaped;
        w->shape_extents.x = ext->bounding_shape_extents_x;
        w->shape_extents.y = ext->bounding_shape_extents_y;
        w->shape_extents.w = ext->bounding_shape_extents_width;
        w->shape_extents.h = ext->bounding_shape_extents_height;
        free(ext);
    }
    if (!w->shaped)
        return;

    xcb_shape_get_rectangles_reply_t *r = xcb_shape_get_rectangles_reply(comp.conn,
        xcb_shape_get_rectangles(comp.conn, w->id, XCB_SHAPE_SK_BOUNDING), NULL);
    if (!r)
        return;

    int n = xcb_shape_get_rectangles_rectangles_length(r);
    if (n > 0) {
        g->shape_rects = malloc(sizeof(xcb_rectangle_t) * (size_t)n);
        if (g->shape_rects) {
            memcpy(g->shape_rects, xcb_shape_get_rectangles_rectangles(r),
                   sizeof(xcb_rectangle_t) * (size_t)n);
            g->shape_count = n;
        }
    }
    free(r);
}

/* One shape rectangle as a GL scissor box. The scissor is in physical
 * pixels counted from the *bottom* of the drawable, which is neither the
 * unit the rest of this file works in nor the direction X counts in. */
static void scissor_for(const CompOutput *o, int x, int y, int w, int h)
{
    float scale = o->scale > 0.0f ? o->scale : 1.0f;

    /* Through the lens as well: a scissor box is where something is on
     * screen, and under a lens that is not where it is in root
     * coordinates -- which is exactly what clipped a magnified window's
     * rounded corners to the place the window would have been. */
    float ox, oy;
    float k = lens_of(o, &ox, &oy);

    float lx = (float)x * k + ox;
    float ly = (float)y * k + oy;

    int px = (int)((lx - (float)o->rect.x) * scale);
    int py = (int)((ly - (float)o->rect.y) * scale);
    int pw = (int)((float)w * k * scale + 0.5f);
    int ph = (int)((float)h * k * scale + 0.5f);

    glScissor(px, o->physical.h - (py + ph), pw, ph);
}

/* Defined below, once the repaint rectangle exists: the same thing as
 * scissor_for(), intersected with what this frame is allowed to touch. */
static bool scissor_to(const CompOutput *o, const CompRect *r);

/* The shadow box minus the window's silhouette, as scissor rectangles.
 *
 * The shader can punch one rectangle out of the shadow, and a rectangle
 * is the wrong shape: it leaves a notch of missing shadow at each rounded
 * corner -- inside the rectangle, outside the window, so neither the
 * window nor its shadow is drawn there and the desktop shows through.
 * Cutting the real silhouette is what lets the shadow reach into the
 * corner, which is what the XRender backend does with an XFixes
 * subtraction it gets for three asynchronous requests. Here the shape is
 * already in hand as rectangles, so the complement is arithmetic.
 *
 * X hands shape rectangles back Y-banded: sorted by y, bands of equal
 * height, no overlaps. So one walk emits the gaps -- the rows between
 * bands, and within a band the spans the shape does not cover.
 *
 * Returns the number written, or -1 to say "too many, use the rectangle"
 * -- a pathologically shaped window is not worth hundreds of draws for a
 * shadow nobody is looking at that closely. */
#define SHADOW_CUT_MAX 64

static int shadow_cut_rects(const CompRect *box, const CompWindow *w,
                            const GlxWindow *g, int dx, int dy, CompRect *out)
{
    int n = 0;
    int box_r = box->x + box->w;
    int box_b = box->y + box->h;
    int y = box->y;

    /* Emits one rectangle, clipped to the box and dropped if empty. */
    #define EMIT(ex, ey, ew, eh) do {                                     \
        CompRect r_ = { (ex), (ey), (ew), (eh) };                         \
        CompRect hit_;                                                    \
        if (rect_intersect(&r_, box, &hit_)) {                            \
            if (n >= SHADOW_CUT_MAX) return -1;                           \
            out[n++] = hit_;                                              \
        }                                                                 \
    } while (0)

    for (int i = 0; i < g->shape_count; ) {
        int band_y = w->y + dy + g->shape_rects[i].y;
        int band_h = g->shape_rects[i].height;

        /* Everything above this band is shadow. */
        if (band_y > y)
            EMIT(box->x, y, box->w, band_y - y);

        /* And within it, whatever the shape's spans leave over. */
        int x = box->x;
        while (i < g->shape_count &&
               w->y + dy + g->shape_rects[i].y == band_y &&
               g->shape_rects[i].height == band_h) {
            int sx = w->x + dx + g->shape_rects[i].x;
            if (sx > x)
                EMIT(x, band_y, sx - x, band_h);
            int end = sx + g->shape_rects[i].width;
            if (end > x)
                x = end;
            i++;
        }
        if (x < box_r)
            EMIT(x, band_y, box_r - x, band_h);

        y = band_y + band_h;
    }

    if (y < box_b)
        EMIT(box->x, y, box->w, box_b - y);

    #undef EMIT
    return n;
}

/* The shadow: one quad, the profile texture, and the window's own
 * silhouette left out of it (see the shader above). The rectangle it is
 * cast around is what can be *seen* of the window -- its shape extents
 * where it has them, never larger than the window itself, the same clamp
 * the XRender backend needs and for the same reason (a shape belongs to
 * the size the window had when it was set). */
static void draw_shadow(const CompOutput *o, const CompSceneNode *n,
                        CompWindow *w, const GlxWindow *g,
                        const float projection[16])
{
    CompShadowStyle st;
    if (!shadow_program || !shadow_for_window(w, &st))
        return;
    if (st.opacity <= 0.0f || st.radius <= 0)
        return;

    CompRect base = n->geometry;
    if (w->shaped && w->shape_extents.w > 0 && w->shape_extents.h > 0) {
        CompRect ext = { w->x + w->shape_extents.x, w->y + w->shape_extents.y,
                         w->shape_extents.w, w->shape_extents.h };
        CompRect clamped;
        if (rect_intersect(&ext, &n->geometry, &clamped))
            base = clamped;
    }

    int r = st.radius;
    if (!shadow_profile_texture(r))
        return;

    CompRect box = { base.x + st.offset_x - r, base.y + st.offset_y - r,
                     base.w + r * 2, base.h + r * 2 };

    float m[16];
    rect_matrix(&box, &n->transform, m);

    glUseProgram(shadow_program);
    glUniformMatrix4fv(su_projection, 1, GL_FALSE, projection);
    glUniformMatrix4fv(su_transform, 1, GL_FALSE, m);
    glUniform4f(su_color, st.r, st.g, st.b, st.opacity * n->opacity);
    glUniform2f(su_size, (float)box.w, (float)box.h);
    glUniform1f(su_span, (float)(r * 2));
    glUniform1i(su_profile, 0);
    glBindTexture(GL_TEXTURE_2D, shadow_texture);

    /* The silhouette, where the window has one and is where it says it
     * is -- a scissor box lives in screen pixels, so it can follow a
     * move but not a scale (the same limit the clipping of the window
     * itself has). Anything else falls back to the rectangle, which is
     * only visibly wrong on a rounded corner, and a window mid-scale
     * doesn't have its corners where they will end up anyway. */
    CompRect cut[SHADOW_CUT_MAX];
    int cuts = -1;
    float tdx = 0.0f, tdy = 0.0f;
    bool move_only = comp_transform_is_identity(&n->transform) ||
                     comp_transform_is_translation(&n->transform, &tdx, &tdy);

    if (g && g->shape_count > 0 && move_only) {
        /* Where the shadow is being *drawn*, which during a move is not
         * where the window is: the quad's matrix carries the transform,
         * so the scissor boxes have to be carried the same distance. */
        CompRect moved = { box.x + (int)tdx, box.y + (int)tdy, box.w, box.h };
        cuts = shadow_cut_rects(&moved, w, g, (int)tdx, (int)tdy, cut);
    }

    if (cuts >= 0) {
        /* No hole in the shader: the scissor boxes *are* the hole. */
        glUniform4f(su_hole, 0.0f, 0.0f, 0.0f, 0.0f);
        for (int i = 0; i < cuts; i++)
            if (scissor_to(o, &cut[i]))
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    } else {
        glUniform4f(su_hole, (float)(base.x - box.x), (float)(base.y - box.y),
                    (float)base.w, (float)base.h);
        scissor_for(o, repaint_rect.x, repaint_rect.y,
                    repaint_rect.w, repaint_rect.h);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
}

/* Everything this frame is drawn through the scissor, so a rectangle
 * that is empty after being clipped is a rectangle nobody has to draw.
 * `r` is in root coordinates; the scissor is in physical pixels counted
 * from the bottom of the drawable. */
static bool scissor_to(const CompOutput *o, const CompRect *r)
{
    CompRect hit;
    if (!rect_intersect(r, &repaint_rect, &hit))
        return false;
    if (!rect_intersect(&hit, &o->rect, &hit))
        return false;

    scissor_for(o, hit.x, hit.y, hit.w, hit.h);
    return true;
}

/* What the back buffer is missing, and therefore what this frame has to
 * draw.
 *
 * GLX_EXT_buffer_age answers the one question that makes partial
 * repainting safe: how many swaps ago this buffer was last shown. Age 1
 * is the frame before last; age n means the union of the last n frames'
 * damage is exactly what has changed since it was current. Age 0 means
 * the driver is not saying -- a resized drawable, a fresh one, a driver
 * that discards -- and the honest answer to that is to redraw
 * everything, which is also what happens without the extension at all.
 *
 * Getting this wrong doesn't look like a small mistake: it is the black
 * and red flashing and the misplaced pieces of window that the first
 * version of this file had, because the parts nobody repainted were
 * showing a frame from two swaps ago or memory nobody had written. */
static void repaint_region_for(CompOutput *o, GlxOutput *go,
                               const CompRegion *damage)
{
    (void)o;

    if (region_is_full(damage) || !have_buffer_age) {
        region_set_full(&go->repaint);
        return;
    }

    unsigned age = 0;
    glXQueryDrawable(dpy, go->drawable, GLX_BACK_BUFFER_AGE_EXT, &age);

    if (age == 0 || (int)age > go->history_len) {
        region_set_full(&go->repaint);
        return;
    }

    go->repaint = *damage;
    for (unsigned i = 0; i < age && (int)i < go->history_len; i++) {
        const CompRegion *past = &go->history[i];
        if (region_is_full(past)) {
            region_set_full(&go->repaint);
            return;
        }
        for (int k = 0; k < past->count; k++)
            region_add(&go->repaint, &past->rects[k]);
    }
}

/* The damage as the history wants it: a rectangle list, with "everything"
 * spelled as the output's own rectangle rather than as the flag. As a
 * flag it would say "this frame is dirty" for ever after; as a rectangle
 * it says "all of it changed then", which ages out of the window like
 * any other entry. */
static void frame_damage_of(const CompOutput *o, const CompRegion *damage,
                            CompRegion *out)
{
    if (region_is_full(damage)) {
        region_clear(out);
        region_add(out, &o->rect);
        return;
    }
    *out = *damage;
}

static void glx_begin(CompOutput *o, const CompRegion *damage)
{
    GlxOutput *go = o->render_data;
    if (!go)
        return;

    glXMakeContextCurrent(dpy, go->drawable, go->drawable, context);

    glViewport(0, 0, o->physical.w, o->physical.h);

    repaint_region_for(o, go, damage);
    frame_damage_of(o, damage, &go->frame_damage);

    glClearColor(0.109f, 0.109f, 0.109f, 1.0f);

    if (region_is_full(&go->repaint)) {
        glDisable(GL_SCISSOR_TEST);
        glClear(GL_COLOR_BUFFER_BIT);
        return;
    }

    /* Only the ground that is about to be redrawn. Clearing the whole
     * buffer here would throw away precisely the pixels the buffer age
     * just told us are still good. */
    glEnable(GL_SCISSOR_TEST);
    for (int i = 0; i < go->repaint.count; i++) {
        const CompRect *r = &go->repaint.rects[i];
        scissor_for(o, r->x, r->y, r->w, r->h);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    glDisable(GL_SCISSOR_TEST);
}

/* One pass over the scene, everything clipped to `repaint_rect`. Called
 * once per damaged rectangle, so a frame where two small things changed
 * costs two small passes instead of one screen-sized one. */
/* An effect's own ground, under every window (scene.h): one quad in a
 * flat colour, through the same shader everything else goes through. A
 * 1x1 texture rather than a shader of its own -- the fragment program
 * here is a texel times an opacity, and a texel is exactly what a colour
 * is. */
static void draw_backdrop(CompOutput *o, const CompScene *s,
                          const float projection[16])
{
    const CompSceneBackdrop *b = &s->backdrop;
    if (b->rect.w <= 0 || b->rect.h <= 0)
        return;

    CompRect ignored;
    if (!rect_intersect(&b->rect, &repaint_rect, &ignored))
        return;

    static GLuint tex;
    if (!tex) {
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else {
        glBindTexture(GL_TEXTURE_2D, tex);
    }

    unsigned char px[4] = {
        (unsigned char)(b->r * 255.0f + 0.5f),
        (unsigned char)(b->g * 255.0f + 0.5f),
        (unsigned char)(b->b * 255.0f + 0.5f),
        255,
    };
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, px);

    CompTransform identity;
    comp_transform_identity(&identity);
    float m[16];
    rect_matrix(&b->rect, &identity, m);

    glUseProgram(program);
    glUniformMatrix4fv(u_projection, 1, GL_FALSE, projection);
    glUniform1i(u_texture, 0);
    glUniform1f(u_y_flip, 0.0f);
    glUniformMatrix4fv(u_transform, 1, GL_FALSE, m);
    glUniform1f(u_opacity, b->opacity);

    scissor_for(o, repaint_rect.x, repaint_rect.y,
                repaint_rect.w, repaint_rect.h);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static void draw_pass(CompOutput *o, CompScene *s, const float projection[16])
{
    draw_backdrop(o, s, projection);

    for (int i = 0; i < s->count; i++) {
        CompSceneNode *n = &s->nodes[i];
        CompWindow *w = n->win;

        if (n->visible_rect.w <= 0 || n->visible_rect.h <= 0)
            continue;

        GlxWindow *g = glx_window_get(w);
        if (!g)
            continue;

        /* Nothing of this window is in the rectangle being repainted --
         * grown by the shadow's reach, since a window paints outside
         * itself. This test is where the saving actually comes from: the
         * scissor would clip it anyway, but binding a pixmap and issuing
         * draws for a window nobody can see is the work worth not doing. */
        CompRect touch = n->visible_rect;
        int reach = shadow_margin_for_window(w);
        touch.x -= reach;
        touch.y -= reach;
        touch.w += reach * 2;
        touch.h += reach * 2;
        CompRect ignored;
        if (!rect_intersect(&touch, &repaint_rect, &ignored))
            continue;

        /* The shape first: the shadow is cast around the window's
         * silhouette, and both of them are about to want it. */
        shape_fetch(w, g);

        /* Under the window, and before its texture is bound: the shadow
         * program has its own idea of what is in texture unit 0. */
        draw_shadow(o, n, w, g, projection);
        glUseProgram(program);
        glUniformMatrix4fv(u_projection, 1, GL_FALSE, projection);
        glUniform1i(u_texture, 0);

        if (!glx_window_bind(w, g))
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

        /* A shaped window is drawn through its silhouette, one scissor
         * box per rectangle: rounded corners are the everyday case here,
         * since kiwm rounds every frame it draws, and without this the
         * corners come back square with whatever the pixmap holds
         * outside them.
         *
         * Only while the window is where it says it is, or is being
         * moved: a scissor box is in screen pixels, so it can follow a
         * translation but not a scale or a rotation. A window mid-scale
         * is drawn whole for those frames, which is what the XRender
         * backend does with the same reasoning. */
        float tdx = 0.0f, tdy = 0.0f;
        bool move_only = comp_transform_is_identity(&n->transform) ||
                         comp_transform_is_translation(&n->transform, &tdx, &tdy);

        if (g->shape_count > 0 && move_only) {
            for (int k = 0; k < g->shape_count; k++) {
                const xcb_rectangle_t *sr = &g->shape_rects[k];
                CompRect piece = { w->x + (int)tdx + sr->x,
                                   w->y + (int)tdy + sr->y,
                                   sr->width, sr->height };
                if (!scissor_to(o, &piece))
                    continue;
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            }
        } else if (!move_only && w->shaped &&
                   w->shape_extents.w > 0 && w->shape_extents.h > 0) {
            /* Being scaled, so the silhouette cannot come along -- a
             * scissor box lives in screen pixels. Its *extents* can,
             * carried through the same transform, and for the window this
             * matters to they are nothing like its rectangle:
             * VirtualBox's mini-toolbar is a screen-sized window with a
             * small bar shaped out of it, and drawing the rectangle while
             * an effect shrinks it puts a screen-sized ghost of stale
             * contents in the middle of the grid. */
            CompRect ext = { w->x + w->shape_extents.x,
                             w->y + w->shape_extents.y,
                             w->shape_extents.w, w->shape_extents.h };
            CompRect moved = comp_transform_rect(&n->transform, &ext);
            if (scissor_to(o, &moved))
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        } else {
            /* Unshaped: the damage rectangle is the whole clip. */
            scissor_for(o, repaint_rect.x, repaint_rect.y,
                        repaint_rect.w, repaint_rect.h);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        }
    }
}

/* An effect's labels (scene.h), over everything and never scaled. The
 * image is uploaded once and remembered in the image itself, because a
 * label outlives the frame that first drew it -- a filter box redrawn
 * every frame would be a Pango layout every frame. */
static void draw_chrome(CompOutput *o, const CompScene *s,
                        const float projection[16])
{
    if (s->chrome_count == 0)
        return;

    glUseProgram(program);
    glUniformMatrix4fv(u_projection, 1, GL_FALSE, projection);
    glUniform1i(u_texture, 0);
    /* The pixels came from Cairo, which lays out a row top-first, the
     * same way the texture coordinates here run. */
    glUniform1f(u_y_flip, 0.0f);

    for (int i = 0; i < s->chrome_count; i++) {
        const CompSceneChrome *c = &s->chrome[i];
        const unsigned char *pixels = text_pixels(c->image);
        unsigned int *tex = text_gl_texture(c->image);
        if (!pixels || !tex)
            continue;

        if (*tex == 0) {
            glGenTextures(1, tex);
            glBindTexture(GL_TEXTURE_2D, *tex);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            /* Cairo's ARGB32 is native-endian premultiplied, which on a
             * little-endian machine is B,G,R,A in memory -- hence BGRA
             * rather than RGBA, and the same premultiplied blend the
             * windows use. */
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                         text_width(c->image), text_height(c->image), 0,
                         GL_BGRA, GL_UNSIGNED_BYTE, pixels);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        } else {
            glBindTexture(GL_TEXTURE_2D, *tex);
        }

        CompTransform identity;
        comp_transform_identity(&identity);
        float m[16];
        rect_matrix(&c->rect, &identity, m);

        glUniformMatrix4fv(u_transform, 1, GL_FALSE, m);
        glUniform1f(u_opacity, c->opacity);

        scissor_for(o, repaint_rect.x, repaint_rect.y,
                    repaint_rect.w, repaint_rect.h);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
}

static void glx_draw_scene(CompOutput *o, CompScene *s, const CompRegion *damage)
{
    GlxOutput *go = o->render_data;
    if (!go || !program)
        return;

    /* The region begin() worked out from the buffer age, not the raw
     * damage: what this buffer is missing is the damage of every frame
     * since it was last shown. */
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

    /* Everything is drawn scissored, whether the frame is partial or
     * not: a full repaint is simply one rectangle the size of the
     * output, so there is one code path rather than two, and the one
     * that runs every frame is the one that has been tested. */
    glEnable(GL_SCISSOR_TEST);

    if (region_is_full(&go->repaint)) {
        repaint_rect = o->rect;
        draw_pass(o, s, projection);
    } else {
        for (int i = 0; i < go->repaint.count; i++) {
            repaint_rect = go->repaint.rects[i];
            draw_pass(o, s, projection);
        }
    }

    /* Chrome last, over every window, and inside the same scissor: a
     * label in a part of the screen this frame is not repainting would
     * be a label drawn onto a buffer nobody is showing. */
    if (region_is_full(&go->repaint)) {
        repaint_rect = o->rect;
        draw_chrome(o, s, projection);
    } else {
        for (int i = 0; i < go->repaint.count; i++) {
            repaint_rect = go->repaint.rects[i];
            draw_chrome(o, s, projection);
        }
    }

    glDisable(GL_SCISSOR_TEST);
    glDisableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
    glDisable(GL_BLEND);
}

static void glx_end(CompOutput *o)
{
    GlxOutput *go = o->render_data;

    glDisable(GL_SCISSOR_TEST);
    glFlush();

    if (!go)
        return;

    /* Remembered for the frames that will inherit this buffer: in a few
     * swaps' time it comes back as the back buffer, and what it is
     * missing then is everything drawn since -- starting with this. */
    for (int i = AGE_HISTORY - 1; i > 0; i--)
        go->history[i] = go->history[i - 1];
    go->history[0] = go->frame_damage;
    if (go->history_len < AGE_HISTORY)
        go->history_len++;
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
        free(g->shape_rects);
        free(g);
    }

    while (tfp_configs) {
        TfpConfig *c = tfp_configs;
        tfp_configs = c->next;
        free(c);
    }

    if (shadow_texture) {
        glDeleteTextures(1, &shadow_texture);
        shadow_texture = 0;
        shadow_texture_radius = 0;
    }
    if (shadow_program) {
        glDeleteProgram(shadow_program);
        shadow_program = 0;
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
    .window_shape_invalidate = glx_window_shape_invalidate,
    .window_free        = glx_window_free,
    .window_has_content = glx_window_has_content,
    .shutdown           = glx_shutdown,
};

const CompRenderer *renderer_glx(void)
{
    return &glx_renderer;
}
