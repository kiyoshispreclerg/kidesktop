/*
 * asyncmd.c - run a shell command without ever blocking the main loop,
 * and hand its stdout back to whoever asks for it.
 *
 * Every widget that needs data from an external program used to call
 * popen()/fgets()/pclose() straight from its on_tick, i.e. from inside
 * xispanel.c's single select() loop. That works right up until one of
 * those commands is slow, and then the *whole panel* -- X events, tray,
 * notifications, clicks, autohide, animations -- stops for as long as the
 * command runs. The case that forced this file: `nmcli dev wifi list`
 * re-scans the radio whenever NetworkManager's cached scan is older than
 * ~30s, which on a USB wifi dongle measured 6.3 seconds, every ~36s,
 * against a widget polling it every 3s. Nothing about the panel's own
 * code was slow; it was just waiting on a child process.
 *
 * The fix is structural rather than per-command, since "this particular
 * command is fast enough" is never a property you can rely on (lsblk
 * stalls on a spun-down USB disk, nmcli stalls when NetworkManager is
 * busy, and so on): NOTHING in a widget may wait on a child process.
 * So this file owns the children, and callers only ever read a cached
 * snapshot of the last completed run:
 *
 *   uint64_t gen = asyncmd_get(cmd, refresh_ms, &text);
 *
 * returns immediately, always. `gen` is 0 until the first run finishes,
 * then increments once per completed run, so a caller detects "new data"
 * by comparing it against the generation it last parsed -- no callbacks,
 * and therefore no pointer from a job back into a widget that a config
 * reload could free underneath it (widgets are destroyed and rebuilt
 * wholesale by reload_all_panels(); a callback-based design would need
 * every widget to remember to cancel, which is exactly the kind of
 * per-caller opt-out this codebase avoids in shared mechanisms).
 *
 * Asking is also what schedules: a get() whose cached result is older
 * than `refresh_ms` spawns the next run in the background and returns the
 * *old* result meanwhile. Slots are keyed by the command string, so two
 * unrelated callers asking for the same command (widgets/storage.c's icon
 * and storage_events.c's hotplug toasts both want the same `lsblk`) share
 * one child process and one snapshot instead of forking twice on
 * overlapping schedules, and the shared slot refreshes at the shortest
 * interval any of them asked for.
 *
 * Main-loop integration, same shape as audio_events.c's long-lived
 * child: asyncmd_fds() folds every running job's pipe into the select()
 * set so a finishing command wakes the loop immediately, and
 * asyncmd_poll() drains them. It returns 1 when a run completed, which
 * xispanel.c turns into a schedule_widget_repoll() so the new data is
 * painted right away instead of at the widget's next natural tick.
 */
#include "xispanel.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <unistd.h>

/* Slots are never freed, only reused once every slot is taken and a new
 * command shows up (least-recently-asked-for wins) -- a panel's set of
 * shelled-out commands is small and fixed by its config, so this only
 * ever matters if someone configures more distinct commands than this,
 * in which case the two oldest just stop being cached and re-run more
 * often. Not worth a dynamic table. */
#define ASYNCMD_MAX_JOBS 12
#define ASYNCMD_MAX_OUTPUT 16384

/* A command still running this long after it was spawned is assumed hung
 * (the disk it was stat()ing never spun up, the daemon it queried never
 * answered) and gets SIGKILLed, so one wedged child can't silently freeze
 * a widget's data forever. Deliberately far longer than any command here
 * legitimately takes -- even the 6.3s wifi rescan has room to spare. */
#define ASYNCMD_TIMEOUT_MS 20000

/* Floor between spawn attempts after a fork()/pipe() failure, so a system
 * that's out of fds or processes doesn't get hammered once per tick by
 * every widget at once. Same reasoning as audio_events.c's respawn
 * backoff. */
#define ASYNCMD_RETRY_MS 5000

typedef struct {
    int in_use;
    char cmd[256];

    /* Shortest refresh any caller has asked for since the last spawn.
     * Recomputed per cycle rather than latched, so a slot whose fastest
     * caller disappeared (widget removed by a config reload) relaxes back
     * to whatever the remaining callers want instead of staying pinned at
     * an interval nobody needs any more. */
    unsigned req_ms;
    unsigned refresh_ms;

    pid_t pid; /* -1 when no child is running */
    int fd;    /* child's stdout, O_NONBLOCK; -1 when no child is running */
    uint64_t spawn_ms;
    uint64_t next_spawn_ms; /* backoff floor after a spawn failure */

    char pending[ASYNCMD_MAX_OUTPUT]; /* accumulating across reads */
    size_t pending_len;

    char result[ASYNCMD_MAX_OUTPUT]; /* last *completed* run's stdout */
    size_t result_len;
    uint64_t result_ms;
    uint64_t generation;

    uint64_t last_used_ms; /* for slot eviction, see ASYNCMD_MAX_JOBS */
} AsyncJob;

