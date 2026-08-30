/*
 * kicomp test client: one 32-bit (ARGB) window filled with a
 * half-transparent colour.
 *
 * This is the acceptance test for the first kicomp milestone. Without a
 * compositor the window's alpha channel means nothing and the square is
 * drawn opaque (or garbage); with kicomp running it blends over whatever
 * is behind it.
 *
 * By default the window is an ordinary managed one, so kiwm frames and
 * decorates it -- which is the harder half of the test: the frame has to
 * be created in the client's own depth for the alpha to survive into it
 * (see kiwm's client.c). --override makes it override-redirect instead,
 * bypassing the WM entirely, which isolates the compositor's blending
 * from anything the WM does.
 *
 *   cc -o argb-window argb-window.c -lxcb -lxcb-render
 *   ./argb-window                          # 300x300 at +200+200, 50% red
 *   ./argb-window 400 400 600 200 0.35 0x30a0ff
 *   ./argb-window --override
 */
#include <xcb/xcb.h>
#include <xcb/render.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    bool override = false;
    int pos[6] = { 300, 300, 200, 200, 0, 0 };  /* w h x y (alpha/rgb read separately) */
    double alpha = 0.5;
    unsigned long rgb = 0xff3030;
    int npos = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--override")) {
            override = true;
        } else if (npos < 4) {
            pos[npos++] = atoi(argv[i]);
        } else if (npos == 4) {
            alpha = atof(argv[i]);
            npos++;
        } else {
            rgb = strtoul(argv[i], NULL, 0);
            npos++;
        }
    }

    int w = pos[0], h = pos[1], x = pos[2], y = pos[3];

    int screen_num;
    xcb_connection_t *c = xcb_connect(NULL, &screen_num);
    if (xcb_connection_has_error(c)) {
        fprintf(stderr, "argb-window: cannot connect to X\n");
        return 1;
    }

    xcb_screen_iterator_t si = xcb_setup_roots_iterator(xcb_get_setup(c));
    for (int i = 0; i < screen_num; i++)
        xcb_screen_next(&si);
    xcb_screen_t *screen = si.data;

    /* Find a 32-bit TrueColor visual -- the whole point of the test. */
    xcb_visualid_t visual = 0;
    xcb_depth_iterator_t di = xcb_screen_allowed_depths_iterator(screen);
    for (; di.rem && !visual; xcb_depth_next(&di)) {
        if (di.data->depth != 32)
            continue;
        xcb_visualtype_t *v = xcb_depth_visuals(di.data);
        int n = xcb_depth_visuals_length(di.data);
        for (int i = 0; i < n; i++) {
            if (v[i]._class == XCB_VISUAL_CLASS_TRUE_COLOR) {
                visual = v[i].visual_id;
                break;
            }
        }
    }
    if (!visual) {
        fprintf(stderr, "argb-window: no 32-bit TrueColor visual on this screen\n");
        return 1;
    }

    /* A window whose depth differs from its parent's needs its own
     * colormap and an explicit border pixel. */
    xcb_colormap_t cmap = xcb_generate_id(c);
    xcb_create_colormap(c, XCB_COLORMAP_ALLOC_NONE, cmap, screen->root, visual);

    xcb_window_t win = xcb_generate_id(c);
    uint32_t values[] = { 0, 0, override ? 1 : 0, XCB_EVENT_MASK_EXPOSURE, cmap };
    xcb_create_window(c, 32, win, screen->root, x, y, w, h, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, visual,
                      XCB_CW_BACK_PIXEL | XCB_CW_BORDER_PIXEL |
                      XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK |
                      XCB_CW_COLORMAP, values);
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, win, XCB_ATOM_WM_NAME,
                        XCB_ATOM_STRING, 8, 11, "argb-window");
    xcb_map_window(c, win);

    /* Picture format for that visual, so the fill lands with real alpha. */
    xcb_render_query_pict_formats_reply_t *fmts =
        xcb_render_query_pict_formats_reply(c, xcb_render_query_pict_formats(c), NULL);
    xcb_render_pictformat_t fmt = 0;
    if (fmts) {
        xcb_render_pictscreen_iterator_t ps =
            xcb_render_query_pict_formats_screens_iterator(fmts);
        for (; ps.rem && !fmt; xcb_render_pictscreen_next(&ps)) {
            xcb_render_pictdepth_iterator_t pd = xcb_render_pictscreen_depths_iterator(ps.data);
            for (; pd.rem && !fmt; xcb_render_pictdepth_next(&pd)) {
                xcb_render_pictvisual_t *pv = xcb_render_pictdepth_visuals(pd.data);
                int n = xcb_render_pictdepth_visuals_length(pd.data);
                for (int i = 0; i < n; i++)
                    if (pv[i].visual == visual) {
                        fmt = pv[i].format;
                        break;
                    }
            }
        }
    }
    if (!fmt) {
        fprintf(stderr, "argb-window: no picture format for the ARGB visual\n");
        return 1;
    }

    xcb_render_picture_t pict = xcb_generate_id(c);
    xcb_render_create_picture(c, pict, win, fmt, 0, NULL);

    if (alpha < 0.0) alpha = 0.0;
    if (alpha > 1.0) alpha = 1.0;

    /* XRender colours are premultiplied by alpha. */
    xcb_render_color_t col = {
        .red   = (uint16_t)((((rgb >> 16) & 0xff) * 0x101) * alpha),
        .green = (uint16_t)((((rgb >>  8) & 0xff) * 0x101) * alpha),
        .blue  = (uint16_t)(((rgb & 0xff) * 0x101) * alpha),
        .alpha = (uint16_t)(0xffff * alpha),
    };

    printf("argb-window: %dx%d+%d+%d alpha=%.2f rgb=0x%06lx (window 0x%x)\n",
           w, h, x, y, alpha, rgb, win);

    xcb_generic_event_t *ev;
    for (;;) {
        xcb_rectangle_t r = { 0, 0, (uint16_t)w, (uint16_t)h };
        xcb_render_fill_rectangles(c, XCB_RENDER_PICT_OP_SRC, pict, col, 1, &r);
        xcb_flush(c);

        ev = xcb_wait_for_event(c);
        if (!ev)
            break;
        free(ev);   /* any event: repaint (Expose is the only one selected) */
    }

    free(fmts);
    xcb_disconnect(c);
    return 0;
}
