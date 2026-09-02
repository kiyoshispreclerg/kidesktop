/*
 * GLX presenter: the buffer swap.
 *
 * With GL there is no separate presentation step to speak of -- the
 * renderer draws into the drawable's back buffer and presenting is
 * glXSwapBuffers on it. So this presenter is deliberately thin, and it is
 * paired with renderer-glx.c rather than being independent of it: the
 * drawable belongs to the renderer, which is the one that has a context
 * current on it.
 *
 * The split still earns its keep, because it is where the *next* step
 * goes: presenting a GL buffer through Present (and, on the XiS server,
 * flipping it per CRTC, Fase 8) replaces this file and nothing else.
 */
#include "presenter.h"
#include "renderer.h"

/* From renderer-glx.c: swapping is the renderer's drawable, not ours. */
void renderer_glx_swap(CompOutput *o);
bool renderer_glx_ready(void);

static bool glx_present_init(CompOutput *o)
{
    (void)o;
    /* The renderer made the drawable; if it never came up, there is
     * nothing to swap and saying so here is better than swapping into a
     * drawable that doesn't exist. */
    return renderer_glx_ready();
}

static void glx_present_destroy(CompOutput *o)
{
    (void)o;   /* the drawable is the renderer's to destroy */
}

static bool glx_present(CompOutput *o, CompPresentMode mode,
                        const CompRegion *damage)
{
    /* The damage went into the frame as a scissor box; the swap itself is
     * all or nothing. GLX_EXT_buffer_age plus a partial swap
     * (GLX_MESA_copy_sub_buffer / EXT_swap_buffers_with_damage) is the
     * follow-up that makes this region mean something here too. */
    (void)damage;

    if (mode == COMP_PRESENT_FLIP)
        return false;

    renderer_glx_swap(o);
    return true;
}

static uint64_t glx_present_msc(CompOutput *o)
{
    (void)o;
    return 0;   /* GLX_OML_sync_control is the follow-up */
}

static const CompPresenter glx_presenter = {
    .name    = "glx",
    .init    = glx_present_init,
    .destroy = glx_present_destroy,
    .present = glx_present,
    .get_msc = glx_present_msc,
};

const CompPresenter *presenter_glx(void)
{
    return &glx_presenter;
}
