/* km_clip - kimemoryd's X side: watches CLIPBOARD (XFixes), fetches every
 * new copy into km_store with where it came from.
 *
 * Only CLIPBOARD: PRIMARY is off in the KiDesktop X server
 * (DisablePrimarySelection) and would only add noise.
 *
 * Each copy is classified from its TARGETS list:
 *   secret  x-kde-passwordManagerHint present, or the source app is in
 *           `exclude`: nothing is read or stored.
 *   rich    app-private targets (office/graphics formats, non-png images,
 *           Qt/KDE internal mime types) or an app in `never_takeover`:
 *           only the canonical representations are fetched (text, html,
 *           uri-list, file-manager copy lists, png).
 *   simple  everything else: *every* target is mirrored, so the item can
 *           later be served exactly as the source app offered it. */
#ifndef KM_CLIP_H
#define KM_CLIP_H

#include <X11/Xlib.h>
#include <stddef.h>

enum { KM_TAKEOVER_SIMPLE, KM_TAKEOVER_ONEXIT, KM_TAKEOVER_NEVER };

typedef struct {
    int takeover;               /* KM_TAKEOVER_* */
    char exclude[512];          /* comma list of WM_CLASS / exe basenames never recorded */
    char never_takeover[512];   /* comma list treated as `rich` whatever their targets */
    size_t max_text;            /* bytes; bigger text reps are skipped */
    size_t max_image;           /* bytes; bigger image reps are skipped */
    size_t max_mirror;          /* bytes; a simple copy mirroring more than this becomes rich */
} KmClipConfig;

/* Needs XFixes; returns 0 if it is missing. Starts capturing whatever the
 * clipboard already holds. */
int  km_clip_init(Display *dpy, const KmClipConfig *cfg);
void km_clip_set_config(const KmClipConfig *cfg);

/* Feed every X event; returns 1 if it was kimemoryd's. */
int  km_clip_handle_event(XEvent *ev);

/* ms until the pending fetch times out (-1: nothing pending), and the
 * call to make once it has. */
int  km_clip_timeout_ms(void);
void km_clip_tick(void);

/* Id of the item the clipboard currently holds (0: unknown/secret). */
unsigned km_clip_current(void);

#endif /* KM_CLIP_H */
