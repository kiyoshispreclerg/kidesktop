/*
 * The GL drawing shared by every GL platform (GLX today, EGL next).
 *
 * A GL compositor is two separable things: *getting* a context, a
 * target per output and each window's pixels as a texture -- which is
 * where GLX and EGL differ in every line -- and *drawing* the scene with
 * them, which is the same shaders, the same matrices, the same scissor
 * boxes and the same damage history whichever way the texture arrived.
 * This file is the second half. A platform (renderer-glx.c) makes a
 * context current, hands each output a GlOutput and each window a
 * texture through the GlPlatform ops, and the scene gets drawn.
 *
 * What this file deliberately does not know: how the texture behind a
 * window is bound (texture-from-pixmap, a dma-buf EGLImage), what the
 * output's drawable is, or how old the buffer being drawn into is. The
 * platform answers that last one as a number in gl_begin(), because the
 * answer is the whole difference between "repaint what changed" and the
 * black-and-red flashing the first GLX version had: a buffer that is
 * `age` presentations old is missing the damage of every frame since,
 * and nothing else.
 */
#include "renderer-gl.h"
#include "region.h"
#include "output.h"
#include "window.h"
#include "shadow.h"
#include "text.h"
#include "transform.h"

#include <xcb/shape.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The platform's ops, from gl_setup(). */
static const GlPlatform *platform;


static GLuint program;
static GLint u_projection, u_transform, u_opacity, u_texture, u_y_flip, u_use_uv;
static GLint u_mask, u_use_mask;
static GLuint quad_vbo;
static GLuint mesh_vbo;

/* The shadow program, and the profile texture it reads (see shadow.h:
 * one dimension, 2*radius alpha texels, the same numbers XRender builds
 * its tiles from). */
static GLuint shadow_program;
static GLint su_projection, su_transform, su_color, su_size, su_span,
             su_hole, su_profile, su_use_uv;
static GLuint shadow_texture;
static int shadow_texture_radius;


/* The rectangle currently being repainted, in root coordinates.
 * Everything drawn is scissored to it (and to whatever else it is
 * already clipped by), so one pass over the scene per rectangle covers
 * exactly the damage and nothing else. */
static CompRect repaint_rect;

/* The output being painted, from gl_begin() to gl_end(): the projection
 * and the scissor boxes need to know which way up its target is. */
static const GlOutput *frame_target;

static GlWindow *windows;

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
    /* A second coordinate, used only for a mesh (scene.h): there the
     * position is a root-coordinate vertex, nowhere near 0..1, so the
     * texture coordinate cannot be derived from it as the quad path
     * does. `use_uv` picks between the two. */
    "attribute vec2 uv;\n"
    "uniform mat4 projection;\n"
    "uniform mat4 transform;\n"
    "uniform float y_flip;\n"
    "uniform float use_uv;\n"
    "varying vec2 texcoord;\n"
    "varying vec2 maskcoord;\n"
    "void main() {\n"
    "    vec2 uvc = mix(position, uv, use_uv);\n"
    "    texcoord = vec2(uvc.x,\n"
    "                    mix(uvc.y, 1.0 - uvc.y, y_flip));\n"
    /* The mask is built the way X measures a window -- y downwards --
     * so it is sampled by the quad's own coordinate and never by the
     * flipped one, whatever way up this driver hands over the pixmap.
     * For a mesh that coordinate is the vertex's place in the grid,
     * which is its place in the window: the silhouette then bends with
     * the window instead of being cut out of the screen. */
    "    maskcoord = uvc;\n"
    "    gl_Position = projection * transform * vec4(position, 0.0, 1.0);\n"
    "}\n";

