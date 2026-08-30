/*
 * kisession - session leader and service supervisor for KiDesktop.
 *
 * Replaces the earlier shell prototype. The reason it stopped being a
 * script: supervising N services needs "tell me when *any* child died",
 * which POSIX sh doesn't have (`wait -n` is a bash/ksh extension, dash
 * rejects it), so shell supervision degenerates into a per-service
 * `kill -0` polling loop. Here SIGCHLD + waitpid(-1, WNOHANG) is exact
 * and instant, and the loop blocks in poll() with no timeout at all when
 * nothing is pending. The XDG autostart scan was the other half: parsing
 * ~50 .desktop files with awk cost ~313 subprocesses and ~485ms at login,
 * versus one fopen per file here.
 *
 * What it owns:
 *   - the session's lifetime. The display manager tracks *this* process,
 *     not the window manager, so a WM crash or a deliberate
 *     `some-wm --replace` doesn't end the session.
 *   - the session's own services, listed in kisession.conf and restarted
 *     with backoff when they die. This list is deliberately separate from
 *     XDG autostart (regular installed apps): these are the desktop's own
 *     pieces, and kiconf edits them in its own tab.
 *   - XDG autostart, run only once the base services are actually up and
 *     answering (see service_ready()) -- an app with a tray icon that
 *     starts before xispanel owns the StatusNotifierWatcher bus name gets
 *     no tray icon for the rest of its run.
 *
 * Config: $XDG_CONFIG_HOME/kisession.conf (fallback ~/.config/kisession.conf)
 *
 *   wm = <command>          which window manager to launch; unset or not
 *                           installed falls back to the first available of
 *                           a built-in candidate list.
 *   SERVICE <name> <0|1>    whitespace/tab separated. A service missing
 *                           from the file keeps its built-in default, so
 *                           adding a service in a later version doesn't
 *                           require the user to edit anything.
 *
 * The file is created with every service at its default if absent.
 *
 * SIGHUP re-reads it and applies the difference: newly enabled services
 * start, newly disabled ones stop, and a changed `wm =` restarts just the
 * window manager. That is what kiconf's "Aplicar" relies on, same pattern
 * as kiconfd and xiskeys -- kiconf writes the config file and signals the
 * daemon, no separate control protocol.
 *
 * SIGTERM/SIGINT shut the session down: autostart apps first, then
 * services in reverse start order, then the WM.
 */

#include <X11/Xatom.h>
#include <X11/Xlib.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define KISESSION_VERSION "0.1.1"

#define MAX_ARGS 16
#define MAX_PIDS_PER_SVC 4
#define MAX_AUTOSTART_PIDS 256
#define MAX_SEEN_ENTRIES 512
#define LINE_LEN 1024

/* A service that stays up this long is considered to have started
 * successfully, so its restart backoff resets. Anything dying sooner is
 * treated as a crash loop and backs off. */
#define BACKOFF_RESET_SEC 10
#define BACKOFF_MAX_SEC 30

/* How long autostart waits for the base services before giving up and
 * launching anyway: a service that is missing, disabled or wedged must
 * not cost the user their autostart apps. */
#define BASE_READY_TIMEOUT_SEC 15

static const char *const WM_CANDIDATES[] = {"kiwm", "compiz", "kwin_x11", "kwin", "openbox", NULL};

/* Polkit agents, first one installed wins. Disabled by default: nothing
 * in the KiDesktop stack triggers polkit on its own, and an agent nobody
 * needs is just another resident process. */
static const char *const POLKIT_CANDIDATES[] = {
    "lxpolkit",
    "polkit-mate-authentication-agent-1",
    "/usr/lib/policykit-1-gnome/polkit-gnome-authentication-agent-1",
    "/usr/libexec/polkit-gnome-authentication-agent-1",
    NULL,
};

typedef enum {
    SVC_ENV,        /* not a process at all (dbus environment bootstrap) */
    SVC_ONESHOT,    /* started once, never restarted */
    SVC_SUPERVISED, /* restarted with backoff when it exits */
    SVC_WM,         /* supervised, plus external --replace adoption */
    SVC_AUTOSTART,  /* the XDG autostart pass */
} SvcKind;

typedef struct {
    const char *name;
    SvcKind kind;
    /* Fixed command line, or NULL when the service resolves its own at
     * runtime (wm, audio, polkit, and the env/autostart pseudo-services). */
    const char *const *argv;
    int default_enabled;
    const char *comment; /* written into the generated default config */
    /* Milliseconds the *initial* startup sequence waits for this service
     * to become ready before starting the next one. 0 = don't wait. Only
     * kiconfd uses it: the WM and the panel read the cursor theme and the
     * X resources it publishes once, at their own startup, so starting
     * them in the same pass is a race they can lose. Restarts later on
     * never wait. */
    int gate_ms;
} SvcDef;

static const char *const ARGV_XISGUARD[] = {"xisguard", NULL};
static const char *const ARGV_KICONFD[] = {"kiconfd", NULL};
static const char *const ARGV_XISBACK[] = {"xisback", NULL};
static const char *const ARGV_XISPANEL[] = {"xispanel", NULL};
static const char *const ARGV_XISKEYS[] = {"xiskeys", NULL};
static const char *const ARGV_LOCKER[] = {"xss-lock", "--", "i3lock", NULL};

/* Start order is table order. xisguard first so the XNOTIFY permission
 * daemon is already arbitrating before anything else touches the display;
 * kiconfd before the visible pieces so they come up already themed. */
static const SvcDef SERVICES[] = {
    {"dbus", SVC_ENV, NULL, 1, "session bus + activation environment", 0},
    {"xisguard", SVC_ONESHOT, ARGV_XISGUARD, 1, "XNOTIFY permissions (exits by itself without the extension)", 0},
    {"kiconfd", SVC_SUPERVISED, ARGV_KICONFD, 1, "theme/cursor/settings daemon", 2000},
    {"xisback", SVC_SUPERVISED, ARGV_XISBACK, 1, "wallpaper", 0},
    {"xispanel", SVC_SUPERVISED, ARGV_XISPANEL, 1, "panel/taskbar", 0},
    {"xiskeys", SVC_SUPERVISED, ARGV_XISKEYS, 1, "global hotkeys", 0},
    {"audio", SVC_ONESHOT, NULL, 1, "pipewire/pulseaudio, only if nothing already started one", 0},
    {"locker", SVC_SUPERVISED, ARGV_LOCKER, 1, "xss-lock + i3lock screen locking", 0},
    {"polkit", SVC_SUPERVISED, NULL, 0, "polkit authentication agent (off: nothing here needs one yet)", 0},
    {"wm", SVC_WM, NULL, 1, "window manager, see 'wm =' above", 0},
    {"autostart", SVC_AUTOSTART, NULL, 1, "XDG autostart entries, started after the services above", 0},
};
#define N_SERVICES ((int)(sizeof(SERVICES) / sizeof(SERVICES[0])))

