/* km_ctl - kimemoryd's control socket,
 * $XDG_RUNTIME_DIR/kimemory-ctl.<display>.sock (mode 0600): one JSON
 * request line in, one response back, connection closed -- the same
 * protocol shape as xisguard-ctl. Used by the kimemory CLI and, later,
 * xisserve's --clipboard page.
 *
 *   PING
 *   STATUS                                 version, count, current, owned, disk_bytes
 *   LIST   scope win query limit           newest first; scope all|window|app|doc,
 *                                          win "active" or a window id
 *   GET    id                              text of a text/link/files item, or the
 *                                          png path of an image
 *   SET    id                              put it back on the clipboard
 *   FAV    id fav
 *   REMOVE id
 *   CLEAR  keep_favs
 *
 * LIST replies {"ok":true,"items":[ then one item object per line, then ]}. */
#ifndef KM_CTL_H
#define KM_CTL_H

#include <X11/Xlib.h>

int  km_ctl_init(Display *dpy, const char *path, const char *version);
int  km_ctl_fd(void);
void km_ctl_accept(void);   /* call when km_ctl_fd() is readable */
void km_ctl_close(void);

#endif /* KM_CTL_H */
