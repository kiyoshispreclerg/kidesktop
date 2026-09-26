/* xis_spawn - see xis_spawn.h for what "detached" has to mean here and
 * why the cgroup half of it needs systemd-run. */
#include "xis_spawn.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* A command that is more than one simple command can't be `exec`'d --
 * the shell has to stay alive to sequence it. Anything in this set means
 * "keep the shell": `;` and newline (sequence), `&` (background, and
 * `&&`), `|` (pipe, and `||`), `(`/`)` (subshell -- `exec (a)` is a
 * syntax error, not a command). Quotes, $VAR, redirections and leading
 * VAR=val assignments are all fine after an `exec` and deliberately not
 * listed. */
static int cmd_needs_shell(const char *cmd)
{
    return strpbrk(cmd, ";&|\n()") != NULL;
}

/* $PATH lookup for an executable, so a missing systemd-run is detected
 * before we commit to exec'ing it (and can fall back) rather than after.
 * Writes nothing and returns 0 if not found. */
static int find_in_path(const char *bin, char *out, size_t outsz)
{
    const char *path = getenv("PATH");
    if (!path || !path[0]) {
        path = "/usr/local/bin:/usr/bin:/bin";
    }
    for (const char *p = path; *p;) {
        const char *colon = strchr(p, ':');
        size_t len = colon ? (size_t)(colon - p) : strlen(p);
        /* An empty PATH element means the current directory; never
         * accept one for this lookup. */
        if (len > 0 && len < outsz) {
            snprintf(out, outsz, "%.*s/%s", (int)len, p, bin);
            if (access(out, X_OK) == 0) {
                return 1;
            }
        }
        if (!colon) {
            break;
        }
        p = colon + 1;
    }
    out[0] = 0;
    return 0;
}

/* Can this session hand a launched app its own transient scope? All
 * three have to hold: a systemd boot at all, a *live* systemd --user for
 * this uid (--user talks to that, not to PID 1), and systemd-run itself
 * installed. Cached: the answer can't change within one session, and
 * this runs on every launch. */
static int transient_scope_available(void)
{
    static int cached = -1;
    if (cached >= 0) {
        return cached;
    }
    cached = 0;

    if (access("/run/systemd/system", F_OK) != 0) {
        return cached; /* not a systemd boot (sysvinit) -- nothing to escape */
    }

    const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
    char private_sock[256];
    if (runtime_dir && runtime_dir[0]) {
        snprintf(private_sock, sizeof(private_sock), "%s/systemd/private", runtime_dir);
    } else {
        snprintf(private_sock, sizeof(private_sock), "/run/user/%u/systemd/private", (unsigned)getuid());
    }
    if (access(private_sock, F_OK) != 0) {
        return cached; /* no per-user manager: --user would just fail */
    }

    char bin[512];
    if (!find_in_path("systemd-run", bin, sizeof(bin))) {
        return cached;
    }

    cached = 1;
    return cached;
}

/* Names the transient scope after the program being launched, in the
 * app-<name>-<unique>.scope shape systemd's own launchers use -- so a
 * cgroup-grouping task manager labels the app as itself instead of as
 * whatever generated name systemd-run would have picked (or, before any
 * of this, as xispanel). Unique part is the caller's pid, which is the
 * pid that ends up *in* the scope (we exec from here), so it is unique
 * for as long as that scope can exist.
 *
 * `cmd`'s first token is the program: leading VAR=val assignments and
 * any quoting the caller added are skipped, and only the basename's
 * name-safe characters are kept. */
