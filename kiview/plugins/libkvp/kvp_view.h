/* SPDX-License-Identifier: GPL-3.0-or-later */
/* kvp_view - the plugin's view of a flat document: where it sits in the
 * area, for a zoom mode, a scroll position and a rotation. Pure arithmetic
 * (no X, no cairo), in logical pixels: the density only multiplies at
 * drawing time.
 *
 * Scaled space is the document at the current zoom, rotated. The area shows
 * a window of it starting at the origin (ox, oy); an axis where the
 * document is smaller than the area is centered (negative origin). */
#ifndef KVP_VIEW_H
#define KVP_VIEW_H

#include <stdbool.h>

#define KVP_ZOOM_MIN 0.005
#define KVP_ZOOM_MAX 64.0

typedef enum {
    KVP_FIT,       /* whole document in the area, never enlarged */
    KVP_FIT_WIDTH, /* width of the area, may enlarge */
    KVP_ACTUAL,    /* 1 document pixel per logical pixel */
    KVP_CUSTOM,
} KvpMode;

typedef struct {
    int iw, ih; /* the document, unrotated */
    int rot;    /* quarter turns clockwise, 0..3 */
    int aw, ah; /* the area */
    KvpMode mode;
    double zoom;
    double px, py; /* scroll, meaningful on an axis the document overflows */
} KvpView;

void kvp_view_init(KvpView *v, int iw, int ih);
void kvp_view_set_area(KvpView *v, int aw, int ah);
void kvp_view_set_mode(KvpView *v, KvpMode mode);
/* Zoom to `zoom` keeping the document point under (ax, ay) of the area. */
void kvp_view_zoom_set(KvpView *v, double zoom, double ax, double ay);
void kvp_view_zoom_by(KvpView *v, double factor, double ax, double ay);
void kvp_view_pan(KvpView *v, double dx, double dy);
void kvp_view_rotate(KvpView *v, int quarter_turns);

/* The document's size once rotated, in document pixels and scaled. */
void kvp_view_rotated_size(const KvpView *v, int *w, int *h);
void kvp_view_scaled_size(const KvpView *v, double *w, double *h);
/* Where the area's top-left falls in scaled space. */
void kvp_view_origin(const KvpView *v, double *ox, double *oy);
bool kvp_view_scrolls_x(const KvpView *v);
bool kvp_view_scrolls_y(const KvpView *v);

#endif
