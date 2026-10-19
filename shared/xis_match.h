/* xis_match - "does this window match this list?", for config values like
 *
 *     windows = normal,dialog,!class:firefox
 *     windows = class:kitty,class:xterm
 *     windows = windows,!dialog
 *
 * A list is comma/space separated terms. A term is a window *type* (whatever
 * the caller's lookup understands: a name or a group of names, as bits), or
 * `class:NAME` (WM_CLASS instance or class, case-insensitive), and either can
 * be negated with a leading `!`.
 *
 *   - positive terms add: a window matches if its type is among the
 *     positive types or its class among the positive classes;
 *   - negative terms win: a window hit by any of them never matches;
 *   - with no positive term at all, everything is the starting set
 *     ("!class:firefox" is "everything but Firefox").
 *
 * `all` and `none` stand alone as the whole list.
 *
 * Parsed once at config load into a fixed-size struct (no allocation, no
 * freeing); testing is a mask AND plus a strcasecmp against at most a handful
 * of short names, and only for the classes the list actually names. The type
 * bits are opaque here: the caller decides what bit 3 means. */
#ifndef XIS_MATCH_H
#define XIS_MATCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define XIS_MATCH_MAX_CLASSES 8
#define XIS_MATCH_NAME_MAX    48

typedef struct {
    uint32_t types;        /* positive type bits (all of them when no positive term) */
    uint32_t neg_types;
    int npos, nneg;
    char pos[XIS_MATCH_MAX_CLASSES][XIS_MATCH_NAME_MAX];
    char neg[XIS_MATCH_MAX_CLASSES][XIS_MATCH_NAME_MAX];
} XisMatch;

/* Type name or group -> bits; 0 when the name is unknown. */
typedef uint32_t (*XisMatchLookup)(const char *name, size_t len, void *ud);

void xis_match_from_mask(XisMatch *m, uint32_t types);

/* Parses `list` into `m` (replacing it). An unknown term is reported on
 * stderr as "<what> '<term>'" and ignored. */
void xis_match_parse(XisMatch *m, const char *list, XisMatchLookup lookup, void *ud,
                     const char *what);

/* `type_bit` is the window's single type bit; `wm_class`/`wm_instance` may
 * be NULL or empty (not known yet). */
bool xis_match_test(const XisMatch *m, uint32_t type_bit, const char *wm_class,
                    const char *wm_instance);

/* Whether any term names a class (so the caller knows the class has to have
 * been read). */
bool xis_match_uses_classes(const XisMatch *m);

#endif /* XIS_MATCH_H */
