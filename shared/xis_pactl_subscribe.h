/* xis_pactl_subscribe - a long-lived `pactl subscribe` child, wrapped as an
 * fd-plus-poll pair for a select()/poll()-style event loop, shared by every
 * program that wants to react to PulseAudio/PipeWire-pulse events instead
 * of polling `pactl list` on a timer: xispanel's audio_events.c and
 * xisserve's pulse.c both used to spawn their own copy of this (same
 * popen(), same O_NONBLOCK fd, same line-buffering across partial reads);
 * this is that code, factored out once both needed it.
 *
 * Each caller owns its own XisPactlSubscribe instance and therefore its own
 * `pactl subscribe` child process -- this does *not* multiplex one
 * subscription across processes. That would need a small broker daemon
 * (its own singleton/socket/reconnect machinery, the kind xisback/xisguard
 * already are) to save spawning a second `pactl subscribe`, which is a
 * thin client that blocks on read() at ~0% CPU and a few MB of RSS: not
 * worth a new daemon for.
 *
 * The child is spawned lazily (on the first _fd() call) and respawned with
 * a backoff after it dies (sound server restarted, pactl missing, no
 * server yet at the caller's own startup) -- see xis_pactl_subscribe_fd().
 *
 * Every event line comes back parsed into (type, facility, index):
 *
 *   Event 'change' on sink #0        -> ("change", "sink", 0)
 *   Event 'new' on sink-input #42    -> ("new", "sink-input", 42)
 *
 * Filtering which (type, facility) pairs matter is left to the callback --
 * xispanel only reacts to "change" on "sink"/"server", xisserve's
 * sticky-default only reacts to "new" on "sink-input"/"source-output".
 */
#ifndef XIS_PACTL_SUBSCRIBE_H
#define XIS_PACTL_SUBSCRIBE_H

#include <stddef.h>

typedef struct XisPactlSubscribe XisPactlSubscribe;

/* `index` is -1 if the line's "#N" suffix didn't parse (never happens for
 * pactl's own output, but a malformed/foreign line is dropped rather than
 * trusted with a garbage index). */
typedef void (*XisPactlSubscribeCb)(const char *type, const char *facility, int index, void *user_data);

XisPactlSubscribe *xis_pactl_subscribe_new(XisPactlSubscribeCb cb, void *user_data);
void xis_pactl_subscribe_free(XisPactlSubscribe *sub);

/* Returns the child's stdout fd, spawning (or respawning, past the
 * backoff) it first if it isn't already running -- call this every time
 * the caller's own loop is about to block (select()/poll()/whatever),
 * exactly like an already-open fd would be used. Returns -1 when there is
 * currently nothing to wait on: no sound server yet, or still backing off
 * after the last child died. Never blocks. */
int xis_pactl_subscribe_fd(XisPactlSubscribe *sub);

/* Call once the fd from xis_pactl_subscribe_fd() is readable. Drains
 * whatever is currently buffered (never blocks), dispatching one `cb` call
 * per complete line; a partial line at EOF-of-buffer is carried over to
 * the next call. Detects the child exiting (EOF) and starts the respawn
 * backoff itself -- the caller doesn't need to notice. */
void xis_pactl_subscribe_poll(XisPactlSubscribe *sub);

#endif /* XIS_PACTL_SUBSCRIBE_H */
