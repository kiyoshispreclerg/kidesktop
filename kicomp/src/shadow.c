/* Shadow configuration and per-window style -- see shadow.h. The drawing
 * itself is the renderer's (renderer-xrender.c), since what a shadow *is*
 * made of is a backend question and what it looks like is not. */
#include "shadow.h"
#include "effect.h"   /* comp_window_type_mask_parse */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

CompShadowConfig comp_shadow;

static bool parse_color(const char *s, float *r, float *g, float *b)
{
    if (*s == '#')
        s++;
    if (strlen(s) != 6)
        return false;

    char *end = NULL;
    unsigned long v = strtoul(s, &end, 16);
    if (!end || *end)
        return false;

    *r = ((v >> 16) & 0xff) / 255.0f;
    *g = ((v >> 8) & 0xff) / 255.0f;
    *b = (v & 0xff) / 255.0f;
    return true;
}

void shadow_config_defaults(void)
{
    memset(&comp_shadow, 0, sizeof(comp_shadow));

    /* Off by default: a shadow is a look, and one the user should be the
     * one to ask for. */
    comp_shadow.enabled = false;

    /* Ordinary windows and menus. Not docks (a panel flush against a
     * screen edge casts its shadow onto nothing) and not the desktop. */
    comp_shadow.windows = comp_window_type_mask_parse("windows,menus");

    comp_shadow.active.radius = 12;
    comp_shadow.active.opacity = 0.45f;
    comp_shadow.active.offset_x = 0;
    comp_shadow.active.offset_y = 6;
    comp_shadow.active.r = comp_shadow.active.g = comp_shadow.active.b = 0.0f;

    comp_shadow.inactive = comp_shadow.active;
}

bool shadow_config_key(const char *key, const char *value)
{
    CompShadowStyle *a = &comp_shadow.active;
    CompShadowStyle *i = &comp_shadow.inactive;

    if (strcmp(key, "enabled") == 0) {
        comp_shadow.enabled = atoi(value) != 0;
    } else if (strcmp(key, "windows") == 0) {
        comp_shadow.windows = comp_window_type_mask_parse(value);
    } else if (strcmp(key, "radius") == 0) {
        int n = atoi(value);
        /* Past a point the blur is wider than the gap between windows and
         * everything turns to soup; and a radius of 0 is "no shadow",
         * which is what enabled=0 is for. */
        if (n < 1) n = 1;
        if (n > 64) n = 64;
        a->radius = n;
    } else if (strcmp(key, "opacity") == 0) {
        float f = (float)atof(value);
        if (f < 0.0f) f = 0.0f;
        if (f > 1.0f) f = 1.0f;
        a->opacity = f;
    } else if (strcmp(key, "offset_x") == 0) {
        a->offset_x = atoi(value);
    } else if (strcmp(key, "offset_y") == 0) {
        a->offset_y = atoi(value);
    } else if (strcmp(key, "color") == 0) {
        if (!parse_color(value, &a->r, &a->g, &a->b)) {
            fprintf(stderr, "kicomp: config: invalid shadow color '%s' "
                            "(expected #rrggbb)\n", value);
            return true;
        }
    } else if (strcmp(key, "radius_inactive") == 0) {
        int n = atoi(value);
        if (n < 1) n = 1;
        if (n > 64) n = 64;
        i->radius = n;
        comp_shadow.has_inactive_radius = true;
    } else if (strcmp(key, "opacity_inactive") == 0) {
        float f = (float)atof(value);
        if (f < 0.0f) f = 0.0f;
        if (f > 1.0f) f = 1.0f;
        i->opacity = f;
        comp_shadow.has_inactive_opacity = true;
    } else if (strcmp(key, "offset_x_inactive") == 0) {
        i->offset_x = atoi(value);
        comp_shadow.has_inactive_offset_x = true;
    } else if (strcmp(key, "offset_y_inactive") == 0) {
        i->offset_y = atoi(value);
        comp_shadow.has_inactive_offset_y = true;
    } else if (strcmp(key, "color_inactive") == 0) {
        if (!parse_color(value, &i->r, &i->g, &i->b)) {
            fprintf(stderr, "kicomp: config: invalid shadow color '%s' "
                            "(expected #rrggbb)\n", value);
            return true;
        }
        comp_shadow.has_inactive_color = true;
    } else {
        return false;
    }

    return true;
}

void shadow_config_finish(void)
{
    CompShadowStyle *a = &comp_shadow.active;
    CompShadowStyle *i = &comp_shadow.inactive;

    /* Anything the config didn't say about unfocused windows is the same
     * as for focused ones -- so "same shadow, just fainter" is one line. */
    if (!comp_shadow.has_inactive_radius)   i->radius = a->radius;
    if (!comp_shadow.has_inactive_opacity)  i->opacity = a->opacity;
    if (!comp_shadow.has_inactive_offset_x) i->offset_x = a->offset_x;
    if (!comp_shadow.has_inactive_offset_y) i->offset_y = a->offset_y;
    if (!comp_shadow.has_inactive_color) {
        i->r = a->r;
        i->g = a->g;
        i->b = a->b;
    }
}

bool shadow_for_window(const CompWindow *w, CompShadowStyle *out)
{
    if (!comp_shadow.enabled)
        return false;
    if (w->input_only || w->wm_layer[0])
        return false;
    if (!(comp_shadow.windows & COMP_WINDOW_BIT(w->type)))
        return false;

    /* A window filling its screen has nothing to cast a shadow onto: its
     * edges are the screen's edges. Drawing one is invisible at best, and
     * at worst a dark band down the side of the next monitor. */
    if (w->state & (COMP_STATE_MAXIMIZED | COMP_STATE_FULLSCREEN))
        return false;

    *out = w->focused ? comp_shadow.active : comp_shadow.inactive;
    return out->opacity > 0.0f && out->radius > 0;
}

int shadow_margin(void)
{
    if (!comp_shadow.enabled)
        return 0;

    const CompShadowStyle *styles[2] = { &comp_shadow.active, &comp_shadow.inactive };
    int margin = 0;

    for (int i = 0; i < 2; i++) {
        const CompShadowStyle *s = styles[i];
        if (s->opacity <= 0.0f || s->radius <= 0)
            continue;

        int ox = s->offset_x < 0 ? -s->offset_x : s->offset_x;
        int oy = s->offset_y < 0 ? -s->offset_y : s->offset_y;
        int reach = s->radius + (ox > oy ? ox : oy);
        if (reach > margin)
            margin = reach;
    }

    return margin;
}
