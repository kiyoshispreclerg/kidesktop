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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The platform's ops, from gl_setup(). */
static const GlPlatform *platform;


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


/* The rectangle currently being repainted, in root coordinates.
 * Everything drawn is scissored to it (and to whatever else it is
 * already clipped by), so one pass over the scene per rectangle covers
 * exactly the damage and nothing else. */
static CompRect repaint_rect;

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
    free(g->shape_rects);
    g->shape_rects = NULL;
    g->shape_count = 0;
    g->shape_known = false;
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
    glViewport(0, 0, o->physical.w, o->physical.h);

    repaint_region_for(go, damage, age);
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

/* One pass over the scene, everything clipped to `repaint_rect`. Called
 * once per damaged rectangle, so a frame where two small things changed
 * costs two small passes instead of one screen-sized one. */
static void draw_pass(CompOutput *o, CompScene *s, const float projection[16])
{
    draw_backdrop(o, s, projection);

    for (int i = 0; i < s->count; i++) {
        CompSceneNode *n = &s->nodes[i];
        CompWindow *w = n->win;

        if (n->visible_rect.w <= 0 || n->visible_rect.h <= 0)
            continue;

        GlWindow *g = gl_window_get(w);
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

        if (!platform->window_bind(w, g))
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
    platform = NULL;
}

