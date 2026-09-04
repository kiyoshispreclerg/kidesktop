/*
 * kicomp - text, for the effects that have something to say.
 *
 * A compositor has no business drawing chrome, with one exception: an
 * effect that is a *mode* has to tell the user what it is doing. The
 * filter box show-windows types into, and the name under each window in
 * its grid, are that exception -- there is no other program to ask,
 * because the grid only exists inside the compositor.
 *
 * Two decisions worth stating:
 *
 *   - The style is the *decoration's*, not one of ours. A desktop with
 *     one titlebar font and one accent colour should not grow a second
 *     set because the compositor arrived. So the theme is read the way
 *     xiskeys reads other daemons' configs: kiwm.conf for the theme path
 *     and its colours, then the theme's own `colors` file, read-only and
 *     best-effort. Anything missing falls back to something plain.
 *
 *   - Text is rendered once, not per frame. Pango and Cairo produce an
 *     ARGB image; that image becomes an X pixmap the XRender backend
 *     composites and a texture the GL one uploads. A label that hasn't
 *     changed costs a composite, not a layout.
 */
#ifndef KICOMP_TEXT_H
#define KICOMP_TEXT_H

#include "comp.h"

typedef struct CompTextStyle {
    char font[96];          /* fontconfig family; empty = sans-serif */
    double size;            /* pixels */
    int weight;             /* Pango weight, 100..1000 */
    int slant;              /* 0 normal, 1 italic, 2 oblique */

    float fg[4];

    bool shadow;
    float shadow_color[4];
    int shadow_dx, shadow_dy;

    bool outline;
    float outline_color[4];
    double outline_width;

    /* The plate the text sits on. Alpha 0 draws no plate at all. */
    float bg[4];
    int pad_x, pad_y;
} CompTextStyle;

/* The decoration's title style, read once. Never NULL: a session with no
 * kiwm config still gets a readable default. */
const CompTextStyle *text_theme_style(void);

typedef struct CompTextImage CompTextImage;

/* Lays the string out and rasterises it. `max_width` > 0 ellipsises to
 * that many pixels. NULL for an empty string or if anything failed --
 * every caller has to be able to draw nothing. */
CompTextImage *text_render(const char *utf8, const CompTextStyle *style,
                           int max_width);
void text_free(CompTextImage *img);

int text_width(const CompTextImage *img);
int text_height(const CompTextImage *img);

/* What the renderers need: the XRender backend takes the picture, the GL
 * one takes the pixels and uploads them once (the texture id lives in the
 * image, since it outlives a frame). */
xcb_render_picture_t text_picture(const CompTextImage *img);
const unsigned char *text_pixels(const CompTextImage *img);
unsigned int *text_gl_texture(CompTextImage *img);

void text_shutdown(void);

#endif /* KICOMP_TEXT_H */
