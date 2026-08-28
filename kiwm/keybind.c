/* kiwm - configurable global keyboard shortcuts (kiwm.conf's key_* keys).
 *
 * One table (`binds` below) is the single source of truth for: which
 * actions exist, what each is called in the config file, what its built-in
 * default binding is, which keys get passively grabbed on the root window,
 * and what the generated default kiwm.conf documents. Adding a shortcut is
 * a row here plus a case in run_action() -- nothing else in kiwm has to
 * know about it.
 *
 * Spec syntax is "Mod+Mod+Key", case-insensitive, e.g. "Meta+Down",
 * "Alt+Shift+Tab", "Ctrl+Alt+F1". An empty value leaves the action
 * unbound (and ungrabbed). Besides the literal modifier names, two
 * symbolic ones resolve to whatever kiwm.conf's mod_cycle=/mod_control=
 * are set to: "ModCycle" and "ModControl" -- that's what the built-in
 * defaults use, so flipping mod_cycle=meta moves every default cycling
 * shortcut along with it, exactly as it did when these were hardcoded.
 */
#include "keybind.h"
#include "client.h"
#include "output.h"
#include "osd.h"

#include <X11/keysym.h>

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define KEYBIND_SPEC_MAX 64

typedef struct {
    const char *conf_key;   /* kiwm.conf key name */
    KeyAction action;
    int arg;
    const char *def_spec;   /* built-in default, "" = unbound by default */
    const char *doc;        /* one-line description for the generated config */

    /* Filled in by keybind_config_set() / keybind_init() */
    char spec[KEYBIND_SPEC_MAX];
    bool conf_seen;         /* the config file had a line for this key */
    uint16_t mods;
    xcb_keycode_t keycode;
} Keybind;

/* Only desktops 1..8 get a direct-jump binding of their own below.
 * num_desktops can go up to MAX_DESKTOPS, but a config file listing 32
 * key_desktop_N lines would be noise -- the rest stay reachable via the
 * desktop switcher (key_desktop_next/prev) and via a pager. */

/* Designated initializers (rather than plain positional ones) so the
 * runtime-only fields after `doc` stay implicitly zeroed without every row
 * having to spell them out. */
#define BIND(key, act, argument, def, description) \
    { .conf_key = (key), .action = (act), .arg = (argument), .def_spec = (def), .doc = (description) }

static Keybind binds[] = {
    BIND("key_window_next",  KB_WINDOW_NEXT,  0, "ModCycle+Tab",         "next window (window switcher)"),
    BIND("key_window_prev",  KB_WINDOW_PREV,  0, "ModCycle+Shift+Tab",   "previous window"),
    BIND("key_desktop_next", KB_DESKTOP_NEXT, 0, "ModControl+Tab",       "next virtual desktop on the active output"),
    BIND("key_desktop_prev", KB_DESKTOP_PREV, 0, "ModControl+Shift+Tab", "previous virtual desktop"),
    BIND("key_maximize",     KB_MAXIMIZE,     0, "ModControl+Up",        "maximize / restore the focused window"),
    BIND("key_minimize",     KB_MINIMIZE,     0, "ModControl+Down",      "minimize the focused window"),
    BIND("key_tile_left",    KB_TILE_LEFT,    0, "ModControl+Left",      "tile the focused window to the left half (again = restore)"),
    BIND("key_tile_right",   KB_TILE_RIGHT,   0, "ModControl+Right",     "tile the focused window to the right half (again = restore)"),
    BIND("key_fullscreen",   KB_FULLSCREEN,   0, "",                     "toggle fullscreen on the focused window"),
    BIND("key_shade",        KB_SHADE,        0, "",                     "roll the focused window up into its titlebar / unroll"),
    BIND("key_keep_above",   KB_KEEP_ABOVE,   0, "",                     "toggle always-on-top on the focused window"),
    BIND("key_sticky",       KB_STICKY,       0, "",                     "toggle showing the focused window on every desktop"),
    BIND("key_close",        KB_CLOSE,        0, "Alt+F4",               "close the focused window"),
    BIND("key_desktop_1",    KB_DESKTOP_GOTO, 0, "",                     "switch the active output to desktop 1 (unbound by default)"),
    BIND("key_desktop_2",    KB_DESKTOP_GOTO, 1, "",                     "...desktop 2"),
    BIND("key_desktop_3",    KB_DESKTOP_GOTO, 2, "",                     "...desktop 3"),
    BIND("key_desktop_4",    KB_DESKTOP_GOTO, 3, "",                     "...desktop 4"),
    BIND("key_desktop_5",    KB_DESKTOP_GOTO, 4, "",                     "...desktop 5"),
    BIND("key_desktop_6",    KB_DESKTOP_GOTO, 5, "",                     "...desktop 6"),
    BIND("key_desktop_7",    KB_DESKTOP_GOTO, 6, "",                     "...desktop 7"),
    BIND("key_desktop_8",    KB_DESKTOP_GOTO, 7, "",                     "...desktop 8"),
};

