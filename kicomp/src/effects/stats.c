/*
 * stats: a small panel of numbers about the compositor itself, one per
 * output, on Meta+F12.
 *
 * Every other effect here is about the windows. This one is about the
 * thing drawing them, and it exists because the questions it answers are
 * ones you otherwise have to guess at from the outside: which backend
 * actually came up, what frame rate this monitor is being paced to, and
 * how many frames it is really doing. A compositor that repaints
 * constantly and one that repaints when something changes look identical
 * until you can see the number.
 *
 * Per output, and deliberately so: an output has its own frame clock and
 * answers to its own damage (section 19/39), so "the frame rate" is not a
 * session-wide fact. Two monitors showing different numbers is the normal
 * case, not a bug in the reading.
 *
 * Not a mode: nothing is grabbed, the desktop keeps working, and the
 * panel stays until the same key takes it away.
 *
 * kicomp.conf:
 *
 *   [effect:stats]
 *   enabled = 0                 # off by default: it is an instrument
 *   hotkey  = Meta+F12
 *   size    = 260               # the panel's width in px; the font
 *                               # follows from it
 *   corner  = top-right         # top-left | top-right | bottom-left |
 *                               # bottom-right
 *   margin  = 16                # gap from the screen's edges
 *
 * The colours and the face are the decoration's, the same ones the window
 * names in show-windows use (text.h) -- a desktop with one titlebar font
 * should not grow a second because the compositor has something to say.
 */
#include "../effect.h"
#include "../animation.h"
#include "../output.h"
#include "../input.h"
#include "../window.h"
#include "../scene.h"
#include "../text.h"
#include "../renderer.h"
#include "../presenter.h"
#include "../unredirect.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    CORNER_TOP_LEFT,
    CORNER_TOP_RIGHT,
    CORNER_BOTTOM_LEFT,
    CORNER_BOTTOM_RIGHT,
} StatsCorner;

typedef struct {
    char hotkey[64];
    int size;
    int margin;
    StatsCorner corner;
} StatsConfig;

typedef struct {
    /* One rendered panel per output, rebuilt when its text changes --
     * which is once a second, when the frame count rolls over. */
    struct CompTextImage *panel[MAX_OUTPUTS];
    char shown[MAX_OUTPUTS][256];
    double last_build;

    /* The key was pressed again. An effect cannot free itself from
     * inside the callback that asked -- the core owns it -- so what the
     * key does is say so, and `finished` answers the next time the core
     * asks. */
    bool done;
} StatsData;

static const CompEffectOps stats_ops;
static CompEffect *active;

/* How this output's frames are actually landing: which of msc/oml/vblank/
 * sw/randr is doing the pacing, not just which presenter is loaded --
 * "present" says nothing about whether the last frame flipped, copied or
 * never got a completion at all, and "copy" is worth calling out as the
 * one presenter with no server-side sync whatsoever. */
static void sync_text(const CompOutput *o, char *out, size_t outsz)
{
    CompWindow *holder = unredirect_holder(o);
    if (holder) {
        snprintf(out, outsz, "handed to 0x%x (app flips directly)", holder->id);
        return;
    }

    if (presenter && presenter->sync_info) {
        presenter->sync_info((CompOutput *)o, out, outsz);
        return;
    }

    snprintf(out, outsz, "sw clock, randr %.0f Hz (unmeasured)", o->refresh_hz);
}

/* Everything worth saying about one output, as the text to draw. */
static void panel_text(const CompOutput *o, char *out, size_t outsz)
{
    const char *rname = renderer ? renderer->name : "none";
    const char *pname = presenter ? presenter->name : "none";

    char scale[32] = "";
    if (o->scale != 1.0f)
        snprintf(scale, sizeof(scale), "  scale %.2gx", (double)o->scale);

    char sync[128];
    sync_text(o, sync, sizeof(sync));

    snprintf(out, outsz,
             "%s  %dx%d%s\n"
             "%s + %s\n"
             "%s\n"
             "%d fps  of %.0f target",
             o->name, o->rect.w, o->rect.h, scale,
             rname, pname,
             sync,
             o->fps, o->refresh_hz);
}

static void rebuild(CompEffect *e, double now)
{
    StatsData *d = e->data;
    const StatsConfig *cfg = e->instance->config;

    d->last_build = now;

    /* The font follows the panel's width, because that is the knob a
     * user actually has an opinion about: "make it bigger" means the
     * whole thing, not the type. */
    CompTextStyle st = *text_theme_style();
    double size = (double)cfg->size / 20.0;
    if (size < 7.0) size = 7.0;
    if (size > 48.0) size = 48.0;
    st.size = size;
    st.pad_x = (int)(size * 0.8);
    st.pad_y = (int)(size * 0.5);

    for (int i = 0; i < comp.output_count && i < MAX_OUTPUTS; i++) {
        char text[256];
        panel_text(&comp.outputs[i], text, sizeof(text));

        if (d->panel[i] && !strcmp(d->shown[i], text))
            continue;

        text_free(d->panel[i]);
        snprintf(d->shown[i], sizeof(d->shown[i]), "%s", text);
        d->panel[i] = text_render(text, &st, 0);

        output_damage_rect(&comp.outputs[i].rect);
    }
}

