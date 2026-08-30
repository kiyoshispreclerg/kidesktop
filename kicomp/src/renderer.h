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

    void (*begin)(CompOutput *o);     /* clear/paint the background */
    void (*draw_scene)(CompOutput *o, CompScene *s);
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
void renderer_window_free(CompWindow *w);
void renderer_background_invalidate(void);

/* Picture format of the root visual. An XRender detail, but the COPY
 * presenter needs the same answer and there should be exactly one place
 * that knows how to compute it. */
xcb_render_pictformat_t comp_root_pictformat(void);

/* Global teardown: pict formats cache, background picture. */
void renderer_shutdown(void);

#endif /* KICOMP_RENDERER_H */
