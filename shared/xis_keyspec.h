/* xis_keyspec.h - turning a pressed key combination into the key-spec text
 * every KiDesktop hotkey config uses: "<Mod>+<Mod>+...+<Key>", modifiers
 * Ctrl/Alt/Meta/Shift (plus kiwm's symbolic "ModKey"), the key an X11
 * keysym name or a single printable character. That grammar is parsed by
 * xiskeys.c, xispanel/hotkey.c, kiwm/keybind.c and kicomp/src/input.c
 * (each its own copy); this is the writing side, used by xiskeycapture.
 *
 * Only the four modifiers the parsers know are kept -- Lock/NumLock never
 * belong in a spec (every parser grabs with and without them), and
 * Mod3/Mod5 (AltGr, ISO_Level3) have no name in the grammar at all. */
#ifndef XIS_KEYSPEC_H
#define XIS_KEYSPEC_H

#include <stddef.h>
#include <X11/X.h>

/* The modifier bits a spec can express. */
#define XIS_KEYSPEC_MODS (ControlMask | Mod1Mask | Mod4Mask | ShiftMask)

/* Writes `mods`+`keysym` as a spec into out. `modkey` (one X modifier
 * bit, or 0) is kiwm's mod_key=: when set, that bit is written as
 * "ModKey" and the key name is restricted to what kiwm/keybind.c's own
 * keysym_from_name() understands, falling back to its "0x..." form.
 * keysym 0 writes the modifiers alone ("Meta"), the bare-modifier form
 * xispanel's hotkey= accepts for a tap. Returns 0 if nothing fit. */
int xis_keyspec_format(unsigned int mods, unsigned long keysym, unsigned int modkey,
                       char *out, size_t outsz);

/* The X modifier bit a modifier key itself sets (Control_L -> ControlMask,
 * Super_L -> Mod4Mask...), or 0 if `keysym` isn't one of the four. */
unsigned int xis_keyspec_modifier_bit(unsigned long keysym);

/* "Ctrl"/"Alt"/"Meta"/"Shift" (or "ModKey"/...) -> X modifier bit; 0 if unknown. */
unsigned int xis_keyspec_parse_modifier(const char *name);

#endif /* XIS_KEYSPEC_H */