static AsyncJob g_jobs[ASYNCMD_MAX_JOBS];
static uint64_t g_generation = 0;

/* asyncmd hands a run's stdout back as one flat string, so the line
 * splitting that used to come free from fgets() lives here instead --
 * every consumer so far parses a line-oriented listing, and each one
 * rolling its own splitter would be the same loop three times. */
int asyncmd_next_line(const char **pp, char *out, size_t outsz)
{
    const char *p = *pp;
    if (!p || !*p) {
        return 0;
    }
    const char *nl = strchr(p, '\n');
    size_t len = nl ? (size_t)(nl - p) : strlen(p);
    *pp = nl ? nl + 1 : p + len;
    if (len >= outsz) {
        len = outsz - 1;
    }
    memcpy(out, p, len);
    out[len] = 0;
    while (len > 0 && out[len - 1] == '\r') {
        out[--len] = 0;
    }
    return 1;
}

static AsyncJob *find_slot(const char *cmd)
{
    for (int i = 0; i < ASYNCMD_MAX_JOBS; i++) {
        if (g_jobs[i].in_use && !strcmp(g_jobs[i].cmd, cmd)) {
            return &g_jobs[i];
        }
    }
    return NULL;
}

static AsyncJob *claim_slot(const char *cmd, uint64_t now)
{
    AsyncJob *victim = NULL;
    for (int i = 0; i < ASYNCMD_MAX_JOBS; i++) {
        if (!g_jobs[i].in_use) {
            victim = &g_jobs[i];
            break;
        }
        /* Never evict a slot with a child still attached to it -- that
         * would orphan the pid/fd with nothing left to reap or close. */
        if (g_jobs[i].pid > 0) {
            continue;
        }
        if (!victim || g_jobs[i].last_used_ms < victim->last_used_ms) {
            victim = &g_jobs[i];
        }
    }
    if (!victim) {
        return NULL;
    }
    memset(victim, 0, sizeof(*victim));
    victim->in_use = 1;
    victim->pid = -1;
    victim->fd = -1;
    victim->req_ms = 0;
    victim->last_used_ms = now;
    snprintf(victim->cmd, sizeof(victim->cmd), "%s", cmd);
    return victim;
}

/* Clears a slot's pid once its child is known to be gone -- which is what
 * frees the slot to run again, since a slot with a live pid never spawns.
 *
 * Two different worlds have to work here. xispanel.c sets SIGCHLD to
 * SIG_IGN (so run_detached()'s intermediate fork doesn't leave zombies),
 * and under SIG_IGN the kernel reaps every child itself, which makes
 * waitpid() fail with ECHILD rather than ever returning a pid -- so
 * ECHILD means "already reaped", not an error. The harness in TESTS/ runs
 * this file with SIGCHLD at its default, where waitpid() does return the
 * pid, and returns 0 for a child that closed stdout but hasn't finished
 * exiting yet -- that one stays pending and is retried by reap_finished()
 * on a later poll. WNOHANG throughout: blocking here, however briefly,
 * would reintroduce exactly the stall this file exists to remove. */
static int try_reap(AsyncJob *j)
{
    if (j->pid <= 0) {
        return 1;
    }
    pid_t r = waitpid(j->pid, NULL, WNOHANG);
    if (r == j->pid || (r < 0 && errno == ECHILD)) {
        j->pid = -1;
        return 1;
    }
    return 0;
}

static void detach_child(AsyncJob *j)
{
    if (j->fd >= 0) {
        close(j->fd);
        j->fd = -1;
    }
    try_reap(j);
}

static void reap_finished(void)
{
    for (int i = 0; i < ASYNCMD_MAX_JOBS; i++) {
        AsyncJob *j = &g_jobs[i];
        if (g_jobs[i].in_use && j->pid > 0 && j->fd < 0) {
            try_reap(j);
        }
    }
}

/* Publishes whatever the finished child wrote as the slot's new result.
 * Called on EOF (the child closed stdout, i.e. it is done writing) --
 * partial output from a command killed on timeout is published too, since
 * a truncated snapshot the caller can parse beats no snapshot at all. */
static void complete(AsyncJob *j, uint64_t now)
{
    memcpy(j->result, j->pending, j->pending_len);
    j->result_len = j->pending_len;
    j->result[j->pending_len] = 0;
    j->pending_len = 0;
    j->result_ms = now;
    j->generation = ++g_generation;
}

