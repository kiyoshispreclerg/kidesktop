#ifndef KIWM_KEYBIND_H
#define KIWM_KEYBIND_H

#include "wm.h"

#include <stdio.h>

/* Configurable global (root-window) keyboard shortcuts -- kiwm.conf's
 * key_* keys, e.g. "key_minimize=Meta+Down". Everything kiwm used to grab
 * with hardcoded keycodes in setup_wm() now goes through this table, so
 * every one of them is rebindable and new actions only need a table row
 * here plus a case in run_action(), not another grab site.
 *
 * Deliberately *only* pure-keyboard globals: the mouse+modifier bindings
 * (mod_cycle/mod_control + drag to move/resize) stay where they are, in
 * events.c's button handling, since those are about which modifier is held
 * during a pointer gesture rather than a discrete key press to dispatch. */

typedef enum {
    KB_WINDOW_NEXT = 0,   /* window switcher, forwards (osd.c) */
    KB_WINDOW_PREV,
    KB_DESKTOP_NEXT,      /* per-output desktop switcher, forwards (osd.c) */
    KB_DESKTOP_PREV,
    KB_MAXIMIZE,          /* toggle maximize/restore of the focused window */
    KB_MINIMIZE,
    KB_TILE_LEFT,         /* half-screen tiling, toggles back off if already there */
    KB_TILE_RIGHT,
    KB_FULLSCREEN,
    KB_SHADE,
    KB_KEEP_ABOVE,
    KB_STICKY,
    KB_CLOSE,
    KB_DESKTOP_GOTO,      /* switch the active output to desktop `arg` (0-based) */
} KeyAction;

/* Parses one kiwm.conf line if it's a key_* binding, returning false (and
 * touching nothing) if `key` isn't one of ours, so config.c can fall
 * through to its own unknown-key warning. Only records the spec string --
 * resolving it to a keycode needs the X connection, which config_load()
 * deliberately runs before (see config.h). */
bool keybind_config_set(const char *key, const char *val);

/* Resolves every binding's spec to modifiers+keycode and installs the
 * passive root grabs. Call once from setup_wm(), after config_load() (for
 * mod_cycle/mod_control, which the default specs refer to symbolically)
 * and after the root event mask is selected. */
void keybind_init(void);

/* Runs whatever action this key press is bound to; returns false if it
 * isn't bound to anything, so events.c can keep handling it. */
bool keybind_handle_key_press(xcb_key_press_event_t *ev);

/* Writes the key_* section of a freshly generated default kiwm.conf
 * (config.c's write_default_config()), so the shipped file always lists
 * exactly the bindings this build actually has. */
void keybind_write_default_config(FILE *f);

/* keysym -> keycode against the server's current keyboard mapping, 0 when
 * the keysym isn't on the keyboard at all. Lives here because this is
 * where the mapping scan already was; menu.c uses it for the few
 * navigation keys its popups answer to. */
xcb_keycode_t keycode_for_keysym(xcb_keysym_t keysym);

#endif /* KIWM_KEYBIND_H */
