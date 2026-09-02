/*
 * X-DENSITY, client side, for kiwm's own decorations -- see density.c.
 *
 * kiwm is a client of this protocol like any other: a compositor asks the
 * *frame* to redraw its decoration densely, and kiwm publishes a pixmap
 * for it to sample. Entirely optional -- with no compositor asking,
 * nothing here allocates or publishes anything.
 */
#ifndef KIWM_DENSITY_H
#define KIWM_DENSITY_H

#include "wm.h"

/* _X_DENSITY_REQUESTED changed on this client's frame. */
void deco_density_request_changed(Client *c);

/* Called at the end of every decoration repaint: renders the same
 * decoration into the density pixmap and republishes it (the republish is
 * also the "contents changed" signal -- a pixmap has no Damage of its
 * own). A no-op at density 1. */
void deco_density_publish(Client *c, int w, int h, bool focused);

/* Density no longer wanted, or the client is going away. */
void deco_density_forget(Client *c);

#endif /* KIWM_DENSITY_H */