typedef struct {
    int enabled;
    pid_t pids[MAX_PIDS_PER_SVC];
    int n_pids;
    /* wm only: a pid we did not fork (an external `--replace`), so
     * waitpid() can never see it and it has to be polled instead. */
    int adopted;
    /* Oneshot services are started exactly once per session, and that has
     * to be remembered independently of whether a process is still
     * running: xisguard exiting immediately (no XNOTIFY) and audio
     * declining to start (a server is already up) both leave no pid
     * behind, and without this flag the main loop would read that as
     * "not running yet" and start them again every pass. */
    int oneshot_done;
    int fail_count;
    time_t started_at;
    time_t next_start; /* 0 = may start now; otherwise a backoff deadline */
} SvcState;

static SvcState g_state[N_SERVICES];
static char g_configpath[PATH_MAX];
static char g_wm_cmd[PATH_MAX];

static Display *g_dpy;
static Window g_root;

static volatile sig_atomic_t g_quit = 0;
static volatile sig_atomic_t g_reload = 0;
static volatile sig_atomic_t g_child = 0;
static int g_sigpipe[2] = {-1, -1};

static pid_t g_autostart_pids[MAX_AUTOSTART_PIDS];
static int g_n_autostart_pids = 0;
static int g_autostart_done = 0;
static time_t g_services_started_at = 0;

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' || end[-1] == '\r')) {
        *--end = '\0';
    }
    return s;
}

static const char *env_or(const char *name, const char *fallback)
{
    const char *v = getenv(name);
    return (v && *v) ? v : fallback;
}

/* Resolves `cmd` the way a shell would: an absolute/relative path is
 * checked directly, a bare name is searched along $PATH. Returns 1 and
 * fills `out` with a runnable path, or 0 if nothing was found. */
static int find_in_path(const char *cmd, char *out, size_t outsz)
{
    if (!cmd || !*cmd) {
        return 0;
    }
    if (strchr(cmd, '/')) {
        if (access(cmd, X_OK) == 0) {
            snprintf(out, outsz, "%s", cmd);
            return 1;
        }
        return 0;
    }
    const char *path = env_or("PATH", "/usr/local/bin:/usr/bin:/bin");
    const char *p = path;
    while (*p) {
        const char *colon = strchr(p, ':');
        size_t len = colon ? (size_t)(colon - p) : strlen(p);
        char cand[PATH_MAX];
        if (len == 0) {
            snprintf(cand, sizeof(cand), "./%s", cmd);
        } else {
            snprintf(cand, sizeof(cand), "%.*s/%s", (int)len, p, cmd);
        }
        if (access(cand, X_OK) == 0) {
            snprintf(out, outsz, "%s", cand);
            return 1;
        }
        if (!colon) {
            break;
        }
        p = colon + 1;
    }
    return 0;
}

/* True if a process with exactly this name (/proc/<pid>/comm) is running.
 * Exact match on comm only -- never a substring or command-line match, so
 * it can't be fooled by an unrelated process that merely mentions the
 * name somewhere in its arguments. */
static int process_running(const char *comm)
{
    DIR *d = opendir("/proc");
    if (!d) {
        return 0;
    }
    struct dirent *ent;
    int found = 0;
    while (!found && (ent = readdir(d)) != NULL) {
        if (ent->d_name[0] < '0' || ent->d_name[0] > '9') {
            continue;
        }
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "/proc/%s/comm", ent->d_name);
        FILE *f = fopen(path, "r");
        if (!f) {
            continue;
        }
        char buf[256] = "";
        if (fgets(buf, sizeof(buf), f)) {
            char *nl = strchr(buf, '\n');
            if (nl) {
                *nl = '\0';
            }
            found = (strcmp(buf, comm) == 0);
        }
        fclose(f);
    }
    closedir(d);
    return found;
}