static void spawn(AsyncJob *j, uint64_t now)
{
    int p[2];
    if (pipe(p) != 0) {
        j->next_spawn_ms = now + ASYNCMD_RETRY_MS;
        return;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(p[0]);
        close(p[1]);
        j->next_spawn_ms = now + ASYNCMD_RETRY_MS;
        return;
    }
    if (pid == 0) {
        close(p[0]);
        if (p[1] != STDOUT_FILENO) {
            dup2(p[1], STDOUT_FILENO);
            close(p[1]);
        }
        /* stderr to /dev/null rather than inherited: these commands warn
         * chattily (nmcli about unknown devices, lsblk about unreadable
         * ones) and it would otherwise all land in the session log. */
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execl("/bin/sh", "sh", "-c", j->cmd, (char *)NULL);
        _exit(127);
    }
    close(p[1]);
    int flags = fcntl(p[0], F_GETFL, 0);
    if (flags >= 0) {
        fcntl(p[0], F_SETFL, flags | O_NONBLOCK);
    }
    j->pid = pid;
    j->fd = p[0];
    j->spawn_ms = now;
    j->pending_len = 0;
    /* This run answers everyone who asked during the previous cycle; the
     * next cycle's interval is recollected from scratch (see req_ms). */
    j->refresh_ms = j->req_ms ? j->req_ms : 1000;
    j->req_ms = 0;
}

uint64_t asyncmd_get(const char *cmd, unsigned refresh_ms, const char **out_text)
{
    if (out_text) {
        *out_text = "";
    }
    if (!cmd || !cmd[0]) {
        return 0;
    }
    uint64_t now = now_ms();

    AsyncJob *j = find_slot(cmd);
    if (!j) {
        j = claim_slot(cmd, now);
        if (!j) {
            return 0;
        }
    }
    j->last_used_ms = now;
    if (refresh_ms < 1) {
        refresh_ms = 1;
    }
    if (j->req_ms == 0 || refresh_ms < j->req_ms) {
        j->req_ms = refresh_ms;
    }

    /* Spawn the next run when the cached snapshot has aged out -- but
     * only if nothing is running already: a command slower than its own
     * refresh interval (the 6.3s wifi scan against a 3s widget tick) must
     * not stack up children faster than they finish. */
    if (j->pid < 0 && now >= j->next_spawn_ms) {
        int stale = j->result_ms == 0 || now - j->result_ms >= (uint64_t)(j->req_ms ? j->req_ms : refresh_ms);
        if (stale) {
            spawn(j, now);
        }
    }

    if (out_text) {
        *out_text = j->result;
    }
    return j->generation;
}

int asyncmd_fds(fd_set *rfds, int maxfd)
{
    for (int i = 0; i < ASYNCMD_MAX_JOBS; i++) {
        int fd = g_jobs[i].in_use ? g_jobs[i].fd : -1;
        if (fd < 0) {
            continue;
        }
        FD_SET(fd, rfds);
        if (fd > maxfd) {
            maxfd = fd;
        }
    }
    return maxfd;
}

int asyncmd_poll(uint64_t now)
{
    int completed = 0;
    reap_finished();

    for (int i = 0; i < ASYNCMD_MAX_JOBS; i++) {
        AsyncJob *j = &g_jobs[i];
        if (!j->in_use || j->fd < 0) {
            continue;
        }
        /* Read until EAGAIN regardless of select() readiness: the fd is
         * O_NONBLOCK, so an unready one costs one cheap failed read, and
         * this keeps the drain correct even on the loop iterations that
         * woke for something else entirely. */
        for (;;) {
            if (j->pending_len >= ASYNCMD_MAX_OUTPUT - 1) {
                /* Output larger than a snapshot slot: keep the first
                 * ASYNCMD_MAX_OUTPUT-1 bytes and stop reading. Every
                 * consumer here parses a line-oriented listing where the
                 * head is the useful part, and a truncated listing is
                 * still parseable -- unlike a wrapped-around buffer. */
                complete(j, now);
                detach_child(j);
                completed = 1;
                break;
            }
            ssize_t n = read(j->fd, j->pending + j->pending_len, ASYNCMD_MAX_OUTPUT - 1 - j->pending_len);
            if (n > 0) {
                j->pending_len += (size_t)n;
                continue;
            }
            if (n == 0) {
                complete(j, now); /* child closed stdout: the run is done */
                detach_child(j);
                completed = 1;
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break; /* drained for now, child still running */
            }
            detach_child(j); /* read error: drop this run, no result published */
            break;
        }

        if (j->pid > 0 && j->fd >= 0 && now - j->spawn_ms >= ASYNCMD_TIMEOUT_MS) {
            kill(j->pid, SIGKILL);
            /* Left attached on purpose: the SIGKILL closes the pipe, so
             * the next poll reads EOF and completes/reaps through the
             * normal path rather than needing a second teardown here. */
        }
    }
    return completed;
}
