#!/usr/bin/env python3
"""xisguard test matrix - see README.md next to this file.

Runs every combination of the session's starting state (system rules, user
rules, guard running or not, and the guard's own settings) in its own
headless X server, then changes each variable at runtime, tests, changes it
back and tests again. "Tests" means launching three programs that try
Xnotify-protected actions: one with a system rule, one with a user rule and
one with no rule at all.

Two checks per test round:
  - oracle: the observed result matches what the spec (Model.expect) says;
  - revert: after a variable is changed back, results and guard status are
    identical to the ones observed when the session started.

Each configuration runs inside its own bubblewrap sandbox: private /tmp and
PID namespace, the X server's system config directory replaced by a test
copy, separate HOME and XDG_RUNTIME_DIR, fake xisserve/pkexec in PATH. Nothing
touches the real session; ending a configuration is ending its sandbox.
"""
import argparse
import concurrent.futures
import itertools
import json
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO_GUARD = os.path.normpath(os.path.join(HERE, ".."))
FORK = os.path.expanduser("~/Downloads/kiyoshi-forks/xserver/build-test-debug")
DEFAULT_XVFB = os.path.join(FORK, "hw/vfb/Xvfb")
DEFAULT_XEPHYR = os.path.join(FORK, "hw/kdrive/ephyr/Xephyr")
DEFAULT_SYSCONFDIR = "/opt/xlibre-test/etc/X11"

# Paths as seen from inside the sandbox.
W = "/tmp/w"
BIN = W + "/bin"
GUARD = BIN + "/xisguard"
PROBE = {"sys": BIN + "/xprobe-sys", "user": BIN + "/xprobe-user"}

SYS_LEVELS = ("none", "base", "rules")
UNKNOWN_LEVELS = ("ask", "quiet", "kill")

ROUND_SPACING = 3.3   # server throttles repeated prompts per exe+action for 3 s
GUARD_GONE_WAIT = 6.5  # server declares the guard dead after 5 s of silence
PROBE_HOLD_MS = 1200
PROBE_TIMEOUT = 12


# ---------------------------------------------------------------- configs

class Config:
    """Starting state of one session."""

    def __init__(self, sys_, usr, guard, perm="0", sec=0, unk="ask"):
        self.sys = sys_          # none | base | rules
        self.usr = usr           # 0 | 1  (perms.conf with rules for xprobe-user)
        self.guard = guard       # 0 | 1  (xisguard running at session start)
        self.perm = perm         # "0" | "1" | "fresh" (no xnotify.conf: new install default)
        self.sec = sec           # secure_mode
        self.unk = unk           # what happens to unknown programs: ask | quiet | kill

    @property
    def id(self):
        s = "sys-%s_usr-%d_guard-%d" % (self.sys, self.usr, self.guard)
        if self.guard:
            s += "_perm-%s_sec-%d_unk-%s" % (self.perm, self.sec, self.unk)
        return s

    @classmethod
    def from_id(cls, cid):
        kv = dict(part.split("-", 1) for part in cid.split("_"))
        return cls(kv["sys"], int(kv["usr"]), int(kv["guard"]), kv.get("perm", "0"),
                   int(kv.get("sec", 0)), kv.get("unk", "ask"))


def all_configs():
    out = []
    for sys_, usr in itertools.product(SYS_LEVELS, (0, 1)):
        out.append(Config(sys_, usr, 0))
        for perm, sec, unk in itertools.product(("0", "1"), (0, 1), UNKNOWN_LEVELS):
            out.append(Config(sys_, usr, 1, perm, sec, unk))
        # A new install (no xnotify.conf yet) starts permissive.
        out.append(Config(sys_, usr, 1, "fresh", 0, "ask"))
    return out


# ---------------------------------------------------------------- spec

