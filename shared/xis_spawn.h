/* xis_spawn - launching a *user application* so it is genuinely
 * independent of the program that launched it, shared by every KiDesktop
 * daemon that launches one: xispanel/xisserve's run_detached() and
 * xisback/xiskeys' run_action() are all thin wrappers over it now.
 *
 * "Detached" needs three separate things to be true, and the plain
 * fork+setsid+double-fork+`sh -c` pattern only ever delivered the first
 * two:
 *
 *   1. No zombie / no wait: the caller never reaps it. (SIGCHLD is
 *      SIG_IGN in both callers, and the double fork below reparents the
 *      grandchild to init anyway.)
 *   2. Own session, own ppid: setsid() plus the second fork, so a
 *      process-tree view doesn't nest the app under the panel.
 *   3. Its own *cgroup*, which is the one that actually matters on a
 *      systemd session and was missing. A child inherits the cgroup of
 *      whoever forked it, and neither fork() nor setsid() nor exec()
 *      changes that -- so every app launched from a panel that systemd
 *      started as app-xispanel-<hash>.scope stayed a member of that
 *      scope for its whole life. Two visible consequences: KDE's task
 *      manager (and anything else that groups by cgroup) attributed
 *      every one of those apps to xispanel, and when xispanel exited,
 *      systemd stopped the now-empty-of-its-main-process scope and
 *      killed every app still in it. Quitting the panel took the whole
 *      session's apps down with it.
 *
 * The fix for (3) is the same one GNOME Shell and Plasma use for apps
 * they launch: ask the systemd *user* manager for a fresh transient
 * scope and have the app be born in that instead. We do it through
 * `systemd-run --user --scope`, which creates the unit and then execs
 * the command in place (no extra process left behind), rather than by
 * talking to the D-Bus StartTransientUnit API ourselves -- xispanel
 * links no D-Bus library at all (see its dlopen()ing mpris.c/sni.c),
 * and this is one exec per user-initiated click, not a hot path.
 *
 * Where there is no systemd user manager -- a Devuan/sysvinit session is
 * a supported target here -- there is no cgroup to escape from in the
 * first place, and the command is exec'd directly, exactly as before.
 *
 * Second, smaller thing this fixes: `sh -c '<cmd>'` left a dash process
 * sitting in the tree forever, parked in wait() for the app it started
 * (dash, unlike bash, does not tail-exec the last command of a -c
 * string). So every launched app showed up under an `sh -c` stub. The
 * command string gets an `exec ` prefix whenever it is a single command,
 * which makes the shell replace itself. A genuinely compound command
 * (`a; b`, `a || b` -- storage.c's unmount-then-power-off chain,
 * build_terminal_exec()'s fallback chain) still needs a real shell to
 * sequence it, and keeps one.
 */
#ifndef XIS_SPAWN_H
#define XIS_SPAWN_H

/* Runs `cmd` (a shell command string) as an independent user
 * application: double-forked, setsid()'d, never waited on, and -- on a
 * systemd session -- inside its own transient scope so it outlives the
 * caller and is not attributed to it. No-op if `cmd` is NULL/empty.
 *
 * Fire-and-forget by design: there is no pid to return (the pid the
 * caller could see is the intermediate child that exits immediately) and
 * no way to report that the command itself failed to exec. A caller that
 * needs a command's *output* or exit status wants xispanel's asyncmd.c
 * instead -- those are the panel's own subprocesses, deliberately still
 * its children in its own cgroup, not user applications. */
void xis_spawn_detached(const char *cmd);

/* Same, with `env` -- a NULL-terminated array of "NAME=VALUE" strings --
 * exported to the command only (set inside the forked child, so the
 * caller's own environment is never touched). This is what xisback's
 * click actions (XISBACK_OUTPUT/DESKTOP/IMAGE/CLICK_X/CLICK_Y) and
 * xiskeys' bindings (XISKEYS_ACTION) need: they can't setenv() around
 * the call, because the fork happens in here. `env` may be NULL. */
void xis_spawn_detached_env(const char *cmd, const char *const *env);

#endif /* XIS_SPAWN_H */
