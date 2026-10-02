/* xis_direction - the one place every KiDesktop program asks "is the UI
 * laid out right-to-left?", so kiwm, xispanel, xisserve and kiconf can
 * never disagree about it.
 *
 * Resolution order, first hit wins:
 *
 *   1. $XIS_DIRECTION=ltr|rtl -- per-process override, for testing RTL in
 *      a nested Xephyr without an Arabic/Hebrew locale or translation
 *      installed (`XIS_DIRECTION=rtl kiwm ...`).
 *   2. direction=ltr|rtl in $XDG_CONFIG_HOME/ki-direction.conf -- the
 *      session-wide setting (same naming/location as ki-zones.conf).
 *   3. auto (the default, and what "auto" in either of the above means):
 *      the first language in $LANGUAGE, else the LC_MESSAGES locale, is
 *      checked against the right-to-left scripts' language codes (ar, he,
 *      fa, ur, yi, ...).
 *
 * The answer is computed once and cached: direction is a session-level
 * choice, nothing here is expected to flip at runtime. Call it after
 * xis_i18n_init() (setlocale() must already have run for step 3 to see
 * the real LC_MESSAGES).
 *
 * What a caller does with XIS_DIR_RTL is its own business -- this only
 * decides, it doesn't mirror anything (see RTL_I18N_A11Y_PLAN.md's 3.2
 * for what each program flips). Text itself needs nothing from here:
 * Pango's bidi already orders an Arabic run correctly inside an LTR UI. */
#ifndef XIS_DIRECTION_H
#define XIS_DIRECTION_H

#include <stdbool.h>

typedef enum {
    XIS_DIR_LTR = 0,
    XIS_DIR_RTL = 1,
} XisDirection;

XisDirection xis_direction(void);

static inline bool xis_direction_is_rtl(void)
{
    return xis_direction() == XIS_DIR_RTL;
}

#endif /* XIS_DIRECTION_H */
