/* The pure part of xis_inputscale: finding the confined CRTC under a point
 * and mapping a logical point onto the physical framebuffer. */
#include "xis_inputscale.h"
#include <stdio.h>

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(void)
{
    /* a 3840x2160 panel at 1920,0 drawn from a 1920x1080 logical desktop,
     * next to an ordinary 1920x1080 one at 0,0 */
    XisScaleMap m = { .n = 1 };
    m.crtc[0] = (XisConfinedCrtc){ .crtc = 7, .x = 1920, .y = 0, .pw = 3840, .ph = 2160,
                                   .cx = 1920, .cy = 0, .cw = 1920, .ch = 1080, .sx = 2, .sy = 2 };
    int px, py;

    CHECK(xis_scalemap_find(&m, 100, 100) == NULL);          /* the unscaled monitor */
    CHECK(xis_scalemap_find(&m, 1919, 500) == NULL);
    CHECK(xis_scalemap_find(&m, 1920, 0) == &m.crtc[0]);
    CHECK(xis_scalemap_find(&m, 5759, 2159) == &m.crtc[0]);  /* a physical point is found too */
    CHECK(xis_scalemap_find(&m, 5760, 0) == NULL);

    xis_scalemap_to_physical(&m.crtc[0], 1920, 0, &px, &py);
    CHECK(px == 1920 && py == 0);                            /* the origin stays */
    xis_scalemap_to_physical(&m.crtc[0], 2920, 540, &px, &py);
    CHECK(px == 3920 && py == 1080);                         /* 1000,540 from it -> 2000,1080 */
    xis_scalemap_to_physical(&m.crtc[0], 3839, 1079, &px, &py);
    CHECK(px == 5758 && py == 2158);                         /* the logical corner stays inside */

    if (!fails)
        puts("test_inputscale: ok");
    return fails != 0;
}
