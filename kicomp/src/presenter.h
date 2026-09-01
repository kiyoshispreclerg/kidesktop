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

    /* Media stream counter, when the backend can report one. Returns 0
     * when unknown -- the per-output frame clock (Fase 7) will use it. */
    uint64_t (*get_msc)(CompOutput *o);
} CompPresenter;

const CompPresenter *presenter_copy(void);

extern const CompPresenter *presenter;

void presenter_shutdown(void);

#endif /* KICOMP_PRESENTER_H */
