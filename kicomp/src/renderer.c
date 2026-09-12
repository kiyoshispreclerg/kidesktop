/* The core's side of the renderer interface: one wrapper per backend hook
 * (renderer.h), each dispatching through the vtable.
 *
 * These are free functions rather than calls through `renderer->` at every
 * call site because that is what they are to the rest of the compositor:
 * window.c does not want to know that "drop this window's contents" is a
 * backend question, only that it is one. And a backend that has no such
 * concept leaves the op NULL, which is the difference between "no stash
 * here" and every caller having to ask whether stashes exist.
 */
#include "renderer.h"

const CompRenderer *renderer;

void renderer_window_invalidate(CompWindow *w)
{
    if (renderer && renderer->window_invalidate)
        renderer->window_invalidate(w);
}

void renderer_window_shape_invalidate(CompWindow *w)
{
    if (renderer && renderer->window_shape_invalidate)
        renderer->window_shape_invalidate(w);
}

void renderer_window_free(CompWindow *w)
{
    if (renderer && renderer->window_free)
        renderer->window_free(w);
}

bool renderer_window_has_content(const CompWindow *w)
{
    return renderer && renderer->window_has_content &&
           renderer->window_has_content(w);
}

void renderer_window_density_invalidate(CompWindow *w, bool decoration)
{
    if (renderer && renderer->window_density_invalidate)
        renderer->window_density_invalidate(w, decoration);
}

void renderer_window_stash(CompWindow *w, const CompRect *was)
{
    if (renderer && renderer->window_stash)
        renderer->window_stash(w, was);
}

bool renderer_window_has_stash(const CompWindow *w)
{
    return renderer && renderer->window_has_stash &&
           renderer->window_has_stash(w);
}

CompRect renderer_window_stash_rect(const CompWindow *w)
{
    if (renderer && renderer->window_stash_rect)
        return renderer->window_stash_rect(w);
    return (CompRect){ 0, 0, 0, 0 };
}

void renderer_stash_hold(CompWindow *w)
{
    if (renderer && renderer->stash_hold)
        renderer->stash_hold(w);
}

void renderer_stash_release(CompWindow *w)
{
    if (renderer && renderer->stash_release)
        renderer->stash_release(w);
}

void renderer_stash_drop_unheld(CompWindow *w)
{
    if (renderer && renderer->stash_drop_unheld)
        renderer->stash_drop_unheld(w);
}

void renderer_background_invalidate(void)
{
    if (renderer && renderer->background_invalidate)
        renderer->background_invalidate();
}

xcb_pixmap_t renderer_output_pixmap(const CompOutput *o)
{
    if (renderer && renderer->output_pixmap)
        return renderer->output_pixmap(o);
    return XCB_NONE;
}

void renderer_output_pixmap_idle(CompOutput *o, xcb_pixmap_t pixmap)
{
    if (renderer && renderer->output_pixmap_idle)
        renderer->output_pixmap_idle(o, pixmap);
}

bool renderer_output_has_free_buffer(const CompOutput *o)
{
    return renderer && renderer->output_has_free_buffer &&
           renderer->output_has_free_buffer(o);
}

void renderer_shutdown(void)
{
    if (renderer && renderer->shutdown)
        renderer->shutdown();
}
