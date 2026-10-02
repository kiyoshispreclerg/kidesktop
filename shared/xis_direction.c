/* xis_direction.c - see xis_direction.h. */
#include "xis_direction.h"

#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Languages written right-to-left, as the two/three-letter code that
 * opens a locale name ("ar_EG.UTF-8", "fa", "he_IL", "ckb_IQ", ...).
 * "iw" is Hebrew's pre-1989 code, still seen in old locale aliases. */
static const char *const kRtlLanguages[] = {
    "ar", "he", "iw", "fa", "ur", "yi", "ps", "sd", "ug", "dv", "ckb", "syr",
};

/* "ltr"/"rtl" -> that direction, "auto"/anything else -> -1 (keep looking). */
static int parse_direction(const char *s)
{
    if (!s)
        return -1;
    while (*s == ' ' || *s == '\t')
        s++;
    if (strncasecmp(s, "rtl", 3) == 0)
        return XIS_DIR_RTL;
    if (strncasecmp(s, "ltr", 3) == 0)
        return XIS_DIR_LTR;
    return -1;
}

static int direction_from_config(void)
{
    char path[1024];
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) {
        snprintf(path, sizeof(path), "%s/ki-direction.conf", xdg);
    } else {
        const char *home = getenv("HOME");
        if (!home || !*home)
            return -1;
        snprintf(path, sizeof(path), "%s/.config/ki-direction.conf", home);
    }

    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    int dir = -1;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *s = line;
        while (*s == ' ' || *s == '\t')
            s++;
        if (strncmp(s, "direction", 9) != 0)
            continue;
        s += 9;
        while (*s == ' ' || *s == '\t')
            s++;
        if (*s != '=')
            continue;
        dir = parse_direction(s + 1);
    }
    fclose(f);
    return dir;
}

/* Does a locale/language name ("ar_EG.UTF-8", "he", "fa_IR@...") start
 * with one of kRtlLanguages, followed by a separator or its end? */
static bool language_is_rtl(const char *name)
{
    if (!name || !*name)
        return false;
    size_t len = strcspn(name, "_.@:");
    for (size_t i = 0; i < sizeof(kRtlLanguages) / sizeof(kRtlLanguages[0]); i++)
        if (strlen(kRtlLanguages[i]) == len && strncmp(name, kRtlLanguages[i], len) == 0)
            return true;
    return false;
}

static XisDirection direction_from_locale(void)
{
    /* gettext ignores $LANGUAGE under the C locale, and so does this --
     * otherwise a stray LANGUAGE=ar would mirror an untranslated UI. */
    const char *msgs = setlocale(LC_MESSAGES, NULL);
    bool c_locale = !msgs || strcmp(msgs, "C") == 0 || strcmp(msgs, "POSIX") == 0;

    const char *language = getenv("LANGUAGE");
    if (!c_locale && language && *language)
        return language_is_rtl(language) ? XIS_DIR_RTL : XIS_DIR_LTR;
    return language_is_rtl(msgs) ? XIS_DIR_RTL : XIS_DIR_LTR;
}

XisDirection xis_direction(void)
{
    static int cached = -1;
    if (cached >= 0)
        return (XisDirection)cached;

    int dir = parse_direction(getenv("XIS_DIRECTION"));
    if (dir < 0)
        dir = direction_from_config();
    if (dir < 0)
        dir = (int)direction_from_locale();
    cached = dir;
    return (XisDirection)cached;
}
