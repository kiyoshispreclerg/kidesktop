/* The effect core: a list of running effects and a table of the modules
 * that start them. See effect.h -- deliberately the smallest thing that
 * can carry fade/zoom/geometry/wobbly without knowing any of them. */
#include "effect.h"
#include "animation.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Adding an effect: write effects/<name>.c, declare its module in
 * effect.h, add it here. That is the whole registration story (section
 * 42) -- no event loop, no scheduler, no renderer change. */
static const CompEffectModule *const modules[] = {
    &effect_geometry,
    &effect_fade_in,
    &effect_fade_out,
    &effect_scale_in,
    &effect_scale_out,
};

#define MODULE_COUNT ((int)(sizeof(modules) / sizeof(modules[0])))

/* One config slot per module, in the same order. Filled with each
 * module's own defaults on the first lookup, then overridden by
 * kicomp.conf's [effect:<name>] sections. */
static CompEffectConfig configs[MODULE_COUNT];
static bool configs_ready;

static CompEffect *running;
static bool enabled;

/* kicomp.conf's names for CompEventKind -- keep in the enum's order. */
static const char *const event_names[COMP_EVENT_COUNT] = {
    "open", "close",
    "minimize", "restore",
    "maximize", "unmaximize",
    "shade", "unshade",
    "fullscreen", "unfullscreen",
    "focus", "unfocus",
    "move",
    "desktop-leave", "desktop-enter",
};

const char *comp_event_name(CompEventKind kind)
{
    if (kind < 0 || kind >= COMP_EVENT_COUNT)
        return "?";
    return event_names[kind];
}

uint32_t comp_event_mask_parse(const char *list)
{
    if (strcmp(list, "all") == 0)
        return COMP_EVENTS_ALL;
    if (strcmp(list, "none") == 0)
        return 0;

    uint32_t mask = 0;
    const char *p = list;

    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',')
            p++;
        const char *start = p;
        while (*p && *p != ',' && *p != ' ' && *p != '\t')
            p++;
        size_t len = (size_t)(p - start);
        if (len == 0)
            continue;

        bool found = false;
        for (int i = 0; i < COMP_EVENT_COUNT; i++) {
            if (strlen(event_names[i]) == len && strncmp(start, event_names[i], len) == 0) {
                mask |= COMP_EVENT_BIT(i);
                found = true;
                break;
            }
        }
        if (!found)
            fprintf(stderr, "kicomp: config: unknown event '%.*s'\n", (int)len, start);
    }

    return mask;
}

/* The mask as a printable list, for the startup log. */
static void event_mask_string(uint32_t mask, char *out, size_t outsz)
{
    out[0] = '\0';
    if (mask == 0) {
        snprintf(out, outsz, "none");
        return;
    }

    size_t off = 0;
    for (int i = 0; i < COMP_EVENT_COUNT; i++) {
        if (!(mask & COMP_EVENT_BIT(i)))
            continue;
        int n = snprintf(out + off, outsz - off, "%s%s", off ? "," : "", event_names[i]);
        if (n < 0 || (size_t)n >= outsz - off)
            break;
        off += (size_t)n;
    }
}

static void configs_init(void)
{
    if (configs_ready)
        return;
    for (int i = 0; i < MODULE_COUNT; i++) {
        configs[i].enabled = modules[i]->default_enabled;
        configs[i].duration = modules[i]->default_duration;
        configs[i].events = modules[i]->default_events;
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

bool effect_config_key(const char *name, const char *key, const char *value)
{
    for (int i = 0; i < MODULE_COUNT; i++) {
        if (strcmp(modules[i]->name, name) != 0)
            continue;
        if (!modules[i]->config_key)
            return false;
        return modules[i]->config_key(key, value);
    }
    return false;
}

void effects_init(void)
{
    configs_init();
    enabled = true;

    comp_info("effects on, animation unit %.0f ms", comp.anim_duration_ms);
    for (int i = 0; i < MODULE_COUNT; i++) {
        char events[256];
        event_mask_string(configs[i].events, events, sizeof(events));
        comp_info("  %-10s %s, %4.0f ms  on: %s", modules[i]->name,
                  configs[i].enabled ? "on " : "off",
                  comp_anim_duration(configs[i].duration), events);
    }
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

void effects_window_event(CompWindow *w, const CompEvent *ev)
{
    if (!enabled)
        return;

    configs_init();
    comp_log("window 0x%x: %s", w->id, comp_event_name(ev->kind));

    for (int i = 0; i < MODULE_COUNT; i++) {
        if (!modules[i]->window_event || !configs[i].enabled)
            continue;
        /* The mask is the filter: an effect is never handed an event the
         * user didn't ask it to answer to, which is what makes "roll up
         * on shade but don't slide on it" a config line rather than a
         * rule inside an effect. */
        if (!(configs[i].events & COMP_EVENT_BIT(ev->kind)))
            continue;
        modules[i]->window_event(w, ev);
    }
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
