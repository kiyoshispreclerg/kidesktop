/* ks_guard - see ks_guard.h. */
#include "ks_guard.h"
#include "ks_config.h"
#include "ks_log.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define PERIOD_GAP_S   15   /* reports further apart than this start a new period */
#define RECONNECT_S    30
#define MAX_PERIODS    32

static char sock_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
static int fd = -1;
static int subscribed;
static time_t next_connect;
static int refused_logged;
static char buf[8192];
static size_t buf_len;

typedef struct {
    char exe[256];      /* without "|args" */
    char action[24];
    char ev[12];        /* REPORT / REQUEST */
    int pid;
    time_t start, last;
} Period;
static Period periods[MAX_PERIODS];
static int nperiods;

static void period_flush(int i)
{
    Period *p = &periods[i];
    const char *base = ks_exe_base(p->exe);
    if (!ks_excluded(base, base, p->exe)) {
        char range[64], detail[80];
        ks_log_range(range, sizeof(range), p->start, p->last);
        snprintf(detail, sizeof(detail), "%s %s pid=%d", p->action, p->ev, p->pid);
        ks_log_event(p->start, "guard", base, p->exe, -1, "", range, detail);
    }
    periods[i] = periods[--nperiods];
}

static void on_line(const char *line)
{
    if (!subscribed) {
        if (strstr(line, "\"ok\":true")) {
            subscribed = 1;
            refused_logged = 0;
            fprintf(stderr, "kistoryd: subscribed to xisguard events\n");
        } else if (!refused_logged) {
            fprintf(stderr, "kistoryd: xisguard refused SUBSCRIBE (%s) -- is kistoryd in its subscribers=?\n", line);
            refused_logged = 1;
        }
        return;
    }

    /* {"ev":"REPORT","action":"RECORD","pid":N,"exe":"...","ts":N} */
    char ev[12] = "", action[24] = "", exe[256] = "";
    int pid = 0;
    long ts = 0;
    const char *p;
    if ((p = strstr(line, "\"ev\":\"")))     sscanf(p + 6, "%11[^\"]", ev);
    if ((p = strstr(line, "\"action\":\""))) sscanf(p + 10, "%23[^\"]", action);
    if ((p = strstr(line, "\"pid\":")))      pid = atoi(p + 6);
    if ((p = strstr(line, "\"ts\":")))       ts = atol(p + 5);
    if ((p = strstr(line, "\"exe\":\""))) {
        size_t n = 0;
        for (p += 7; *p && *p != '"' && *p != '|' && n + 1 < sizeof(exe); p++) {
            if (*p == '\\' && p[1])
                p++;   /* unescape \" \\ \/ */
            exe[n++] = *p;
        }
        exe[n] = '\0';
    }
    if (!ev[0] || !action[0] || !exe[0])
        return;
    time_t t = ts > 0 ? (time_t)ts : time(NULL);

    for (int i = 0; i < nperiods; i++) {
        Period *q = &periods[i];
        if (strcmp(q->exe, exe) || strcmp(q->action, action) || strcmp(q->ev, ev))
            continue;
        if (t - q->last <= PERIOD_GAP_S) {
            q->last = t;
            q->pid = pid;
            return;
        }
        period_flush(i);
        break;
    }
    if (nperiods == MAX_PERIODS)
        period_flush(0);
    Period *q = &periods[nperiods++];
    memset(q, 0, sizeof(*q));
    snprintf(q->exe, sizeof(q->exe), "%s", exe);
    snprintf(q->action, sizeof(q->action), "%s", action);
    snprintf(q->ev, sizeof(q->ev), "%s", ev);
    q->pid = pid;
    q->start = q->last = t;
}

static void disconnect(void)
{
    if (fd >= 0)
        close(fd);
    fd = -1;
    subscribed = 0;
    buf_len = 0;
    next_connect = time(NULL) + RECONNECT_S;
}

static void try_connect(void)
{
    next_connect = time(NULL) + RECONNECT_S;
    if (!ks_conf.guard_actions[0])
        return;
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return;
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    memcpy(addr.sun_path, sock_path, sizeof(sock_path));
    char req[400];
    snprintf(req, sizeof(req), "{\"cmd\":\"SUBSCRIBE\",\"actions\":\"%s\"}\n", ks_conf.guard_actions);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || write(fd, req, strlen(req)) < 0) {
        close(fd);
        fd = -1;
        return;   /* xisguard not running: try again later */
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
}

int ks_guard_init(int display)
{
    const char *run = getenv("XDG_RUNTIME_DIR");
    snprintf(sock_path, sizeof(sock_path), "%s/xisguard-ctl.%d.sock", run && *run ? run : "/tmp", display);
    try_connect();
    return 1;
}

int ks_guard_fd(void)
{
    return fd;
}

void ks_guard_handle(void)
{
    for (;;) {
        ssize_t n = read(fd, buf + buf_len, sizeof(buf) - 1 - buf_len);
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
            disconnect();   /* xisguard exited or dropped us */
            return;
        }
        if (n < 0)
            return;
        buf_len += (size_t)n;
        buf[buf_len] = '\0';
        char *start = buf, *nl;
        while ((nl = strchr(start, '\n'))) {
            *nl = '\0';
            on_line(start);
            start = nl + 1;
        }
        buf_len = strlen(start);
        memmove(buf, start, buf_len + 1);
        if (buf_len == sizeof(buf) - 1)
            buf_len = 0;   /* a line this long is garbage: drop it */
    }
}

int ks_guard_timeout_ms(void)
{
    time_t now = time(NULL), next = 0;
    if (fd < 0 && ks_conf.guard_actions[0])
        next = next_connect;
    for (int i = 0; i < nperiods; i++) {
        time_t due = periods[i].last + PERIOD_GAP_S + 1;
        if (!next || due < next)
            next = due;
    }
    if (!next)
        return -1;
    return next <= now ? 0 : (int)(next - now) * 1000;
}

void ks_guard_tick(void)
{
    time_t now = time(NULL);
    for (int i = nperiods - 1; i >= 0; i--)
        if (now - periods[i].last > PERIOD_GAP_S)
            period_flush(i);
    if (fd < 0 && now >= next_connect)
        try_connect();
}

void ks_guard_flush(void)
{
    while (nperiods)
        period_flush(nperiods - 1);
}
