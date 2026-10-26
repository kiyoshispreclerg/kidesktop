"""Scripted scenarios for the xisguard test kit - see README.md.

The matrix (matrix.py) covers every combination of a few state variables.
What does not fit a cartesian product lives here: options whose effect needs
a specific sequence to see (no_pause, ignore.conf, notify_actions, the CLI
--allow/--deny overrides, SET_PERMISSIVE with persist) and the permanent
answers of the dialog (Allow, Deny, Trust and their EXACT variants), in normal
and in secure mode, where a permanent answer goes through pkexec into the
system rules.

Each scenario runs in its own sandbox, exactly like a matrix configuration,
and records a list of checks: what was looked at, what was expected, what
came out.
"""
import json
import os
import shutil
import subprocess
import sys
import time

import matrix as M

W, BIN, PROBE = M.W, M.BIN, M.PROBE

SCENARIOS = {}


def scenario(name, sys_="base", usr=0, conf=None, guard_args=(), ignore=None, setup=None):
    """Registers fn(w) as a scenario. conf: xnotify.conf keys over the
    defaults; ignore: lines for ignore.conf; setup(w): runs before the X
    server starts."""
    def deco(fn):
        SCENARIOS[name] = dict(fn=fn, sys=sys_, usr=usr, conf=conf or {},
                               guard_args=list(guard_args), ignore=ignore, setup=setup)
        return fn
    return deco


# ---------------------------------------------------------------- worker

