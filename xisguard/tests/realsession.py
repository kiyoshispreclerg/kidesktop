#!/usr/bin/env python3
"""xisguard + Xnotify end to end, with the real binaries.

Unlike matrix.py (fake xisserve/pkexec, Xvfb), this one runs the installed
pieces: the Xephyr of the test prefix (/opt/xlibre-test by default), the
installed xisguard and the real xisserve --question popup, answered through
AT-SPI (the buttons are picked by position, so any locale works).

What it checks is the rule book for the server's system rules
(SYSCONFDIR/xnotify.conf*):

  - with system rules the server follows them, and keeps following them
    after permissive mode goes on and off, after the guard dies and comes
    back, and with a perms.conf that also lists the guard itself;
  - without system rules the server is permissive: user rules have no
    effect, nothing prompts, the guard only gets REPORTs;
  - user rules apply only while there is at least one system rule
    (ALLOW XNOTIFY <guard> alone counts);
  - removing / restoring the system rules at runtime (RELOAD) switches
    between the two.

The system rule dir is swapped with bwrap, so no root is needed; the
session's own HOME, runtime dir and processes are never touched.

  python3 xisguard/tests/realsession.py [--xserver PATH] [--guard PATH] [-k REGEX]
"""
import argparse
import datetime
import json
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
PROBE_HOLD_MS = 1500
GUARD_GONE_WAIT = 6.5      # server declares the guard dead after 5 s of silence
SPACING = 3.2              # server throttles the same exe+action for 3 s

# xisguard's --question buttons, in the order it passes them
BTN = {"allow": 0, "deny": 1, "allow_session": 2, "deny_session": 3}


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(text)


# ---------------------------------------------------------------- AT-SPI

class Answerer(threading.Thread):
    """Watches for XisGuard popups and clicks the queued answer.

    Every popup is recorded as (exe in its text, answer given)."""

    def __init__(self):
        super().__init__(daemon=True)
        import pyatspi
        self.pyatspi = pyatspi
        self.answer = "deny_session"
        self.seen = []
        self.lock = threading.Lock()
        self.stop = False
        self.handled = set()

    def _walk(self, acc, depth=0):
        if acc is None or depth > 12:
            return
        yield acc
        for i in range(acc.childCount):
            try:
                yield from self._walk(acc.getChildAtIndex(i), depth + 1)
            except Exception:
                pass

    def _popups(self):
        desk = self.pyatspi.Registry.getDesktop(0)
        for i in range(desk.childCount):
            app = desk.getChildAtIndex(i)
            if app is None or "xisserve" not in (app.name or ""):
                continue
            for j in range(app.childCount):
                win = app.getChildAtIndex(j)
                if win is not None and win.name == "XisGuard":
                    yield app, win

    def run(self):
        while not self.stop:
            try:
                for app, win in self._popups():
                    key = (app.get_process_id(), win.name)
                    if key in self.handled:
                        continue
                    texts, buttons = [], []
                    for a in self._walk(win):
                        role = a.getRole()
                        if role == self.pyatspi.ROLE_PUSH_BUTTON:
                            buttons.append(a)
                        elif role == self.pyatspi.ROLE_LABEL:
                            texts.append(a.name or "")
                    if len(buttons) < 4:
                        continue
                    with self.lock:
                        answer = self.answer
                        self.seen.append(("\n".join(texts), answer))
                    self.handled.add(key)
                    buttons[BTN[answer]].queryAction().doAction(0)
            except Exception:
                pass
            time.sleep(0.1)

    def take(self):
        with self.lock:
            s, self.seen = self.seen, []
        return s


# ---------------------------------------------------------------- session