class Model:
    """What the session should look like, and what each probe should get.

    This is the spec the observed results are checked against. It follows
    the documented behaviour (XISGUARD.md, README.md, the fork's Xnotify.md);
    where it is a judgement call, README.md of this kit says so.
    """

    def __init__(self, cfg, answer):
        self.answer = answer               # what the fake dialog answers: deny | allow
        self.sys_files = cfg.sys           # what is on disk
        self.sys_loaded = cfg.sys          # what the X server has loaded
        self.usr = cfg.usr
        self.guard = cfg.guard
        self.permissive_on_start = 1 if cfg.perm in ("1", "fresh") else 0
        self.permissive = self.permissive_on_start if cfg.guard else 0
        self.secure = cfg.sec
        self.unknown = cfg.unk
        self.disabled = 0
        self.grants = set()                # session grants from "allow" answers

    def snapshot(self):
        return {k: (sorted(v) if isinstance(v, set) else v) for k, v in vars(self).items()}

    # --- transitions (called by the worker right after doing the real thing)

    def server_reload(self):
        """A RELOAD reaches the server: it re-reads the system rules and drops
        every dynamic rule (session grants included). User rules are the
        guard's job to keep in force."""
        self.sys_loaded = self.sys_files
        self.grants.clear()

    def guard_started(self):
        self.guard = 1
        self.permissive = self.permissive_on_start
        self.disabled = 0
        self.server_reload()               # the guard sends RELOAD at start

    def guard_stopped(self):
        self.guard = 0
        self.permissive = 0
        self.disabled = 0
        self.server_reload()               # server falls back to its static rules

    # --- the oracle

    def expect(self, probe, exe, action):
        """-> (verdict, prompted): verdict is ALLOW, DENY or KILLED."""
        if self.sys_loaded == "none":
            return "ALLOW", False          # no system config: legacy, allow all
        if self.guard and (self.disabled or self.permissive):
            return "ALLOW", False
        if probe == "sys" and self.sys_loaded == "rules":
            return "ALLOW", False
        if probe == "user" and self.guard and self.usr and not self.secure:
            return "ALLOW", False
        if not self.guard:
            return "DENY", False           # static rules only, nobody to ask
        if (exe, action) in self.grants:
            return "ALLOW", False
        if self.unknown == "quiet":
            return "DENY", False
        if self.unknown == "kill":
            return "KILLED", False
        if self.answer == "allow":
            self.grants.add((exe, action))
            return "ALLOW", True
        return "DENY", True


# ---------------------------------------------------------------- worker

def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(text)


def rule_lines(text):
    return sorted(ln.strip() for ln in text.splitlines() if ln.strip() and not ln.strip().startswith("#"))


