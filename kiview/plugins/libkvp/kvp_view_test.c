/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kvp_msg.h"
#include "kvp_view.h"

static int fails;
#define CHECK(c)                                                          \
    do {                                                                  \
        if (!(c)) {                                                       \
            fprintf(stderr, "%s:%d: failed: %s\n", __FILE__, __LINE__, #c); \
            fails++;                                                      \
        }                                                                 \
    } while (0)
#define NEAR(a, b) CHECK(fabs((a) - (b)) < 1e-6)

int main(void)
{
    KvpView v = {.aw = 800, .ah = 600};
    kvp_view_init(&v, 4000, 3000);
    NEAR(v.zoom, 0.2); /* fit */
    CHECK(!kvp_view_scrolls_x(&v) && !kvp_view_scrolls_y(&v));

    kvp_view_init(&v, 100, 50); /* small: fit never enlarges */
    NEAR(v.zoom, 1.0);
    double ox, oy;
    kvp_view_origin(&v, &ox, &oy);
    NEAR(ox, -350); /* centred */
    NEAR(oy, -275);

    kvp_view_init(&v, 4000, 3000);
    kvp_view_set_mode(&v, KVP_ACTUAL);
    NEAR(v.zoom, 1.0);
    CHECK(kvp_view_scrolls_x(&v) && kvp_view_scrolls_y(&v));
    kvp_view_origin(&v, &ox, &oy);
    NEAR(ox, 1600); /* middle of the document */
    NEAR(oy, 1200);

    /* the point under the anchor stays put */
    double ax = 100, ay = 50;
    double u = (ox + ax) / v.zoom, w = (oy + ay) / v.zoom;
    kvp_view_zoom_by(&v, 2, ax, ay);
    kvp_view_origin(&v, &ox, &oy);
    NEAR((ox + ax) / v.zoom, u);
    NEAR((oy + ay) / v.zoom, w);
    CHECK(v.mode == KVP_CUSTOM);

    /* pan clamps at the edges */
    kvp_view_pan(&v, -1e9, -1e9);
    kvp_view_origin(&v, &ox, &oy);
    NEAR(ox, 0);
    NEAR(oy, 0);
    kvp_view_pan(&v, 1e9, 1e9);
    kvp_view_origin(&v, &ox, &oy);
    NEAR(ox, 8000 - 800);
    NEAR(oy, 6000 - 600);

    /* zoom limits */
    kvp_view_zoom_set(&v, 1e6, 0, 0);
    NEAR(v.zoom, KVP_ZOOM_MAX);
    kvp_view_zoom_set(&v, 1e-9, 0, 0);
    NEAR(v.zoom, KVP_ZOOM_MIN);

    /* rotation swaps the sides; fit follows */
    kvp_view_init(&v, 4000, 3000);
    kvp_view_rotate(&v, 1);
    int rw, rh;
    kvp_view_rotated_size(&v, &rw, &rh);
    CHECK(rw == 3000 && rh == 4000);
    NEAR(v.zoom, 0.15);
    kvp_view_rotate(&v, -2);
    CHECK(v.rot == 3);

    /* the area changing refits */
    kvp_view_init(&v, 4000, 3000);
    kvp_view_set_area(&v, 400, 300);
    NEAR(v.zoom, 0.1);

    /* decode reductions and EXIF orientation */
    CHECK(kvp_pick_denom(0, 800, 600, 4000, 3000, 0) == 4);  /* fit at 0.2 */
    CHECK(kvp_pick_denom(0, 800, 600, 4000, 3000, 1) == 4);
    CHECK(kvp_pick_denom(0, 800, 600, 100, 100, 0) == 1);
    CHECK(kvp_pick_denom(1.0, 800, 600, 4000, 3000, 0) == 1);
    CHECK(kvp_pick_denom(0.5, 0, 0, 4000, 3000, 0) == 2);
    CHECK(kvp_pick_denom(0.01, 0, 0, 4000, 3000, 0) == 8);
    bool fl;
    int rr;
    kvp_orientation(6, &fl, &rr);
    CHECK(!fl && rr == 1);
    kvp_orientation(5, &fl, &rr);
    CHECK(fl && rr == 3);
    kvp_orientation(99, &fl, &rr);
    CHECK(!fl && rr == 0);

    /* messages */
    char *e = kvp_escape("a b=c%é");
    CHECK(strcmp(e, "a%20b%3Dc%25%C3%A9") == 0);
    char *d = kvp_unescape(e);
    CHECK(strcmp(d, "a b=c%é") == 0);
    g_free(e);
    g_free(d);
    KvpMsg *m = kvp_msg_parse("zoom mode=by factor=1.25 x=10 junk y=%2D5");
    CHECK(m && strcmp(m->verb, "zoom") == 0);
    NEAR(kvp_msg_get_double(m, "factor", 0), 1.25);
    CHECK(kvp_msg_get_int(m, "x", 0) == 10);
    CHECK(kvp_msg_get_int(m, "y", 0) == -5);
    CHECK(kvp_msg_get(m, "nope") == NULL);
    kvp_msg_free(m);
    CHECK(kvp_msg_parse("   ") == NULL);

    printf(fails ? "FAILED: %d\n" : "ok\n", fails);
    return fails != 0;
}