class ScenarioWorker(M.Worker):
    def __init__(self, args):
        self.scn_name = args.scenario
        self.scn = SCENARIOS[args.scenario]
        args.worker = M.Config(self.scn["sys"], self.scn["usr"], 1).id
        M.Worker.__init__(self, args)
        self.checks = []
        self.exe_no = 0
        self.last_run = {}

    # --- setup

    def write_guard_conf(self):
        conf = {"no_pause": 0, "quiet": 0, "always_kill": 0, "log_level": 3,
                "secure_mode": 0, "permissive_on_start": 0}
        conf.update(self.scn["conf"])
        M.write(self.confdir + "/xnotify.conf", "".join("%s=%s\n" % kv for kv in conf.items()))

    def run(self):
        os.makedirs(self.run_dir, mode=0o700, exist_ok=True)
        os.makedirs(self.confdir, exist_ok=True)
        self.write_sys(self.scn["sys"])
        if self.scn["usr"]:
            M.write(self.confdir + "/perms.conf", self.user_rules_text())
        self.write_guard_conf()
        if self.scn["ignore"] is not None:
            M.write(self.confdir + "/ignore.conf", "".join(l + "\n" for l in self.scn["ignore"]))
        self.answer(99)
        result = {"scenario": self.scn_name, "config": "scn-" + self.scn_name,
                  "checks": self.checks, "error": None}
        try:
            if self.scn["setup"]:
                self.scn["setup"](self)
            self.start_xserver()
            self.start_guard(self.scn["guard_args"])
            self.scn["fn"](self)
        except Exception as e:   # noqa: BLE001 - report it, keep the logs
            import traceback
            traceback.print_exc()
            result["error"] = "%s: %s" % (type(e).__name__, e)
        finally:
            if self.guard_proc:
                self.guard_proc.send_signal(M.signal.SIGTERM)
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

    # --- tools for the scenarios

    def answer(self, value):
        M.write(W + "/answer", "%s\n" % value)

    def new_exe(self, tag):
        """A program nobody has seen yet (rules match the path)."""
        self.exe_no += 1
        path = "%s/%s%02d/xprobe-%s" % (BIN, tag, self.exe_no, tag)
        os.makedirs(os.path.dirname(path))
        shutil.copy2(BIN + "/xprobe", path)
        return path

    def probe(self, exe, action, hold=M.PROBE_HOLD_MS, retry=False):
        """Runs exe trying action -> {"verdict", "prompt", "paused"}."""
        key = (exe, action, hold)
        wait = self.last_run.get(key, 0) + M.ROUND_SPACING - time.time()
        if wait > 0:
            time.sleep(wait)
        self.last_run[key] = time.time()
        self.read_new_prompts()   # drop anything older
        # Action through the environment: the arguments, which "exact" rules
        # match, stay "--hold-ms N" for every action.
        env = dict(self.env, XPROBE_RETRY_MS="2000" if retry else "0", XPROBE_ACTION=action)
        p = subprocess.Popen([exe, "--hold-ms", str(hold)], env=env,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            p.wait(M.PROBE_TIMEOUT)
            verdict = {0: "ALLOW", 1: "DENY", -M.signal.SIGKILL: "KILLED"}.get(
                p.returncode, "ERR(%s)" % p.returncode)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait()
            verdict = "HUNG"
        time.sleep(0.5)
        lines = [ln for ln in self.read_new_prompts()
                 if ("Permission: %s " % action) in ln and (exe + " ") in ln]
        return {"verdict": verdict, "prompt": bool(lines),
                "paused": any(ln.rstrip().endswith("state=T") for ln in lines)}

    def check(self, what, got, want):
        """want: a value, or a dict whose keys must all match in got."""
        if isinstance(want, dict) and isinstance(got, dict):
            ok = all(got.get(k) == v for k, v in want.items())
        else:
            ok = got == want
        self.checks.append({"what": what, "ok": ok, "want": want, "got": got})
        print("[%s] %s %s" % (self.scn_name, "ok  " if ok else "FAIL", what), file=sys.stderr, flush=True)
        return ok

    def expect_probe(self, what, exe, action, want, **kw):
        """want: "ALLOW", "DENY", "KILLED", optionally "+prompt"."""
        r = self.probe(exe, action, **kw)
        got = r["verdict"] + ("+prompt" if r["prompt"] else "")
        self.check(what, got, want)
        return r

    def restart_guard(self, extra_args=(), absorb=True):
        self.stop_guard()
        self.start_guard(extra_args)
        if absorb:
            self.absorb_first_request()

    def absorb_first_request(self):
        """The first request after the guard (re)starts is lost (FINDINGS.md,
        #6). One throwaway program takes it, so the checks that follow test
        what they mean to; scenario guard_restart_first_request covers #6."""
        self.probe(self.new_exe("absorb"), "SCREEN")

    def read(self, path):
        try:
            with open(path) as f:
                return f.read()
        except OSError:
            return ""

    def conf_value(self, key):
        for ln in self.read(self.confdir + "/xnotify.conf").splitlines():
            if ln.startswith(key + "="):
                return ln.split("=", 1)[1].strip()
        return None

    def active(self, exe):
        """{action: ignored} for exe in LIST_ACTIVE."""
        time.sleep(0.3)
        out = {}
        for e in self.ctl({"cmd": "LIST_ACTIVE"}).get("active", []):
            if e.get("exe") == exe:
                out[e["action"]] = e.get("ignored")
        return out

    @property
    def perms_rules(self):
        return M.rule_lines(self.read(self.confdir + "/perms.conf"))

    @property
    def system_rules(self):
        return M.rule_lines(self.read(self.sysconf + "/xnotify.conf.d/50-xisguard.conf"))

    @property
    def pkexec_calls(self):
        return len([l for l in self.read(W + "/pkexec.log").splitlines() if l.strip()])


# ---------------------------------------------------------------- options

def no_pause_body(w, start):
    def round_(label, no_pause):
        r = w.expect_probe("%s: unknown program is asked" % label, w.new_exe("np"), "SCREEN", "DENY+prompt")
        w.check("%s: program paused while the dialog is up" % label, r["paused"], not no_pause)
    round_("start no_pause=%d" % start, start)
    w.ctl({"cmd": "SET_STATUS", "no_pause": 1 - start})
    w.check("SET_STATUS no_pause=%d reported" % (1 - start), w.guard_status()["no_pause"], 1 - start)
    round_("runtime no_pause=%d" % (1 - start), 1 - start)
    w.ctl({"cmd": "SET_STATUS", "no_pause": start})
    round_("back to no_pause=%d" % start, start)
    w.check("xnotify.conf keeps no_pause=%d" % start, w.conf_value("no_pause"), str(start))


@scenario("no_pause-start0", conf={"no_pause": 0})
def _(w):
    no_pause_body(w, 0)


@scenario("no_pause-start1", conf={"no_pause": 1})
def _(w):
    no_pause_body(w, 1)


IGN = BIN + "/ign/xprobe-ign"


def make_ign(w):
    os.makedirs(os.path.dirname(IGN), exist_ok=True)
    shutil.copy2(BIN + "/xprobe", IGN)


@scenario("ignore_conf", sys_="rules", ignore=["# test", PROBE["sys"], IGN], setup=make_ign)
def _(w):
    start = w.read(w.confdir + "/ignore.conf")
    lst = w.ctl({"cmd": "LIST_IGNORED"})
    w.check("LIST_IGNORED lists ignore.conf", sorted(lst.get("ignored", [])), sorted([PROBE["sys"], IGN]))
    w.expect_probe("ignored program with a system rule still works", PROBE["sys"], "SCREEN", "ALLOW")
    w.check("LIST_ACTIVE flags it ignored", w.active(PROBE["sys"]), {"SCREEN": True})
    w.expect_probe("ignore.conf does not grant anything: ignored program without rule is asked",
                   IGN, "SCREEN", "DENY+prompt")
    w.ctl({"cmd": "REMOVE_IGNORED", "pattern": PROBE["sys"]})
    w.check("REMOVE_IGNORED: no longer flagged", w.active(PROBE["sys"]), {"SCREEN": False})
    w.check("REMOVE_IGNORED: gone from ignore.conf",
            PROBE["sys"] in M.rule_lines(w.read(w.confdir + "/ignore.conf")), False)
    w.ctl({"cmd": "ADD_IGNORED", "pattern": PROBE["sys"]})
    w.check("ADD_IGNORED: flagged again", w.active(PROBE["sys"]), {"SCREEN": True})
    w.check("ignore.conf back to its rules (comments may move)",
            M.rule_lines(w.read(w.confdir + "/ignore.conf")), M.rule_lines(start))


@scenario("notify_actions", sys_="rules")
def _(w):
    default = w.ctl({"cmd": "GET_STATUS"}).get("notify_actions")
    w.check("default notify_actions", sorted(default.split(",")), ["COMPOSITE", "SCREEN"])
    w.expect_probe("system rule: SCREEN", PROBE["sys"], "SCREEN", "ALLOW")
    w.expect_probe("system rule: SELECTION", PROBE["sys"], "SELECTION", "ALLOW")
    w.check("LIST_ACTIVE shows only the notified actions", sorted(w.active(PROBE["sys"])), ["SCREEN"])
    for na, want in (("SELECTION", ["SELECTION"]), ("NONE", []), ("SCREEN,SELECTION", ["SCREEN", "SELECTION"])):
        w.ctl({"cmd": "SET_STATUS", "notify_actions": na})
        w.check("notify_actions=%s: LIST_ACTIVE" % na, sorted(w.active(PROBE["sys"])), want)
    w.ctl({"cmd": "SET_STATUS", "notify_actions": "NONE"})
    w.expect_probe("notify_actions=NONE does not stop the dialog", w.new_exe("na"), "SELECTION", "DENY+prompt")
    w.ctl({"cmd": "SET_STATUS", "notify_actions": default})
    w.check("back to the default", w.ctl({"cmd": "GET_STATUS"}).get("notify_actions"), default)
    w.check("xnotify.conf has it back", w.conf_value("notify_actions"), default)


# Spec choice: CLI overrides beat user rules (perms.conf), as the README
# says, but not system rules, which the server applies without asking.
@scenario("cli_deny", sys_="rules", usr=1, guard_args=["--deny", "SCREEN"])
def _(w):
    w.expect_probe("--deny SCREEN: system rule still wins", PROBE["sys"], "SCREEN", "ALLOW")
    w.expect_probe("--deny SCREEN beats the user rule", PROBE["user"], "SCREEN", "DENY")
    w.expect_probe("--deny SCREEN: unknown program denied without asking", w.new_exe("cd"), "SCREEN", "DENY")
    w.expect_probe("--deny SCREEN: other actions untouched (user rule)", PROBE["user"], "SELECTION", "ALLOW")
    w.expect_probe("--deny SCREEN: other actions untouched (unknown)", w.new_exe("cd"), "SELECTION", "DENY+prompt")
    w.check("not saved to xnotify.conf", "deny" in w.read(w.confdir + "/xnotify.conf").lower(), False)
    w.restart_guard()
    w.expect_probe("restart without --deny: user rule works again", PROBE["user"], "SCREEN", "ALLOW")
    w.expect_probe("restart without --deny: unknown is asked again", w.new_exe("cd"), "SCREEN", "DENY+prompt")


@scenario("cli_allow", guard_args=["--allow", "SCREEN"])
def _(w):
    w.expect_probe("--allow SCREEN: unknown program allowed at the first try, no dialog",
                   w.new_exe("ca"), "SCREEN", "ALLOW")
    w.expect_probe("--allow SCREEN: other actions still asked", w.new_exe("ca"), "SELECTION", "DENY+prompt")
    w.restart_guard()
    w.expect_probe("restart without --allow: asked again", w.new_exe("ca"), "SCREEN", "DENY+prompt")


@scenario("cli_allow_and_deny", guard_args=["--allow", "SCREEN", "--deny", "SCREEN"])
def _(w):
    w.expect_probe("--allow and --deny the same action: deny wins", w.new_exe("cb"), "SCREEN", "DENY")


@scenario("cli_deny_all", sys_="rules", usr=1, guard_args=["--deny", "ALL"])
def _(w):
    w.expect_probe("--deny ALL: system rule still wins", PROBE["sys"], "SELECTION", "ALLOW")
    w.expect_probe("--deny ALL beats user rules", PROBE["user"], "SELECTION", "DENY")
    w.expect_probe("--deny ALL: unknown denied without asking", w.new_exe("cx"), "SCREEN", "DENY")


@scenario("permissive_persist")
def _(w):
    def st():
        s = w.ctl({"cmd": "GET_STATUS"})
        return {"permissive": s.get("permissive"), "permissive_on_start": s.get("permissive_on_start")}

    w.ctl({"cmd": "SET_PERMISSIVE", "enabled": 1})
    w.check("enabled=1 without persist", st(), {"permissive": 1, "permissive_on_start": 0})
    w.check("  file unchanged", w.conf_value("permissive_on_start"), "0")
    w.restart_guard()
    w.check("  after restart: off", st(), {"permissive": 0})
    w.expect_probe("  after restart: unknown is asked", w.new_exe("pp"), "SCREEN", "DENY+prompt")

    w.ctl({"cmd": "SET_PERMISSIVE", "enabled": 1, "persist": 1})
    w.check("enabled=1 persist=1", st(), {"permissive": 1, "permissive_on_start": 1})
    w.check("  file saved", w.conf_value("permissive_on_start"), "1")
    w.restart_guard()
    w.check("  after restart: on", st(), {"permissive": 1})
    w.expect_probe("  after restart: unknown allowed", w.new_exe("pp"), "SCREEN", "ALLOW")

    w.ctl({"cmd": "SET_PERMISSIVE", "enabled": 0, "persist": 1})
    w.check("enabled=0 persist=1", st(), {"permissive": 0, "permissive_on_start": 0})
    w.check("  file saved", w.conf_value("permissive_on_start"), "0")
    w.restart_guard()
    w.check("  after restart: off", st(), {"permissive": 0})
    w.expect_probe("  after restart: unknown is asked", w.new_exe("pp"), "SCREEN", "DENY+prompt")


@scenario("guard_restart_first_request")
def _(w):
    w.expect_probe("before: unknown program is asked", w.new_exe("gr"), "SCREEN", "DENY+prompt")
    w.restart_guard(absorb=False)
    w.expect_probe("first request after a guard restart is asked too", w.new_exe("gr"), "SCREEN", "DENY+prompt")
    w.expect_probe("second request after a guard restart", w.new_exe("gr"), "SCREEN", "DENY+prompt")


# ---------------------------------------------------------------- answers

# Dialog button values (xisguard.c, show_xisserve_dialog / process_next_alert).
ANSWERS = {
    "allow": 0, "deny": 2, "trust": 3, "allow_exact": 4, "trust_exact": 7,
    "allow_session": 1, "trust_session": 6,
}


def answer_body(w, kind, secure):
    code = ANSWERS[kind]
    allowish = kind != "deny"
    exact = kind.endswith("_exact")
    trust = kind.startswith("trust")
    session = kind.endswith("_session")
    exe = w.new_exe("an")
    args = "--hold-ms %d" % M.PROBE_HOLD_MS
    key = exe + "|" + args if exact else exe
    rule = None if session else "%s %s %s" % ("DENY" if kind == "deny" else "ALLOW",
                                             "ALL" if trust else "SCREEN", key)

    w.answer(code)
    w.expect_probe("answer %s (%d): granted after the dialog" % (kind, code) if allowish
                   else "answer deny (2): denied", exe, "SCREEN",
                   ("ALLOW" if allowish else "DENY") + "+prompt", retry=allowish)
    w.answer(99)
    time.sleep(1)   # secure mode: pkexec + server RELOAD

    if secure:
        w.check("secure: rule written to the system rules", w.system_rules, [rule] if rule else [])
        w.check("secure: perms.conf untouched", w.perms_rules, [])
        w.check("secure: pkexec called once" if rule else "secure: pkexec not called",
                w.pkexec_calls, 1 if rule else 0)
    else:
        w.check("rule written to perms.conf", w.perms_rules, [rule] if rule else [])
        w.check("system rules untouched", w.system_rules, [])
        w.check("pkexec not called", w.pkexec_calls, 0)

    again = "ALLOW" if allowish else "DENY"
    w.expect_probe("same program again: no dialog", exe, "SCREEN", again)
    w.expect_probe("same program, other action", exe, "SELECTION", "ALLOW" if trust else "DENY+prompt")
    w.expect_probe("same program, other arguments", exe, "SCREEN",
                   "DENY+prompt" if exact else again, hold=M.PROBE_HOLD_MS + 100)

    w.restart_guard()
    w.expect_probe("after a guard restart" + (": session grant gone" if session else ": rule kept"),
                   exe, "SCREEN", "DENY+prompt" if session else again)

    if rule:
        w.stop_guard()
        want = again if secure else "DENY"
        w.expect_probe("guard stopped: %s" % ("system rule still applies" if secure
                                              else "user rule needs the guard"), exe, "SCREEN", want)


for _kind in ANSWERS:
    for _secure in (0, 1):
        def _make(kind=_kind, secure=_secure):
            scenario("answer-%s-%s" % (kind, "secure" if secure else "normal"),
                     conf={"secure_mode": secure})(lambda w: answer_body(w, kind, secure))
        _make()


def pkexec_cancel_setup(w):
    M.write(W + "/pkexec_fail", "")


@scenario("answer-allow-secure-pkexec_cancelled", conf={"secure_mode": 1}, setup=pkexec_cancel_setup)
def _(w):
    exe = w.new_exe("pc")
    w.answer(0)
    w.expect_probe("Allow with polkit cancelled: allowed for the session", exe, "SCREEN", "ALLOW+prompt",
                   retry=True)
    w.answer(99)
    time.sleep(1)
    w.check("pkexec was tried", w.pkexec_calls, 1)
    w.check("nothing written to the system rules", w.system_rules, [])
    w.check("nothing written to perms.conf", w.perms_rules, [])
    w.expect_probe("same session: still allowed", exe, "SCREEN", "ALLOW")
    w.restart_guard()
    w.expect_probe("after a guard restart: asked again", exe, "SCREEN", "DENY+prompt")


# ---------------------------------------------------------------- report

def report_lines(results):
    lines = ["## Scenarios", ""]
    passed = 0
    failed_lines = []
    for r in results:
        bad = [c for c in r.get("checks", []) if not c["ok"]]
        ok = not bad and not r.get("error")
        passed += ok
        if not ok:
            for c in bad:
                failed_lines.append("| %s | %s | `%s` | `%s` |" % (
                    r["scenario"], c["what"], json.dumps(c["want"]), json.dumps(c["got"])))
            if r.get("error"):
                failed_lines.append("| %s | ERROR | | `%s` |" % (r["scenario"], r["error"]))
    if failed_lines:
        lines += ["| scenario | check | want | got |", "|---|---|---|---|"] + failed_lines + [""]
    lines += ["| scenario | checks | result |", "|---|---|---|"]
    for r in results:
        bad = [c for c in r.get("checks", []) if not c["ok"]]
        lines.append("| %s | %d | %s |" % (r["scenario"], len(r.get("checks", [])),
                                           "ERROR" if r.get("error") else ("FAIL (%d)" % len(bad) if bad else "PASS")))
    lines.append("")
    return lines, passed
