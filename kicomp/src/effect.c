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
    &effect_shade,
    &effect_minimize,
    &effect_desktop_wall,
    &effect_smooth_move,
    &effect_dodge,
    &effect_show_windows,
    &effect_expo,
    &effect_visual_bell,
};

#define MODULE_COUNT ((int)(sizeof(modules) / sizeof(modules[0])))

/* The effects running right now, and whether effects are on at all. */
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
    "bell",
};

const char *comp_event_name(CompEventKind kind)
{
    if (kind < 0 || kind >= COMP_EVENT_COUNT)
        return "?";
    return event_names[kind];
}

/* Window type names, in CompWindowType's order. */
static const char *const window_type_names[COMP_WINDOW_TYPE_COUNT] = {
    "unknown", "normal", "dialog", "utility", "toolbar", "splash",
    "menu", "dropdown-menu", "popup-menu", "combo",
    "tooltip", "notification", "dnd",
    "dock", "desktop",
};

/* Group aliases, so the common cases don't have to be spelled out one
 * type at a time. */
static const struct {
    const char *name;
    uint32_t mask;
} window_type_groups[] = {
    { "windows", COMP_WINDOW_BIT(COMP_WINDOW_UNKNOWN) |
                 COMP_WINDOW_BIT(COMP_WINDOW_NORMAL) |
                 COMP_WINDOW_BIT(COMP_WINDOW_DIALOG) |
                 COMP_WINDOW_BIT(COMP_WINDOW_UTILITY) |
                 COMP_WINDOW_BIT(COMP_WINDOW_TOOLBAR) |
                 COMP_WINDOW_BIT(COMP_WINDOW_SPLASH) },
    { "menus",   COMP_WINDOW_BIT(COMP_WINDOW_MENU) |
                 COMP_WINDOW_BIT(COMP_WINDOW_DROPDOWN_MENU) |
                 COMP_WINDOW_BIT(COMP_WINDOW_POPUP_MENU) |
                 COMP_WINDOW_BIT(COMP_WINDOW_COMBO) },
    { "popups",  COMP_WINDOW_BIT(COMP_WINDOW_MENU) |
                 COMP_WINDOW_BIT(COMP_WINDOW_DROPDOWN_MENU) |
                 COMP_WINDOW_BIT(COMP_WINDOW_POPUP_MENU) |
                 COMP_WINDOW_BIT(COMP_WINDOW_COMBO) |
                 COMP_WINDOW_BIT(COMP_WINDOW_TOOLTIP) |
                 COMP_WINDOW_BIT(COMP_WINDOW_NOTIFICATION) |
                 COMP_WINDOW_BIT(COMP_WINDOW_DND) },
};

#define WINDOW_GROUP_COUNT ((int)(sizeof(window_type_groups) / sizeof(window_type_groups[0])))

const char *comp_window_type_name(CompWindowType type)
{
    if (type < 0 || type >= COMP_WINDOW_TYPE_COUNT)
        return "?";
    return window_type_names[type];
}

/* Shared by the two mask parsers: walks a comma/space separated list and
 * hands each name to `lookup`, which returns the bits for it or 0 for a
 * name it doesn't know. */
static uint32_t mask_parse(const char *list, const char *what,
                           uint32_t (*lookup)(const char *name, size_t len))
{
    if (strcmp(list, "all") == 0)
        return 0xffffffffu;
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

        uint32_t bits = lookup(start, len);
        if (bits)
            mask |= bits;
        else
            fprintf(stderr, "kicomp: config: unknown %s '%.*s'\n",
                    what, (int)len, start);
    }

    return mask;
}

static uint32_t window_type_lookup(const char *name, size_t len)
{
    for (int i = 0; i < COMP_WINDOW_TYPE_COUNT; i++)
        if (strlen(window_type_names[i]) == len &&
            strncmp(name, window_type_names[i], len) == 0)
            return COMP_WINDOW_BIT(i);

    for (int i = 0; i < WINDOW_GROUP_COUNT; i++)
        if (strlen(window_type_groups[i].name) == len &&
            strncmp(name, window_type_groups[i].name, len) == 0)
            return window_type_groups[i].mask;

    return 0;
}

static uint32_t event_lookup(const char *name, size_t len)
{
    for (int i = 0; i < COMP_EVENT_COUNT; i++)
        if (strlen(event_names[i]) == len && strncmp(name, event_names[i], len) == 0)
            return COMP_EVENT_BIT(i);
    return 0;
}

uint32_t comp_window_type_mask_parse(const char *list)
{
    return mask_parse(list, "window type", window_type_lookup);
}

uint32_t comp_event_mask_parse(const char *list)
{
    return mask_parse(list, "event", event_lookup);
}

/* A mask as a printable list, for the startup log. */
static void mask_string(uint32_t mask, int count, const char *const *names,
                        char *out, size_t outsz)
{
    out[0] = '\0';

    int set = 0;
    for (int i = 0; i < count; i++)
        if (mask & (1u << i))
            set++;

    if (set == 0) {
        snprintf(out, outsz, "none");
        return;
    }
    if (set == count) {
        snprintf(out, outsz, "all");
        return;
    }

    size_t off = 0;
    for (int i = 0; i < count; i++) {
        if (!(mask & (1u << i)))
            continue;
        int n = snprintf(out + off, outsz - off, "%s%s", off ? "," : "", names[i]);
        if (n < 0 || (size_t)n >= outsz - off)
            break;
        off += (size_t)n;
    }
}

/* ------------------------------------------------------------------ */
/* instances                                                           */
/* ------------------------------------------------------------------ */

/* Base instances (one per module) plus whatever named ones the config
 * asked for, in creation order. Dispatch walks this list. */
static CompEffectInstance *instances;
static bool instances_ready;