class Session:
    def __init__(self, args, out):
        self.args, self.out = args, out
        self.display = args.display
        self.run = out + "/run"
        self.home = out + "/home"
        self.bin = out + "/bin"
        self.sysconf = args.sysconfdir
        self.ctl_path = "%s/xisguard-ctl.%d.sock" % (self.run, self.display)
        self.env = dict(os.environ, HOME=self.home, XDG_RUNTIME_DIR=self.run,
                        DISPLAY=":%d" % self.display, GTK_MODULES="gail:atk-bridge",
                        NO_AT_BRIDGE="0")
        self.env.pop("XAUTHORITY", None)
        self.xserver = self.guard = None
        self.probe_no = 0
        self.last_probe = 0
        self.results = []      # (scenario, step, check, ok, detail)
        self.scenario = ""
        self.answerer = None

    # --- files

    def set_sysconf(self, kind):
        for name in os.listdir(self.sysconf):
            p = os.path.join(self.sysconf, name)
            shutil.rmtree(p) if os.path.isdir(p) else os.unlink(p)
        if kind == "none":
            return
        text = "ALLOW XNOTIFY %s\n" % self.args.guard
        if kind == "rules":
            text += "ALLOW ALL %s/xprobe-sys\n" % self.bin
        write(self.sysconf + "/xnotify.conf", text)

    def set_home(self):
        shutil.rmtree(self.home, ignore_errors=True)
        conf = self.home + "/.config/xisguard"
        # The guard listing itself is what demoted it before (ALLOW ALL used
        # to overwrite the XNOTIFY bit); keep it here on purpose.
        # xisserve draws the popup itself; a real session has it allowed
        # (otherwise its own popup queues a prompt behind itself).
        write(conf + "/perms.conf", "ALLOW ALL %s\nALLOW ALL %s\nALLOW ALL %s/xprobe-user\n"
              % (self.args.guard, shutil.which("xisserve"), self.bin))
        write(conf + "/xnotify.conf", "no_pause=0\nquiet=0\nalways_kill=0\nlog_level=3\n"
              "secure_mode=0\npermissive_on_start=0\n")

    # --- processes

    def start_x(self):
        sock = "/tmp/.X11-unix/X%d" % self.display
        log = open(self.out + "/xserver.log", "a")
        log.write("==== %s\n" % self.scenario)
        log.flush()
        env = dict(self.env, DISPLAY=self.args.host_display)
        if self.args.host_xauthority:
            env["XAUTHORITY"] = self.args.host_xauthority
        self.xserver = subprocess.Popen(
            [self.args.xserver, ":%d" % self.display, "-screen", "900x600", "-nolisten", "tcp",
             "-title", "xisguard realsession " + self.scenario],
            env=env, stdout=log, stderr=log)
        deadline = time.time() + 15
        while time.time() < deadline:
            try:
                s = socket.socket(socket.AF_UNIX)
                s.connect(sock)
                s.close()
                return
            except OSError:
                time.sleep(0.1)
        raise RuntimeError("X server did not start, see xserver.log")

    def stop_x(self):
        if self.xserver:
            self.xserver.terminate()
            try:
                self.xserver.wait(5)
            except subprocess.TimeoutExpired:
                self.xserver.kill()
            self.xserver = None
        time.sleep(0.5)

    def start_guard(self):
        log = open(self.out + "/guard.log", "a")
        log.write("==== start (%s)\n" % self.scenario)
        log.flush()
        self.guard = subprocess.Popen([self.args.guard], env=self.env, stdout=log, stderr=log)
        deadline = time.time() + 10
        while time.time() < deadline:
            if self.guard.poll() is not None:
                raise RuntimeError("xisguard exited, see guard.log")
            if self.ctl({"cmd": "PING"}).get("pong"):
                time.sleep(1.0)
                return
            time.sleep(0.1)
        raise RuntimeError("xisguard control socket never answered")

    def stop_guard(self, wait=True):
        if self.guard:
            self.guard.terminate()
            try:
                self.guard.wait(5)
            except subprocess.TimeoutExpired:
                self.guard.kill()
            self.guard = None
        if wait:
            time.sleep(GUARD_GONE_WAIT)

    def ctl(self, req):
        try:
            s = socket.socket(socket.AF_UNIX)
            s.settimeout(3)
            s.connect(self.ctl_path)
            s.sendall((json.dumps(req) + "\n").encode())
            data = b""
            while not data.endswith(b"\n"):
                chunk = s.recv(65536)
                if not chunk:
                    break
                data += chunk
            s.close()
            return json.loads(data.decode() or "{}")
        except (OSError, ValueError):
            return {}

    def xlog(self):
        with open(self.out + "/xserver.log", errors="replace") as f:
            return f.read().split("==== %s\n" % self.scenario)[-1]

    def glog(self):
        with open(self.out + "/guard.log", errors="replace") as f:
            return f.read()

    # --- probes

    def probe(self, which, action="SCREEN", answer="deny_session"):
        """Run one probe; returns 'ALLOW'/'DENY' plus '+prompt' if a popup was
        shown for it (answered with `answer`)."""
        wait = self.last_probe + SPACING - time.time()
        if wait > 0:
            time.sleep(wait)
        self.last_probe = time.time()
        if which == "none":
            self.probe_no += 1
            exe = "%s/n%03d/xprobe-none" % (self.bin, self.probe_no)
            os.makedirs(os.path.dirname(exe), exist_ok=True)
            shutil.copy2(self.bin + "/xprobe", exe)
        else:
            exe = "%s/xprobe-%s" % (self.bin, which)
        self.answerer.answer = answer
        self.answerer.take()
        env = dict(self.env, XPROBE_RETRY_MS="4000" if answer.startswith("allow") else "0")
        p = subprocess.run([exe, action, "--hold-ms", str(PROBE_HOLD_MS)], env=env,
                           capture_output=True, text=True, timeout=60)
        verdict = {0: "ALLOW", 1: "DENY"}.get(p.returncode, "ERR(%d)" % p.returncode)
        time.sleep(0.3)
        prompts = [t for t, _ in self.answerer.take() if exe in t]
        return verdict + ("+prompt" if prompts else ""), exe

    # --- checks

    def check(self, step, name, ok, detail=""):
        self.results.append((self.scenario, step, name, bool(ok), detail))
        mark = "ok  " if ok else "FAIL"
        print("  [%s] %-28s %s %s" % (mark, step, name, detail), flush=True)

    def expect(self, step, which, want, action="SCREEN", answer="deny_session"):
        got, exe = self.probe(which, action, answer)
        self.check(step, "%s %s" % (which, action), got == want,
                   "got %s want %s" % (got, want))
        return exe

    def expect_rules(self, step):
        """System rules loaded + guard up, not permissive."""
        self.expect(step, "sys", "ALLOW")
        self.expect(step, "user", "ALLOW")
        self.expect(step, "none", "DENY+prompt")
        self.expect(step, "none", "ALLOW+prompt", answer="allow_session")
        self.expect(step, "sys", "ALLOW", "SELECTION")
        self.expect(step, "none", "DENY+prompt", "SELECTION")

    def expect_static_only(self, step):
        """System rules loaded, no guard."""
        self.expect(step, "sys", "ALLOW")
        self.expect(step, "user", "DENY")
        self.expect(step, "none", "DENY")

    def expect_permissive(self, step, reports=True):
        self.expect(step, "sys", "ALLOW")
        self.expect(step, "user", "ALLOW")
        exe = self.expect(step, "none", "ALLOW")
        self.expect(step, "none", "ALLOW", "SELECTION")
        if reports:
            time.sleep(0.5)
            self.check(step, "guard got REPORT", ("REPORT: " + exe) in self.glog(), exe)

    def guard_trusted(self, step):
        bad = self.xlog().count("is not an authorized guard")
        self.check(step, "guard never rejected", bad == 0, "%d rejections" % bad)

    def reload(self):
        self.ctl({"cmd": "RELOAD"})
        time.sleep(1.5)