static const char *fragment_source =
    "#version 120\n"
    "uniform sampler2D texture0;\n"
    "uniform sampler2D mask;\n"
    "uniform float use_mask;\n"
    "uniform float opacity;\n"
    "varying vec2 texcoord;\n"
    "varying vec2 maskcoord;\n"
    "void main() {\n"
    "    vec4 c = texture2D(texture0, texcoord);\n"
    /* Premultiplied, so the silhouette multiplies the whole texel --
     * colour and alpha together -- rather than the alpha alone. */
    "    c *= mix(1.0, texture2D(mask, maskcoord).a, use_mask);\n"
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
    /* For a bent shadow (draw_shadow_mesh): the vertex's place inside the
     * shadow's own rectangle, which is what the profile below is measured
     * in. The position is then free to be anywhere -- that is what lets a
     * blurred rectangle be painted onto a sheet that is not one. */
    "attribute vec2 uv;\n"
    "uniform mat4 projection;\n"
    "uniform mat4 transform;\n"
    "uniform vec2 size;\n"
    "uniform float use_uv;\n"
    "varying vec2 local;\n"
    "void main() {\n"
    "    local = mix(position, uv, use_uv) * size;\n"
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
    glBindAttribLocation(program, 1, "uv");
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
    u_mask = glGetUniformLocation(program, "mask");
    u_use_mask = glGetUniformLocation(program, "use_mask");
    u_use_uv = glGetUniformLocation(program, "use_uv");

    /* One unit quad, reused for every window: the transform is what makes
     * it the right size in the right place, which is the same thing the
     * scene node already says. */
    static const float quad[] = { 0, 0, 1, 0, 0, 1, 1, 1 };
    glGenBuffers(1, &quad_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, quad_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);

    /* Filled per frame for a mesh node (draw_mesh_node): interleaved
     * x,y (root coordinates) and u,v (place in the grid). */
    glGenBuffers(1, &mesh_vbo);

    /* The shadow program is optional in the sense that failing to build
     * it costs shadows, not the session: everything else still draws. */
    vs = compile(GL_VERTEX_SHADER, shadow_vertex_source, "shadow vertex");
    fs = compile(GL_FRAGMENT_SHADER, shadow_fragment_source, "shadow fragment");
    if (vs && fs) {
        shadow_program = glCreateProgram();
        glAttachShader(shadow_program, vs);
        glAttachShader(shadow_program, fs);
        glBindAttribLocation(shadow_program, 0, "position");
        glBindAttribLocation(shadow_program, 1, "uv");
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
            su_use_uv = glGetUniformLocation(shadow_program, "use_uv");
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


/* ------------------------------------------------------------------ */
/* windows                                                             */
/* ------------------------------------------------------------------ */

static GlWindow *gl_window_find(xcb_window_t id)
{
    for (GlWindow *g = windows; g; g = g->next)
        if (g->id == id)
            return g;
    return NULL;
}

static GlWindow *gl_window_get(CompWindow *w)
{
    GlWindow *g = gl_window_find(w->id);
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

/* The texture a platform imports a window into, made on first use with
 * the sampling every window wants. */
GLuint gl_window_texture(GlWindow *g)
{
    if (!g->texture) {
        glGenTextures(1, &g->texture);
        glBindTexture(GL_TEXTURE_2D, g->texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    return g->texture;
}

static void gl_shape_forget(GlWindow *g)
{
    if (g->shape_mask) {
        glDeleteTextures(1, &g->shape_mask);
        g->shape_mask = 0;
    }
    g->shape_mask_tried = false;

    free(g->shape_rects);
    g->shape_rects = NULL;
    g->shape_count = 0;
    g->shape_known = false;
}

/* ---- the stash: contents a resize replaced (renderer-gl.h) ---- */

static void gl_stash_free(GlWindow *g)
{
    if (!g->stash_platform && !g->stash_texture)
        return;

    /* The platform releases what it named through a GlWindow, so the
     * stash is handed to it as one: its own platform half and the texture
     * that half is bound into, and nothing else of this window's. */
    if (g->stash_platform) {
        GlWindow tmp;
        memset(&tmp, 0, sizeof(tmp));
        tmp.id = g->id;
        tmp.platform = g->stash_platform;
        tmp.texture = g->stash_texture;
        platform->window_unbind(&tmp);
        free(g->stash_platform);
        g->stash_platform = NULL;
    }
    if (g->stash_texture) {
        glDeleteTextures(1, &g->stash_texture);
        g->stash_texture = 0;
    }
    g->stash_holds = 0;
}

void gl_window_stash(CompWindow *w, const CompRect *was)
{
    GlWindow *g = gl_window_find(w->id);

    /* Nothing bound to stash. Whatever was there is still the most recent
     * thing this window ever looked like, so leave it. */
    if (!g || !g->platform || !g->content)
        return;

    /* An effect is drawing the stash right now: it keeps it. Replacing it
     * here would delete the texture mid-animation, and the node would
     * quietly fall back to the window's *live* contents while still being
     * drawn at the size the old ones were -- which for a shade is the
     * collapsed titlebar stretched over the whole window. A window that
     * resizes twice on its way into a shade (Qt sends more configures
     * than X clients usually do) is exactly how that happens, and the
     * first stash is the one the effect wanted anyway. */
    if (g->stash_holds > 0)
        return;

    /* Otherwise one stash at a time: nothing is holding the old one, and
     * the newest contents are the ones worth keeping. */
    gl_stash_free(g);

    /* Moved whole, not copied: the texture stays bound to the platform's
     * image, which stays bound to the pixmap we named -- and a named
     * pixmap is ours until we free it, whatever the window does next.
     * That is the entire trick, and why there is nothing to read back. */
    g->stash_platform = g->platform;
    g->stash_texture = g->texture;
    g->stash_y_inverted = g->y_inverted;
    /* The rectangle those pixels covered -- the *old* one. The window's
     * geometry is already the new size by the time this runs, which is
     * the trap: stashing window_rect(w) here records the size the
     * contents are not. */
    g->stash_rect = *was;
    g->stash_holds = 0;

    /* The live half starts over: the next bind names the new pixmap into
     * a new texture. */
    g->platform = NULL;
    g->texture = 0;
    g->content = false;
    g->width = g->height = 0;

    /* The silhouette belongs to the size that just changed. */
    gl_shape_forget(g);
}

bool gl_window_has_stash(const CompWindow *w)
{
    GlWindow *g = gl_window_find(w->id);
    return g && g->stash_platform != NULL;
}

CompRect gl_window_stash_rect(const CompWindow *w)
{
    GlWindow *g = gl_window_find(w->id);
    return g ? g->stash_rect : (CompRect){ 0, 0, 0, 0 };
}

void gl_stash_hold(CompWindow *w)
{
    GlWindow *g = gl_window_find(w->id);
    if (g)
        g->stash_holds++;
}

void gl_stash_release(CompWindow *w)
{
    GlWindow *g = gl_window_find(w->id);
    if (!g)
        return;
    if (g->stash_holds > 0)
        g->stash_holds--;
    if (g->stash_holds == 0)
        gl_stash_free(g);
}

void gl_stash_drop_unheld(CompWindow *w)
{
    GlWindow *g = gl_window_find(w->id);
    if (g && g->stash_holds == 0)
        gl_stash_free(g);
}

void gl_window_invalidate(CompWindow *w)
{
    GlWindow *g = gl_window_find(w->id);
    if (g) {
        platform->window_unbind(g);
        gl_shape_forget(g);
    }
}

void gl_window_shape_invalidate(CompWindow *w)
{
    GlWindow *g = gl_window_find(w->id);
    if (g)
        gl_shape_forget(g);
}

void gl_window_free(CompWindow *w)
{
    GlWindow **pp = &windows;
    while (*pp) {
        GlWindow *g = *pp;
        if (g->id != w->id) {
            pp = &g->next;
            continue;
        }
        *pp = g->next;
        platform->window_unbind(g);
        free(g->platform);
        if (g->texture)
            glDeleteTextures(1, &g->texture);
        /* Whatever an effect was still holding goes with it: the window
         * is gone, and so is anything that was drawing it. */
        gl_stash_free(g);
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

    /* y grows downwards in X, upwards in GL -- unless the target's rows
     * are stored top-first, in which case GL's "up" is X's down and the
     * flip is not wanted. */
    float dir = frame_target && frame_target->y_down ? 1.0f : -1.0f;

    memset(m, 0, sizeof(float) * 16);
    m[0] = 2.0f * k / w;
    m[5] = dir * 2.0f * k / h;
    m[10] = 1.0f;
    m[12] = -1.0f + 2.0f * (ox - (float)o->rect.x) / w;
    m[13] = -dir * (1.0f - 2.0f * (oy - (float)o->rect.y) / h);
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
static void shape_fetch(CompWindow *w, GlWindow *g);

static void shape_fetch(CompWindow *w, GlWindow *g)
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

    if (frame_target && frame_target->y_down)
        glScissor(px, py, pw, ph);
    else
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
                            const GlWindow *g, int dx, int dy, CompRect *out)
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
                        CompWindow *w, const GlWindow *g,
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
 * The buffer age answers the one question that makes partial repainting
 * safe: how many presentations ago this buffer was last shown. Age 1 is
 * the frame before last; age n means the union of the last n frames'
 * damage is exactly what has changed since it was current. Age 0 means
 * the platform is not saying -- a resized drawable, a fresh one, a
 * driver that discards, no GLX_EXT_buffer_age at all -- and the honest
 * answer to that is to redraw everything.
 *
 * Getting this wrong doesn't look like a small mistake: it is the black
 * and red flashing and the misplaced pieces of window that the first
 * version of this file had, because the parts nobody repainted were
 * showing a frame from two swaps ago or memory nobody had written. */
static void repaint_region_for(GlOutput *go, const CompRegion *damage,
                               unsigned age)
{
    if (region_is_full(damage)) {
        region_set_full(&go->repaint);
        return;
    }

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

/* The platform has made this output's target current; `age` is how many
 * presentations old the buffer is (0: unknown, and the frame is drawn
 * whole). */
void gl_begin(CompOutput *o, GlOutput *go, const CompRegion *damage,
              unsigned age)
{
    frame_target = go;
    glViewport(0, 0, o->physical.w, o->physical.h);

    repaint_region_for(go, damage, age);
    frame_damage_of(o, damage, &go->frame_damage);

    glClearColor(0.109f, 0.109f, 0.109f, 1.0f);

    if (region_is_full(&go->repaint) && o->covered.count == 0) {
        glDisable(GL_SCISSOR_TEST);
        glClear(GL_COLOR_BUFFER_BIT);
        return;
    }

    /* Only the ground that is about to be redrawn. Clearing the whole
     * buffer here would throw away precisely the pixels the buffer age
     * just told us are still good.
     *
     * And not under an opaque window (CompOutput's covered): the window
     * replaces those pixels a moment later, so clearing them first is a
     * pass over the buffer for nothing. */
    CompRegion ground = go->repaint;
    region_intersect_rect(&ground, &o->rect);
    for (int i = 0; i < o->covered.count; i++)
        region_subtract_rect(&ground, &o->covered.rects[i]);

    glEnable(GL_SCISSOR_TEST);
    for (int i = 0; i < ground.count; i++) {
        const CompRect *r = &ground.rects[i];
        scissor_for(o, r->x, r->y, r->w, r->h);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    glDisable(GL_SCISSOR_TEST);
}

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
    glUniform1f(u_use_mask, 0.0f);
    glUniformMatrix4fv(u_projection, 1, GL_FALSE, projection);
    glUniform1i(u_texture, 0);
    glUniform1f(u_y_flip, 0.0f);
    glUniformMatrix4fv(u_transform, 1, GL_FALSE, m);
    glUniform1f(u_opacity, b->opacity);

    scissor_for(o, repaint_rect.x, repaint_rect.y,
                repaint_rect.w, repaint_rect.h);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

/* One pass over the scene, everything clipped to `repaint_rect`. Called
 * once per damaged rectangle, so a frame where two small things changed
 * costs two small passes instead of one screen-sized one. */
/* The bound quad through one scissor rectangle -- split around the
 * window's opaque rectangle when there is one: what falls inside it is
 * drawn with blending off, the rest (up to four rectangles) blended.
 * Each part is still intersected with repaint_rect by scissor_to(). */
static void draw_piece(const CompOutput *o, const CompRect *piece,
                       const CompRect *opaque)
{
    CompRect solid;
    if (opaque->w <= 0 || opaque->h <= 0 ||
        !rect_intersect(piece, opaque, &solid)) {
        if (scissor_to(o, piece))
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        return;
    }

    if (scissor_to(o, &solid)) {
        glDisable(GL_BLEND);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glEnable(GL_BLEND);
    }

    CompRegion rest;
    region_clear(&rest);
    region_add(&rest, piece);
    region_subtract_rect(&rest, &solid);
    for (int k = 0; k < rest.count; k++)
        if (scissor_to(o, &rest.rects[k]))
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

/* The window's silhouette as an alpha texture, in the pixmap's own
 * coordinates, made on first use and kept with the shape rectangles.
 *
 * Only ever needed by a node something is turning or scaling: a scissor
 * box is an axis-aligned rectangle in screen pixels, so it can follow a
 * window being slid but not one being rotated, and the shape has to
 * travel *with* the pixels instead of around them. Sampled through the
 * quad's own coordinate, which is the same space the rectangles are
 * measured in once the border is accounted for -- the pixmap starts at
 * (x - border, y - border) and a shape rectangle at (x, y).
 *
 * 0 when there is nothing to mask with, which the caller reads as "draw
 * it square", the same answer it had before this existed. */
static GLuint shape_mask_texture(CompWindow *w, GlWindow *g)
{
    if (g->shape_mask_tried)
        return g->shape_mask;
    g->shape_mask_tried = true;

    if (!w->shaped || g->shape_count <= 0)
        return 0;

    CompRect wr = window_rect(w);
    if (wr.w <= 0 || wr.h <= 0 || wr.w > 8192 || wr.h > 8192)
        return 0;

    unsigned char *bits = calloc((size_t)wr.w * (size_t)wr.h, 1);
    if (!bits)
        return 0;

    for (int i = 0; i < g->shape_count; i++) {
        const xcb_rectangle_t *sr = &g->shape_rects[i];
        int x0 = sr->x + w->border;
        int y0 = sr->y + w->border;
        int x1 = x0 + (int)sr->width;
        int y1 = y0 + (int)sr->height;

        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > wr.w) x1 = wr.w;
        if (y1 > wr.h) y1 = wr.h;

        for (int y = y0; y < y1; y++)
            memset(bits + (size_t)y * (size_t)wr.w + x0, 0xff, (size_t)(x1 - x0));
    }

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, wr.w, wr.h, 0,
                 GL_ALPHA, GL_UNSIGNED_BYTE, bits);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    free(bits);
    g->shape_mask = tex;
    return tex;
}

/* A rectangle measured in the window's own coordinates -- a shape
 * rectangle, the shape's extents -- placed where the node is actually
 * being drawn.
 *
 * The two are not the same place. A shape is measured against the
 * window's real rectangle, which is where the window *is*; the node's
 * geometry is where it is being *shown*, and an effect is free to make
 * that somewhere else and a different size (an expo cell, a cover in a
 * row). Reading the shape at the window's real position and then
 * applying the node's transform to it asks the transform a question
 * about a rectangle it was never built to move, and the scissor that
 * comes out has nothing to do with the node. */
static CompRect in_node_space(const CompSceneNode *n, const CompWindow *w,
                              int rx, int ry, int rw, int rh)
{
    CompRect wr = window_rect(w);
    if (wr.w <= 0 || wr.h <= 0)
        return (CompRect){ n->geometry.x + rx, n->geometry.y + ry, rw, rh };

    float sx = (float)n->geometry.w / (float)wr.w;
    float sy = (float)n->geometry.h / (float)wr.h;

    return (CompRect){
        n->geometry.x + (int)((float)rx * sx),
        n->geometry.y + (int)((float)ry * sy),
        (int)((float)rw * sx + 0.5f),
        (int)((float)rh * sy + 0.5f),
    };
}

/* A point of a mesh at (u, v), where 0..1 spans the window -- bilinear
 * inside, and *extrapolated* outside, which is the whole reason this
 * exists: the shadow's rectangle reaches past the window by the blur
 * radius, so bending it means asking the mesh where it would be a little
 * beyond its own edge. Clamping the cell and letting the weights run past
 * 0..1 continues the boundary cell's own slope, so the shadow leaves the
 * window's edge in the direction that edge is actually leaning. */
static void mesh_sample(const CompSceneMesh *mesh, float u, float v,
                        float *out_x, float *out_y)
{
    int cols = mesh->cols, rows = mesh->rows;

    float fu = u * (float)cols;
    float fv = v * (float)rows;

    int i0 = (int)floorf(fu);
    int j0 = (int)floorf(fv);
    if (i0 < 0) i0 = 0;
    if (i0 > cols - 1) i0 = cols - 1;
    if (j0 < 0) j0 = 0;
    if (j0 > rows - 1) j0 = rows - 1;

    float tu = fu - (float)i0;
    float tv = fv - (float)j0;

    int stride = cols + 1;
    int a = j0 * stride + i0;
    int b = a + 1;
    int c = a + stride;
    int d = c + 1;

    float top_x = mesh->x[a] + (mesh->x[b] - mesh->x[a]) * tu;
    float top_y = mesh->y[a] + (mesh->y[b] - mesh->y[a]) * tu;
    float bot_x = mesh->x[c] + (mesh->x[d] - mesh->x[c]) * tu;
    float bot_y = mesh->y[c] + (mesh->y[d] - mesh->y[c]) * tu;

    *out_x = top_x + (bot_x - top_x) * tv;
    *out_y = top_y + (bot_y - top_y) * tv;
}

/* The shadow of a window that is not a rectangle any more.
 *
 * A shadow is a blurred rectangle and cannot be bent by its own shader --
 * but it does not have to be. The blur profile and the hole are measured
 * in the shadow rectangle's own coordinates, which the vertex shader now
 * takes as a per-vertex `uv` (shadow_vertex_source), so the rectangle can
 * be *painted onto* whatever geometry it is given. Here that geometry is
 * the window's own mesh, sampled over the part of itself the shadow box
 * covers -- past its edges included (mesh_sample) -- so the shadow leans
 * and curves with the window instead of sitting under it as a rectangle
 * the bend has left behind. */
static void draw_shadow_mesh(const CompOutput *o, const CompSceneNode *n,
                             CompWindow *w, const float projection[16])
{
    const CompSceneMesh *mesh = n->mesh;

    CompShadowStyle st;
    if (!shadow_program || !shadow_for_window(w, &st))
        return;
    if (st.opacity <= 0.0f || st.radius <= 0)
        return;

    int r = st.radius;
    if (!shadow_profile_texture(r))
        return;

    /* The window's rectangle as the mesh's 0..1 stands for, and the
     * shadow's box around it -- the same box the rectangular path builds,
     * so a window that stops bending keeps the shadow it had. */
    CompRect base = n->geometry;
    if (base.w <= 0 || base.h <= 0)
        return;
    CompRect box = { base.x + st.offset_x - r, base.y + st.offset_y - r,
                     base.w + r * 2, base.h + r * 2 };

    int cols = mesh->cols, rows = mesh->rows;
    static float verts[MESH_MAX_COLS * MESH_MAX_ROWS * 6 * 4];
    int v = 0;

    for (int gy = 0; gy < rows; gy++) {
        for (int gx = 0; gx < cols; gx++) {
            /* Each corner twice over: where it is in the shadow's own
             * rectangle (for the profile) and where the window's mesh
             * puts that place (for the screen). */
            const float bu[4] = { (float)gx / cols, (float)(gx + 1) / cols,
                                  (float)gx / cols, (float)(gx + 1) / cols };
            const float bv[4] = { (float)gy / rows, (float)gy / rows,
                                  (float)(gy + 1) / rows, (float)(gy + 1) / rows };
            float px[4], py[4];
            for (int k = 0; k < 4; k++) {
                /* The shadow box point, expressed in the window's own
                 * 0..1 -- outside it wherever the box reaches past. */
                float mu = ((float)box.x + bu[k] * (float)box.w - (float)base.x)
                           / (float)base.w;
                float mv = ((float)box.y + bv[k] * (float)box.h - (float)base.y)
                           / (float)base.h;
                mesh_sample(mesh, mu, mv, &px[k], &py[k]);
            }

            const int idx[6] = { 0, 1, 2, 1, 3, 2 };
            for (int t = 0; t < 6; t++) {
                int k = idx[t];
                verts[v++] = px[k];
                verts[v++] = py[k];
                verts[v++] = bu[k];
                verts[v++] = bv[k];
            }
        }
    }

    float identity[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };

    glUseProgram(shadow_program);
    glUniformMatrix4fv(su_projection, 1, GL_FALSE, projection);
    glUniformMatrix4fv(su_transform, 1, GL_FALSE, identity);
    glUniform4f(su_color, st.r, st.g, st.b, st.opacity * n->opacity);
    glUniform2f(su_size, (float)box.w, (float)box.h);
    glUniform1f(su_span, (float)(r * 2));
    glUniform1i(su_profile, 0);
    glUniform1f(su_use_uv, 1.0f);
    /* The window's own place in the box, so the shadow is not drawn
     * behind it -- in the box's coordinates, which the bend carries. */
    glUniform4f(su_hole, (float)(base.x - box.x), (float)(base.y - box.y),
                (float)base.w, (float)base.h);
    glBindTexture(GL_TEXTURE_2D, shadow_texture);

    glBindBuffer(GL_ARRAY_BUFFER, mesh_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)v * (GLsizeiptr)sizeof(float),
                 verts, GL_STREAM_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), NULL);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          (const void *)(2 * sizeof(float)));

    scissor_for(o, repaint_rect.x, repaint_rect.y, repaint_rect.w, repaint_rect.h);
    glDrawArrays(GL_TRIANGLES, 0, v / 4);

    glUniform1f(su_use_uv, 0.0f);
}

/* A window handed over as a deformed grid (scene.h): two triangles per
 * cell, the vertices already in root coordinates so the transform is the
 * identity and the projection alone puts them on screen, and a texture
 * coordinate per vertex so the window's pixmap follows the bend. This is
 * the shape a matrix cannot say -- the magic lamp's genie neck, a wobbling
 * window's sheet -- and the one thing the quad path below cannot draw.
 *
 * The silhouette comes along: kiwm rounds every frame it draws, and the
 * mask is sampled by the vertex's place in the grid, so the corners stay
 * round and round *with* the bend rather than being cut out of the screen
 * where the window used to be.
 *
 * The shadow comes along only when the mesh asks for it (scene.h), and it
 * bends with the window rather than staying the rectangle underneath --
 * see draw_shadow_mesh. */
static void draw_mesh_node(CompOutput *o, CompSceneNode *n, CompWindow *w,
                           GlWindow *g, const float projection[16])
{
    const CompSceneMesh *mesh = n->mesh;
    int cols = mesh->cols, rows = mesh->rows;
    if (cols < 1 || rows < 1 || cols > MESH_MAX_COLS || rows > MESH_MAX_ROWS)
        return;

    /* Under the window, and before its texture is bound: the shadow
     * program has its own idea of what is in texture unit 0. */
    if (mesh->shadow)
        draw_shadow_mesh(o, n, w, projection);

    glUseProgram(program);
    glUniformMatrix4fv(u_projection, 1, GL_FALSE, projection);
    glUniform1i(u_texture, 0);

    if (!platform->window_bind(w, g))
        return;

    static float verts[MESH_MAX_COLS * MESH_MAX_ROWS * 6 * 4];
    int v = 0;
    for (int gy = 0; gy < rows; gy++) {
        for (int gx = 0; gx < cols; gx++) {
            int i00 = gy * (cols + 1) + gx;
            int i10 = i00 + 1;
            int i01 = i00 + (cols + 1);
            int i11 = i01 + 1;

            float u0 = (float)gx / (float)cols;
            float u1 = (float)(gx + 1) / (float)cols;
            float t0 = (float)gy / (float)rows;
            float t1 = (float)(gy + 1) / (float)rows;

            const int idx[6] = { i00, i10, i01, i10, i11, i01 };
            const float us[6]  = { u0, u1, u0, u1, u1, u0 };
            const float ts[6]  = { t0, t0, t1, t0, t1, t1 };
            for (int k = 0; k < 6; k++) {
                verts[v++] = mesh->x[idx[k]];
                verts[v++] = mesh->y[idx[k]];
                verts[v++] = us[k];
                verts[v++] = ts[k];
            }
        }
    }

    float identity[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    glUniformMatrix4fv(u_transform, 1, GL_FALSE, identity);
    glUniform1f(u_opacity, n->opacity);
    glUniform1f(u_y_flip, g->y_inverted ? 0.0f : 1.0f);
    glUniform1f(u_use_uv, 1.0f);

    /* The silhouette, worn as a mask: a scissor box cannot follow a bend,
     * and every frame kiwm draws has rounded corners to keep. */
    GLuint shape = w->shaped ? shape_mask_texture(w, g) : 0;
    if (shape) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, shape);
        glActiveTexture(GL_TEXTURE0);
        glUniform1i(u_mask, 1);
        glUniform1f(u_use_mask, 1.0f);
    } else {
        glUniform1f(u_use_mask, 0.0f);
    }

    glBindBuffer(GL_ARRAY_BUFFER, mesh_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)v * (GLsizeiptr)sizeof(float),
                 verts, GL_STREAM_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), NULL);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          (const void *)(2 * sizeof(float)));

    /* Every cell is inside the mesh's bounding box, so one scissor to
     * what this pass repaints is all the clipping there is. */
    scissor_for(o, repaint_rect.x, repaint_rect.y, repaint_rect.w, repaint_rect.h);
    glDrawArrays(GL_TRIANGLES, 0, v / 4);

    /* Back to the plain quad for whatever node is drawn next. */
    glDisableVertexAttribArray(1);
    glUniform1f(u_use_uv, 0.0f);
    glBindBuffer(GL_ARRAY_BUFFER, quad_vbo);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);
}

