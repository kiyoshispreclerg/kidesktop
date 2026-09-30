/* ks_guard - kistoryd's xisguard source: which programs captured the
 * screen, recorded input, injected input... as the X server reported them
 * to xisguard. kistoryd subscribes on xisguard's existing control socket
 * ($XDG_RUNTIME_DIR/xisguard-ctl.<display>.sock, SUBSCRIBE) and keeps the
 * connection open; no extra process on either side. xisguard only accepts
 * subscribers listed in its `subscribers=` (kistoryd is by default).
 *
 * The X server reports an action every ~2 s while a program keeps using
 * it, so reports are merged into periods per (program, action, kind):
 *
 *   guard   app: exe basename   exe: path   subject: HH:MM:SS-HH:MM:SS
 *           detail: ACTION REPORT|REQUEST pid=N
 *
 * REQUEST: the program asked for a permission it didn't have yet;
 * REPORT: it used one it had. Actions come from `guard_actions`. */
#ifndef KS_GUARD_H
#define KS_GUARD_H

int  ks_guard_init(int display);
int  ks_guard_fd(void);          /* -1 while not connected */
void ks_guard_handle(void);      /* call when readable */
int  ks_guard_timeout_ms(void);  /* next reconnect / period flush, -1: none */
void ks_guard_tick(void);
void ks_guard_flush(void);

#endif /* KS_GUARD_H */