static void build_scope_unit(const char *cmd, char *out, size_t outsz)
{
    const char *p = cmd;
    const char *tok_end;
    for (;;) {
        while (*p == ' ' || *p == '\t' || *p == '\'' || *p == '"') {
            p++;
        }
        tok_end = p;
        while (*tok_end && *tok_end != ' ' && *tok_end != '\t' && *tok_end != '\'' && *tok_end != '"') {
            tok_end++;
        }
        /* An =-containing first token is an environment assignment, not
         * the program (`GDK_SCALE=2 foo`); the program is the next one. */
        const char *eq = memchr(p, '=', (size_t)(tok_end - p));
        if (!eq || tok_end == p) {
            break;
        }
        p = tok_end;
        if (!*p) {
            break;
        }
    }
    const char *slash = p;
    for (const char *s = p; s < tok_end; s++) {
        if (*s == '/') {
            slash = s + 1;
        }
    }

    char name[64];
    size_t n = 0;
    for (const char *s = slash; s < tok_end && n + 1 < sizeof(name); s++) {
        unsigned char c = (unsigned char)*s;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.') {
            name[n++] = (char)c;
        } else if (c == '-' && n > 0) {
            name[n++] = (char)c;
        }
    }
    name[n] = 0;
    if (!n) {
        snprintf(name, sizeof(name), "app");
    }

    snprintf(out, outsz, "app-%s-%u.scope", name, (unsigned)getpid());
}

/* setenv() each "NAME=VALUE" of a NULL-terminated array. Only ever
 * called in the forked child, right before exec. An entry with no '=' or
 * an empty name is skipped rather than treated as an error -- there is
 * nobody left to report it to at this point. */
static void apply_env(const char *const *env)
{
    for (const char *const *e = env; e && *e; e++) {
        const char *eq = strchr(*e, '=');
        if (!eq || eq == *e) {
            continue;
        }
        char name[128];
        size_t len = (size_t)(eq - *e);
        if (len >= sizeof(name)) {
            continue;
        }
        memcpy(name, *e, len);
        name[len] = 0;
        setenv(name, eq + 1, 1);
    }
}

void xis_spawn_detached(const char *cmd)
{
    xis_spawn_detached_env(cmd, NULL);
}

void xis_spawn_detached_env(const char *cmd, const char *const *env)
{
    if (!cmd || !cmd[0]) {
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("xis_spawn: fork");
        return;
    }
    if (pid != 0) {
        /* Reap the intermediate child here rather than relying on the
         * caller's SIGCHLD disposition: it has already exited (or is
         * about to -- all it does is fork and _exit), so this doesn't
         * block on the application, and it means a caller with no
         * SIGCHLD handler and no SIG_IGN (kiwm) collects no zombie
         * either. Harmlessly fails with ECHILD where SIGCHLD *is*
         * SIG_IGN and the kernel got there first (xispanel/xisserve). */
        waitpid(pid, NULL, 0);
        return;
    }

    setsid();
    /* The double fork: this first child exits immediately below,
     * orphaning the grandchild that execs `cmd` -- the kernel reparents
     * it to init (or whatever subreaper owns the tree), not to us.
     * setsid() alone detaches the controlling terminal but leaves ppid
     * pointing at the caller for as long as the app runs. */
    pid_t pid2 = fork();
    if (pid2 < 0) {
        _exit(1);
    }
    if (pid2 > 0) {
        _exit(0);
    }

    apply_env(env);

    /* `exec ` so the shell replaces itself instead of parking in wait()
     * for the app -- see xis_spawn.h. */
    char shell_cmd[4096];
    if (cmd_needs_shell(cmd)) {
        snprintf(shell_cmd, sizeof(shell_cmd), "%s", cmd);
    } else {
        snprintf(shell_cmd, sizeof(shell_cmd), "exec %s", cmd);
    }

    if (transient_scope_available()) {
        char unit[160];
        build_scope_unit(cmd, unit, sizeof(unit));
        char unit_arg[176];
        snprintf(unit_arg, sizeof(unit_arg), "--unit=%s", unit);
        /* --collect: a scope whose app exits nonzero is garbage-collected
         * instead of lingering in "failed" state and blocking its own
         * name. -q: no "Running as unit ..." on our stderr.
         * The command still goes through a shell, because callers pass a
         * command *string* (quoting, $VAR, redirections) rather than an
         * argv -- but that shell exec's into the app per shell_cmd above,
         * so nothing extra survives in the tree. */
        char *argv[] = {"systemd-run", "--user", "--scope", "--collect", "-q", unit_arg,
                        "--", "/bin/sh", "-c", shell_cmd, NULL};
        execvp("systemd-run", argv);
        /* Only reachable if systemd-run vanished between the cached
         * availability check and now; launching the app un-escaped beats
         * not launching it. */
    }

    execl("/bin/sh", "sh", "-c", shell_cmd, (char *)NULL);
    _exit(127);
}