# ---------------------------------------------------------------- scenarios

SCENARIOS = []


def scenario(fn):
    SCENARIOS.append(fn)
    return fn


@scenario
def rules(s):
    """System rules: followed across permissive toggles and guard restarts."""
    s.set_sysconf("rules")
    s.start_x()
    s.expect_static_only("no guard yet")
    s.start_guard()
    s.guard_trusted("guard up")
    s.expect_rules("guard up")
    for i in (1, 2):
        s.ctl({"cmd": "SET_PERMISSIVE", "enabled": 1})
        s.expect_permissive("permissive on #%d" % i)
        s.ctl({"cmd": "SET_PERMISSIVE", "enabled": 0})
        time.sleep(1)
        s.expect_rules("permissive off #%d" % i)
    s.guard_trusted("after toggles")
    s.stop_guard()
    s.expect_static_only("guard stopped")
    s.start_guard()
    s.expect_rules("guard restarted")
    s.guard_trusted("end")


@scenario
def rules_removed_at_runtime(s):
    """System rules removed + RELOAD -> permissive; restored + RELOAD -> rules."""
    s.set_sysconf("rules")
    s.start_x()
    s.start_guard()
    s.expect_rules("start")
    s.set_sysconf("none")
    s.reload()
    s.expect_permissive("rules removed")
    s.set_sysconf("rules")
    s.reload()
    s.expect_rules("rules restored")
    s.guard_trusted("end")


@scenario
def no_rules(s):
    """No system rules: always permissive, user rules irrelevant, REPORTs only."""
    s.set_sysconf("none")
    s.start_x()
    s.check("start", "server in legacy mode", "legacy mode" in s.xlog())
    s.expect_permissive("no guard", reports=False)
    s.start_guard()
    s.expect_permissive("guard up")
    s.ctl({"cmd": "SET_PERMISSIVE", "enabled": 1})
    s.expect_permissive("permissive on")
    s.ctl({"cmd": "SET_PERMISSIVE", "enabled": 0})
    time.sleep(1)
    s.expect_permissive("permissive off")
    s.stop_guard()
    s.expect_permissive("guard stopped", reports=False)
    s.start_guard()
    s.set_sysconf("rules")
    s.reload()
    s.expect_rules("rules added at runtime")