static void instance_free_all(void)
{
    CompEffectInstance *i = instances;
    while (i) {
        CompEffectInstance *next = i->next;
        free(i->config);
        free(i);
        i = next;
    }
    instances = NULL;
    instances_ready = false;
}

static void instance_append(CompEffectInstance *inst)
{
    CompEffectInstance **pp = &instances;
    while (*pp)
        pp = &(*pp)->next;
    *pp = inst;
}

static CompEffectInstance *instance_new(const CompEffectModule *m, const char *name)
{
    CompEffectInstance *inst = calloc(1, sizeof(*inst));
    if (!inst)
        return NULL;

    inst->module = m;
    snprintf(inst->name, sizeof(inst->name), "%s", name);
    inst->enabled = m->default_enabled;
    inst->duration = m->default_duration;
    inst->events = m->default_events;
    inst->windows = m->default_windows;
    inst->easing = m->default_easing;

    if (m->config_size) {
        inst->config = calloc(1, m->config_size);
        if (!inst->config) {
            free(inst);
            return NULL;
        }
        if (m->config_defaults)
            m->config_defaults(inst->config);
    }

    instance_append(inst);
    return inst;
}

static void instances_init(void)
{
    if (instances_ready)
        return;
    instances_ready = true;
    for (int i = 0; i < MODULE_COUNT; i++)
        instance_new(modules[i], modules[i]->name);
}

CompEffectInstance *effect_base_instance(const char *module)
{
    instances_init();
    for (CompEffectInstance *i = instances; i; i = i->next)
        if (strcmp(i->name, module) == 0)
            return i;
    return NULL;
}

CompEffectInstance *effect_named_instance(const char *module, const char *instance)
{
    instances_init();

    CompEffectInstance *base = effect_base_instance(module);
    if (!base)
        return NULL;

    char full[64];
    snprintf(full, sizeof(full), "%s:%s", module, instance);

    for (CompEffectInstance *i = instances; i; i = i->next)
        if (strcmp(i->name, full) == 0)
            return i;

    /* Born as a copy of the base, so a specialized instance states only
     * what differs -- everything it stays silent about is whatever the
     * base ended up with. */
    CompEffectInstance *inst = calloc(1, sizeof(*inst));
    if (!inst)
        return NULL;

    *inst = *base;
    inst->next = NULL;
    snprintf(inst->name, sizeof(inst->name), "%s", full);

    if (base->module->config_size && base->config) {
        inst->config = malloc(base->module->config_size);
        if (!inst->config) {
            free(inst);
            return NULL;
        }
        memcpy(inst->config, base->config, base->module->config_size);
    }

    instance_append(inst);
    return inst;
}

bool effect_instance_config_key(CompEffectInstance *inst, const char *key,
                                const char *value)
{
    if (!inst->module->config_key)
        return false;
    return inst->module->config_key(inst->config, key, value);
}

double effect_instance_duration(const CompEffectInstance *inst)
{
    return comp_anim_duration(inst->duration);
}

float effect_ease(const CompEffect *e, float p)
{
    return comp_ease(e->instance ? e->instance->easing : COMP_EASE_OUT, p);
}

void effects_init(void)
{
    instances_init();
    enabled = true;

    /* A module that arms something of its own gets its chance now, with
     * its settings already parsed. */
    for (CompEffectInstance *i = instances; i; i = i->next)
        if (i->enabled && i->module->init)
            i->module->init(i);

    comp_info("effects on, animation unit %.0f ms", comp.anim_duration_ms);
    for (CompEffectInstance *i = instances; i; i = i->next) {
        char events[512], windows[512];
        mask_string(i->events, COMP_EVENT_COUNT, event_names, events, sizeof(events));
        mask_string(i->windows, COMP_WINDOW_TYPE_COUNT, window_type_names,
                    windows, sizeof(windows));
        comp_info("  %-22s %s %4.0f ms %-7s on: %s  for: %s", i->name,
                  i->enabled ? "on " : "off",
                  effect_instance_duration(i), comp_easing_name(i->easing),
                  events, windows);
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
    /* The instance's name, not the module's: with several instances of
     * one effect running at different settings, "scale-out" alone would
     * not say which one this is. */
    comp_log("effect %s started (%.0f ms)",
             e->instance ? e->instance->name : e->ops->name, e->duration);
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

    instances_init();
    comp_log("window 0x%x (%s): %s", w->id, comp_window_type_name(w->type),
             comp_event_name(ev->kind));

    /* The WM's own overlays are never animated by an effect: they are the
     * WM's business, and whether they are composited at all is already a
     * separate decision (--skip-wm-layers). */
    if (w->wm_layer[0] || w->input_only)
        return;

    for (CompEffectInstance *i = instances; i; i = i->next) {
        if (!i->enabled || !i->module->window_event)
            continue;
        /* The two masks are the filter: an effect is never handed an
         * event the user didn't ask it to answer to, nor a kind of window
         * they didn't point it at. That is what makes "roll up on shade
         * but don't slide on it", or "fade tooltips but not panels",
         * config lines rather than rules inside an effect. */
        if (!(i->events & COMP_EVENT_BIT(ev->kind)))
            continue;
        if (!(i->windows & COMP_WINDOW_BIT(w->type)))
            continue;
        i->module->window_event(w, ev, i);
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
    /* Effects first, instances after: a running effect points at the
     * instance it came from (and at its config block), so tearing the
     * instances down first would leave the destroy ops reading freed
     * memory. */
    CompEffect *e = running;
    while (e) {
        CompEffect *next = e->next;
        if (e->ops->destroy)
            e->ops->destroy(e);
        free(e);
        e = next;
    }
    running = NULL;

    instance_free_all();
}
