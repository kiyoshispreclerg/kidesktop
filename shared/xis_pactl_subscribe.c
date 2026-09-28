/* xis_pactl_subscribe.c - see xis_pactl_subscribe.h. */
#include "xis_pactl_subscribe.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Same backoff xispanel's own audio_events.c used before this was
 * factored out of it: keeps a permanently-broken case (pactl missing, no
 * sound server) from busy-spawning popen() every single loop iteration. */
#define RESPAWN_MS 5000
#define LINE_BUF_SZ 256

struct XisPactlSubscribe {
    XisPactlSubscribeCb cb;
    void *user_data;
    FILE *sub;
    int fd;
    uint64_t next_spawn_ms;
    char line_buf[LINE_BUF_SZ];
    size_t line_len;
};

static uint64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

XisPactlSubscribe *xis_pactl_subscribe_new(XisPactlSubscribeCb cb, void *user_data)
{
    XisPactlSubscribe *sub = calloc(1, sizeof(*sub));
    if (!sub) {
        return NULL;
    }
    sub->cb = cb;
    sub->user_data = user_data;
    sub->fd = -1;
    return sub;
}

static void stop(XisPactlSubscribe *sub)
{
    if (sub->sub) {
        pclose(sub->sub);
        sub->sub = NULL;
    }
    sub->fd = -1;
    sub->line_len = 0;
    sub->next_spawn_ms = mono_ms() + RESPAWN_MS;
}

void xis_pactl_subscribe_free(XisPactlSubscribe *sub)
{
    if (!sub) {
        return;
    }
    if (sub->sub) {
        pclose(sub->sub);
    }
    free(sub);
}

int xis_pactl_subscribe_fd(XisPactlSubscribe *sub)
{
    if (!sub) {
        return -1;
    }
    if (sub->sub) {
        return sub->fd;
    }
    uint64_t now = mono_ms();
    if (now < sub->next_spawn_ms) {
        return -1;
    }
    /* LC_ALL=C: the "Event '...' on ..." words this parses would
     * otherwise come out translated under a non-English locale (same trap
     * every other pactl call in this codebase already documents). */
    sub->sub = popen("LC_ALL=C pactl subscribe 2>/dev/null", "r");
    if (!sub->sub) {
        sub->next_spawn_ms = now + RESPAWN_MS;
        return -1;
    }
    sub->fd = fileno(sub->sub);
    int flags = fcntl(sub->fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(sub->fd, F_SETFL, flags | O_NONBLOCK);
    }
    sub->line_len = 0;
    return sub->fd;
}

/* "Event 'TYPE' on FACILITY #INDEX" -> the three fields the callback gets;
 * INDEX is optional in the format (some facilities, e.g. "server", carry
 * #0 but nothing meaningful) so a missing/unparsed one comes back -1
 * rather than failing the whole line. */
static void handle_line(XisPactlSubscribe *sub, const char *line)
{
    char type[16], facility[24];
    int index = -1;
    if (sscanf(line, "Event '%15[^']' on %23[^ #] #%d", type, facility, &index) < 2) {
        return;
    }
    if (sub->cb) {
        sub->cb(type, facility, index, sub->user_data);
    }
}

void xis_pactl_subscribe_poll(XisPactlSubscribe *sub)
{
    if (!sub || !sub->sub) {
        return;
    }
    for (;;) {
        char chunk[128];
        ssize_t n = read(sub->fd, chunk, sizeof(chunk));
        if (n > 0) {
            for (ssize_t i = 0; i < n; i++) {
                if (chunk[i] == '\n') {
                    sub->line_buf[sub->line_len] = 0;
                    handle_line(sub, sub->line_buf);
                    sub->line_len = 0;
                } else if (sub->line_len + 1 < sizeof(sub->line_buf)) {
                    sub->line_buf[sub->line_len++] = chunk[i];
                }
                /* A line too long for line_buf (never legitimately
                 * happens for this protocol) is silently truncated rather
                 * than desyncing the parser -- overflow bytes are dropped
                 * until the next '\n'. */
            }
            if (n == (ssize_t)sizeof(chunk)) {
                continue; /* more may already be queued */
            }
            return;
        }
        if (n == 0) {
            stop(sub); /* child exited: sound server restarted, pactl killed, etc. */
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return; /* fully drained for now */
        }
        if (errno == EINTR) {
            continue;
        }
        stop(sub);
        return;
    }
}