@scenario
def guard_rule_only(s):
    """Only ALLOW XNOTIFY <guard>: that is enough for user rules to apply."""
    s.set_sysconf("base")
    s.start_x()
    s.start_guard()
    s.guard_trusted("guard up")
    s.expect("guard up", "user", "ALLOW")
    s.expect("guard up", "sys", "DENY+prompt")      # no system rule for it now
    s.expect("guard up", "none", "DENY+prompt")
    s.stop_guard()
    s.expect("guard stopped", "user", "DENY")


# ---------------------------------------------------------------- driver

def inner(args):
    out = args.out
    s = Session(args, out)
    s.answerer = Answerer()
    s.answerer.start()
    for fn in SCENARIOS:
        if args.k and not re.search(args.k, fn.__name__):
            continue
        s.scenario = fn.__name__
        print("== %s: %s" % (fn.__name__, fn.__doc__), flush=True)
        s.set_home()
        try:
            fn(s)
        except Exception as e:
            s.check("abort", "exception", False, repr(e))
        finally:
            s.stop_guard(wait=False)
            s.stop_x()
    s.answerer.stop = True
    with open(out + "/results.json", "w") as f:
        json.dump(s.results, f, indent=1)
    fails = [r for r in s.results if not r[3]]
    lines = ["# xisguard realsession %s" % datetime.datetime.now().strftime("%Y-%m-%d %H:%M"),
             "", "server: `%s`  guard: `%s`" % (args.xserver, args.guard), "",
             "%d checks, %d failed" % (len(s.results), len(fails)), "",
             "| scenario | step | check | | detail |", "|---|---|---|---|---|"]
    for sc, step, name, ok, detail in s.results:
        lines.append("| %s | %s | %s | %s | %s |" % (sc, step, name, "ok" if ok else "**FAIL**", detail))
    write(out + "/report.md", "\n".join(lines) + "\n")
    print("\n%d checks, %d failed -> %s/report.md" % (len(s.results), len(fails), out))
    return 1 if fails else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--xserver", default="/opt/xlibre-test/bin/Xephyr")
    ap.add_argument("--sysconfdir", default="/opt/xlibre-test/etc/X11",
                    help="the server's compile-time SYSCONFDIR (replaced inside bwrap)")
    ap.add_argument("--guard", default=shutil.which("xisguard") or "/usr/local/bin/xisguard")
    ap.add_argument("--display", type=int, default=31)
    ap.add_argument("--host-display", default=os.environ.get("DISPLAY", ":0"))
    ap.add_argument("--out", default=None)
    ap.add_argument("-k", default=None, help="regex on scenario names")
    ap.add_argument("--inner", action="store_true", help=argparse.SUPPRESS)
    ap.add_argument("--host-xauthority", default=os.environ.get("XAUTHORITY"), help=argparse.SUPPRESS)
    args = ap.parse_args()

    if args.inner:
        sys.exit(inner(args))

    for tool in ("bwrap", "dbus-run-session", "cc", "xisserve"):
        if not shutil.which(tool):
            sys.exit("%s is required" % tool)
    if os.path.exists("/tmp/.X11-unix/X%d" % args.display):
        sys.exit("display :%d is in use, pick another with --display" % args.display)
    out = args.out or "/tmp/xisguard-real/" + datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    os.makedirs(out + "/sysconf")
    os.makedirs(out + "/run", mode=0o700)
    os.makedirs(out + "/bin")
    subprocess.run(["cc", "-O1", "-o", out + "/bin/xprobe", HERE + "/xprobe.c", "-lX11"], check=True)
    for name in ("sys", "user"):
        shutil.copy2(out + "/bin/xprobe", out + "/bin/xprobe-" + name)

    cmd = ["bwrap", "--dev-bind", "/", "/", "--bind", out + "/sysconf", args.sysconfdir,
           "--unshare-pid", "--proc", "/proc", "--die-with-parent", "--",
           "env", "-u", "DBUS_SESSION_BUS_ADDRESS", "-u", "AT_SPI_BUS_ADDRESS",
           "dbus-run-session", "--",
           sys.executable, os.path.abspath(__file__), "--inner", "--out", out,
           "--xserver", args.xserver, "--sysconfdir", args.sysconfdir, "--guard", args.guard,
           "--display", str(args.display), "--host-display", args.host_display]
    if args.k:
        cmd += ["-k", args.k]
    if args.host_xauthority:
        cmd += ["--host-xauthority", args.host_xauthority]
    sys.exit(subprocess.call(cmd))


if __name__ == "__main__":
    main()
