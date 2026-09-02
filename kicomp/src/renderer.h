/*
 * kicomp - renderer abstraction (section 28).
 *
 * The core knows only these five entry points. renderer-xrender.c is the
 * first (and so far only) backend; renderer-gl.c is Fase 6.
 */
#ifndef KICOMP_RENDERER_H
#define KICOMP_RENDERER_H

#include "comp.h"
#include "scene.h"

typedef struct CompRenderer {
    const char *name;

    bool (*init)(CompOutput *o);      /* create this output's target */
    void (*destroy)(CompOutput *o);

    /* `damage` is what changed since this output was last painted, in
     * root coordinates (region.h). A backend is free to ignore it and
     * repaint the whole output -- the result is identical, only slower --
     * but is expected to honour it: restrict the background fill to it,
     * skip windows it doesn't touch, and clip whatever it draws.
     *
     * The region is what the *core* tracked, and it stays backend-neutral
     * on purpose. XRender turns it into an XFixes region and clips the
     * target Picture; a GL backend would scissor with it. One caveat for
     * that backend: with a swapchain the buffer being drawn into is not
     * the one presented last frame, so it must widen this region by its
     * own buffer age -- the core cannot know how many frames back a given
     * buffer is. */
    void (*begin)(CompOutput *o, const CompRegion *damage);
    void (*draw_scene)(CompOutput *o, CompScene *s, const CompRegion *damage);
    void (*end)(CompOutput *o);
} CompRenderer;

const CompRenderer *renderer_xrender(void);

/* Chosen once at startup, in main.c. */
extern const CompRenderer *renderer;

/* Backend hooks the rest of the core calls without knowing the backend:
 *
 *   window_invalidate  the window's contents pixmap is stale (resized,
 *                      unmapped, about to go away) -- drop it, the next
 *                      paint rebinds.
 *   window_free        the window is gone; release everything.
 *   background_invalidate  the root background changed (_XROOTPMAP_ID).
 */
void renderer_window_invalidate(CompWindow *w);
/* Only the cached shape region -- for a ShapeNotify, where the window's
 * contents are untouched and dropping the bound pixmap with them would be
 * pure waste (kiwm reshapes a frame on every resize). */
void renderer_window_shape_invalidate(CompWindow *w);
void renderer_window_free(CompWindow *w);

/* The stash: the window's previous contents, kept instead of thrown away
 * when a resize replaces them (see comp.h's prev_pixmap).
 *
 *   stash        move the current contents aside; `was` is the rectangle
 *                they covered, which the caller has to pass because the
 *                window's own fields already describe the new size by
 *                the time this is called; whatever was stashed before
 *                and nobody held is freed
 *   has_stash    is there something to draw
 *   stash_rect   the rectangle those contents covered
 *   hold/release an effect claiming them for as long as it draws them
 *   drop_unheld  free a stash nothing claimed -- called once the batch of
 *                events has been classified, so a plain resize doesn't
 *                leave a pixmap lying around
 */
void renderer_window_stash(CompWindow *w, const CompRect *was);
bool renderer_window_has_stash(const CompWindow *w);
CompRect renderer_window_stash_rect(const CompWindow *w);
void renderer_stash_hold(CompWindow *w);
void renderer_stash_release(CompWindow *w);
void renderer_stash_drop_unheld(CompWindow *w);

/* Whether the window has drawable contents bound right now. An effect
 * that means to outlive the window must check this before retaining it:
 * a window that was never painted has no pixmap to keep, and naming one
 * after it is unmapped simply fails. */
bool renderer_window_has_content(const CompWindow *w);
void renderer_background_invalidate(void);

/* The cached Picture for a window's X-DENSITY pixmap is stale (the client
 * published a different XID, or stopped). */
void renderer_window_density_invalidate(CompWindow *w);

/* The pixmap behind an output's render target, for a presenter that
 * hands the server a pixmap rather than reading a Picture (Present).
 * XCB_NONE when the backend has no such thing -- a GL backend would
 * answer that, and would come with a presenter that doesn't ask. */
xcb_pixmap_t renderer_output_pixmap(const CompOutput *o);

/* Picture format of the root visual. An XRender detail, but the COPY
 * presenter needs the same answer and there should be exactly one place
 * that knows how to compute it. */
xcb_render_pictformat_t comp_root_pictformat(void);

/* Global teardown: pict formats cache, background picture. */
void renderer_shutdown(void);

#endif /* KICOMP_RENDERER_H */