static const int bind_count = (int)(sizeof(binds) / sizeof(binds[0]));

/* ---- spec parsing ---- */

/* Named keysyms kiwm understands in a binding spec. Anything printable and
 * single-character (a, 7, /) is handled without a table entry -- for ASCII
 * those keysyms *are* the character's own code -- and "0x<hex>" passes a
 * raw keysym through, so this only has to cover the non-printing keys
 * people actually bind things to rather than all of keysymdef.h. */
static const struct { const char *name; xcb_keysym_t sym; } key_names[] = {
    { "tab", XK_Tab },              { "escape", XK_Escape },      { "esc", XK_Escape },
    { "return", XK_Return },        { "enter", XK_Return },       { "space", XK_space },
    { "backspace", XK_BackSpace },  { "delete", XK_Delete },      { "insert", XK_Insert },
    { "home", XK_Home },            { "end", XK_End },
    { "pageup", XK_Prior },         { "page_up", XK_Prior },      { "prior", XK_Prior },
    { "pagedown", XK_Next },        { "page_down", XK_Next },     { "next", XK_Next },
    { "up", XK_Up },                { "down", XK_Down },
    { "left", XK_Left },            { "right", XK_Right },
    { "menu", XK_Menu },            { "print", XK_Print },        { "pause", XK_Pause },
    { "f1", XK_F1 },   { "f2", XK_F2 },   { "f3", XK_F3 },   { "f4", XK_F4 },
    { "f5", XK_F5 },   { "f6", XK_F6 },   { "f7", XK_F7 },   { "f8", XK_F8 },
    { "f9", XK_F9 },   { "f10", XK_F10 }, { "f11", XK_F11 }, { "f12", XK_F12 },
};

static xcb_keysym_t keysym_from_name(const char *name)
{
    for (size_t i = 0; i < sizeof(key_names) / sizeof(key_names[0]); i++)
        if (strcasecmp(name, key_names[i].name) == 0)
            return key_names[i].sym;

    if (strncmp(name, "0x", 2) == 0 || strncmp(name, "0X", 2) == 0)
        return (xcb_keysym_t)strtoul(name, NULL, 16);

    /* Single printable ASCII character: its keysym is its own code. */
    if (name[0] > 0x20 && (unsigned char)name[0] < 0x7f && name[1] == '\0')
        return (xcb_keysym_t)name[0];

    return XCB_NO_SYMBOL;
}

/* keysym -> keycode, same full-mapping scan main.c's own keysym_to_keycode()
 * does (kept private to each module rather than shared: this one runs once
 * per configured binding at startup and nowhere else). */
static xcb_keycode_t keycode_for_keysym(xcb_keysym_t keysym)
{
    const xcb_setup_t *setup = xcb_get_setup(wm.conn);
    xcb_keycode_t min_kc = setup->min_keycode;
    xcb_keycode_t max_kc = setup->max_keycode;

    xcb_get_keyboard_mapping_reply_t *reply = xcb_get_keyboard_mapping_reply(
        wm.conn, xcb_get_keyboard_mapping(wm.conn, min_kc, (uint8_t)(max_kc - min_kc + 1)), NULL);
    if (!reply)
        return 0;

    xcb_keysym_t *syms = xcb_get_keyboard_mapping_keysyms(reply);
    int per = reply->keysyms_per_keycode;
    xcb_keycode_t found = 0;

    for (xcb_keycode_t kc = min_kc; kc <= max_kc && !found; kc++)
        for (int i = 0; i < per; i++)
            if (syms[(kc - min_kc) * per + i] == keysym) {
                found = kc;
                break;
            }

    free(reply);
    return found;
}

