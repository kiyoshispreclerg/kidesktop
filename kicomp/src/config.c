/* kicomp.conf -- see config.h. Deliberately a copy of kiwm's parser shape
 * rather than a shared library: two small programs that must not depend on
 * each other (section 14) are better off each owning fifty lines of
 * key=value than sharing a module. */
#include "config.h"
#include "comp.h"
#include "effect.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    /* The one number every effect is written in terms of (see
     * animation.h's comp_anim_duration): one "unit" of animation. Effects
     * ask for a multiple of it -- half for something that should feel
     * instant, double for a long transition -- so that this single key
     * speeds up or slows down the whole desktop coherently, and nothing
     * anywhere hardcodes milliseconds. */
    comp.anim_duration_ms = 160.0;
    comp.effects = true;
    comp.single_drawable = false;
    comp.skip_wm_layers = false;

    /* "auto" means "whatever capability detection picks", which is what
     * anyone who hasn't got a reason to care should leave it as. Naming
     * a backend is for testing one against the other. */
    snprintf(comp.renderer_name, sizeof(comp.renderer_name), "auto");
    snprintf(comp.presenter_name, sizeof(comp.presenter_name), "auto");
}

void config_load(void)
{
    apply_builtin_defaults();

    char path[512];
    config_path(path, sizeof(path));

    FILE *f = fopen(path, "r");
    if (!f)
        return;   /* no config is a perfectly good configuration */

    /* The section currently in force: NULL for the global part at the
     * top of the file, or one effect's settings after an
     * [effect:<name>] header. */
    CompEffectConfig *section = NULL;
    char section_name[32] = "";
    bool section_unknown = false;

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
                fprintf(stderr, "kicomp: config: unterminated section: '%s'\n", line);
                continue;
            }
            *close = '\0';
            char *name = p + 1;

            section = NULL;
            section_unknown = false;
            section_name[0] = '\0';

            if (strncmp(name, "effect:", 7) == 0) {
                snprintf(section_name, sizeof(section_name), "%s", name + 7);
                section = effect_config(section_name);
                if (!section) {
                    /* Naming an effect that doesn't exist is worth saying
                     * out loud -- silently ignoring it is how a typo
                     * becomes "the config doesn't work". */
                    fprintf(stderr, "kicomp: config: no such effect: '%s'\n", section_name);
                    section_unknown = true;
                }
            } else {
                fprintf(stderr, "kicomp: config: unknown section: '[%s]'\n", name);
                section_unknown = true;
            }
            continue;
        }

        char *eq = strchr(p, '=');
        if (!eq) {
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

        if (section_unknown)
            continue;

        if (section) {
            /* Inside [effect:<name>]. Three keys are universal -- whether
             * it runs, how long it takes relative to the global unit, and
             * which events it answers to -- and past those, whatever the
             * module itself understands. */
            if (strcmp(key, "enabled") == 0) {
                section->enabled = atoi(val) != 0;
            } else if (strcmp(key, "events") == 0) {
                section->events = comp_event_mask_parse(val);
            } else if (strcmp(key, "duration") == 0) {
                double f = atof(val);
                if (f < 0.0) f = 0.0;
                if (f > 10.0) f = 10.0;
                section->duration = f;
            } else if (!effect_config_key(section_name, key, val)) {
                fprintf(stderr, "kicomp: config: unknown key '%s' in [effect:%s]\n",
                        key, section_name);
            }
            continue;
        }

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
        } else if (strcmp(key, "renderer") == 0) {
            snprintf(comp.renderer_name, sizeof(comp.renderer_name), "%s", val);
        } else if (strcmp(key, "presenter") == 0) {
            snprintf(comp.presenter_name, sizeof(comp.presenter_name), "%s", val);
        } else {
            fprintf(stderr, "kicomp: config: unknown key '%s'\n", key);
        }
    }

    fclose(f);
}
