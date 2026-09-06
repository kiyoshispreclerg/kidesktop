/* kicomp.conf -- see config.h. Deliberately a copy of kiwm's parser shape
 * rather than a shared library: two small programs that must not depend on
 * each other (section 14) are better off each owning fifty lines of
 * key=value than sharing a module. */
#include "config.h"
#include "comp.h"
#include "effect.h"
#include "shadow.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Per-output settings, kept as a small list rather than merged into
 * CompOutput: outputs come and go with hotplugs, and the configuration
 * for a monitor has to survive it being unplugged and plugged back in. */
#define MAX_OUTPUT_RULES 16

static struct {
    char name[32];      /* RandR output name, or "*" for the default */
    float scale;
} output_rules[MAX_OUTPUT_RULES];
static int output_rule_count;

float config_output_scale(const char *name)
{
    float fallback = -1.0f;

    for (int i = 0; i < output_rule_count; i++) {
        if (strcmp(output_rules[i].name, "*") == 0) {
            fallback = output_rules[i].scale;
            continue;
        }
        if (strcmp(output_rules[i].name, name) == 0)
            return output_rules[i].scale;
    }
    return fallback;
}

static void output_rule_set(const char *name, float scale)
{
    for (int i = 0; i < output_rule_count; i++) {
        if (strcmp(output_rules[i].name, name) == 0) {
            output_rules[i].scale = scale;
            return;
        }
    }
    if (output_rule_count >= MAX_OUTPUT_RULES)
        return;

    snprintf(output_rules[output_rule_count].name,
             sizeof(output_rules[output_rule_count].name), "%s", name);
    output_rules[output_rule_count].scale = scale;
    output_rule_count++;
}

static void config_path(char *out, size_t outsz)
{
    const char *xdg_config = getenv("XDG_CONFIG_HOME");
    if (xdg_config && *xdg_config) {
        snprintf(out, outsz, "%s/kicomp.conf", xdg_config);
        return;
    }

    const char *home = getenv("HOME");
    if (!home || !*home)
        home = "/tmp";
    snprintf(out, outsz, "%s/.config/kicomp.conf", home);
}

static void apply_builtin_defaults(void)
{
    output_rule_count = 0;

    /* The one number every effect is written in terms of (see
     * animation.h's comp_anim_duration): one "unit" of animation. Effects
     * ask for a multiple of it -- half for something that should feel
     * instant, double for a long transition -- so that this single key
     * speeds up or slows down the whole desktop coherently, and nothing
     * anywhere hardcodes milliseconds. */
    comp.anim_duration_ms = 160.0;
    comp.effects = true;
    shadow_config_defaults();
    comp.single_drawable = false;
    comp.skip_wm_layers = false;
    /* On: it is what makes the expo grid able to show a desktop that
     * isn't on screen, and it costs one pixmap per hidden window. */
    /* On: the whole point of a per-CRTC flip is that it happens without
     * anyone asking. It gives the output back the moment there is
     * anything to draw over it (unredirect.h). */
    comp.unredirect = true;

    comp.keep_stowed = true;
    comp.show_stowed_output = COMP_NO_OUTPUT;

    /* "auto" means "whatever capability detection picks", which is what
     * anyone who hasn't got a reason to care should leave it as. Naming
     * a backend is for testing one against the other. */
    snprintf(comp.renderer_name, sizeof(comp.renderer_name), "auto");
    snprintf(comp.presenter_name, sizeof(comp.presenter_name), "auto");
}

/* One pass over the file. `instances_pass` selects which sections are
 * acted on: the first pass takes the globals and the base [effect:<name>]
 * sections, the second the specialized [effect:<name>:<instance>] ones.
 *
 * Two passes, because an instance inherits from its base and the file
 * shouldn't have to be written in any particular order for that to work:
 * whichever way round the sections appear, the base is complete before
 * the instances copy it. */
