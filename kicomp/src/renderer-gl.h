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

    /* Row 0 of the target is its *top*: a texture-backed framebuffer,
     * where the first row of memory is the first row the server scans
     * out. A window's back buffer is the other way up (row 0 at the
     * bottom, GL's convention), and the projection and scissor boxes
     * are built for that unless the platform says otherwise here. */
    bool y_down;
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

    /* The same silhouette as an alpha texture, built only if something
     * ever draws this window turned or scaled -- a scissor box is a
     * screen-space rectangle and cannot follow a rotation, so that is
     * the one case where the shape has to travel with the pixels
     * instead of around them. Made once and kept with the rectangles;
     * 0 while it has not been needed, and again after a reshape. */
    GLuint shape_mask;
    bool shape_mask_tried;

    /* The platform's half (a GLXPixmap, an EGLImage): one malloc'd block
     * the platform allocates in window_bind and this side frees with the
     * window, after window_unbind has let go of what it names. */
    void *platform;

    /* The contents a resize replaced, set aside rather than freed
     * (renderer.h's stash) -- the platform half and the texture reading
     * it, moved here whole. What lets shade roll up a window whose live
     * pixmap has already collapsed to a titlebar: the pixels that have to
     * roll are gone from the window, but the pixmap we named is still
     * ours until we free it, and the texture is still bound to it.
     *
     * `stash_holds` is how many effects are drawing it; at zero the flush
     * drops it (gl_stash_drop_unheld), so a plain resize leaves nothing
     * behind. */
    void *stash_platform;
    GLuint stash_texture;
    CompRect stash_rect;        /* the rectangle those pixels covered */
    bool stash_y_inverted;
    int stash_holds;

    /* The silhouette those same pixels were cut to, captured alongside
     * them for the same reason: by the time an effect draws the stash,
     * `shape_rects` above has already moved on to the *new* geometry's
     * shape (gl_window_stash forgets it in the same breath it stashes
     * the pixmap), and clipping old contents to a new silhouette is how
     * a rolled-up window ends up with a bite taken out of it (see
     * draw_node's from_stash). A copy, not a reference: shape_rects is
     * freed and rebuilt in place on the next reshape, which for a
     * shading window is imminent. */
    xcb_rectangle_t *stash_shape_rects;
    int stash_shape_count;
    bool stash_shaped;

    /* X-DENSITY (density.h): the client's own denser pixels, one layer
     * for its contents ([0]) and one for the WM's decoration around them
     * ([1]), each a pixmap the client published rather than one this
     * side named. Held as a GlWindow of its own so the platform imports
     * it through the same texture machinery as the window itself, with
     * one difference the platform has to know about: the pixmap is the
     * client's, borrowed, never freed from here (GlPlatform::pixmap_bind).
     *
     * Not in the `windows` list -- reached only from here -- and its `id`
     * is the *pixmap's* XID, which is how a republished pixmap is told
     * from the one already imported. NULL until the client publishes
     * something; freed again when it withdraws it
     * (gl_window_density_invalidate) or the window goes. */
    struct GlWindow *dense[2];
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
    /* Bind a pixmap *somebody else* owns -- an X-DENSITY layer the client
     * published (GlWindow::dense) -- to gl_window_texture(g), setting
     * g->content, width/height, y_inverted the same as window_bind. The
     * size and depth are the pixmap's own, already asked of the server.
     * window_unbind lets go of it the same way, except that it must not
     * free the pixmap: it was never this side's to free. */
    bool (*pixmap_bind)(xcb_pixmap_t pixmap, int width, int height,
                        uint8_t depth, GlWindow *g);
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
void gl_window_density_invalidate(CompWindow *w, bool decoration);
void gl_window_free(CompWindow *w);
bool gl_window_has_content(const CompWindow *w);

/* The stash (renderer.h), the same for every platform: the window's
 * previous contents kept instead of thrown away when a resize replaces
 * them, so an effect can still draw what the window looked like. */
void gl_window_stash(CompWindow *w, const CompRect *was);
bool gl_window_has_stash(const CompWindow *w);
CompRect gl_window_stash_rect(const CompWindow *w);
void gl_stash_hold(CompWindow *w);
void gl_stash_release(CompWindow *w);
void gl_stash_drop_unheld(CompWindow *w);

#endif /* KICOMP_RENDERER_GL_H */