/* One node, inside repaint_rect -- which by the time this runs is one
 * piece of the node's own clip (scene.h) ∩ one damaged rectangle, so
 * every scissor box below is already inside both. */
static void draw_node(CompOutput *o, CompSceneNode *n, CompWindow *w,
                      GlWindow *g, const float projection[16])
{
    /* The shape first: the shadow is cast around the window's
     * silhouette, and both of them are about to want it. */
    shape_fetch(w, g);

    /* A deformed grid is its own path: no shadow, no shape mask, every
     * vertex already placed (scene.h). */
    if (n->mesh) {
        draw_mesh_node(o, n, w, g, projection);
        return;
    }

    /* Under the window, and before its texture is bound: the shadow
     * program has its own idea of what is in texture unit 0. */
    draw_shadow(o, n, w, g, projection);
    glUseProgram(program);
    glUniformMatrix4fv(u_projection, 1, GL_FALSE, projection);
    glUniform1i(u_texture, 0);

    /* An effect drawing what the window looked like before its last
     * resize (shade, rolling a window up behind its own titlebar). The
     * node's geometry describes those contents, not the window's current
     * ones -- and the texture is the one set aside with them, still bound
     * to the pixmap that was named then, so there is nothing to bind and
     * nothing to read back. */
    bool from_stash = n->use_stash && g->stash_platform && g->stash_texture;

    if (from_stash) {
        glBindTexture(GL_TEXTURE_2D, g->stash_texture);
    } else if (!platform->window_bind(w, g)) {
        return;
    }

    /* The window itself reaches no further than the area the scene says
     * it covers. For an ordinary window that changes nothing -- the quad
     * is its rectangle and `clip` is that rectangle grown by the reach of
     * a shadow that has already been drawn. It is what makes a *crop*
     * work: shade shortens visible_rect and nothing else, and the picture
     * has to stop where it stops rather than spilling into the shadow's
     * margin. (XRender composites exactly this rectangle, which is why
     * the same effect has always cropped cleanly there.) */
    CompRect full_repaint = repaint_rect;
    CompRect body;
    if (!rect_intersect(&full_repaint, &n->visible_rect, &body))
        return;
    repaint_rect = body;

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
    glUniform1f(u_y_flip, (from_stash ? g->stash_y_inverted : g->y_inverted)
                              ? 0.0f : 1.0f);

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

    /* The part of the window known to be opaque (window.h's opaque: the
     * client inside the frame, when it has no alpha and no shape) is
     * drawn with blending off, and only the rest -- the frame around it,
     * translucent under kiwm -- is blended. Blending reads the pixels it
     * is about to replace; on a video that is a full pass over the
     * client's area a frame, read for nothing. Only while the window is
     * where it says it is: the rectangle is in screen pixels. */
    CompRect opaque = { 0, 0, 0, 0 };
    if (!from_stash && comp_transform_is_identity(&n->transform) &&
        n->opacity >= 1.0f)
        opaque = window_opaque_rect(w);

    /* A node being turned or scaled cannot keep its silhouette as
     * scissor boxes, so it wears it as a mask instead -- which is what
     * makes an expo cell or a cover in a row keep the rounded corners
     * kiwm gave it. Built on the first frame that needs it, and only
     * ever for such a node: the ordinary case below stays exactly as
     * cheap as it was. */
    /* Never for the stash: every silhouette this window has -- the
     * rectangles and the mask alike -- describes the size it is now, and
     * these contents are the size it was. Cutting the old picture with
     * the new shape is how a rolled-up window ends up with a bite taken
     * out of it. The crop above is the only clip a stash needs. */
    GLuint mask = (!from_stash && !move_only && w->shaped)
                      ? shape_mask_texture(w, g) : 0;
    if (mask) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, mask);
        glActiveTexture(GL_TEXTURE0);
        glUniform1i(u_mask, 1);
        glUniform1f(u_use_mask, 1.0f);
    } else {
        glUniform1f(u_use_mask, 0.0f);
    }

    if (mask) {
        /* The mask cuts the silhouette, so the only clip left is the
         * part of the screen this pass is repainting. */
        draw_piece(o, &repaint_rect, &opaque);
    } else if (!from_stash && g->shape_count > 0 && move_only) {
        for (int k = 0; k < g->shape_count; k++) {
            const xcb_rectangle_t *sr = &g->shape_rects[k];
            CompRect piece = in_node_space(n, w, sr->x, sr->y,
                                           sr->width, sr->height);
            piece.x += (int)tdx;
            piece.y += (int)tdy;
            draw_piece(o, &piece, &opaque);
        }
    } else if (!from_stash && !move_only && w->shaped &&
               w->shape_extents.w > 0 && w->shape_extents.h > 0) {
        /* Being scaled, so the silhouette cannot come along -- a
         * scissor box lives in screen pixels. Its *extents* can,
         * carried through the same transform, and for the window this
         * matters to they are nothing like its rectangle:
         * VirtualBox's mini-toolbar is a screen-sized window with a
         * small bar shaped out of it, and drawing the rectangle while
         * an effect shrinks it puts a screen-sized ghost of stale
         * contents in the middle of the grid. */
        CompRect ext = in_node_space(n, w, w->shape_extents.x,
                                     w->shape_extents.y,
                                     w->shape_extents.w, w->shape_extents.h);
        CompRect moved = comp_transform_rect(&n->transform, &ext);
        if (scissor_to(o, &moved))
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    } else {
        /* Unshaped, or a stash: the damage rectangle is the whole clip. */
        draw_piece(o, &repaint_rect, &opaque);
    }

    repaint_rect = full_repaint;
}

