#!/usr/bin/env python3
"""The acceptance checks from the arbiter requirements spec (section 8),
run against the bench fakes. No radio, no real program.

    python3 tests/scenarios.py                       the Python arbiter (in-process)
    ARBITER_BIN=build/arbiter python3 tests/scenarios.py   the C++ binary (subprocess)
    python3 tests/scenarios.py 1 4                   only these

Both must pass the same checks: that is the parity proof for the port.
In binary mode the checks read the fakes' records and the arbiter's log
file, never the program's internals.
"""

import os
import signal
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, ROOT)
sys.path.insert(0, HERE)

import arbiter as A          # noqa: E402
from fakes import FakeFlrig, FakeFldigi, FakeSubspace, VaracLog   # noqa: E402

BIN = os.environ.get("ARBITER_BIN", "")
POLL_MS = 50          # bench: faster than the field default of 200
FRAME_GAP_S = 0.8     # bench: the fake's gap between frames is 0.3 s
RENEW_S = 1.0         # bench: see renews happen within a scenario


class Bench:
    """Fakes plus one arbiter: the Python module in a thread, or the binary as a subprocess."""

    def __init__(self, lease_s=300.0, max_connections=2, holds=("on", "on", "on"), extra_ini=""):
        self.dir = tempfile.mkdtemp(prefix="arbiter-bench-")
        self.flrig = FakeFlrig()
        self.fldigi = FakeFldigi(self.flrig)
        self.subspace = FakeSubspace(self.flrig, lease_s=lease_s, max_connections=max_connections)
        self.varalog = VaracLog(os.path.join(self.dir, "VarAC_vara_commands.log"))
        self.ini = os.path.join(self.dir, "arbiter.ini")
        self.logfile = os.path.join(self.dir, "arbiter.log")
        with open(self.ini, "w") as f:
            # only the overrides: both arbiters read the built-in defaults first
            f.write("[ports]\nflrig_port = %d\nsubspace_port = %d\nfldigi_port = %d\n" % (
                self.flrig.port, self.subspace.port, self.fldigi.port))
            f.write("\n[holds]\nsubspace = %s\nfldigi = %s\nvarac = %s\n" % holds)
            f.write("\n[timing]\npoll_ms = %d\nsubspace_frame_gap_s = %s\nrenew_s = %s\nreconnect_s = 0.5\nstate_poll_s = 0.5\n"
                    % (POLL_MS, FRAME_GAP_S, RENEW_S))
            f.write(extra_ini)
            f.write("\n[varac]\nlog_path = %s\n" % self.varalog.path)
        self.arb = None
        self.thread = None
        self.proc = None

    # -- life cycle -------------------------------------------------------------
    def start(self, wait_connected=True):
        if BIN:
            self.proc = subprocess.Popen([BIN, "--console", "--ini", self.ini],
                                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        else:
            s = A.Settings(self.ini)
            self.log = A.Log(self.logfile, echo=False)
            self.arb = A.Arbiter(s, self.log)
            self.thread = threading.Thread(target=self.arb.run, daemon=True)
            self.thread.start()
        if wait_connected:
            wait_for(lambda: self.logged("Subspace: connected") and self.logged("fldigi: connected")
                     and self.logged("flrig: connected"), 6.0)
            time.sleep(0.3)

    def stop_arbiter(self, hard=False):
        """Clean close (window close / SIGTERM), or a hard kill."""
        if BIN:
            if self.proc and self.proc.poll() is None:
                if hard:
                    self.proc.kill()
                else:
                    self.proc.send_signal(signal.SIGTERM)
                self.proc.wait(8.0)
        elif self.arb:
            self.arb.stop.set()
            self.thread.join(5.0)

    def stop(self):
        self.stop_arbiter()
        for f in (self.subspace, self.fldigi, self.flrig):
            f.close()

    # -- what the arbiter did, read from its log --------------------------------------
    def log_lines(self):
        try:
            with open(self.logfile, encoding="utf-8") as f:
                return [l.rstrip("\n") for l in f]
        except OSError:
            return []

    def logged(self, text):
        return any(text in l for l in self.log_lines())

    def held(self, name):
        """True after the last 'disabled' line for that program without a later release."""
        state = False
        for l in self.log_lines():
            if ("  %s: " % name) not in l:
                continue
            body = l.split("  %s: " % name, 1)[1]
            if body.startswith("disabled ("):
                state = True
            elif body.startswith("released"):
                state = False
        return state

    def set_enabled_requests(self):
        return [(p.get("ENABLED"), p.get("REASON")) for t, p, _ in self.subspace.requests if t == "TX.SET_ENABLED"]


def wait_for(cond, timeout, step=0.02):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if cond():
            return True
        time.sleep(step)
    return cond()


def check(ok, what, fails):
    print("   %s  %s" % ("ok  " if ok else "FAIL", what))
    if not ok:
        fails.append(what)


# ----------------------------------------------------------------------------

def scenario_1():
    """Replay of 27 Oct: a VARA link opens, then a Subspace auto-reply comes due."""
    b = Bench()
    fails = []
    try:
        b.start()
        b.varalog.line("CONNECTED")
        check(wait_for(lambda: b.held("Subspace"), 3.0), "Subspace held after CONNECTED", fails)
        reqs = b.set_enabled_requests()
        check(bool(reqs) and reqs[0] == (False, A.R_VARA), "first request is TX.SET_ENABLED false, reason 'VARA link'", fails)
        check(b.subspace.tx_enabled is False and b.subspace.remote, "station shows TX off by remote", fails)
        check(wait_for(lambda: b.held("fldigi") and b.fldigi.rx_only, 2.0), "fldigi held with rx_only", fails)
        b.varalog.line("PTT ON")
        b.flrig.ptt = 1
        time.sleep(0.4)
        b.varalog.line("PTT OFF")
        b.flrig.ptt = 0
        sent = b.subspace.auto_reply_due()
        check(sent is False and b.subspace.refused_sends == 1, "auto-reply refused during the link, nothing keyed", fails)
        check(all(c[0] != "set_ptt" for c in b.flrig.calls), "no set_ptt reached flrig", fails)
        time.sleep(RENEW_S * 2.2)
        check(len(b.set_enabled_requests()) >= 3, "hold renewed while the link stays open", fails)
        b.varalog.line("DISCONNECTED")
        check(wait_for(lambda: not b.held("Subspace") and not b.held("fldigi"), 3.0), "released after DISCONNECTED", fails)
        check(wait_for(lambda: b.subspace.tx_enabled and not b.fldigi.rx_only, 2.0), "station TX on again, fldigi rx_tx", fails)
        check(b.subspace.auto_reply_due() is True, "the reply goes out after the link", fails)
    finally:
        b.stop()
    return fails


def scenario_2():
    """VARA keys while a Subspace message is in progress: Subspace finishes, no hold mid-message."""
    b = Bench()
    fails = []
    try:
        b.start()
        th = threading.Thread(target=b.subspace.transmit, kwargs=dict(frames=3, frame_s=0.5, gap_s=0.3))
        th.start()
        wait_for(lambda: b.subspace.transmitting, 2.0)
        time.sleep(0.4)          # the RIG.PTT event has reached the arbiter by now
        b.varalog.line("CONNECTED")
        b.varalog.line("PTT ON")
        th.join(5.0)
        check(len(b.subspace.sent) == 1, "Subspace message completed, not cut by a hold", fails)
        check(wait_for(lambda: b.held("Subspace"), 4.0), "Subspace held after its message ended (link still open)", fails)
        check(all(c[0] != "set_ptt" for c in b.flrig.calls), "no PTT refused or sent by the arbiter", fails)
        b.varalog.line("PTT OFF")
        b.varalog.line("DISCONNECTED")
        check(wait_for(lambda: not b.held("Subspace"), 3.0), "released after DISCONNECTED", fails)
    finally:
        b.stop()
    return fails


def scenario_3():
    """Arbiter killed while holding: Subspace releases itself at the lease; fldigi stays rx_only.
    Clean close: both release at once."""
    b = Bench(lease_s=3.0)
    fails = []
    try:
        if BIN:
            b.start()
        else:
            b.proc = subprocess.Popen([sys.executable, os.path.join(ROOT, "arbiter.py"), "--console", "--ini", b.ini],
                                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            wait_for(lambda: b.logged("Subspace: connected"), 6.0)
        b.varalog.line("CONNECTED")
        check(wait_for(lambda: b.subspace.tx_enabled is False and b.fldigi.rx_only, 5.0), "both held by the arbiter process", fails)
        b.proc.kill()
        b.proc.wait(5.0)
        check(wait_for(lambda: b.subspace.tx_enabled, 6.0), "Subspace released itself when the 3 s lease ran out", fails)
        check(b.fldigi.rx_only, "fldigi still rx_only after a hard kill (documented fail-closed case)", fails)
        b.fldigi.rx_only = False
        b.subspace.requests.clear()
        if BIN:
            b.proc = subprocess.Popen([BIN, "--console", "--ini", b.ini], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        else:
            b.proc = subprocess.Popen([sys.executable, os.path.join(ROOT, "arbiter.py"), "--console", "--ini", b.ini],
                                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        check(wait_for(lambda: b.subspace.tx_enabled is False and b.fldigi.rx_only, 6.0), "held again by a new arbiter", fails)
        b.proc.send_signal(signal.SIGTERM)
        b.proc.wait(8.0)
        check(wait_for(lambda: b.subspace.tx_enabled and not b.fldigi.rx_only, 3.0), "clean close released both at once", fails)
    finally:
        if b.proc and b.proc.poll() is None:
            b.proc.kill()
        b.varalog.line("DISCONNECTED")
        b.proc = None
        b.stop()
    return fails


def scenario_4():
    """Operator re-enables held Subspace: no re-hold until the busy period ends."""
    b = Bench()
    fails = []
    try:
        b.start()
        b.varalog.line("CONNECTED")
        check(wait_for(lambda: b.held("Subspace"), 3.0), "held", fails)
        n = len(b.set_enabled_requests())
        b.subspace.operator_click(True)
        check(wait_for(lambda: b.logged("operator re-enabled"), 2.0), "override seen and logged", fails)
        time.sleep(RENEW_S * 2)
        offs = [r for r in b.set_enabled_requests()[n:] if r[0] is False]
        check(not offs, "no new hold while the link stays open", fails)
        check(b.subspace.tx_enabled, "station TX stays on", fails)
        b.varalog.line("DISCONNECTED")
        check(wait_for(lambda: b.logged("override forgotten"), 3.0), "busy period ended, override forgotten", fails)
        b.varalog.line("CONNECTED")
        check(wait_for(lambda: len([r for r in b.set_enabled_requests()[n:] if r[0] is False]) >= 1, 3.0),
              "held again in the next busy period", fails)
        b.varalog.line("DISCONNECTED")
        wait_for(lambda: not b.held("Subspace"), 3.0)
    finally:
        b.stop()
    return fails


def scenario_5():
    """The dial moves during a VARA link: no action, one log line."""
    b = Bench()
    fails = []
    try:
        b.start()
        b.varalog.line("CONNECTED")
        wait_for(lambda: b.held("Subspace"), 3.0)
        b.flrig.vfo = 7110000
        time.sleep(1.6)      # the dial is read once a second
        check(all(c[0] != "set_vfo" and c[0] != "set_ptt" for c in b.flrig.calls), "arbiter touched nothing on flrig", fails)
        dial = [l for l in b.log_lines() if "dial 7110000" in l]
        check(len(dial) == 1 and "VARA link" in dial[0], "dial change logged once, with the link noted", fails)
        b.varalog.line("DISCONNECTED")
        wait_for(lambda: not b.held("Subspace"), 3.0)
    finally:
        b.stop()
    return fails


def scenario_6():
    """fldigi in an over when a VARA link opens: no rx_only until the over ends."""
    b = Bench()
    fails = []
    try:
        b.start()
        b.fldigi._tx()
        time.sleep(0.4)          # the arbiter has polled TX by now
        b.varalog.line("CONNECTED")
        time.sleep(0.6)
        check(not any(c[0] == "rx_only" for c in b.fldigi.calls), "no rx_only while fldigi transmits", fails)
        check(wait_for(lambda: b.held("Subspace"), 2.0), "Subspace held meanwhile (reason VARA link or fldigi TX)", fails)
        b.fldigi._rx()
        check(wait_for(lambda: b.fldigi.rx_only, 2.0), "rx_only once the over ended", fails)
        b.fldigi._tx()
        check(bool(b.fldigi.calls) and b.fldigi.calls[-1][0] == "tx_refused", "a macro send is refused while held", fails)
        b.varalog.line("DISCONNECTED")
        check(wait_for(lambda: not b.fldigi.rx_only, 3.0), "rx_tx after the link", fails)
    finally:
        b.stop()
    return fails


def scenario_7():
    """Subspace 'Connections Full': plain-words status, retry, and hold once a slot opens."""
    b = Bench(max_connections=0)
    fails = []
    try:
        b.start(wait_connected=False)
        check(wait_for(lambda: b.logged("raise 'TCP max connections'"), 5.0), "refusal reported in plain words", fails)
        b.subspace.max_connections = 2
        check(wait_for(lambda: b.logged("Subspace: connected"), 5.0), "reconnected once a slot opened", fails)
        b.varalog.line("CONNECTED")
        check(wait_for(lambda: b.held("Subspace"), 3.0), "held after reconnect", fails)
        b.varalog.line("DISCONNECTED")
        wait_for(lambda: not b.held("Subspace"), 3.0)
    finally:
        b.stop()
    return fails


def scenario_8():
    """VarAC held through its cluster lock: Arbiter row busy while fldigi transmits, keepalive
    refreshed, busy 0 on release, row removed when the arbiter closes."""
    b = Bench(extra_ini="\n[varac]\nvarac_renew_s = 0.5\n")
    fails = []
    try:
        # the [varac] section is written twice by Bench: fold the renew key in after the path
        with open(b.ini) as f:
            txt = f.read()
        txt = txt.replace("\n[varac]\nvarac_renew_s = 0.5\n", "").rstrip("\n") + "\nvarac_renew_s = 0.5\n"
        with open(b.ini, "w") as f:
            f.write(txt)
        b.start()
        check(b.varalog.arbiter_row() is None, "no Arbiter row before any hold", fails)
        b.fldigi._tx()
        check(wait_for(lambda: b.held("VarAC"), 3.0), "VarAC held while fldigi transmits", fails)
        row = b.varalog.arbiter_row()
        check(row is not None and row[0] == 1, "Arbiter row present and busy", fails)
        ka = row[1] if row else None
        time.sleep(1.2)
        row2 = b.varalog.arbiter_row()
        check(row2 is not None and row2[1] != ka, "keepalive refreshed while held", fails)
        b.fldigi._rx()
        check(wait_for(lambda: not b.held("VarAC"), 3.0), "VarAC released when fldigi stops", fails)
        check(b.varalog.arbiter_row()[0] == 0, "Arbiter row busy 0 after release", fails)
        b.stop_arbiter()
        check(wait_for(lambda: b.varalog.arbiter_row() is None, 3.0), "Arbiter row removed when the arbiter closed", fails)
    finally:
        b.stop()
    return fails


SCENARIOS = {
    1: ("Replay of 27 Oct: auto-reply due during a VARA link", scenario_1),
    2: ("VARA keys while a Subspace message is in progress", scenario_2),
    3: ("Arbiter killed while holding; clean close", scenario_3),
    4: ("Operator re-enables held Subspace", scenario_4),
    5: ("Dial moves during a VARA link", scenario_5),
    6: ("fldigi in an over when a VARA link opens", scenario_6),
    7: ("Subspace 'Connections Full'", scenario_7),
    8: ("VarAC held through its cluster lock", scenario_8),
}


def main(argv):
    picks = [int(a) for a in argv] or sorted(SCENARIOS)
    print("arbiter under test: %s" % (BIN or "arbiter.py (in-process)"))
    total_fail = 0
    for n in picks:
        title, fn = SCENARIOS[n]
        print("%d. %s" % (n, title))
        fails = fn()
        print("   => %s\n" % ("PASS" if not fails else "FAIL (%d)" % len(fails)))
        total_fail += len(fails)
    print("ALL PASS" if not total_fail else "%d check(s) failed" % total_fail)
    return 0 if not total_fail else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