static void config_pass(FILE *f, bool instances_pass)
{
    CompEffectInstance *section = NULL;
    char output_section[32] = { 0 };
    bool shadow_section = false;
    bool section_unknown = false;
    bool section_skipped = false;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = 0;

        char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '\0' || *p == '#')
            continue;

        if (*p == '[') {
            char *close = strchr(p, ']');
            if (!close) {
                if (!instances_pass)
                    fprintf(stderr, "kicomp: config: unterminated section: '%s'\n", line);
                continue;
            }
            *close = '\0';
            char *name = p + 1;

            section = NULL;
            output_section[0] = '\0';
            shadow_section = false;
            section_unknown = false;
            section_skipped = false;

            if (strncmp(name, "output:", 7) == 0) {
                /* Per-output settings ([output:DP-1], [output:*]). Not an
                 * effect and not shadows: what a *monitor* is, rather than
                 * what the compositor does with it. */
                if (instances_pass) {
                    section_skipped = true;
                } else {
                    snprintf(output_section, sizeof(output_section), "%s", name + 7);
                    if (!output_section[0]) {
                        fprintf(stderr, "kicomp: config: [output:] with no name\n");
                        section_unknown = true;
                    }
                }
                continue;
            }

            if (strcmp(name, "shadow") == 0) {
                /* Not an effect: shadows don't animate, so they are a
                 * section of their own (shadow.h). */
                shadow_section = !instances_pass;
                section_skipped = instances_pass;
                continue;
            }

            if (strncmp(name, "effect:", 7) != 0) {
                if (!instances_pass)
                    fprintf(stderr, "kicomp: config: unknown section: '[%s]'\n", name);
                section_unknown = true;
                continue;
            }

            char *module = name + 7;
            char *instance = strchr(module, ':');
            if (instance)
                *instance++ = '\0';

            /* Each pass ignores the other's sections -- and says nothing
             * about them, or every message would be printed twice. */
            if ((instance != NULL) != instances_pass) {
                section_skipped = true;
                continue;
            }

            section = instance ? effect_named_instance(module, instance)
                               : effect_base_instance(module);
            if (!section) {
                /* Naming an effect that doesn't exist is worth saying out
                 * loud -- silently ignoring it is how a typo becomes "the
                 * config doesn't work". */
                fprintf(stderr, "kicomp: config: no such effect: '%s'\n", module);
                section_unknown = true;
            }
            continue;
        }

        char *eq = strchr(p, '=');
        if (!eq) {
            if (!instances_pass)
                fprintf(stderr, "kicomp: config: skipping malformed line: '%s'\n", line);
            continue;
        }
        *eq = '\0';
        char *key = p;
        char *val = eq + 1;

        size_t klen = strlen(key);
        while (klen > 0 && (key[klen - 1] == ' ' || key[klen - 1] == '\t'))
            key[--klen] = '\0';
        while (*val == ' ' || *val == '\t')
            val++;

        /* A trailing comment ends the value: "origin = pointer  # or
         * window" means pointer. Only after whitespace, so a '#' that is
         * part of a value (a colour, say) survives. */
        for (char *c = val; *c; c++) {
            if (*c == '#' && c > val && (c[-1] == ' ' || c[-1] == '\t')) {
                *c = '\0';
                break;
            }
        }

        /* ...and so does trailing whitespace. Numeric values survived it
         * by luck (atoi stops at the space); a value compared as a
         * string, like origin=, did not -- which is exactly how
         * "origin = pointer   # ..." silently stayed on the default. */
        size_t vlen = strlen(val);
        while (vlen > 0 && (val[vlen - 1] == ' ' || val[vlen - 1] == '\t'))
            val[--vlen] = '\0';

        if (section_unknown || section_skipped)
            continue;

        if (output_section[0]) {
            if (strcmp(key, "scale") == 0) {
                /* `auto` is the same as saying nothing: let the server's
                 * own DPI property decide (TESTS/DPI-PER-OUTPUT.md). */
                if (strcmp(val, "auto") == 0) {
                    output_rule_set(output_section, -1.0f);
                } else {
                    float f = (float)atof(val);
                    if (f < 1.0f) {
                        fprintf(stderr, "kicomp: config: [output:%s] scale %.2f "
                                        "is below 1 -- this only ever shrinks the "
                                        "logical desktop; use xrandr --scale to "
                                        "grow it\n", output_section, f);
                        f = 1.0f;
                    }
                    if (f > 4.0f)
                        f = 4.0f;
                    output_rule_set(output_section, f);
                }
            } else {
                fprintf(stderr, "kicomp: config: unknown key '%s' in [output:%s]\n",
                        key, output_section);
            }
            continue;
        }

        if (shadow_section) {
            if (!shadow_config_key(key, val))
                fprintf(stderr, "kicomp: config: unknown key '%s' in [shadow]\n", key);
            continue;
        }

        if (section) {
            /* Inside an effect section. Five keys are universal -- whether
             * it runs, how long it takes relative to the global unit,
             * which events it answers to, which window types it applies
             * to and how the movement is weighted (easing) -- and past
             * those, whatever the module itself understands. */
            if (strcmp(key, "enabled") == 0) {
                section->enabled = atoi(val) != 0;
            } else if (strcmp(key, "events") == 0) {
                section->events = comp_event_mask_parse(val);
            } else if (strcmp(key, "windows") == 0) {
                section->windows = comp_window_type_mask_parse(val);
            } else if (strcmp(key, "easing") == 0) {
                int curve = comp_easing_parse(val);
                if (curve < 0)
                    fprintf(stderr, "kicomp: config: unknown easing '%s'\n", val);
                else
                    section->easing = (CompEasing)curve;
            } else if (strcmp(key, "duration") == 0) {
                double d = atof(val);
                if (d < 0.0) d = 0.0;
                if (d > 10.0) d = 10.0;
                section->duration = d;
            } else if (!effect_instance_config_key(section, key, val)) {
                fprintf(stderr, "kicomp: config: unknown key '%s' in [effect:%s]\n",
                        key, section->name);
            }
            continue;
        }

        /* Global keys, first pass only. */
        if (instances_pass)
            continue;

        if (strcmp(key, "animation_duration") == 0) {
            double ms = atof(val);
            /* 0 is a legitimate answer: "animate nothing, but keep the
             * effects running so nothing else changes shape". Anything
             * beyond a couple of seconds is a typo, not a preference. */
            if (ms < 0.0) ms = 0.0;
            if (ms > 2000.0) ms = 2000.0;
            comp.anim_duration_ms = ms;
        } else if (strcmp(key, "effects") == 0) {
            comp.effects = atoi(val) != 0;
        } else if (strcmp(key, "single_drawable") == 0) {
            comp.single_drawable = atoi(val) != 0;
        } else if (strcmp(key, "skip_wm_layers") == 0) {
            comp.skip_wm_layers = atoi(val) != 0;
        } else if (strcmp(key, "unredirect_fullscreen") == 0) {
            comp.unredirect = atoi(val) != 0;
        } else if (strcmp(key, "keep_hidden_contents") == 0) {
            comp.keep_stowed = atoi(val) != 0;
        } else if (strcmp(key, "renderer") == 0) {
            snprintf(comp.renderer_name, sizeof(comp.renderer_name), "%s", val);
        } else if (strcmp(key, "presenter") == 0) {
            snprintf(comp.presenter_name, sizeof(comp.presenter_name), "%s", val);
        } else {
            fprintf(stderr, "kicomp: config: unknown key '%s'\n", key);
        }
    }
}

void config_load(void)
{
    apply_builtin_defaults();

    char path[512];
    config_path(path, sizeof(path));

    FILE *f = fopen(path, "r");
    if (!f)
        return;   /* no config is a perfectly good configuration */

    config_pass(f, false);   /* globals, [shadow], base effect sections */
    rewind(f);
    config_pass(f, true);    /* the specialized instances, which copy them */

    fclose(f);

    /* Whatever [shadow] didn't say about unfocused windows follows what
     * it said about focused ones. */
    shadow_config_finish();
}
