/* The effect core: a list of running effects and a table of the modules
 * that start them. See effect.h -- deliberately the smallest thing that
 * can carry fade/zoom/geometry/wobbly without knowing any of them. */
#include "effect.h"
#include "animation.h"

#include <stdlib.h>
#include <string.h>

/* Adding an effect: write effects/<name>.c, declare its module in
 * effect.h, add it here. That is the whole registration story (section
 * 42) -- no event loop, no scheduler, no renderer change. */
static const CompEffectModule *const modules[] = {
    &effect_geometry,
};

#define MODULE_COUNT ((int)(sizeof(modules) / sizeof(modules[0])))

/* One config slot per module, in the same order. Filled with each
 * module's own defaults on the first lookup, then overridden by
 * kicomp.conf's [effect:<name>] sections. */
static CompEffectConfig configs[MODULE_COUNT];
static bool configs_ready;

static CompEffect *running;
static bool enabled;

static void configs_init(void)
{
    if (configs_ready)
        return;
    for (int i = 0; i < MODULE_COUNT; i++) {
        configs[i].enabled = modules[i]->default_enabled;
        configs[i].duration = modules[i]->default_duration;
    }
    configs_ready = true;
}

CompEffectConfig *effect_config(const char *name)
{
    configs_init();
    for (int i = 0; i < MODULE_COUNT; i++)
        if (strcmp(modules[i]->name, name) == 0)
            return &configs[i];
    return NULL;
}

bool effect_is_enabled(const char *name)
{
    if (!enabled)
        return false;
    const CompEffectConfig *c = effect_config(name);
    return c && c->enabled;
}

double effect_duration(const char *name)
{
    const CompEffectConfig *c = effect_config(name);
    return comp_anim_duration(c ? c->duration : 1.0);
}

void effects_init(void)
{
    configs_init();
    enabled = true;

    comp_info("effects on, animation unit %.0f ms", comp.anim_duration_ms);
    for (int i = 0; i < MODULE_COUNT; i++)
        comp_info("  %-10s %s, %.0f ms", modules[i]->name,
                  configs[i].enabled ? "on " : "off",
                  comp_anim_duration(configs[i].duration));
}

bool effects_enabled(void)
{
    return enabled;
}

void effects_add(CompEffect *e)
{
    e->next = running;
    running = e;
    comp_log("effect %s started (%.0f ms)", e->ops->name, e->duration);
}

bool effects_active(void)
{
    return running != NULL;
}

void effects_update(double now)
{
    CompEffect **pp = &running;
    while (*pp) {
        CompEffect *e = *pp;

        e->ops->update(e, now);

        if (e->ops->finished(e, now)) {
            /* One last update() ran above, so whatever the effect wanted
             * to leave on screen (and the damage for it) is already
             * posted; from the next frame the scene is simply itself
             * again. */
            comp_log("effect %s finished", e->ops->name);
            *pp = e->next;
            if (e->ops->destroy)
                e->ops->destroy(e);
            free(e);
            continue;
        }

        pp = &e->next;
    }
}

void effects_apply(CompScene *s, CompOutput *o)
{
    for (CompEffect *e = running; e; e = e->next)
        if (e->ops->apply)
            e->ops->apply(e, s, o);
}

void effects_window_configured(CompWindow *w, const CompRect *from,
                               const CompRect *to, bool interactive)
{
    if (!enabled)
        return;
    for (int i = 0; i < MODULE_COUNT; i++)
        if (modules[i]->window_configured)
            modules[i]->window_configured(w, from, to, interactive);
}

void effects_window_mapped(CompWindow *w)
{
    if (!enabled)
        return;
    for (int i = 0; i < MODULE_COUNT; i++)
        if (modules[i]->window_mapped)
            modules[i]->window_mapped(w);
}

void effects_window_unmapped(CompWindow *w)
{
    if (!enabled)
        return;
    for (int i = 0; i < MODULE_COUNT; i++)
        if (modules[i]->window_unmapped)
            modules[i]->window_unmapped(w);
}

void effects_window_gone(CompWindow *w)
{
    CompEffect **pp = &running;
    while (*pp) {
        CompEffect *e = *pp;
        if (e->window != w) {
            pp = &e->next;
            continue;
        }
        *pp = e->next;
        if (e->ops->destroy)
            e->ops->destroy(e);
        free(e);
    }
}

void effects_shutdown(void)
{
    CompEffect *e = running;
    while (e) {
        CompEffect *next = e->next;
        if (e->ops->destroy)
            e->ops->destroy(e);
        free(e);
        e = next;
    }
    running = NULL;
}
