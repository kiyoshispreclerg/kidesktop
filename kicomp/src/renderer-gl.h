/*
 * kicomp - the GL drawing shared by the GL platforms (renderer-gl.c).
 *
 * A platform (renderer-glx.c; renderer-egl.c next) owns the context and
 * everything that gets pixels *into* GL: the target per output and each
 * window's contents as a texture. This is what it gets in return -- the
 * scene drawn -- and what it has to provide, which is deliberately two
 * ops: bind a window's pixels to its texture, and let go of them.
 */
#ifndef KICOMP_RENDERER_GL_H
#define KICOMP_RENDERER_GL_H

#include "renderer.h"

#include <epoxy/gl.h>

/* How many past frames' damage to remember. A buffer this old or older
 * cannot be trusted at all, so anything beyond this is a full repaint --
 * and drivers in practice hand back ages of 1..3. */
#define GL_AGE_HISTORY 4

/* Per-output drawing state. The platform embeds one in whatever it hangs
 * off CompOutput::render_data and passes it to gl_begin/draw/end. */
typedef struct {
    /* What was repainted in each of the last few frames, newest first.
     * A buffer that is `age` presentations old already holds everything
     * except what has changed since -- so repainting the union of the
     * last `age` frames' damage brings it up to date, and repainting the
     * whole screen every frame to be safe is what this replaces. */
    CompRegion history[GL_AGE_HISTORY];
    int history_len;

    /* This frame's answer, kept from begin() to end(): the drawing draws
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
} GlOutput;

/* Per-window state: the texture the platform imports the window into,
 * and the silhouette the drawing clips it through. comp.h's
 * pixmap/picture pair is XRender's vocabulary, and a texture is not a
 * Picture, so this lives in a table of its own keyed by window id. */
typedef struct GlWindow {
    struct GlWindow *next;
    xcb_window_t id;

    GLuint texture;         /* gl_window_texture() makes it on first use */
    int width, height;      /* what the import covers; a resize re-imports */
    bool argb;
    bool content;           /* the platform has the window's pixels named */
    bool y_inverted;        /* the import's first texture row is the top one */

    /* The window's silhouette, in window-local pixels, as the scissor
     * rectangles the quad is drawn through. XRender hands its region to
     * the server and forgets about it; GL has no such thing, so the
     * rectangles are fetched once and kept until the shape changes --
     * kiwm reshapes a frame on every resize step, so fetching them per
     * frame would be a round trip in the middle of the paint. */
    xcb_rectangle_t *shape_rects;
    int shape_count;
    bool shape_known;

    /* The platform's half (a GLXPixmap, an EGLImage): one malloc'd block
     * the platform allocates in window_bind and this side frees with the
     * window, after window_unbind has let go of what it names. */
    void *platform;
} GlWindow;

typedef struct {
    /* Name the window's current contents and bind them to
     * gl_window_texture(g), setting g->content, g->width/height and
     * g->y_inverted. Called every frame the window is drawn, since on
     * most drivers rebinding is how new contents reach the texture;
     * false means the window can't be drawn this frame. */
    bool (*window_bind)(CompWindow *w, GlWindow *g);
    /* Let go of what window_bind named (resized, unmapped, going away),
     * leaving the texture itself. Must cope with never having bound. */
    void (*window_unbind)(GlWindow *g);
} GlPlatform;

/* With the platform's context current: builds the programs once. */
bool gl_setup(const GlPlatform *p);
/* With the context still current: every window's GL half, the programs. */
void gl_teardown(void);

/* One frame of one output, its target already current. `age` is how
 * many presentations old the buffer being drawn into is: 0 when unknown,
 * and the frame is drawn whole. */
void gl_begin(CompOutput *o, GlOutput *go, const CompRegion *damage,
              unsigned age);
void gl_draw_scene(CompOutput *o, GlOutput *go, CompScene *s);
void gl_end(GlOutput *go);

GLuint gl_window_texture(GlWindow *g);

/* The CompRenderer window ops, the same for every platform. */
void gl_window_invalidate(CompWindow *w);
void gl_window_shape_invalidate(CompWindow *w);
void gl_window_free(CompWindow *w);
bool gl_window_has_content(const CompWindow *w);

#endif /* KICOMP_RENDERER_GL_H */