class Worker:
    """Runs one configuration. Lives inside the sandbox."""

    def __init__(self, args):
        self.cfg = Config.from_id(args.worker)
        self.args = args
        self.actions = args.actions.split(",")
        self.model = Model(self.cfg, args.answer)
        self.display = args.display
        self.sysconf = args.sysconfdir
        self.home = W + "/home"
        self.run_dir = W + "/run"
        self.confdir = self.home + "/.config/xisguard"
        self.ctl_path = "%s/xisguard-ctl.%d.sock" % (self.run_dir, self.display)
        self.xserver = None
        self.guard_proc = None
        self.prompt_off = 0
        self.round_no = 0
        self.last_round = 0.0
        self.steps = []
        self.env = dict(os.environ)
        self.env.update({
            "HOME": self.home,
            "XDG_RUNTIME_DIR": self.run_dir,
            "XDG_CONFIG_HOME": self.home + "/.config",
            "XISGUARD_TEST_DIR": W,
            "PATH": "%s/fakebin:%s:%s" % (W, BIN, os.environ.get("PATH", "/usr/bin:/bin")),
            "LANG": "C", "LC_ALL": "C", "LANGUAGE": "C",
            "DISPLAY": ":%d" % self.display,
        })
        if not args.host_display:
            self.env.pop("XAUTHORITY", None)

    # --- files

    def write_sys(self, level):
        for name in os.listdir(self.sysconf):
            p = os.path.join(self.sysconf, name)
            shutil.rmtree(p) if os.path.isdir(p) else os.unlink(p)
        if level == "none":
            return
        write(self.sysconf + "/xnotify.conf",
              "# xisguard test matrix: base system config\nALLOW XNOTIFY %s\n" % GUARD)
        if level == "rules":
            write(self.sysconf + "/xnotify.conf.d/10-test.conf",
                  "".join("ALLOW %s %s\n" % (a, PROBE["sys"]) for a in self.actions))

    def user_rules_text(self):
        return "".join("ALLOW %s %s\n" % (a, PROBE["user"]) for a in self.actions)

    def write_guard_conf(self):
        if self.cfg.perm == "fresh":
            return
        unk = self.cfg.unk if self.cfg.guard else "ask"
        write(self.confdir + "/xnotify.conf",
              "no_pause=0\nquiet=%d\nalways_kill=%d\nlog_level=3\nsecure_mode=%d\n"
              "permissive_on_start=%d\n" % (
                  unk == "quiet", unk == "kill", self.cfg.sec if self.cfg.guard else 0,
                  int(self.cfg.perm == "1") if self.cfg.guard else 0))

    # --- processes

    def start_xserver(self):
        sock = "/tmp/.X11-unix/X%d" % self.display
        if self.args.host_display:
            env = dict(self.env, DISPLAY=self.args.host_display)
            cmd = [self.args.xserver, ":%d" % self.display, "-screen", "640x480",
                   "-nolisten", "tcp", "-title", "xisguard-matrix " + self.cfg.id]
        else:
            env = self.env
            cmd = [self.args.xserver, ":%d" % self.display, "-screen", "0", "1280x1024x24",
                   "-nolisten", "tcp"]
        log = open(W + "/xserver.log", "a")
        self.xserver = subprocess.Popen(cmd, env=env, stdout=log, stderr=log)
        deadline = time.time() + 15
        while time.time() < deadline:
            if os.path.exists(sock):
                try:
                    s = socket.socket(socket.AF_UNIX)
                    s.connect(sock)
                    s.close()
                    return
                except OSError:
                    pass
            if self.xserver.poll() is not None:
                break
            time.sleep(0.1)
        raise RuntimeError("X server did not start, see xserver.log")

    def start_guard(self, extra_args=()):
        log = open(W + "/guard.log", "a")
        log.write("==== start (round %d) %s\n" % (self.round_no, " ".join(extra_args)))
        log.flush()
        self.guard_proc = subprocess.Popen([GUARD] + list(extra_args), env=self.env,
                                           stdout=log, stderr=log)
        deadline = time.time() + 10
        while time.time() < deadline:
            if self.guard_proc.poll() is not None:
                raise RuntimeError("xisguard exited with %s, see guard.log" % self.guard_proc.returncode)
            if self.ctl({"cmd": "PING"}).get("pong"):
                time.sleep(0.6)   # let the server apply the initial sync
                return
            time.sleep(0.1)
        raise RuntimeError("xisguard control socket never answered")

    def stop_guard(self):
        self.guard_proc.send_signal(signal.SIGTERM)
        try:
            self.guard_proc.wait(5)
        except subprocess.TimeoutExpired:
            self.guard_proc.kill()
            self.guard_proc.wait()
        self.guard_proc = None
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

    def guard_status(self):
        if not self.guard_proc:
            return None
        st = self.ctl({"cmd": "GET_STATUS"})
        keys = ("permissive", "secure_mode", "quiet", "always_kill", "xnotify_enabled", "no_pause")
        return {k: st.get(k) for k in keys}

    # --- a test round

    def run_round(self, label):
        wait = self.last_round + ROUND_SPACING - time.time()
        if wait > 0:
            time.sleep(wait)
        self.last_round = time.time()
        self.round_no += 1

        none_exe = "%s/r%03d/xprobe-none" % (BIN, self.round_no)
        os.makedirs(os.path.dirname(none_exe))
        shutil.copy2(BIN + "/xprobe", none_exe)
        exes = {"sys": PROBE["sys"], "user": PROBE["user"], "none": none_exe}

        procs = {}
        env = dict(self.env, XPROBE_RETRY_MS="2000" if self.model.answer == "allow" else "0")
        for probe, action in itertools.product(("sys", "user", "none"), self.actions):
            cmd = [exes[probe], action, "--hold-ms", str(PROBE_HOLD_MS)]
            procs[probe + ":" + action] = subprocess.Popen(
                cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

        observed = {}
        for key, p in procs.items():
            try:
                p.wait(PROBE_TIMEOUT)
                rc = p.returncode
                verdict = {0: "ALLOW", 1: "DENY", -signal.SIGKILL: "KILLED"}.get(rc, "ERR(%s)" % rc)
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait()
                verdict = "HUNG"
            observed[key] = {"verdict": verdict}
        time.sleep(0.5)

        prompts = self.read_new_prompts()
        for key in observed:
            probe, action = key.split(":")
            # "Program: <exe> <args> (<pid>)"
            pat = re.compile(re.escape(exes[probe]) + r"[ |(]")
            observed[key]["prompt"] = any(
                ("Permission: %s " % action) in line and pat.search(line) for line in prompts)

        expected = {}
        for key in observed:
            probe, action = key.split(":")
            v, pr = self.model.expect(probe, exes[probe] if probe != "none" else "none", action)
            expected[key] = {"verdict": v, "prompt": pr}
        # "none" is a fresh program every round, its grants never carry over.
        self.model.grants = {g for g in self.model.grants if g[0] != "none"}

        mismatches = {k: {"expected": expected[k], "observed": observed[k]}
                      for k in observed if observed[k] != expected[k]}
        step = {"round": self.round_no, "label": label, "model": self.model.snapshot(),
                "observed": observed, "expected": expected, "mismatches": mismatches,
                "guard_status": self.guard_status(), "t": round(time.time(), 2)}
        self.steps.append(step)
        self.progress(step)
        return step

    def read_new_prompts(self):
        path = W + "/prompts.log"
        if not os.path.exists(path):
            return []
        with open(path, errors="replace") as f:
            f.seek(self.prompt_off)
            data = f.read()
            self.prompt_off = f.tell()
        return [ln for ln in data.splitlines() if ln.strip()]

    def progress(self, step):
        bad = len(step["mismatches"])
        print("[%s] round %d %-28s %s" % (self.cfg.id, step["round"], step["label"],
                                          "ok" if not bad else "%d MISMATCH" % bad),
              file=sys.stderr, flush=True)

    # --- runtime changes

    def set_var(self, var, value):
        m = self.model
        if var == "guard":
            if value:
                self.start_guard()
                m.guard_started()
            else:
                self.stop_guard()
                m.guard_stopped()
        elif var == "permissive":
            self.ctl({"cmd": "SET_PERMISSIVE", "enabled": value})
            m.permissive = value
            if not value:
                m.server_reload()          # turning it off is a server RELOAD
        elif var == "secure":
            self.ctl({"cmd": "SET_STATUS", "secure_mode": value})
            m.secure = value
        elif var == "unknown":
            self.ctl({"cmd": "SET_STATUS", "quiet": int(value == "quiet"),
                      "always_kill": int(value == "kill")})
            m.unknown = value
        elif var == "sys":
            self.write_sys(value)
            m.sys_files = value
            if self.guard_proc:
                self.ctl({"cmd": "RELOAD"})
                m.server_reload()
                # Going to or from no system config makes the server drop the
                # guard and take it back after its 5 s heartbeat timeout; test
                # the settled state, not the race.
                time.sleep(GUARD_GONE_WAIT)
        elif var == "usr":
            self.set_user_rules(value)
            m.usr = value
        elif var == "disabled":
            self.ctl({"cmd": "DISABLE" if value else "ENABLE"})
            m.disabled = value
        time.sleep(0.6)

    def set_user_rules(self, on):
        """Like the user would: through kiconf (the control socket) when the
        guard is up and accepts user rules, by editing perms.conf otherwise."""
        perms = self.confdir + "/perms.conf"
        if self.guard_proc and not self.model.secure:
            for a in self.actions:
                self.ctl({"cmd": "ADD_RULE" if on else "REMOVE_RULE",
                          "type": "ALLOW", "action": a, "pattern": PROBE["user"]})
        else:
            write(perms, self.user_rules_text() if on else "")
            time.sleep(1.5)                # the guard polls perms.conf's mtime every second

    def toggles(self):
        """(variable, start value, other values) for this configuration."""
        m = self.model
        out = [("guard", m.guard, [1 - m.guard]),
               ("usr", m.usr, [1 - m.usr])]
        if m.guard:
            out += [("permissive", m.permissive, [1 - m.permissive]),
                    ("secure", m.secure, [1 - m.secure]),
                    ("unknown", m.unknown, [u for u in UNKNOWN_LEVELS if u != m.unknown]),
                    ("disabled", 0, [1])]
        # System rules last, and dropping them last of all: losing them can
        # leave the server in legacy mode for the rest of the session, which
        # would hide whatever the other variables do.
        others = [s for s in ("rules", "base", "none") if s != m.sys_files]
        out.append(("sys", m.sys_files, others))
        return out

    # --- main

    def run(self):
        os.makedirs(self.run_dir, mode=0o700, exist_ok=True)
        os.makedirs(self.confdir, exist_ok=True)
        self.write_sys(self.cfg.sys)
        if self.cfg.usr:
            write(self.confdir + "/perms.conf", self.user_rules_text())
        self.write_guard_conf()
        write(W + "/answer", "1\n" if self.model.answer == "allow" else "99\n")
        result = {"config": self.cfg.id, "answer": self.model.answer, "actions": self.actions,
                  "steps": self.steps, "revert_failures": [], "file_failures": [], "error": None}
        try:
            self.start_xserver()
            if self.cfg.guard:
                self.start_guard()
            base = self.run_round("baseline")
            for var, start, others in self.toggles():
                for value in others:
                    self.set_var(var, value)
                    self.run_round("%s=%s" % (var, value))
                    self.set_var(var, start)
                    back = self.run_round("%s=%s (back)" % (var, start))
                    self.check_revert(result, base, back)
            self.check_files(result)
        except Exception as e:   # noqa: BLE001 - report it, keep the logs
            result["error"] = "%s: %s" % (type(e).__name__, e)
            print("[%s] ERROR %s" % (self.cfg.id, result["error"]), file=sys.stderr, flush=True)
        finally:
            if self.guard_proc:
                self.guard_proc.send_signal(signal.SIGTERM)
                try:
                    self.guard_proc.wait(5)
                except subprocess.TimeoutExpired:
                    self.guard_proc.kill()
            if self.xserver:
                self.xserver.terminate()
                try:
                    self.xserver.wait(5)
                except subprocess.TimeoutExpired:
                    self.xserver.kill()
        with open(W + "/result.json", "w") as f:
            json.dump(result, f, indent=1)

    def check_revert(self, result, base, back):
        # With "allow" answers a program asked once keeps its session grant,
        # so only the verdicts have to come back, not the prompts.
        def view(cell):
            return cell["verdict"] if self.model.answer == "allow" else cell
        diff = {k: {"baseline": base["observed"][k], "after": back["observed"][k]}
                for k in base["observed"] if view(base["observed"][k]) != view(back["observed"][k])}
        if base["guard_status"] != back["guard_status"]:
            diff["guard_status"] = {"baseline": base["guard_status"], "after": back["guard_status"]}
        if diff:
            result["revert_failures"].append({"round": back["round"], "label": back["label"], "diff": diff})

    def check_files(self, result):
        """Answers of "deny for this session" must never write anything."""
        if self.model.answer != "deny":
            return
        extra = os.path.join(self.sysconf, "xnotify.conf.d", "50-xisguard.conf")
        if os.path.exists(extra):
            result["file_failures"].append("system rule written: " + open(extra).read().strip())
        perms = self.confdir + "/perms.conf"
        want = self.user_rules_text() if self.cfg.usr else ""
        have = open(perms).read() if os.path.exists(perms) else ""
        if rule_lines(have) != rule_lines(want):
            result["file_failures"].append("perms.conf differs from the start: %r" % have)


# ---------------------------------------------------------------- driver

def build(args):
    subprocess.check_call(["make", "-s", "-C", REPO_GUARD, "xisguard"])
    probe = os.path.join(HERE, "xprobe")
    src = os.path.join(HERE, "xprobe.c")
    if not os.path.exists(probe) or os.path.getmtime(probe) < os.path.getmtime(src):
        subprocess.check_call(["cc", "-O2", "-Wall", "-Wextra", "-o", probe, src, "-lX11"])
    for path in (args.xserver,):
        if not os.access(path, os.X_OK):
            sys.exit("not executable: %s (see --xserver)" % path)
    if not shutil.which("bwrap"):
        sys.exit("bubblewrap (bwrap) is required")


def prepare_dir(cdir):
    if os.path.exists(cdir):
        shutil.rmtree(cdir)
    os.makedirs(cdir + "/bin")
    os.makedirs(cdir + "/sysconf")
    shutil.copytree(os.path.join(HERE, "fakebin"), cdir + "/fakebin")
    shutil.copy2(os.path.join(REPO_GUARD, "xisguard"), cdir + "/bin/xisguard")
    shutil.copy2(os.path.join(HERE, "xprobe"), cdir + "/bin/xprobe")
    for name in ("sys", "user"):
        shutil.copy2(os.path.join(HERE, "xprobe"), cdir + "/bin/xprobe-" + name)


def run_job(args, job, slot):
    """job: (id, worker args) - a matrix configuration or a scenario."""
    job_id, worker_args = job
    cdir = os.path.join(args.out, job_id)
    prepare_dir(cdir)
    display = 70 + slot if args.xephyr else 1
    cmd = ["bwrap", "--dev-bind", "/", "/", "--tmpfs", "/tmp", "--bind", cdir, W,
           "--bind", cdir + "/sysconf", args.sysconfdir,
           "--proc", "/proc", "--unshare-pid", "--die-with-parent"]
    host_display = ""
    if args.xephyr:
        host_display = os.environ.get("DISPLAY", ":0")
        xauth = os.environ.get("XAUTHORITY", "")
        if xauth.startswith("/tmp/"):
            cmd += ["--ro-bind", xauth, xauth]
    else:
        cmd += ["--unshare-net"]
    cmd += [sys.executable, os.path.abspath(__file__)] + worker_args + [
            "--display", str(display), "--xserver", args.xserver, "--sysconfdir", args.sysconfdir,
            "--answer", args.answer, "--actions", args.actions]
    if host_display:
        cmd += ["--host-display", host_display]
    with open(cdir + "/worker.log", "w") as log:
        rc = subprocess.call(cmd, stdout=log, stderr=subprocess.STDOUT)
    try:
        with open(cdir + "/result.json") as f:
            res = json.load(f)
    except (OSError, ValueError):
        res = {"config": job_id, "steps": [], "checks": [], "revert_failures": [], "file_failures": [],
               "error": "worker exited %d without a result, see worker.log" % rc}
    if "scenario" in res or job_id.startswith("scn-"):
        res.setdefault("scenario", job_id[4:])
        bad = [c for c in res.get("checks", []) if not c["ok"]]
        status = "PASS" if not (bad or res["error"]) else "FAIL"
        print("%-4s %-50s checks=%-3d failed=%-3d%s" % (
            status, job_id, len(res.get("checks", [])), len(bad),
            "  ERROR " + res["error"] if res["error"] else ""), flush=True)
        return res
    oracle = sum(len(s["mismatches"]) for s in res["steps"])
    status = "PASS" if not (oracle or res["revert_failures"] or res["file_failures"] or res["error"]) else "FAIL"
    print("%-4s %-50s rounds=%-3d oracle=%-3d revert=%-3d%s" % (
        status, job_id, len(res["steps"]), oracle, len(res["revert_failures"]),
        "  ERROR " + res["error"] if res["error"] else ""), flush=True)
    return res


def fmt_cell(c):
    return c["verdict"] + ("+prompt" if c.get("prompt") else "")


def write_report(args, results, elapsed, scen_results=()):
    lines = ["# xisguard test matrix", "",
             "- date: %s" % time.strftime("%Y-%m-%d %H:%M:%S"),
             "- answer to prompts: %s, actions: %s" % (args.answer, args.actions),
             "- X server: %s" % args.xserver,
             "- configurations: %d, time: %.0f s" % (len(results), elapsed), ""]

    # Failures grouped by what went wrong, so one bug hit by 40 configs (and
    # both actions) is one line.
    groups = {}

    def add(sig, cfg, action):
        g = groups.setdefault(sig, [set(), set()])
        g[0].add(cfg)
        g[1].add(action)

    for r in results:
        for s in r["steps"]:
            for k, mm in s["mismatches"].items():
                probe, action = k.split(":")
                add(("oracle", s["label"], probe, fmt_cell(mm["expected"]), fmt_cell(mm["observed"])),
                    r["config"], action)
        for rf in r["revert_failures"]:
            for k, d in rf["diff"].items():
                if k == "guard_status":
                    add(("revert", rf["label"], k, json.dumps(d["baseline"]), json.dumps(d["after"])),
                        r["config"], "")
                else:
                    probe, action = k.split(":")
                    add(("revert", rf["label"], probe, fmt_cell(d["baseline"]), fmt_cell(d["after"])),
                        r["config"], action)
        for ff in r["file_failures"]:
            add(("files", "", "", "", ff), r["config"], "")
        if r["error"]:
            add(("error", "", "", "", r["error"]), r["config"], "")

    failed = set().union(*(g[0] for g in groups.values())) if groups else set()
    passed = len(results) - len(failed)
    if results:
        lines += ["**%d/%d configurations passed.**" % (passed, len(results)), ""]
    if scen_results:
        import scenarios
        scen_lines, scen_passed = scenarios.report_lines(scen_results)
        lines += ["**%d/%d scenarios passed.**" % (scen_passed, len(scen_results)), ""] + scen_lines
        passed += scen_passed
    if groups:
        lines += ["## Failures, grouped", "",
                  "| check | step | probe | expected / baseline | got | actions | configs |",
                  "|---|---|---|---|---|---|---|"]
        for sig, (cfgs, acts) in sorted(groups.items(), key=lambda kv: (-len(kv[1][0]), kv[0])):
            cfgs = sorted(cfgs)
            lines.append("| %s | %s | %s | %s | %s | %s | %d: %s |" % (
                sig[0], sig[1], sig[2], sig[3], sig[4], ",".join(sorted(a for a in acts if a)), len(cfgs),
                ", ".join(cfgs[:6]) + (" ..." if len(cfgs) > 6 else "")))
        lines.append("")

    if results:
        lines += ["## Per configuration", ""]
    for r in results:
        bad = r["config"] in failed
        lines += ["### %s %s" % ("FAIL" if bad else "PASS", r["config"]), ""]
        if r["error"]:
            lines += ["error: `%s`" % r["error"], ""]
        if not r["steps"]:
            continue
        keys = list(r["steps"][0]["observed"].keys())
        lines.append("| round | step | " + " | ".join(keys) + " |")
        lines.append("|---|---|" + "---|" * len(keys))
        for s in r["steps"]:
            cells = []
            for k in keys:
                c = fmt_cell(s["observed"][k])
                if k in s["mismatches"]:
                    c = "**%s** (want %s)" % (c, fmt_cell(s["mismatches"][k]["expected"]))
                cells.append(c)
            lines.append("| %d | %s | %s |" % (s["round"], s["label"], " | ".join(cells)))
        for rf in r["revert_failures"]:
            lines.append("")
            lines.append("revert mismatch after `%s`: `%s`" % (rf["label"], json.dumps(rf["diff"])))
        for ff in r["file_failures"]:
            lines.append("")
            lines.append("file check: %s" % ff)
        lines.append("")
    path = os.path.join(args.out, "report.md")
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")
    with open(os.path.join(args.out, "results.json"), "w") as f:
        json.dump(list(results) + list(scen_results), f, indent=1)
    return path, passed


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=os.path.join(os.environ.get("TMPDIR", "/tmp"), "xisguard-matrix",
                                                  time.strftime("%Y%m%d-%H%M%S")),
                    help="results directory (default: $TMPDIR/xisguard-matrix/<date>)")
    ap.add_argument("-j", "--jobs", type=int, default=max(1, min(4, (os.cpu_count() or 2) // 2)),
                    help="configurations run in parallel")
    ap.add_argument("-k", "--filter", default="",
                    help="only configurations whose id matches this regex (see --list)")
    ap.add_argument("--quick", action="store_true",
                    help="skip quiet/kill, secure mode with permissive, and the fresh-install cases")
    ap.add_argument("--suite", choices=("all", "matrix", "scenarios"), default="all",
                    help="the state matrix, the scripted scenarios (scenarios.py), or both")
    ap.add_argument("--list", action="store_true", help="list the configuration ids and exit")
    ap.add_argument("--answer", choices=("deny", "allow"), default="deny",
                    help="what the fake dialog answers to prompts: deny or allow, for this session only")
    ap.add_argument("--actions", default="SCREEN,SELECTION", help="comma separated: SCREEN,SELECTION")
    ap.add_argument("--xephyr", action="store_true",
                    help="use the fork's Xephyr (windows on your screen) instead of headless Xvfb")
    ap.add_argument("--xserver", default="", help="X server binary (default: the fork's Xvfb/Xephyr)")
    ap.add_argument("--sysconfdir", default=DEFAULT_SYSCONFDIR,
                    help="the X server's compile-time SYSCONFDIR, replaced inside the sandbox")
    # internal: the per-configuration process inside the sandbox
    ap.add_argument("--worker", help=argparse.SUPPRESS)
    ap.add_argument("--scenario", help=argparse.SUPPRESS)
    ap.add_argument("--display", type=int, default=1, help=argparse.SUPPRESS)
    ap.add_argument("--host-display", default="", help=argparse.SUPPRESS)
    args = ap.parse_args()
    if not args.xserver:
        args.xserver = DEFAULT_XEPHYR if args.xephyr else DEFAULT_XVFB

    if args.scenario:
        import scenarios
        scenarios.ScenarioWorker(args).run()
        return 0
    if args.worker:
        Worker(args).run()
        return 0

    jobs = []
    if args.suite in ("all", "matrix"):
        cfgs = all_configs()
        if args.quick:
            cfgs = [c for c in cfgs if c.unk == "ask" and c.perm != "fresh" and not (c.sec and c.perm == "1")]
        jobs += [(c.id, ["--worker", c.id]) for c in cfgs]
    if args.suite in ("all", "scenarios"):
        import scenarios
        jobs += [("scn-" + name, ["--scenario", name]) for name in scenarios.SCENARIOS]
    if args.filter:
        jobs = [j for j in jobs if re.search(args.filter, j[0])]
    if args.list:
        print("\n".join(j[0] for j in jobs))
        return 0
    if not jobs:
        sys.exit("no configuration or scenario matches")

    build(args)
    os.makedirs(args.out, exist_ok=True)
    print("%d jobs, %d at a time, results in %s" % (len(jobs), args.jobs, args.out), flush=True)
    t0 = time.time()
    results = [None] * len(jobs)
    free_slots = list(range(args.jobs))
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        futs = {}
        pending = list(enumerate(jobs))
        while pending or futs:
            while pending and free_slots:
                i, j = pending.pop(0)
                slot = free_slots.pop()
                futs[pool.submit(run_job, args, j, slot)] = (i, slot)
            done, _ = concurrent.futures.wait(futs, return_when=concurrent.futures.FIRST_COMPLETED)
            for f in done:
                i, slot = futs.pop(f)
                free_slots.append(slot)
                results[i] = f.result()
    matrix_res = [r for r in results if "scenario" not in r]
    scen_res = [r for r in results if "scenario" in r]
    path, passed = write_report(args, matrix_res, time.time() - t0, scen_res)
    print("\n%d/%d passed (%d configurations, %d scenarios) - report: %s" % (
        passed, len(results), len(matrix_res), len(scen_res), path))
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