static bool parse_mod_token(const char *tok, uint16_t *out)
{
    if (strcasecmp(tok, "alt") == 0 || strcasecmp(tok, "mod1") == 0)          { *out = XCB_MOD_MASK_1; return true; }
    if (strcasecmp(tok, "meta") == 0 || strcasecmp(tok, "super") == 0 ||
        strcasecmp(tok, "win") == 0 || strcasecmp(tok, "mod4") == 0)          { *out = XCB_MOD_MASK_4; return true; }
    if (strcasecmp(tok, "ctrl") == 0 || strcasecmp(tok, "control") == 0)      { *out = XCB_MOD_MASK_CONTROL; return true; }
    if (strcasecmp(tok, "shift") == 0)                                        { *out = XCB_MOD_MASK_SHIFT; return true; }
    /* Symbolic: follows kiwm.conf's mod_cycle=/mod_control=. */
    if (strcasecmp(tok, "modcycle") == 0 || strcasecmp(tok, "mod_cycle") == 0)     { *out = wm.mod_cycle; return true; }
    if (strcasecmp(tok, "modcontrol") == 0 || strcasecmp(tok, "mod_control") == 0) { *out = wm.mod_control; return true; }
    return false;
}

/* "Meta+Shift+Tab" -> mods + keycode. Everything before the last '+' is a
 * modifier, the last token is the key itself. Returns false (leaving the
 * binding unbound) on anything unrecognized, with a warning naming the
 * offending token -- one bad line shouldn't cost the rest of the config,
 * same spirit as config.c's own parser. */
static bool parse_spec(const char *spec, const char *conf_key, uint16_t *out_mods, xcb_keycode_t *out_kc)
{
    char buf[KEYBIND_SPEC_MAX];
    snprintf(buf, sizeof(buf), "%s", spec);

    uint16_t mods = 0;
    char *save = NULL;
    char *tok = strtok_r(buf, "+", &save);
    char *keytok = NULL;

    while (tok) {
        while (*tok == ' ' || *tok == '\t') tok++;
        size_t len = strlen(tok);
        while (len > 0 && (tok[len - 1] == ' ' || tok[len - 1] == '\t')) tok[--len] = '\0';

        char *nexttok = strtok_r(NULL, "+", &save);
        if (!nexttok) {
            keytok = tok;   /* last token is the key */
            break;
        }
        uint16_t m;
        if (!parse_mod_token(tok, &m)) {
            fprintf(stderr, "kiwm: config: %s: unknown modifier '%s'\n", conf_key, tok);
            return false;
        }
        mods |= m;
        tok = nexttok;
    }

    if (!keytok || !*keytok) {
        fprintf(stderr, "kiwm: config: %s: no key in '%s'\n", conf_key, spec);
        return false;
    }

    xcb_keysym_t sym = keysym_from_name(keytok);
    if (sym == XCB_NO_SYMBOL) {
        fprintf(stderr, "kiwm: config: %s: unknown key '%s'\n", conf_key, keytok);
        return false;
    }

    xcb_keycode_t kc = keycode_for_keysym(sym);
    if (!kc) {
        fprintf(stderr, "kiwm: config: %s: key '%s' isn't on the current keyboard layout\n", conf_key, keytok);
        return false;
    }

    *out_mods = mods;
    *out_kc = kc;
    return true;
}

/* ---- grabbing ---- */

/* NumLock (Mod2) and CapsLock (Lock) are ordinary modifier bits as far as
 * a passive grab is concerned: a grab registered for exactly `mods` simply
 * doesn't match while either is on, so every shortcut would silently stop
 * working with NumLock lit. Registering the same grab for all four
 * combinations is the standard fix (client.c's grab_button3_with_locks()
 * does the same for the mod+right-click resize grabs). */
