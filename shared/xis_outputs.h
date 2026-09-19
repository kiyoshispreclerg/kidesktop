/* xis_outputs - stable multi-monitor output identity, shared by
 * xisback/xispanel/kiconfd (and written by kiconf, whose Wallpaper/Telas
 * tabs are what actually put these ids in a config in the first place).
 *
 * A plain XRandR connector name ("DP-3", "HDMI-1") is what every one of
 * these programs historically saved in its own config to mean "this
 * wallpaper layer / panel / screen-layout entry belongs to output X" --
 * but that name is not guaranteed stable: which physical connector gets
 * called "DP-3" vs "DP-1" depends on enumeration order, which can change
 * across a reboot, a driver update, or even just the order two identical
 * GPUs/cables came up in. A name saved last session can silently stop
 * matching anything, and every program here used to paper over that with
 * a "the counts still match, so just re-pair them positionally" guess --
 * which is exactly that, a guess: nothing stops the *order* from also
 * having changed at the same time the names did, silently pairing a
 * layer/panel/layout entry with the wrong physical monitor instead of no
 * monitor.
 *
 * This resolves it the way GNOME/KDE do instead: by the monitor's own
 * EDID identity (manufacturer + product + serial, read straight out of
 * its EDID block) rather than by whichever connector it happens to be
 * plugged into right now. An id saved as "edid:LGD:5a07:00000001" keeps
 * matching the same physical monitor across reboots and connector
 * renames, independent of enumeration order, as long as that monitor is
 * plugged in *somewhere*. xis_resolve_output() re-resolves it fresh on
 * every lookup (every render, every layout apply) -- there is nothing to
 * cache or to get stale, and nothing ever gets silently persisted back
 * to a config file just because an EDID id happened to match a different
 * connector name than last time.
 *
 * A saved id that isn't "edid:..." is read as a literal connector name,
 * for configs written before this existed (or hand-edited) -- exact-name
 * matching still comes first for those, and xis_build_output_rename_map()
 * still carries the old positional-by-count fallback as a last resort,
 * unchanged, but *only* for these plain-name entries: an "edid:..." entry
 * that doesn't currently match anything falls back to whatever the
 * caller already does for "output not found" (typically full-screen),
 * never to a positional guess -- silently downgrading a stable id to a
 * shaky one would defeat the point of having it. */
#ifndef XIS_OUTPUTS_H
#define XIS_OUTPUTS_H

#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>
#include <stddef.h>

#define XIS_OUTPUT_STR_LEN 64
#define XIS_MAX_OUTPUTS 32

typedef struct {
    char name[XIS_OUTPUT_STR_LEN]; /* current XRandR connector name */
    char id[XIS_OUTPUT_STR_LEN];   /* "edid:VVV:PPPP:SSSSSSSS", or "" if this output has no (valid) EDID to read */
} XisOutput;

/* Lists every currently connected, actively-driven (has a CRTC) output --
 * connector name plus EDID identity when it has one. Returns the count
 * written to `outs` (capped at `max`, see XIS_MAX_OUTPUTS for a sane
 * bound to pass). */
int xis_list_outputs(Display *dpy, XisOutput *outs, int max);

/* Computes just one output's "edid:..." id. Returns 1 and fills `out`
 * (>= XIS_OUTPUT_STR_LEN bytes) if `output` has a readable EDID property
 * with a valid EDID header; 0 (leaving `out` untouched) otherwise --
 * some virtual/headless outputs never expose one. */
int xis_output_edid_id(Display *dpy, RROutput output, char *out, size_t outsz);

/* THE per-lookup resolver: translates a saved identifier (as read from a
 * config file -- either an "edid:..." id or a literal connector name)
 * into whichever connector currently really is that monitor, checked
 * fresh against the live XRandR state every call. Returns 1 and fills
 * `out_name` (>= XIS_OUTPUT_STR_LEN bytes) if it currently resolves to a
 * connected output; 0 otherwise ("*" -- meaning "the whole virtual
 * screen", never a real output -- always returns 0 too; callers already
 * special-case "*" before this and should keep doing so). Call this
 * before every geometry/placement lookup (every render, not just at
 * load time) -- it does one XRRGetScreenResourcesCurrent() round trip,
 * cheap enough for that, and it's what makes "edid:..." ids need no
 * persisted rename/reconcile step at all: there's nothing to go stale. */
int xis_resolve_output(Display *dpy, const char *saved_id, char *out_name, size_t outsz);

typedef struct {
    char from[XIS_OUTPUT_STR_LEN]; /* one of saved_ids[], verbatim */
    char to[XIS_OUTPUT_STR_LEN];   /* the real connector name to use instead */
} XisOutputRename;

/* Builds a rename map for the plain-literal-name entries in `saved_ids`
 * (`n_saved` of them; duplicates and "*" are fine, both are handled) that
 * don't currently match any connected output's name: exact matches need
 * no entry (xis_apply_output_rename() already returns a name unchanged
 * when it's not in the map) and get skipped, "edid:..." entries are
 * always skipped (see this file's own top comment on why), and whatever
 * literal names are left over get paired positionally, in order, against
 * whatever connected outputs are left over -- but ONLY when their counts
 * still agree (the one case where "just renumber them in order" isn't a
 * coin flip). Intended for a load-time, persist-the-fix step (mirrors
 * what xisback/xispanel already did before this existed) -- not a
 * per-render call like xis_resolve_output(). Returns the number of pairs
 * written to `map` (capped at `max_map`). */
int xis_build_output_rename_map(Display *dpy, const char *const *saved_ids, int n_saved, XisOutputRename *map, int max_map);

const char *xis_apply_output_rename(const XisOutputRename *map, int n_map, const char *saved_id);

#endif /* XIS_OUTPUTS_H */
