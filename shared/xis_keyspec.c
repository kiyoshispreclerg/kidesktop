/* xis_keyspec.c - see xis_keyspec.h. */
#include "xis_keyspec.h"

#include <X11/Xlib.h>
#include <X11/keysym.h>

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

/* Mirrors kiwm/keybind.c's key_names[]: the only key names kiwm accepts
 * besides a single printable character and "0x<hex>". Update both together. */
static const char *const KIWM_KEY_NAMES[] = {
    "Tab", "Escape", "Return", "space", "BackSpace", "Delete", "Insert", "Home", "End",
    "Prior", "Next", "Up", "Down", "Left", "Right", "Menu", "Print", "Pause",
    "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12",
};

static const struct { unsigned int bit; const char *name; } MOD_ORDER[] = {
    { ControlMask, "Ctrl" }, { Mod1Mask, "Alt" }, { Mod4Mask, "Meta" }, { ShiftMask, "Shift" },
};

unsigned int xis_keyspec_modifier_bit(unsigned long keysym)
{
    switch (keysym) {
    case XK_Control_L: case XK_Control_R:
        return ControlMask;
    case XK_Alt_L: case XK_Alt_R: case XK_Meta_L: case XK_Meta_R:
        return Mod1Mask;
    case XK_Super_L: case XK_Super_R: case XK_Hyper_L: case XK_Hyper_R:
        return Mod4Mask;
    case XK_Shift_L: case XK_Shift_R:
        return ShiftMask;
    default:
        return 0;
    }
}

unsigned int xis_keyspec_parse_modifier(const char *name)
{
    if (!strcasecmp(name, "Ctrl") || !strcasecmp(name, "Control"))
        return ControlMask;
    if (!strcasecmp(name, "Alt"))
        return Mod1Mask;
    if (!strcasecmp(name, "Meta") || !strcasecmp(name, "Super") || !strcasecmp(name, "Win"))
        return Mod4Mask;
    if (!strcasecmp(name, "Shift"))
        return ShiftMask;
    return 0;
}

static int append(char *out, size_t outsz, size_t *len, const char *s)
{
    int n = snprintf(out + *len, outsz - *len, "%s%s", *len ? "+" : "", s);
    if (n < 0 || (size_t)n >= outsz - *len)
        return 0;
    *len += (size_t)n;
    return 1;
}

static void key_name(unsigned long keysym, int kiwm, char *buf, size_t bufsz)
{
    /* A printable character as itself, letters capitalized like every
     * default spec ("Meta+E"); all four parsers accept that. '+' would
     * split the spec, so it goes by name. */
    if (keysym > 0x20 && keysym < 0x7f && keysym != '+') {
        snprintf(buf, bufsz, "%c", toupper((int)keysym));
        return;
    }
    const char *name = XKeysymToString((KeySym)keysym);
    if (kiwm) {
        for (size_t i = 0; name && i < sizeof(KIWM_KEY_NAMES) / sizeof(KIWM_KEY_NAMES[0]); i++) {
            if (!strcasecmp(name, KIWM_KEY_NAMES[i])) {
                snprintf(buf, bufsz, "%s", KIWM_KEY_NAMES[i]);
                return;
            }
        }
        snprintf(buf, bufsz, "0x%lx", keysym);
        return;
    }
    if (name)
        snprintf(buf, bufsz, "%s", name);
    else
        snprintf(buf, bufsz, "0x%lx", keysym);
}

int xis_keyspec_format(unsigned int mods, unsigned long keysym, unsigned int modkey,
                       char *out, size_t outsz)
{
    size_t len = 0;
    if (!outsz)
        return 0;
    out[0] = '\0';
    mods &= XIS_KEYSPEC_MODS;
    modkey &= XIS_KEYSPEC_MODS;

    if (modkey && (mods & modkey)) {
        if (!append(out, outsz, &len, "ModKey"))
            return 0;
        mods &= ~modkey;
    }
    for (size_t i = 0; i < sizeof(MOD_ORDER) / sizeof(MOD_ORDER[0]); i++) {
        if ((mods & MOD_ORDER[i].bit) && !append(out, outsz, &len, MOD_ORDER[i].name))
            return 0;
    }
    if (keysym) {
        char name[64];
        key_name(keysym, modkey != 0, name, sizeof(name));
        if (!append(out, outsz, &len, name))
            return 0;
    }
    return len > 0;
}