static void grab_with_locks(uint16_t mods, xcb_keycode_t keycode)
{
    static const uint16_t locks[] = {
        0,
        XCB_MOD_MASK_LOCK,
        XCB_MOD_MASK_2,
        XCB_MOD_MASK_LOCK | XCB_MOD_MASK_2,
    };
    for (size_t i = 0; i < sizeof(locks) / sizeof(locks[0]); i++)
        xcb_grab_key(wm.conn, 1, wm.root, (uint16_t)(mods | locks[i]), keycode,
                     XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
}

bool keybind_config_set(const char *key, const char *val)
{
    for (int i = 0; i < bind_count; i++) {
        if (strcmp(key, binds[i].conf_key) != 0)
            continue;
        snprintf(binds[i].spec, sizeof(binds[i].spec), "%s", val);
        binds[i].conf_seen = true;
        return true;
    }
    return false;
}

void keybind_init(void)
{
    for (int i = 0; i < bind_count; i++) {
        Keybind *kb = &binds[i];

        /* An empty spec means either "the config didn't mention this key,
         * so use the built-in default" or "the config explicitly cleared
         * it". keybind_config_set() can't tell those apart on its own, so
         * a config line that's present but empty has to win: mark
         * config-set entries by writing the spec (possibly empty) there,
         * and treat a spec still holding its initial '\0' as unset. */
        const char *spec = kb->spec[0] ? kb->spec : (kb->conf_seen ? "" : kb->def_spec);
        if (!spec || !*spec)
            continue;

        if (!parse_spec(spec, kb->conf_key, &kb->mods, &kb->keycode)) {
            kb->keycode = 0;
            continue;
        }
        grab_with_locks(kb->mods, kb->keycode);
    }
}

/* ---- dispatch ---- */

static void run_action(KeyAction action, int arg)
{
    Client *c = wm.focused;

    switch (action) {
    case KB_WINDOW_NEXT:  osd_windows_step(+1); return;
    case KB_WINDOW_PREV:  osd_windows_step(-1); return;
    case KB_DESKTOP_NEXT: osd_desktops_step(+1); return;
    case KB_DESKTOP_PREV: osd_desktops_step(-1); return;

    case KB_DESKTOP_GOTO: {
        /* Same "which screen does this act on" rule the switchers use
         * (kiwm.conf's osd_output_follows_pointer=) -- jumping straight to
         * a desktop is the same kind of effect, just without the hold. */
        int output_idx = output_for_effects();
        if (output_idx >= 0 && arg < wm.num_desktops)
            switch_workspace(output_idx, arg);
        return;
    }

    default:
        break;
    }

    if (!c)
        return;

    switch (action) {
    case KB_MAXIMIZE:   toggle_maximize(c, -1); break;
    case KB_MINIMIZE:   minimize_client(c); break;
    case KB_TILE_LEFT:  toggle_snap_side(c, SNAP_LEFT); break;
    case KB_TILE_RIGHT: toggle_snap_side(c, SNAP_RIGHT); break;
    case KB_FULLSCREEN: toggle_fullscreen(c, -1); break;
    case KB_SHADE:      toggle_shade(c, -1); break;
    case KB_KEEP_ABOVE: toggle_keep_above(c, -1); break;
    case KB_STICKY:     toggle_sticky(c, -1); break;
    case KB_CLOSE:      close_client(c); break;
    default:            break;
    }
}

bool keybind_handle_key_press(xcb_key_press_event_t *ev)
{
    /* Drop the lock bits the grabs above deliberately ignore, then compare
     * only the modifiers a spec can actually name. */
    uint16_t mods = ev->state & (uint16_t)(XCB_MOD_MASK_SHIFT | XCB_MOD_MASK_CONTROL |
                                           XCB_MOD_MASK_1 | XCB_MOD_MASK_3 |
                                           XCB_MOD_MASK_4 | XCB_MOD_MASK_5);

    for (int i = 0; i < bind_count; i++) {
        if (binds[i].keycode && binds[i].keycode == ev->detail && binds[i].mods == mods) {
            run_action(binds[i].action, binds[i].arg);
            return true;
        }
    }
    return false;
}

void keybind_write_default_config(FILE *f)
{
    fprintf(f,
        "# Global keyboard shortcuts. Syntax: Mod+Mod+Key (case-insensitive),\n"
        "# e.g. Meta+Down, Alt+Shift+Tab, Ctrl+Alt+F1. An empty value leaves\n"
        "# the action unbound. Modifiers: Alt, Meta (= Super/Win), Ctrl,\n"
        "# Shift, plus ModCycle/ModControl, which follow mod_cycle= and\n"
        "# mod_control= above -- that's what the defaults below use, so\n"
        "# changing mod_cycle= moves them all along with it. Keys are named\n"
        "# like their X keysyms (Tab, Up, Down, Left, Right, Escape, Return,\n"
        "# space, Home, End, PageUp, PageDown, Delete, F1-F12), a single\n"
        "# printable character (a, 7, /), or a raw 0x<hex> keysym.\n"
        "#\n"
        "# The mouse shortcuts (mod_cycle/mod_control + drag to move, +\n"
        "# right-drag to resize) aren't here -- they follow mod_cycle=/\n"
        "# mod_control= directly.\n");

    for (int i = 0; i < bind_count; i++) {
        fprintf(f, "\n# %s\n%s=%s\n", binds[i].doc, binds[i].conf_key, binds[i].def_spec);
    }
}