/* One coloured quad (scene.h's CompSceneSolid). The same one-texel
 * texture the backdrop uses -- a flat colour is a texel, and going
 * through the window shader means a quad and a window are drawn by the
 * same code, so a cube's face and the windows above it cannot disagree
 * about perspective. */
static void draw_solid(const CompOutput *o, const CompSceneSolid *q,
                       const float projection[16])
{
    CompRect box;
    comp_transform_bbox(&q->transform, &q->rect, &box);
    CompRect ignored;
    if (!rect_intersect(&box, &repaint_rect, &ignored))
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
        (unsigned char)(q->r * 255.0f + 0.5f),
        (unsigned char)(q->g * 255.0f + 0.5f),
        (unsigned char)(q->b * 255.0f + 0.5f),
        255,
    };
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, px);

    float m[16];
    rect_matrix(&q->rect, &q->transform, m);

    glUseProgram(program);
    glUniform1f(u_use_mask, 0.0f);
    glUniformMatrix4fv(u_projection, 1, GL_FALSE, projection);
    glUniform1i(u_texture, 0);
    glUniform1f(u_y_flip, 0.0f);
    glUniformMatrix4fv(u_transform, 1, GL_FALSE, m);
    glUniform1f(u_opacity, q->opacity);

    scissor_for(o, repaint_rect.x, repaint_rect.y,
                repaint_rect.w, repaint_rect.h);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static void draw_pass(CompOutput *o, CompScene *s, const float projection[16])
{
    draw_backdrop(o, s, projection);

    /* The rectangle this pass is repainting; each node narrows it to the
     * pieces of itself that are not under an opaque window, and puts it
     * back afterwards. */
    CompRect damaged = repaint_rect;

    /* Solids and nodes in one walk: a solid's z says where among the
     * nodes it is drawn (scene.h), which is what lets a cube's near face
     * come out in front of the windows floating above its far one. */
    int si = 0;

    for (int i = 0; i < s->count; i++) {
        CompSceneNode *n = &s->nodes[i];
        CompWindow *w = n->win;

        while (si < s->solid_count && s->solids[si].z <= (float)i)
            draw_solid(o, &s->solids[si++], projection);


        if (n->visible_rect.w <= 0 || n->visible_rect.h <= 0)
            continue;

        GlWindow *g = gl_window_get(w);
        if (!g)
            continue;

        /* Only where the node shows *and* the pass is repainting. This
         * test is where the saving actually comes from: the scissor
         * would clip it anyway, but binding a pixmap and issuing draws
         * for a window nobody can see is the work worth not doing -- and
         * a maximized window under another one is drawn nowhere at all
         * for a frame whose damage is all inside the one on top. */
        if (region_is_full(&n->clip)) {
            draw_node(o, n, w, g, projection);
            continue;
        }
        for (int k = 0; k < n->clip.count; k++) {
            CompRect piece;
            if (!rect_intersect(&n->clip.rects[k], &damaged, &piece))
                continue;
            repaint_rect = piece;
            draw_node(o, n, w, g, projection);
        }
        repaint_rect = damaged;
    }

    while (si < s->solid_count)
        draw_solid(o, &s->solids[si++], projection);
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
    glUniform1f(u_use_mask, 0.0f);
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

/* Drawn through the region gl_begin() worked out from the buffer age,
 * not the raw damage: what this buffer is missing is the damage of every
 * frame since it was last shown. */
void gl_draw_scene(CompOutput *o, GlOutput *go, CompScene *s)
{
    if (!program)
        return;

    float projection[16];
    projection_for(o, projection);

    glUseProgram(program);
    glUniformMatrix4fv(u_projection, 1, GL_FALSE, projection);
    glUniform1i(u_texture, 0);
    glUniform1f(u_use_mask, 0.0f);

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

void gl_end(GlOutput *go)
{
    glDisable(GL_SCISSOR_TEST);
    glFlush();
    frame_target = NULL;

    /* Remembered for the frames that will inherit this buffer: in a few
     * swaps' time it comes back as the back buffer, and what it is
     * missing then is everything drawn since -- starting with this. */
    for (int i = GL_AGE_HISTORY - 1; i > 0; i--)
        go->history[i] = go->history[i - 1];
    go->history[0] = go->frame_damage;
    if (go->history_len < GL_AGE_HISTORY)
        go->history_len++;
}

bool gl_window_has_content(const CompWindow *w)
{
    GlWindow *g = gl_window_find(w->id);
    return g && g->content;
}

bool gl_setup(const GlPlatform *p)
{
    platform = p;
    return program || program_build();
}

/* Everything GL-side, with the platform's context still current. The
 * platform frees its own half of each window first through the same
 * unbind it uses for a resize. */
void gl_teardown(void)
{
    while (windows) {
        GlWindow *g = windows;
        windows = g->next;
        if (platform)
            platform->window_unbind(g);
        free(g->platform);
        if (g->texture)
            glDeleteTextures(1, &g->texture);
        if (platform)
            gl_stash_free(g);
        free(g->shape_rects);
        free(g);
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
    if (mesh_vbo) {
        glDeleteBuffers(1, &mesh_vbo);
        mesh_vbo = 0;
    }
    platform = NULL;
}

