/*
 * kicomp - presentation abstraction (section 15).
 *
 * Rendering an output and *presenting* it are separate steps so that the
 * XiS per-CRTC FLIP backend (Fase 8) can slot in without the renderer or
 * the scene knowing anything about it. This prototype ships only the COPY
 * presenter, which composites each output's target onto the Composite
 * overlay window.
 */
#ifndef KICOMP_PRESENTER_H
#define KICOMP_PRESENTER_H

#include "comp.h"

typedef enum {
    COMP_PRESENT_COPY,
    COMP_PRESENT_FLIP
} CompPresentMode;

typedef struct CompPresenter {
    const char *name;

    bool (*init)(CompOutput *o);
    void (*destroy)(CompOutput *o);

    /* `damage` is the part of the output that was actually repainted
     * (region.h): a COPY presenter only has to copy that much, and a FLIP
     * one, which hands over a whole buffer, can ignore it. */
    bool (*present)(CompOutput *o, CompPresentMode mode, const CompRegion *damage);

    /* Is a frame still in flight for this output? An output that has one
     * is not painted again until it lands: a second frame queued behind
     * the first doesn't appear any sooner, it just puts one more frame of
     * latency between what the user did and what they see. Optional -- a
     * presenter that copies synchronously is never busy. */
    bool (*busy)(CompOutput *o);

    /* First refusal on an X event, for a backend whose completions arrive
     * as events (Present's are XGE generic events). True means "mine,
     * handled". Optional. */
    bool (*handle_event)(xcb_generic_event_t *ev);

    /* Media stream counter, when the backend can report one. Returns 0
     * when unknown -- the per-output frame clock (Fase 7) will use it. */
    uint64_t (*get_msc)(CompOutput *o);

    /* One line of human prose on *how* this output's frames are being
     * paced and landed -- the stats effect's answer to "is this actually
     * synced to the monitor, or just to a software timer guessing at the
     * RandR mode". Optional: a presenter that has nothing more specific
     * to say than its own name leaves this NULL and the caller falls
     * back to that. */
    void (*sync_info)(CompOutput *o, char *buf, size_t n);
} CompPresenter;

/* A damaged rectangle, in logical root coordinates, as the physical
 * rectangle of scanout it corresponds to. The identity when the output
 * isn't scaled. Shared by both presenters because both copy from a
 * physical-sized target while damage is tracked logically. */
bool present_physical_rect(const CompOutput *o, const CompRect *logical,
                           CompRect *out);

const CompPresenter *presenter_copy(void);
const CompPresenter *presenter_present(void);
const CompPresenter *presenter_glx(void);

extern const CompPresenter *presenter;

void presenter_shutdown(void);

#endif /* KICOMP_PRESENTER_H */