static int svc_index(const char *name)
{
    for (int i = 0; i < N_SERVICES; i++) {
        if (strcmp(SERVICES[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static int svc_enabled(const char *name)
{
    int i = svc_index(name);
    return i >= 0 && g_state[i].enabled;
}

/* ------------------------------------------------------------------ */
/* config                                                              */
/* ------------------------------------------------------------------ */

static void resolve_configpath(void)
{
    /* The explicit precisions keep the compiler from assuming $HOME or
     * $XDG_CONFIG_HOME could fill the whole buffer and leave no room for
     * the suffix -- they can't, but it can't prove that. */
    const char *xdg = getenv("XDG_CONFIG_HOME");
    char dir[PATH_MAX];
    if (xdg && *xdg) {
        snprintf(dir, sizeof(dir), "%s", xdg);
    } else {
        snprintf(dir, sizeof(dir), "%.*s/.config", (int)(sizeof(dir) - sizeof("/.config") - 1),
                 env_or("HOME", "/tmp"));
    }
    mkdir(dir, 0700);
    snprintf(g_configpath, sizeof(g_configpath), "%.*s/kisession.conf",
             (int)(sizeof(g_configpath) - sizeof("/kisession.conf") - 1), dir);
}

static void write_default_config(void)
{
    FILE *f = fopen(g_configpath, "w");
    if (!f) {
        fprintf(stderr, "kisession: could not write '%s': %s\n", g_configpath, strerror(errno));
        return;
    }
    fprintf(f, "# kisession config\n");
    fprintf(f, "#\n");
    fprintf(f, "# wm = <command>       which window manager to launch. Empty or not\n");
    fprintf(f, "#                      installed falls back to the first available of:\n");
    fprintf(f, "#                      ");
    for (int i = 0; WM_CANDIDATES[i]; i++) {
        fprintf(f, "%s%s", i ? " " : "", WM_CANDIDATES[i]);
    }
    fprintf(f, "\n#\n");
    fprintf(f, "# SERVICE <name> <0|1>  KiDesktop's own services. Separate from XDG\n");
    fprintf(f, "#                      autostart (regular installed apps), which has\n");
    fprintf(f, "#                      its own tab in kiconf. A service missing from\n");
    fprintf(f, "#                      this file keeps its built-in default.\n");
    fprintf(f, "\n");
    fprintf(f, "wm =\n");
    fprintf(f, "\n");
    for (int i = 0; i < N_SERVICES; i++) {
        fprintf(f, "SERVICE\t%s\t%d\t# %s\n", SERVICES[i].name, SERVICES[i].default_enabled, SERVICES[i].comment);
    }
    fclose(f);
}

/* Fills `enabled_out` (one entry per service) and `wm_out` from the
 * config file, starting from the built-in defaults so an absent file or
 * an absent line both mean "default". */
static void read_config(int *enabled_out, char *wm_out, size_t wm_sz)
{
    for (int i = 0; i < N_SERVICES; i++) {
        enabled_out[i] = SERVICES[i].default_enabled;
    }
    wm_out[0] = '\0';

    FILE *f = fopen(g_configpath, "r");
    if (!f) {
        return;
    }
    char line[LINE_LEN];
    while (fgets(line, sizeof(line), f)) {
        char *hash = strchr(line, '#');
        if (hash) {
            *hash = '\0';
        }
        char *l = trim(line);
        if (!*l) {
            continue;
        }

        if (strncmp(l, "SERVICE", 7) == 0 && (l[7] == ' ' || l[7] == '\t')) {
            char *p = trim(l + 7);
            char *name = p;
            while (*p && *p != ' ' && *p != '\t') {
                p++;
            }
            if (*p) {
                *p++ = '\0';
            }
            char *val = trim(p);
            int idx = svc_index(name);
            if (idx < 0) {
                fprintf(stderr, "kisession: config: unknown service '%s', ignoring\n", name);
                continue;
            }
            enabled_out[idx] = (val[0] == '0') ? 0 : 1;
            continue;
        }

        if (strncmp(l, "wm", 2) == 0) {
            char *eq = strchr(l, '=');
            if (eq) {
                snprintf(wm_out, wm_sz, "%s", trim(eq + 1));
                continue;
            }
        }
        fprintf(stderr, "kisession: config: skipping unrecognized line: '%s'\n", l);
    }
    fclose(f);
}

/* The WM to actually run: the configured one if installed, else the first
 * installed candidate. Empty output means none is usable at all, which is
 * the one condition that ends the session. */
static int resolve_wm(const char *configured, char *out, size_t outsz)
{
    char found[PATH_MAX];
    if (configured && *configured) {
        if (find_in_path(configured, found, sizeof(found))) {
            snprintf(out, outsz, "%s", configured);
            return 1;
        }
        fprintf(stderr, "kisession: configured wm '%s' not found, falling back\n", configured);
    }
    for (int i = 0; WM_CANDIDATES[i]; i++) {
        if (find_in_path(WM_CANDIDATES[i], found, sizeof(found))) {
            snprintf(out, outsz, "%s", WM_CANDIDATES[i]);
            return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* spawning                                                            */
/* ------------------------------------------------------------------ */

/* Forks and execs argv, returning the child pid (or -1). The child gets
 * default signal dispositions back: everything kisession blocks or traps
 * would otherwise be inherited across exec and confuse the service. */
static pid_t spawn_argv(char *const argv[])
{
    char found[PATH_MAX];
    if (!find_in_path(argv[0], found, sizeof(found))) {
        fprintf(stderr, "kisession: %s not found; skipping\n", argv[0]);
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "kisession: fork failed: %s\n", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        signal(SIGCHLD, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        signal(SIGHUP, SIG_DFL);
        signal(SIGPIPE, SIG_DFL);
        execv(found, argv);
        _exit(127);
    }
    return pid;
}

static pid_t spawn_const(const char *const *argv)
{
    char *args[MAX_ARGS];
    int n = 0;
    while (argv[n] && n < MAX_ARGS - 1) {
        args[n] = (char *)argv[n];
        n++;
    }
    args[n] = NULL;
    return spawn_argv(args);
}

/* Runs a command line through the shell, the way a .desktop Exec is
 * meant to be run. */
static pid_t spawn_shell(const char *cmdline)
{
    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        signal(SIGCHLD, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        signal(SIGHUP, SIG_DFL);
        signal(SIGPIPE, SIG_DFL);
        execl("/bin/sh", "sh", "-c", cmdline, (char *)NULL);
        _exit(127);
    }
    return pid;
}

/* Waits for a short-lived helper we don't want to supervise (e.g.
 * dbus-update-activation-environment) so it can't show up in the main
 * loop's reaper as an unknown child. */
static void run_and_wait(const char *const *argv)
{
    pid_t pid = spawn_const(argv);
    if (pid > 0) {
        int status;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
            /* retry */
        }
    }
}

/* ------------------------------------------------------------------ */
/* service start/stop                                                  */
/* ------------------------------------------------------------------ */

static void schedule_restart(int idx);

static void svc_record_pid(int idx, pid_t pid)
{
    SvcState *st = &g_state[idx];
    if (pid <= 0 || st->n_pids >= MAX_PIDS_PER_SVC) {
        return;
    }
    st->pids[st->n_pids++] = pid;
    st->started_at = time(NULL);
}

/* pipewire/pulseaudio are normally socket-activated by the systemd/elogind
 * user manager, in which case this is a no-op -- spawning a second one
 * would just fight the first over the audio devices. Only start one when
 * nothing is running yet (no user manager, or a from-scratch install). */
static void start_audio(int idx)
{
    if (process_running("pipewire") || process_running("pulseaudio")) {
        fprintf(stderr, "kisession: audio server already running, not starting a second one\n");
        return;
    }
    char found[PATH_MAX];
    if (find_in_path("pipewire", found, sizeof(found))) {
        static const char *const a_pw[] = {"pipewire", NULL};
        static const char *const a_pwp[] = {"pipewire-pulse", NULL};
        static const char *const a_wp[] = {"wireplumber", NULL};
        svc_record_pid(idx, spawn_const(a_pw));
        if (find_in_path("pipewire-pulse", found, sizeof(found))) {
            svc_record_pid(idx, spawn_const(a_pwp));
        }
        if (find_in_path("wireplumber", found, sizeof(found))) {
            svc_record_pid(idx, spawn_const(a_wp));
        }
    } else if (find_in_path("pulseaudio", found, sizeof(found))) {
        static const char *const a_pa[] = {"pulseaudio", "--start", NULL};
        svc_record_pid(idx, spawn_const(a_pa));
    } else {
        fprintf(stderr, "kisession: no audio server found (pipewire/pulseaudio); skipping\n");
    }
}

static void start_polkit(int idx)
{
    char found[PATH_MAX];
    for (int i = 0; POLKIT_CANDIDATES[i]; i++) {
        if (find_in_path(POLKIT_CANDIDATES[i], found, sizeof(found))) {
            const char *argv[] = {POLKIT_CANDIDATES[i], NULL};
            svc_record_pid(idx, spawn_const(argv));
            return;
        }
    }
    fprintf(stderr, "kisession: no polkit agent installed; skipping\n");
    g_state[idx].enabled = 0;
}

static void start_wm(int idx)
{
    char wm[PATH_MAX];
    if (!resolve_wm(g_wm_cmd, wm, sizeof(wm))) {
        fprintf(stderr, "kisession: no window manager found (looked for the configured one, then:");
        for (int i = 0; WM_CANDIDATES[i]; i++) {
            fprintf(stderr, " %s", WM_CANDIDATES[i]);
        }
        fprintf(stderr, ")\n");
        g_quit = 1;
        return;
    }
    fprintf(stderr, "kisession: starting %s\n", wm);
    const char *argv[] = {wm, NULL};
    g_state[idx].adopted = 0;
    svc_record_pid(idx, spawn_const(argv));
}

static void start_service(int idx)
{
    const SvcDef *def = &SERVICES[idx];
    SvcState *st = &g_state[idx];

    if (!st->enabled || st->n_pids > 0) {
        return;
    }
    time_t now = time(NULL);
    if (st->next_start && now < st->next_start) {
        return;
    }
    st->next_start = 0;

    switch (def->kind) {
    case SVC_ENV:
    case SVC_AUTOSTART:
        return; /* handled elsewhere, never a process */
    case SVC_WM:
        start_wm(idx);
        return;
    case SVC_ONESHOT:
        if (st->oneshot_done) {
            return;
        }
        st->oneshot_done = 1;
        if (strcmp(def->name, "audio") == 0) {
            start_audio(idx);
        } else if (def->argv) {
            fprintf(stderr, "kisession: starting %s\n", def->argv[0]);
            svc_record_pid(idx, spawn_const(def->argv));
        }
        return;
    case SVC_SUPERVISED:
        if (strcmp(def->name, "polkit") == 0) {
            start_polkit(idx);
            /* Nothing installed: start_polkit() turned the service off
             * rather than leaving a start that can never succeed to be
             * retried forever. */
            return;
        }
        if (def->argv) {
            /* Every element must exist: the locker is xss-lock *and*
             * i3lock, and half of it is worse than none. */
            char found[PATH_MAX];
            for (int i = 0; def->argv[i]; i++) {
                if (def->argv[i][0] == '-') {
                    continue;
                }
                if (!find_in_path(def->argv[i], found, sizeof(found))) {
                    fprintf(stderr, "kisession: %s not available (%s missing); skipping\n", def->name, def->argv[i]);
                    st->enabled = 0;
                    return;
                }
            }
            fprintf(stderr, "kisession: starting %s\n", def->argv[0]);
            pid_t pid = spawn_const(def->argv);
            if (pid > 0) {
                svc_record_pid(idx, pid);
            } else {
                /* Back off instead of retrying instantly: a fork failure
                 * or a binary that just vanished would otherwise spin the
                 * loop as fast as the CPU allows. */
                schedule_restart(idx);
            }
        }
        return;
    }
}

static void stop_service(int idx, int sig)
{
    SvcState *st = &g_state[idx];
    for (int i = 0; i < st->n_pids; i++) {
        if (st->pids[i] > 0) {
            kill(st->pids[i], sig);
        }
    }
}

/* ------------------------------------------------------------------ */
/* EWMH: who currently owns the window manager role                    */
/* ------------------------------------------------------------------ */

/* The window a running WM advertises itself through, or 0 if nobody is
 * managing the display. Per EWMH the property must exist on *both* the
 * root and that window, pointing at the window itself -- checking the
 * second one is what tells a live WM apart from a stale property left
 * behind by one that died without cleaning up. Reading a property off a
 * destroyed window raises BadWindow, which is why the ignoring error
 * handler installed in main() is not optional here. */
static Window wm_check_window(void)
{
    if (!g_dpy) {
        return 0;
    }
    Atom check = XInternAtom(g_dpy, "_NET_SUPPORTING_WM_CHECK", True);
    if (check == None) {
        return 0;
    }

    Atom type;
    int format;
    unsigned long nitems, after;
    unsigned char *data = NULL;
    if (XGetWindowProperty(g_dpy, g_root, check, 0, 1, False, XA_WINDOW, &type, &format, &nitems, &after, &data) !=
            Success ||
        !data) {
        return 0;
    }
    Window win = *(Window *)(void *)data;
    XFree(data);
    if (!win) {
        return 0;
    }

    data = NULL;
    if (XGetWindowProperty(g_dpy, win, check, 0, 1, False, XA_WINDOW, &type, &format, &nitems, &after, &data) !=
            Success ||
        !data) {
        return 0;
    }
    Window back = *(Window *)(void *)data;
    XFree(data);
    return back == win ? win : 0;
}

/* Is *someone* managing the display? Deliberately separate from
 * current_wm_pid() below: kiwm sets _NET_SUPPORTING_WM_CHECK but not
 * _NET_WM_PID (checked against kiwm/atoms.c), so requiring a pid here
 * would mean the autostart readiness gate never passed under the
 * KiDesktop WM itself and always waited out its full timeout. */
static int wm_present(void)
{
    return wm_check_window() != 0;
}

/* Best-effort pid of whatever is managing the display, via the check
 * window's _NET_WM_PID. 0 when it can't be told -- no WM, or (commonly)
 * a WM that simply doesn't publish it. This is what lets a
 * `some-wm --replace` typed in a terminal be adopted rather than fought:
 * such a WM isn't our child, so waitpid() will never see it. */
static pid_t current_wm_pid(void)
{
    Window win = wm_check_window();
    if (!win) {
        return 0;
    }
    Atom netpid = XInternAtom(g_dpy, "_NET_WM_PID", True);
    if (netpid == None) {
        return 0;
    }
    Atom type;
    int format;
    unsigned long nitems, after;
    unsigned char *data = NULL;
    if (XGetWindowProperty(g_dpy, win, netpid, 0, 1, False, XA_CARDINAL, &type, &format, &nitems, &after, &data) !=
            Success ||
        !data) {
        return 0;
    }
    pid_t pid = (pid_t) * (unsigned long *)(void *)data;
    XFree(data);
    return pid;
}

/* Xlib's default error handler exits the process. kisession queries
 * windows that belong to other programs and may vanish mid-query (the WM
 * check window above, most of all), so a BadWindow must be a shrug, not
 * the end of the session. */
static int x_error_ignore(Display *dpy, XErrorEvent *ev)
{
    (void)dpy;
    (void)ev;
    return 0;
}

/* ------------------------------------------------------------------ */
/* readiness probes (what gates XDG autostart)                         */
/* ------------------------------------------------------------------ */

static int runtime_socket_exists(const char *name)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", env_or("XDG_RUNTIME_DIR", "/tmp"), name);
    struct stat sb;
    return stat(path, &sb) == 0 && S_ISSOCK(sb.st_mode);
}

/* kiconfd writes Xcursor.theme into RESOURCE_MANAGER as part of its first
 * apply, so this reports "the settings are live", not merely "the process
 * exists". Read straight off the root window -- no xrdb subprocess. */
static int resource_manager_has_cursor(void)
{
    if (!g_dpy) {
        return 1; /* can't tell; don't hold autostart hostage to it */
    }
    Atom resman = XInternAtom(g_dpy, "RESOURCE_MANAGER", True);
    if (resman == None) {
        return 0;
    }
    Atom type;
    int format;
    unsigned long nitems, after;
    unsigned char *data = NULL;
    if (XGetWindowProperty(g_dpy, g_root, resman, 0, 65536, False, XA_STRING, &type, &format, &nitems, &after, &data) !=
            Success ||
        !data) {
        return 0;
    }
    int found = strstr((const char *)data, "Xcursor.theme") != NULL;
    XFree(data);
    return found;
}

/* Whether the tray's StatusNotifierWatcher name is claimed yet. This is
 * the probe that actually matters for autostart: an app with a tray icon
 * checks for this name when *it* starts, and one that misses it falls
 * back to the XEmbed tray (which nothing owns here) and shows no icon at
 * all for the rest of its run. Uses dbus-send rather than linking libdbus
 * for one question asked a handful of times at login. */
static int tray_watcher_up(void)
{
    char found[PATH_MAX];
    if (!find_in_path("dbus-send", found, sizeof(found))) {
        return 1; /* can't ask; fall back to the socket check alone */
    }
    pid_t pid = fork();
    if (pid < 0) {
        return 1;
    }
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
        }
        execl(found, "dbus-send", "--session", "--print-reply", "--dest=org.freedesktop.DBus",
              "/org/freedesktop/DBus", "org.freedesktop.DBus.GetNameOwner",
              "string:org.kde.StatusNotifierWatcher", (char *)NULL);
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        /* retry */
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/* "Up and answering", not "we called fork on it": each probe checks what
 * a *client* of that service would look for, since that is what an
 * autostart app racing us would find. */
static int service_ready(const char *name)
{
    if (strcmp(name, "dbus") == 0) {
        const char *addr = getenv("DBUS_SESSION_BUS_ADDRESS");
        return addr && *addr;
    }
    if (strcmp(name, "kiconfd") == 0) {
        return resource_manager_has_cursor();
    }
    if (strcmp(name, "xisback") == 0) {
        return runtime_socket_exists("xisback.sock");
    }
    if (strcmp(name, "xispanel") == 0) {
        return runtime_socket_exists("xispanel-ctl.sock") && tray_watcher_up();
    }
    if (strcmp(name, "audio") == 0) {
        return process_running("pipewire") || process_running("pulseaudio");
    }
    if (strcmp(name, "wm") == 0) {
        /* Autostart apps map windows immediately; a WM already managing
         * the display keeps them from coming up undecorated. */
        return wm_present();
    }
    return 1;
}

static const char *const BASE_FOR_AUTOSTART[] = {"dbus", "xisback", "xispanel", "kiconfd", "audio", "wm", NULL};

static int base_services_ready(void)
{
    for (int i = 0; BASE_FOR_AUTOSTART[i]; i++) {
        if (!svc_enabled(BASE_FOR_AUTOSTART[i])) {
            continue;
        }
        if (!service_ready(BASE_FOR_AUTOSTART[i])) {
            return 0;
        }
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* XDG autostart                                                       */
/* ------------------------------------------------------------------ */

/* Reads one key from a .desktop file's [Desktop Entry] group only (a
 * "[Desktop Action ...]" group must not answer for it), matching the
 * unlocalized key exactly -- a "Name[pt_BR]" never satisfies "Name".
 * Returns 1 when found. */
static int desktop_get(const char *path, const char *key, char *out, size_t outsz)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        return 0;
    }
    int in_entry = 0;
    int found = 0;
    size_t keylen = strlen(key);
    char line[LINE_LEN];
    while (!found && fgets(line, sizeof(line), f)) {
        char *l = trim(line);
        if (l[0] == '[') {
            in_entry = (strcmp(l, "[Desktop Entry]") == 0);
            continue;
        }
        if (!in_entry || l[0] == '#' || !*l) {
            continue;
        }
        char *eq = strchr(l, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        char *k = trim(l);
        if (strlen(k) == keylen && strcmp(k, key) == 0) {
            snprintf(out, outsz, "%s", trim(eq + 1));
            found = 1;
        }
    }
    fclose(f);
    return found;
}

/* True if a ";"-separated desktop-name list (OnlyShowIn/NotShowIn)
 * mentions KiDesktop. */
static int list_has_kidesktop(const char *list)
{
    const char *p = list;
    while (*p) {
        const char *semi = strchr(p, ';');
        size_t len = semi ? (size_t)(semi - p) : strlen(p);
        if (len == strlen("KiDesktop") && strncmp(p, "KiDesktop", len) == 0) {
            return 1;
        }
        if (!semi) {
            break;
        }
        p = semi + 1;
    }
    return 0;
}

/* Implements the subset of the Desktop Entry spec that actually gates
 * autostart, plus X-GNOME-Autostart-enabled -- not in the spec, but
 * honoured by every session out there, and the key the GNOME control
 * panel writes when a user disables an entry there. */
static int autostart_entry_wanted(const char *path)
{
    char buf[LINE_LEN];

    if (desktop_get(path, "Type", buf, sizeof(buf)) && strcmp(buf, "Application") != 0) {
        return 0;
    }
    if (desktop_get(path, "Hidden", buf, sizeof(buf)) && strcmp(buf, "true") == 0) {
        return 0;
    }
    if (desktop_get(path, "X-GNOME-Autostart-enabled", buf, sizeof(buf)) && strcmp(buf, "false") == 0) {
        return 0;
    }
    if (desktop_get(path, "OnlyShowIn", buf, sizeof(buf)) && !list_has_kidesktop(buf)) {
        return 0;
    }
    if (desktop_get(path, "NotShowIn", buf, sizeof(buf)) && list_has_kidesktop(buf)) {
        return 0;
    }
    if (desktop_get(path, "TryExec", buf, sizeof(buf))) {
        char found[PATH_MAX];
        if (!find_in_path(buf, found, sizeof(found))) {
            return 0;
        }
    }
    return 1;
}

/* Strips the Exec field codes (%f %F %u %U %d %D %n %N %i %c %k %v %m),
 * none of which mean anything for an autostart entry -- there is no file
 * or URL to hand it -- and unescapes "%%". */
static void strip_field_codes(char *s)
{
    char *out = s;
    for (char *p = s; *p; p++) {
        if (*p != '%') {
            *out++ = *p;
            continue;
        }
        char c = p[1];
        if (c == '%') {
            *out++ = '%';
            p++;
        } else if (strchr("fFuUdDnNickvm", c)) {
            p++; /* drop both characters */
        } else {
            *out++ = *p;
        }
    }
    *out = '\0';
}

typedef struct {
    char names[MAX_SEEN_ENTRIES][NAME_MAX + 1];
    int n;
} SeenSet;

static int seen_add(SeenSet *s, const char *name)
{
    for (int i = 0; i < s->n; i++) {
        if (strcmp(s->names[i], name) == 0) {
            return 0; /* already handled by a higher-priority directory */
        }
    }
    if (s->n < MAX_SEEN_ENTRIES) {
        snprintf(s->names[s->n], sizeof(s->names[0]), "%s", name);
        s->n++;
    }
    return 1;
}

static void autostart_scan_dir(const char *dir, SeenSet *seen)
{
    DIR *d = opendir(dir);
    if (!d) {
        return;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        size_t len = strlen(ent->d_name);
        if (len < 9 || strcmp(ent->d_name + len - 8, ".desktop") != 0) {
            continue;
        }
        if (!seen_add(seen, ent->d_name)) {
            continue;
        }
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        struct stat sb;
        if (stat(path, &sb) != 0 || !S_ISREG(sb.st_mode)) {
            continue;
        }
        if (!autostart_entry_wanted(path)) {
            continue;
        }
        char exec[LINE_LEN];
        if (!desktop_get(path, "Exec", exec, sizeof(exec)) || !exec[0]) {
            fprintf(stderr, "kisession: autostart: '%s' has no Exec, skipping\n", ent->d_name);
            continue;
        }
        strip_field_codes(exec);
        fprintf(stderr, "kisession: autostart: %s: %s\n", ent->d_name, exec);
        pid_t pid = spawn_shell(exec);
        if (pid > 0 && g_n_autostart_pids < MAX_AUTOSTART_PIDS) {
            g_autostart_pids[g_n_autostart_pids++] = pid;
        }
    }
    closedir(d);
}

/* ~/.config/autostart wins over the same basename in $XDG_CONFIG_DIRS,
 * per spec: that is how a user disables (Hidden=true) a system entry. */
static void run_autostart(void)
{
    SeenSet seen;
    seen.n = 0;

    char userdir[PATH_MAX];
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) {
        snprintf(userdir, sizeof(userdir), "%s/autostart", xdg);
    } else {
        snprintf(userdir, sizeof(userdir), "%s/.config/autostart", env_or("HOME", "/tmp"));
    }
    autostart_scan_dir(userdir, &seen);

    char dirs[LINE_LEN];
    snprintf(dirs, sizeof(dirs), "%s", env_or("XDG_CONFIG_DIRS", "/etc/xdg"));
    char *save = NULL;
    for (char *tok = strtok_r(dirs, ":", &save); tok; tok = strtok_r(NULL, ":", &save)) {
        if (!*tok) {
            continue;
        }
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/autostart", tok);
        autostart_scan_dir(path, &seen);
    }
}

/* Called from the main loop until it fires exactly once. */
static void maybe_run_autostart(void)
{
    if (g_autostart_done || !svc_enabled("autostart") || g_quit) {
        return;
    }
    int ready = base_services_ready();
    if (!ready) {
        if (time(NULL) - g_services_started_at < BASE_READY_TIMEOUT_SEC) {
            return;
        }
        fprintf(stderr, "kisession: base services not all ready after %ds, starting autostart anyway\n",
                BASE_READY_TIMEOUT_SEC);
    }
    g_autostart_done = 1;
    run_autostart();
}

/* ------------------------------------------------------------------ */
/* signals                                                             */
/* ------------------------------------------------------------------ */

static void handle_signal(int sig)
{
    int saved = errno;
    switch (sig) {
    case SIGCHLD:
        g_child = 1;
        break;
    case SIGHUP:
        g_reload = 1;
        break;
    default:
        g_quit = 1;
        break;
    }
    /* Wake the poll() in the main loop. A self-pipe rather than relying on
     * EINTR: with SA_RESTART semantics differing between libcs, this is
     * the portable way to make a blocking wait return promptly. */
    if (g_sigpipe[1] >= 0) {
        char c = 1;
        ssize_t n = write(g_sigpipe[1], &c, 1);
        (void)n;
    }
    errno = saved;
}

static void install_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGCHLD, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
}

/* ------------------------------------------------------------------ */
/* reaping + restart policy                                            */
/* ------------------------------------------------------------------ */

/* Which service owns `pid`, and which slot within it. -1 if it's not a
 * service child (an autostart app, or a helper). */
static int find_pid(pid_t pid, int *slot_out)
{
    for (int i = 0; i < N_SERVICES; i++) {
        for (int j = 0; j < g_state[i].n_pids; j++) {
            if (g_state[i].pids[j] == pid) {
                if (slot_out) {
                    *slot_out = j;
                }
                return i;
            }
        }
    }
    return -1;
}

static void drop_pid(int idx, int slot)
{
    SvcState *st = &g_state[idx];
    for (int j = slot; j < st->n_pids - 1; j++) {
        st->pids[j] = st->pids[j + 1];
    }
    st->n_pids--;
}

/* A service that stayed up long enough is treated as healthy, so its next
 * crash starts from a 1s delay again rather than from wherever an earlier
 * crash loop left the counter. */
static void schedule_restart(int idx)
{
    SvcState *st = &g_state[idx];
    time_t now = time(NULL);
    if (st->started_at && now - st->started_at >= BACKOFF_RESET_SEC) {
        st->fail_count = 0;
    }
    int delay = 1 << (st->fail_count < 5 ? st->fail_count : 5);
    if (delay > BACKOFF_MAX_SEC) {
        delay = BACKOFF_MAX_SEC;
    }
    if (st->fail_count < 100) {
        st->fail_count++;
    }
    st->next_start = now + delay;
    fprintf(stderr, "kisession: %s exited, restarting in %ds\n", SERVICES[idx].name, delay);
}

static void handle_wm_exit(int idx)
{
    SvcState *st = &g_state[idx];
    if (g_quit || g_reload) {
        return;
    }
    /* Tell a crash apart from an external `some-wm --replace`: if someone
     * else already owns the WM role, adopt and supervise them instead of
     * fighting the display back to the configured default. */
    pid_t other = current_wm_pid();
    if (other > 0 && kill(other, 0) == 0) {
        fprintf(stderr, "kisession: detected external WM replace (pid %ld), adopting it\n", (long)other);
        st->pids[0] = other;
        st->n_pids = 1;
        st->adopted = 1;
        st->started_at = time(NULL);
        st->next_start = 0;
        return;
    }
    if (wm_present()) {
        /* Someone is managing the display but doesn't publish a pid
         * (kiwm among them). Starting "our" WM now would fight a live
         * one, so adopt it anyway and track it by presence instead --
         * pids[0] == 0 marks exactly that in check_adopted_wm(). */
        fprintf(stderr, "kisession: another window manager is running (no pid published), adopting it\n");
        st->pids[0] = 0;
        st->n_pids = 1;
        st->adopted = 1;
        st->started_at = time(NULL);
        st->next_start = 0;
        return;
    }
    schedule_restart(idx);
}

static void reap_children(void)
{
    pid_t pid;
    int status;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        int slot = 0;
        int idx = find_pid(pid, &slot);
        if (idx < 0) {
            /* An autostart app, or a helper we already waited for. */
            for (int i = 0; i < g_n_autostart_pids; i++) {
                if (g_autostart_pids[i] == pid) {
                    g_autostart_pids[i] = g_autostart_pids[--g_n_autostart_pids];
                    break;
                }
            }
            continue;
        }
        drop_pid(idx, slot);
        if (g_quit) {
            continue;
        }
        switch (SERVICES[idx].kind) {
        case SVC_WM:
            g_state[idx].adopted = 0;
            handle_wm_exit(idx);
            break;
        case SVC_SUPERVISED:
            if (g_state[idx].n_pids == 0) {
                schedule_restart(idx);
            }
            break;
        case SVC_ONESHOT:
            /* xisguard exiting is the normal outcome on a server without
             * XNOTIFY, and restarting it would just repeat that. Audio
             * helpers are the user manager's business, not ours. */
            break;
        case SVC_ENV:
        case SVC_AUTOSTART:
            break;
        }
    }
}

/* The adopted WM isn't our child, so waitpid() can never report it --
 * poll it instead, which is why the main loop caps its timeout while an
 * adoption is in effect. */
static void check_adopted_wm(void)
{
    int idx = svc_index("wm");
    SvcState *st = &g_state[idx];
    if (!st->adopted || st->n_pids == 0) {
        return;
    }
    /* pids[0] == 0 means an adopted WM that publishes no pid, so its
     * liveness can only be read off the display itself. */
    if (st->pids[0] > 0 ? (kill(st->pids[0], 0) == 0) : wm_present()) {
        return;
    }
    fprintf(stderr, "kisession: adopted wm (pid %ld) exited\n", (long)st->pids[0]);
    st->n_pids = 0;
    st->adopted = 0;
    if (!g_quit) {
        handle_wm_exit(idx);
    }
}

/* ------------------------------------------------------------------ */
/* config reload                                                       */
/* ------------------------------------------------------------------ */

/* Applies the difference rather than restarting the world: newly enabled
 * services start, newly disabled ones stop, and the WM restarts only when
 * its command actually changed. kiconf's "Aplicar" is a config write plus
 * a SIGHUP, exactly like kiconfd and xiskeys. */
static void reload_config(void)
{
    int enabled[N_SERVICES];
    char wm[PATH_MAX];
    read_config(enabled, wm, sizeof(wm));

    fprintf(stderr, "kisession: reloading config\n");

    for (int i = 0; i < N_SERVICES; i++) {
        if (enabled[i] == g_state[i].enabled) {
            continue;
        }
        g_state[i].enabled = enabled[i];
        if (enabled[i]) {
            fprintf(stderr, "kisession: service '%s' enabled\n", SERVICES[i].name);
            g_state[i].fail_count = 0;
            g_state[i].next_start = 0;
        } else {
            fprintf(stderr, "kisession: service '%s' disabled\n", SERVICES[i].name);
            stop_service(i, SIGTERM);
        }
    }

    int wm_idx = svc_index("wm");
    if (strcmp(wm, g_wm_cmd) != 0) {
        snprintf(g_wm_cmd, sizeof(g_wm_cmd), "%s", wm);
        if (g_state[wm_idx].n_pids > 0) {
            fprintf(stderr, "kisession: window manager changed to '%s', restarting it\n",
                    g_wm_cmd[0] ? g_wm_cmd : "(auto)");
            g_state[wm_idx].fail_count = 0;
            stop_service(wm_idx, SIGTERM);
        }
    }
}

/* ------------------------------------------------------------------ */
/* startup / shutdown                                                  */
/* ------------------------------------------------------------------ */

/* True if any Qt platform-theme plugin with this name is installed, for
 * either Qt version. The plugin directory is multiarch- and
 * distro-dependent, so this globs rather than hardcoding a path. */
static int qt_platformtheme_available(const char *name)
{
    static const char *const patterns[] = {
        "/usr/lib/*/qt5/plugins/platformthemes/lib%s.so",
        "/usr/lib/*/qt6/plugins/platformthemes/lib%s.so",
        "/usr/lib/qt5/plugins/platformthemes/lib%s.so",
        "/usr/lib/qt6/plugins/platformthemes/lib%s.so",
        NULL,
    };
    for (int i = 0; patterns[i]; i++) {
        char pat[PATH_MAX];
        snprintf(pat, sizeof(pat), patterns[i], name);
        glob_t g;
        memset(&g, 0, sizeof(g));
        int hit = (glob(pat, 0, NULL, &g) == 0 && g.gl_pathc > 0);
        globfree(&g);
        if (hit) {
            return 1;
        }
    }
    return 0;
}

/* Qt reads its appearance from whatever QT_QPA_PLATFORMTHEME names, and
 * nothing else sets it -- so without this everything kiconfd writes for Qt
 * is inert, which is exactly what "the session doesn't start with my
 * appearance" looks like on the Qt half of the desktop.
 *
 * "gtk3" (libqgtk3, shipped with Qt itself) is preferred over qt5ct/qt6ct
 * because it puts both toolkits on one source of truth: it follows the
 * GTK3 settings, which kiconfd both writes to settings.ini *and*
 * broadcasts over XSETTINGS, so Qt apps restyle live on "Aplicar" like
 * GTK ones instead of only at their next launch. qt5ct/qt6ct are the
 * fallback for systems without that plugin, and there the version-specific
 * name means only that Qt major version is covered -- one environment
 * variable can't name both.
 *
 * Never overrides a value the user set themselves in their profile. */
static void setup_qt_platformtheme(void)
{
    const char *existing = getenv("QT_QPA_PLATFORMTHEME");
    if (existing && *existing) {
        fprintf(stderr, "kisession: QT_QPA_PLATFORMTHEME already set to '%s', leaving it\n", existing);
        return;
    }

    char found[PATH_MAX];
    const char *choice = NULL;
    if (qt_platformtheme_available("qgtk3")) {
        choice = "gtk3";
    } else if (qt_platformtheme_available("qt6ct") || find_in_path("qt6ct", found, sizeof(found))) {
        choice = "qt6ct";
    } else if (qt_platformtheme_available("qt5ct") || find_in_path("qt5ct", found, sizeof(found))) {
        choice = "qt5ct";
    }

    if (!choice) {
        fprintf(stderr, "kisession: no Qt platform theme plugin found (gtk3/qt5ct/qt6ct); "
                        "Qt apps will keep their default appearance\n");
        return;
    }
    setenv("QT_QPA_PLATFORMTHEME", choice, 1);
    fprintf(stderr, "kisession: QT_QPA_PLATFORMTHEME=%s\n", choice);
}

static void setup_environment(char **argv)
{
    setenv("XDG_CURRENT_DESKTOP", "KiDesktop", 1);
    setenv("XDG_SESSION_DESKTOP", "kidesktop", 1);
    setenv("XDG_MENU_PREFIX", "kidesktop-", 1);
    setenv("XDG_SESSION_TYPE", "x11", 1);
    setup_qt_platformtheme();

    if (!svc_enabled("dbus")) {
        return;
    }

    /* A session bus is not optional for KiDesktop: xispanel's system tray
     * is StatusNotifierItem-only (a DBus protocol), so no bus means no
     * tray, no notifications and no MPRIS. On systemd/elogind the user
     * manager socket-activates dbus-daemon on $XDG_RUNTIME_DIR/bus and the
     * display manager passes the address down, so there is nothing to do.
     * Bare startx on a machine without a user manager has neither, and
     * that is exactly the case where the tray silently comes up empty. */
    const char *addr = getenv("DBUS_SESSION_BUS_ADDRESS");
    if (!addr || !*addr) {
        char sock[PATH_MAX];
        snprintf(sock, sizeof(sock), "%s/bus", env_or("XDG_RUNTIME_DIR", "/nonexistent"));
        struct stat sb;
        if (stat(sock, &sb) == 0 && S_ISSOCK(sb.st_mode)) {
            char val[PATH_MAX + 16];
            snprintf(val, sizeof(val), "unix:path=%s", sock);
            setenv("DBUS_SESSION_BUS_ADDRESS", val, 1);
            fprintf(stderr, "kisession: using the user manager's session bus at %s\n", sock);
        } else if (!getenv("KISESSION_DBUS_REEXEC")) {
            char found[PATH_MAX];
            if (find_in_path("dbus-run-session", found, sizeof(found))) {
                fprintf(stderr, "kisession: no session bus found, restarting under dbus-run-session\n");
                setenv("KISESSION_DBUS_REEXEC", "1", 1);
                char *args[MAX_ARGS];
                int n = 0;
                args[n++] = (char *)"dbus-run-session";
                args[n++] = (char *)"--";
                for (int i = 0; argv[i] && n < MAX_ARGS - 1; i++) {
                    args[n++] = argv[i];
                }
                args[n] = NULL;
                execv(found, args);
                fprintf(stderr, "kisession: exec dbus-run-session failed: %s\n", strerror(errno));
            } else {
                fprintf(stderr, "kisession: no session bus and no dbus-run-session; "
                                "tray and notifications will not work\n");
            }
        }
    }

    static const char *const upd[] = {
        "dbus-update-activation-environment", "--systemd", "DISPLAY", "XAUTHORITY",
        "XDG_CURRENT_DESKTOP", "XDG_SESSION_DESKTOP", "XDG_SESSION_TYPE",
        "QT_QPA_PLATFORMTHEME", NULL,
    };
    char found[PATH_MAX];
    if (find_in_path(upd[0], found, sizeof(found))) {
        run_and_wait(upd);
    }
}

static void shutdown_session(void)
{
    fprintf(stderr, "kisession: shutting down\n");

    /* Autostart apps first (they may want to save something), then the
     * services in reverse start order so the WM goes last. */
    for (int i = 0; i < g_n_autostart_pids; i++) {
        kill(g_autostart_pids[i], SIGTERM);
    }
    for (int i = N_SERVICES - 1; i >= 0; i--) {
        if (!g_state[i].adopted) {
            stop_service(i, SIGTERM);
        }
    }

    /* Give them a moment to go on their own before insisting. */
    for (int waited = 0; waited < 30; waited++) {
        int alive = 0;
        while (waitpid(-1, NULL, WNOHANG) > 0) {
            /* drain */
        }
        for (int i = 0; i < N_SERVICES; i++) {
            for (int j = 0; j < g_state[i].n_pids; j++) {
                if (!g_state[i].adopted && kill(g_state[i].pids[j], 0) == 0) {
                    alive = 1;
                }
            }
        }
        if (!alive) {
            return;
        }
        struct timespec ts = {0, 100L * 1000L * 1000L}; /* 100ms */
        nanosleep(&ts, NULL);
    }

    for (int i = N_SERVICES - 1; i >= 0; i--) {
        if (!g_state[i].adopted) {
            stop_service(i, SIGKILL);
        }
    }
}

/* Milliseconds until the loop must wake on its own: the soonest backoff
 * deadline, capped while something needs periodic checking. -1 means
 * "block until a signal arrives", which is the steady state. */
static int loop_timeout_ms(void)
{
    int timeout = -1;
    time_t now = time(NULL);

    for (int i = 0; i < N_SERVICES; i++) {
        if (!g_state[i].enabled || g_state[i].n_pids > 0 || !g_state[i].next_start) {
            continue;
        }
        int delta = (int)(g_state[i].next_start - now) * 1000;
        if (delta < 0) {
            delta = 0;
        }
        if (timeout < 0 || delta < timeout) {
            timeout = delta;
        }
    }

    int wm_idx = svc_index("wm");
    if (g_state[wm_idx].adopted && (timeout < 0 || timeout > 1000)) {
        timeout = 1000; /* an adopted WM can only be polled */
    }
    if (!g_autostart_done && svc_enabled("autostart") && (timeout < 0 || timeout > 500)) {
        timeout = 500; /* still waiting on base readiness */
    }
    return timeout;
}

int main(int argc, char **argv)
{
    if (argc > 1 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
        printf("kisession %s - session leader and service supervisor for KiDesktop\n", KISESSION_VERSION);
        printf("Usage: kisession\n");
        printf("Config: $XDG_CONFIG_HOME/kisession.conf (fallback ~/.config/kisession.conf)\n");
        printf("SIGHUP reloads the config and applies the difference.\n");
        return 0;
    }
    if (argc > 1 && (!strcmp(argv[1], "-v") || !strcmp(argv[1], "--version"))) {
        printf("kisession %s\n", KISESSION_VERSION);
        return 0;
    }

    resolve_configpath();
    struct stat sb;
    if (stat(g_configpath, &sb) != 0) {
        fprintf(stderr, "kisession: no config at '%s', writing default\n", g_configpath);
        write_default_config();
    }

    int enabled[N_SERVICES];
    read_config(enabled, g_wm_cmd, sizeof(g_wm_cmd));
    memset(g_state, 0, sizeof(g_state));
    for (int i = 0; i < N_SERVICES; i++) {
        g_state[i].enabled = enabled[i];
    }

    if (pipe(g_sigpipe) != 0) {
        fprintf(stderr, "kisession: pipe failed: %s\n", strerror(errno));
        return 1;
    }
    fcntl(g_sigpipe[0], F_SETFL, O_NONBLOCK);
    fcntl(g_sigpipe[1], F_SETFL, O_NONBLOCK);
    install_signals();

    setup_environment(argv);

    /* Opened after the environment is settled (a dbus-run-session re-exec
     * above would have thrown this away). Not fatal if it fails: only the
     * WM-adoption check and the kiconfd readiness probe need it. */
    g_dpy = XOpenDisplay(NULL);
    if (g_dpy) {
        g_root = DefaultRootWindow(g_dpy);
        XSetErrorHandler(x_error_ignore);
    } else {
        fprintf(stderr, "kisession: could not open X display; WM adoption and readiness probes are degraded\n");
    }

    for (int i = 0; i < N_SERVICES; i++) {
        if (g_quit) {
            break;
        }
        start_service(i);
        /* Hold the sequence until this one is actually usable, when the
         * services after it read something it publishes exactly once at
         * their own startup (see SvcDef.gate_ms). */
        for (int waited = 0; SERVICES[i].gate_ms > 0 && g_state[i].enabled && waited < SERVICES[i].gate_ms;
             waited += 50) {
            if (service_ready(SERVICES[i].name)) {
                break;
            }
            struct timespec ts = {0, 50L * 1000L * 1000L};
            nanosleep(&ts, NULL);
        }
    }
    g_services_started_at = time(NULL);

    while (!g_quit) {
        if (g_child) {
            g_child = 0;
            reap_children();
        }
        if (g_reload) {
            g_reload = 0;
            reload_config();
        }
        check_adopted_wm();
        if (g_quit) {
            break;
        }
        for (int i = 0; i < N_SERVICES; i++) {
            start_service(i);
        }
        maybe_run_autostart();
        if (g_quit) {
            break;
        }

        struct pollfd pfd;
        pfd.fd = g_sigpipe[0];
        pfd.events = POLLIN;
        pfd.revents = 0;
        int r = poll(&pfd, 1, loop_timeout_ms());
        if (r > 0 && (pfd.revents & POLLIN)) {
            char buf[64];
            while (read(g_sigpipe[0], buf, sizeof(buf)) > 0) {
                /* drain */
            }
        }
    }

    shutdown_session();
    if (g_dpy) {
        XCloseDisplay(g_dpy);
    }
    return 0;
}