static void stats_update(CompEffect *e, double now)
{
    StatsData *d = e->data;

    /* Once a second, which is exactly as often as the number it is
     * showing changes. A panel of numbers that repaints every frame to
     * show the same numbers would be its own answer. */
    if (now - d->last_build >= 500.0)
        rebuild(e, now);
}

static void stats_apply(CompEffect *e, CompScene *s, CompOutput *o)
{
    StatsData *d = e->data;
    const StatsConfig *cfg = e->instance->config;

    int index = -1;
    for (int i = 0; i < comp.output_count; i++)
        if (comp.outputs[i].id == o->id)
            index = i;
    if (index < 0 || index >= MAX_OUTPUTS || !d->panel[index])
        return;

    int w = text_width(d->panel[index]);
    int h = text_height(d->panel[index]);
    int m = cfg->margin;

    CompRect at;
    at.w = w;
    at.h = h;
    at.x = (cfg->corner == CORNER_TOP_LEFT || cfg->corner == CORNER_BOTTOM_LEFT)
         ? o->rect.x + m
         : o->rect.x + o->rect.w - w - m;
    at.y = (cfg->corner == CORNER_TOP_LEFT || cfg->corner == CORNER_TOP_RIGHT)
         ? o->rect.y + m
         : o->rect.y + o->rect.h - h - m;

    scene_add_chrome(s, d->panel[index], &at, 1.0f);
}

static bool stats_finished(const CompEffect *e, double now)
{
    (void)now;
    /* It ends when the key says so, not when a clock does. */
    return ((const StatsData *)e->data)->done;
}

static void stats_destroy(CompEffect *e)
{
    StatsData *d = e->data;

    for (int i = 0; d && i < MAX_OUTPUTS; i++)
        text_free(d->panel[i]);

    for (int i = 0; i < comp.output_count; i++)
        output_damage_rect(&comp.outputs[i].rect);

    if (e == active)
        active = NULL;
    free(e->data);
    e->data = NULL;
}

static const CompEffectOps stats_ops = {
    .name     = "stats",
    .update   = stats_update,
    .apply    = stats_apply,
    .finished = stats_finished,
    .destroy  = stats_destroy,
};

static void stats_toggle(void *data)
{
    const CompEffectInstance *self = data;

    if (active) {
        ((StatsData *)active->data)->done = true;
        active = NULL;      /* the next press starts a new one */
        return;
    }

    CompEffect *e = calloc(1, sizeof(*e));
    StatsData *d = calloc(1, sizeof(*d));
    if (!e || !d) {
        free(e);
        free(d);
        return;
    }

    e->ops = &stats_ops;
    e->instance = self;
    e->window = NULL;       /* about the compositor, not about a window */
    e->start_time = comp_now_ms();
    e->duration = 0.0;      /* it ends when the key says so */
    e->data = d;

    active = e;
    effects_add(e);
    rebuild(e, comp_now_ms());
}

static void stats_init(const CompEffectInstance *self)
{
    const StatsConfig *cfg = self->config;
    if (!cfg->hotkey[0])
        return;

    char buf[sizeof(cfg->hotkey)];
    snprintf(buf, sizeof(buf), "%s", cfg->hotkey);

    char *save = NULL;
    for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        while (*tok == ' ' || *tok == '\t')
            tok++;
        char *end = tok + strlen(tok);
        while (end > tok && (end[-1] == ' ' || end[-1] == '\t'))
            *--end = '\0';
        if (*tok)
            input_bind_hotkey(tok, stats_toggle, (void *)self);
    }
}

static void stats_defaults(void *config)
{
    StatsConfig *c = config;
    snprintf(c->hotkey, sizeof(c->hotkey), "%s", "Meta+F12");
    c->size = 260;
    c->margin = 16;
    c->corner = CORNER_TOP_RIGHT;
}

static bool stats_config_key(void *config, const char *key, const char *value)
{
    StatsConfig *c = config;

    if (!strcmp(key, "hotkey")) {
        snprintf(c->hotkey, sizeof(c->hotkey), "%s", value);
        return true;
    }
    if (!strcmp(key, "size")) {
        c->size = atoi(value);
        if (c->size < 120) c->size = 120;
        if (c->size > 900) c->size = 900;
        return true;
    }
    if (!strcmp(key, "margin")) {
        c->margin = atoi(value);
        return true;
    }
    if (!strcmp(key, "corner")) {
        if (!strcmp(value, "top-left")) c->corner = CORNER_TOP_LEFT;
        else if (!strcmp(value, "top-right")) c->corner = CORNER_TOP_RIGHT;
        else if (!strcmp(value, "bottom-left")) c->corner = CORNER_BOTTOM_LEFT;
        else if (!strcmp(value, "bottom-right")) c->corner = CORNER_BOTTOM_RIGHT;
        else fprintf(stderr, "kicomp: config: unknown corner '%s'\n", value);
        return true;
    }
    return false;
}

const CompEffectModule effect_stats = {
    .name             = "stats",
    /* Off by default: it is an instrument, not a decoration. */
    .default_enabled  = false,
    .default_duration = 1.0,
    .default_easing   = COMP_EASE_LINEAR,
    .default_events   = 0,
    .default_windows  = 0xffffffffu,

    .init             = stats_init,
    .config_size      = sizeof(StatsConfig),
    .config_defaults  = stats_defaults,
    .config_key       = stats_config_key,
};
