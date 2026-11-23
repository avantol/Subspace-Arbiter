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
from fakes import (FakeFlrig, FakeFldigi, FakeSubspace, FakeVara, FakeVarac, VaracLog,   # noqa: E402
                   free_pair, free_port)

BIN = os.environ.get("ARBITER_BIN", "")
if BIN:
    # The test arbiter runs under another process name. Under the name "arbiter" CAT Watch
    # reports "2 copies of the Arbiter" at a station whose own Arbiter runs (WM8Q 2026-11-22,
    # three times), and the station launcher's check for a running Arbiter finds it.
    _named = os.path.join(os.path.dirname(os.path.abspath(BIN)), "bench-test-arb")
    if not os.path.exists(_named):
        os.symlink(os.path.abspath(BIN), _named)
    BIN = _named
POLL_MS = 50          # bench: faster than the field default of 200
FRAME_GAP_S = 0.8     # bench: the fake's gap between frames is 0.3 s
RENEW_S = 1.0         # bench: see renews happen within a scenario


class Bench:
    """Fakes plus one arbiter: the Python module in a thread, or the binary as a subprocess."""

    def __init__(self, lease_s=300.0, max_connections=2, holds=("on", "on", "on"), extra_ini="",
                 flrig_has_block=True, varac_extra="", subspace_ask=False, fldigi_ask=False, layer=False):
        """subspace_ask / fldigi_ask: that fake is the program of branch study-ask-grant.
        layer: varac/lever = layer, with a fake VARA behind it; connect_varac() is VarAC."""
        self.dir = tempfile.mkdtemp(prefix="arbiter-bench-")
        self.flrig = FakeFlrig(has_block=flrig_has_block)
        self.fldigi = FakeFldigi(self.flrig, has_ask=fldigi_ask)
        self.subspace = FakeSubspace(self.flrig, lease_s=lease_s, max_connections=max_connections,
                                     has_ask=subspace_ask)
        self.vara = None
        self.varac = None
        if layer:
            self.vara = FakeVara()
            self.layer_port = free_pair()
            self.layer_kiss = free_port()
            varac_extra += ("lever = layer\nlayer_port = %d\nlayer_kiss_port = %d\n"
                            "layer_vara_port = %d\nlayer_vara_kiss_port = %d\n"
                            % (self.layer_port, self.layer_kiss, self.vara.cmd_port, self.vara.kiss_port))
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
            f.write(varac_extra)
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

    def connect_varac(self):
        """VarAC connects to the layer (its three connections and its two LISTEN lines)."""
        self.varac = FakeVarac(self.layer_port, self.layer_kiss)
        ok = wait_for(lambda: self.logged("layer: KISS: connected to VARA") and
                      len(self.vara.got("LISTEN CQ")) == 1, 4.0)
        time.sleep(0.2)
        return ok

    def stop(self):
        self.stop_arbiter()
        for f in (self.varac, self.vara, self.subspace, self.fldigi, self.flrig):
            if f:
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
        # the dial is read once a second; with no RIG.FREQ from Subspace the line waits 2 s for one
        wait_for(lambda: b.logged("dial 7110000"), 4.0)
        time.sleep(0.5)
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
        bk = b.varalog.db + ".arbiter-backup"
        check(os.path.exists(bk), "a copy of VarAC.db was kept before the first write", fails)
        if os.path.exists(bk):
            import sqlite3 as _sq
            c = _sq.connect(bk)
            n = c.execute("SELECT COUNT(*) FROM instance WHERE name='Arbiter'").fetchone()[0]
            c.close()
            check(n == 0, "the copy is VarAC.db as it was before the arbiter's row", fails)
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


def scenario_9():
    """fldigi's release goes out on the arbiter's own link, and is sent again until fldigi takes it."""
    b = Bench()
    fails = []
    try:
        b.start()
        for n in (1, 2):
            b.varalog.line("CONNECTED")
            check(wait_for(lambda: b.held("fldigi") and b.fldigi.rx_only, 3.0), "fldigi held (%d)" % n, fails)
            b.varalog.line("DISCONNECTED")
            check(wait_for(lambda: not b.held("fldigi") and not b.fldigi.rx_only, 3.0), "fldigi released (%d)" % n, fails)
        check(sum("fldigi: connected" in l for l in b.log_lines()) == 1,
              "'fldigi: connected' logged once, not once per release", fails)
        b.fldigi.refuse_rx_tx = 2
        b.varalog.line("CONNECTED")
        check(wait_for(lambda: b.held("fldigi") and b.fldigi.rx_only, 3.0), "fldigi held (3)", fails)
        b.varalog.line("DISCONNECTED")
        check(wait_for(lambda: b.logged("fldigi: rx_tx not accepted; sent again until fldigi answers"), 3.0),
              "the refused rx_tx is logged", fails)
        check(wait_for(lambda: not b.fldigi.rx_only, 6.0), "rx_tx sent again until fldigi takes it", fails)
        check(wait_for(lambda: b.logged("fldigi: rx_tx accepted"), 2.0), "the accepted retry is logged", fails)
        check(sum(c[0] == "rx_tx_refused" for c in b.fldigi.calls) == 2, "both refusals seen by fldigi", fails)
    finally:
        b.stop()
    return fails


def scenario_10():
    """[fail-open] Arbiter killed while holding fldigi, then started again with the channel clear:
    the new arbiter restores fldigi (rx_tx) at once, and a clean close leaves no marker behind."""
    b = Bench()
    fails = []
    marker = os.path.join(b.dir, "fldigi_held.flag")
    try:
        if BIN:
            b.start()
        else:
            b.proc = subprocess.Popen([sys.executable, os.path.join(ROOT, "arbiter.py"), "--console", "--ini", b.ini],
                                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            wait_for(lambda: b.logged("Subspace: connected"), 6.0)
        b.varalog.line("CONNECTED")
        check(wait_for(lambda: b.fldigi.rx_only, 5.0), "fldigi held", fails)
        check(os.path.exists(marker), "the hold left its marker", fails)
        b.proc.kill()
        b.proc.wait(5.0)
        b.varalog.line("DISCONNECTED")
        time.sleep(0.5)
        check(b.fldigi.rx_only, "fldigi still rx_only after the kill", fails)
        if BIN:
            b.proc = subprocess.Popen([BIN, "--console", "--ini", b.ini], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        else:
            b.proc = subprocess.Popen([sys.executable, os.path.join(ROOT, "arbiter.py"), "--console", "--ini", b.ini],
                                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        check(wait_for(lambda: not b.fldigi.rx_only, 6.0), "the next arbiter restored fldigi", fails)
        check(wait_for(lambda: b.logged("fldigi: restored to transmit"), 3.0), "the restore is logged", fails)
        check(wait_for(lambda: not os.path.exists(marker), 3.0), "marker removed", fails)
        b.varalog.line("CONNECTED")
        check(wait_for(lambda: b.fldigi.rx_only and os.path.exists(marker), 5.0), "held again, marker written", fails)
        b.proc.send_signal(signal.SIGTERM)
        b.proc.wait(8.0)
        check(wait_for(lambda: not b.fldigi.rx_only and not os.path.exists(marker), 3.0),
              "clean close: released and no marker left", fails)
    finally:
        if b.proc and b.proc.poll() is None:
            b.proc.kill()
        b.varalog.line("DISCONNECTED")
        b.proc = None
        b.stop()
    return fails


def scenario_11():
    """One failed fldigi state query during a hold does not forget the hold: the release still
    sends rx_tx (1.0.2 cleared its own 'held' on any failed query, so fldigi stayed receive-only)."""
    b = Bench()
    fails = []
    try:
        b.start()
        b.varalog.line("CONNECTED")
        check(wait_for(lambda: b.held("fldigi") and b.fldigi.rx_only, 3.0), "fldigi held", fails)
        b.fldigi.fail_state = 1
        check(wait_for(lambda: b.fldigi.fail_state == 0, 3.0), "one state query failed during the hold", fails)
        time.sleep(0.5)
        b.varalog.line("DISCONNECTED")
        check(wait_for(lambda: not b.held("fldigi"), 3.0), "the arbiter released fldigi", fails)
        check(wait_for(lambda: not b.fldigi.rx_only, 3.0), "fldigi got rx_tx after the failed query", fails)
    finally:
        b.stop()
    return fails


def scenario_12():
    """[txpending] A Subspace frame waiting for its key-up (TX.PENDING) holds VarAC and fldigi before
    Subspace keys (W8UFO 10 Nov 19:48:44), never Subspace itself; it lapses after the key-up time
    when no PTT comes; a VARA link that opens during the wait still holds Subspace."""
    b = Bench()
    fails = []
    try:
        b.start()
        b.subspace.pending(2500)
        check(wait_for(lambda: b.held("VarAC") and b.held("fldigi"), 1.5),
              "VarAC and fldigi held while the frame waits", fails)
        check(b.logged("VarAC: disabled (%s)" % A.R_SUBSPACE), "VarAC's hold reason is Subspace TX", fails)
        check(not b.held("Subspace") and b.subspace.tx_enabled, "Subspace itself not held", fails)
        th = threading.Thread(target=b.subspace.transmit, kwargs=dict(frames=1, frame_s=0.4))
        th.start()
        wait_for(lambda: b.subspace.transmitting, 2.0)
        b.subspace.pending(None)                     # the station: keyed, so no longer waiting
        th.join(5.0)
        check(len(b.subspace.sent) == 1, "the frame went out", fails)
        check(wait_for(lambda: not b.held("VarAC") and not b.held("fldigi"), 4.0),
              "VarAC and fldigi released after the message", fails)

        b.subspace.pending(500)                      # a frame that never keys (halted, no PENDING false)
        check(wait_for(lambda: b.held("fldigi"), 1.5), "held again for the next waiting frame", fails)
        check(wait_for(lambda: not b.held("fldigi") and not b.held("VarAC"), 4.0),
              "the claim lapsed after its key-up time with no PTT", fails)

        b.subspace.pending(4000)
        check(wait_for(lambda: b.held("fldigi"), 1.5), "held for a third waiting frame", fails)
        b.varalog.line("CONNECTED")
        check(wait_for(lambda: b.held("Subspace"), 3.0), "a VARA link opening during the wait holds Subspace", fails)
        check(b.logged("Subspace: disabled (%s)" % A.R_VARA), "Subspace's hold reason is the VARA link", fails)
        b.varalog.line("DISCONNECTED")
        check(wait_for(lambda: not b.held("Subspace") and not b.held("fldigi") and not b.held("VarAC"), 6.0),
              "all released after DISCONNECTED", fails)
    finally:
        b.stop()
    return fails


def scenario_13():
    """[unkey-block] VarAC held for a Subspace message while VARA keeps asking to transmit (W8UFO
    10 Nov 19:48): VARA's refused attempts are not VARA TX, VarAC's hold is renewed until the
    message ends and then released, and while VarAC is held flrig is told to ignore VarAC's
    un-keys, so VARA's PTT OFF does not cut Subspace's frames."""
    b = Bench(varac_extra="varac_renew_s = 0.5\nunkey_block_renew_s = 0.5\n")
    fails = []
    try:
        b.start()
        th = threading.Thread(target=b.subspace.transmit, kwargs=dict(frames=4, frame_s=1.5))
        th.start()
        check(wait_for(lambda: b.held("VarAC"), 2.0), "VarAC held for the Subspace message", fails)
        blocks = lambda: [c[1] for c in b.flrig.calls if c[0] == "block_unkey"]
        check(wait_for(lambda: any(ms > 0 for _, ms in blocks()), 2.0), "flrig told to block un-keys", fails)
        check(all(lbl == "XML-RPC.NET" for lbl, ms in blocks() if ms > 0), "the label is VarAC's (XML-RPC.NET)", fails)
        check(wait_for(lambda: b.logged("flrig: un-keys from XML-RPC.NET blocked while VarAC is held"), 2.0),
              "the block is logged", fails)
        row1 = b.varalog.arbiter_row()
        # VARA keeps asking; VarAC refuses each key and sends each un-key (bench 11 Nov)
        tried_keyed = 0
        for _ in range(6):
            b.varalog.line("PTT ON")
            time.sleep(0.35)
            b.varalog.line("PTT OFF")
            if b.flrig.ptt == 1:
                tried_keyed += 1
                b.flrig.labelled_set_ptt(0, "XML-RPC.NET")
                check(b.flrig.ptt == 1, "VarAC's un-key during a Subspace frame ignored at flrig", fails)
            time.sleep(0.35)
        check(tried_keyed > 0, "VarAC's un-key was tried while Subspace was keyed (%d times)" % tried_keyed, fails)
        check(not b.held("Subspace") and not b.logged("hold reason now %s" % A.R_VARA_TX)
              and not b.logged("Subspace: disabled (%s)" % A.R_VARA_TX),
              "VARA's refused attempts are not VARA TX: nothing held for them", fails)
        row2 = b.varalog.arbiter_row()
        check(row1 is not None and row2 is not None and row2[1] != row1[1] and row2[0] == 1,
              "VarAC's hold renewed while VARA kept asking", fails)
        th.join(12.0)
        check(len(b.subspace.sent) == 1, "the message went out", fails)
        check(wait_for(lambda: not b.held("VarAC"), 4.0), "VarAC released when the message ended", fails)
        check(wait_for(lambda: blocks() and blocks()[-1][1] == 0, 3.0), "the un-key block ended with the hold", fails)
        check(wait_for(lambda: b.logged("flrig: un-key block ended"), 2.0), "the end is logged", fails)
        b.flrig.key(1, "Hamlib")
        b.flrig.labelled_set_ptt(0, "XML-RPC.NET")
        check(b.flrig.ptt == 0, "with VarAC released, its un-key is carried out again", fails)
    finally:
        b.stop()
    return fails


def scenario_14():
    """[unkey-block] A flrig without rig.block_unkey (stock, or flrig-subspace before 8): said once,
    flrig still counted as answering, VarAC held and released by its lock as before."""
    b = Bench(flrig_has_block=False)
    fails = []
    try:
        b.start()
        b.fldigi._tx()
        check(wait_for(lambda: b.held("VarAC"), 3.0), "VarAC held while fldigi transmits", fails)
        msg = "flrig: this flrig has no un-key block"
        check(wait_for(lambda: b.logged(msg), 2.0), "the missing command is logged", fails)
        time.sleep(1.5)
        check(sum(msg in l for l in b.log_lines()) == 1, "logged once, not asked again", fails)
        check(not b.logged("flrig: not answering"), "flrig still counted as answering", fails)
        b.fldigi._rx()
        check(wait_for(lambda: not b.held("VarAC"), 3.0), "VarAC released when fldigi stops", fails)
    finally:
        b.stop()
    return fails


def scenario_15():
    """[fldigi-frozen] fldigi answered, then stopped (W1NEM 12 Nov 18:29): a hold that comes due is
    not silent ("not held ... not answering since"), said once per busy period; when fldigi answers
    again it is held as before."""
    b = Bench()
    fails = []
    try:
        b.start()
        b.fldigi.fail_state = 10 ** 6
        check(wait_for(lambda: b.logged("fldigi: not answering"), 3.0), "fldigi stops answering", fails)
        th = threading.Thread(target=b.subspace.transmit, kwargs=dict(frames=3, frame_s=0.5, gap_s=0.3))
        th.start()
        check(wait_for(lambda: b.held("VarAC"), 2.0), "VarAC held for the Subspace message", fails)
        skip = "fldigi: not held (%s): not answering since " % A.R_SUBSPACE
        check(wait_for(lambda: b.logged(skip), 2.0), "the skipped fldigi hold is logged", fails)
        th.join(5.0)
        check(len(b.subspace.sent) == 1, "the message went out", fails)
        check(sum(skip in l for l in b.log_lines()) == 1, "said once in the busy period, not once per tick", fails)
        check(not b.held("fldigi") and not b.fldigi.rx_only, "nothing sent to the silent fldigi", fails)
        if not BIN:
            check(b.arb.fl.status.startswith("not answering since ") and "fldigi may be frozen" in b.arb.fl.status,
                  "window: 'not answering since ...', no setup remedy", fails)
        wait_for(lambda: not b.held("VarAC"), 4.0)
        b.fldigi.fail_state = 0
        check(wait_for(lambda: sum("fldigi: connected" in l for l in b.log_lines()) == 2, 4.0),
              "fldigi answers again", fails)
        th = threading.Thread(target=b.subspace.transmit, kwargs=dict(frames=1, frame_s=0.5))
        th.start()
        check(wait_for(lambda: b.held("fldigi") and b.fldigi.rx_only, 2.0), "fldigi held as before", fails)
        th.join(5.0)
        check(wait_for(lambda: not b.held("fldigi") and not b.fldigi.rx_only, 4.0), "and released", fails)
        check(sum(skip in l for l in b.log_lines()) == 1, "no skip line while fldigi answers", fails)
    finally:
        b.fldigi.fail_state = 0
        b.stop()
    return fails


def scenario_16():
    """[dial] A dial change is Subspace's when its RIG.FREQ comes within 2 s either side of the
    arbiter's dial reading (W1NEM 12 Nov 21:00:29: read first, logged "program unknown")."""
    b = Bench()
    fails = []
    dial = lambda hz: [l for l in b.log_lines() if "flrig: dial %d " % hz in l]
    try:
        b.start()
        time.sleep(1.2)                       # the starting dial has been read
        b.subspace.push("RIG.FREQ", DIAL=3564000)
        time.sleep(0.2)
        b.flrig.vfo = 3564000                 # RIG.FREQ first, dial reading after
        check(wait_for(lambda: dial(3564000), 3.0), "dial change logged (RIG.FREQ first)", fails)
        check(dial(3564000) and dial(3564000)[0].endswith("(Subspace)"), "credited to Subspace", fails)
        time.sleep(2.5)
        b.flrig.vfo = 7115000                 # dial reading first, RIG.FREQ 1.3 s later
        time.sleep(1.3)
        b.subspace.push("RIG.FREQ", DIAL=7115000)
        check(wait_for(lambda: dial(7115000), 3.0), "dial change logged (RIG.FREQ after)", fails)
        check(dial(7115000) and dial(7115000)[0].endswith("(Subspace)"), "credited to Subspace, not 'program unknown'", fails)
        time.sleep(2.5)
        b.flrig.vfo = 14078000                # no RIG.FREQ at all
        check(wait_for(lambda: dial(14078000), 4.0), "dial change logged (no RIG.FREQ)", fails)
        check(dial(14078000) and dial(14078000)[0].endswith("(program unknown)"), "program unknown", fails)
        check(len(dial(3564000)) == 1 and len(dial(7115000)) == 1, "each change logged once", fails)
    finally:
        b.stop()
    return fails


# ----------------------------------------------------------------------------
# [study] ask / grant and the layer: branch study-ask-grant, the C++ arbiter only

def _study_skip():
    print("   skipped: ask / grant and the layer are in the C++ arbiter only (set ARBITER_BIN)")
    return []


def _asking(b, subspace=True, fldigi=True):
    return wait_for(lambda: (not subspace or b.logged("Subspace: asks before it transmits")) and
                    (not fldigi or b.logged("fldigi: asks before it transmits")), 4.0)


def _send(b, **kw):
    t = threading.Thread(target=b.subspace.send, kwargs=kw, daemon=True)
    t.start()
    return t


def scenario_17():
    """Both programs ask. fldigi's start waits for Subspace's message; Subspace's next message
    waits for fldigi's transmission, and for the radio to un-key after it."""
    if not BIN:
        return _study_skip()
    b = Bench(subspace_ask=True, fldigi_ask=True)
    fails = []
    try:
        b.start()
        check(_asking(b), "the arbiter finds that both programs ask", fails)
        t = _send(b, frames=2, frame_s=0.6, gap_s=0.3)
        check(wait_for(lambda: b.flrig.ptt == 1, 2.0) and b.subspace.sends[0][1] == "granted",
              "radio free: Subspace's message is granted and keys", fails)
        check(b.fldigi.start() == "waiting", "fldigi's start waits inside fldigi", fails)
        check(wait_for(lambda: b.logged("fldigi: waits (Subspace TX)"), 2.0), "the arbiter says what fldigi waits for", fails)
        t.join()
        off_t = time.time()
        check(b.fldigi.state == "RX", "fldigi did not start during Subspace's message", fails)
        check(wait_for(lambda: b.fldigi.state == "TX", 3.0) and b.fldigi.starts[-1][1] == "granted",
              "fldigi starts by the grant after Subspace's message", fails)
        waited = time.time() - off_t
        check(waited >= FRAME_GAP_S - 0.15, "%.2f s after Subspace's last PTT off (frame gap %.1f s)" % (waited, FRAME_GAP_S), fails)
        t = _send(b, frames=1, frame_s=0.5)
        check(wait_for(lambda: b.subspace.wait_reasons == ["fldigi TX"], 2.0), "Subspace's next message waits, told 'fldigi TX'", fails)
        time.sleep(0.5)
        check(b.flrig.key_owner == "fldigi" and len(b.subspace.sends) == 1, "Subspace does not key during fldigi's transmission", fails)
        # fldigi reports RX before its PTT off reaches the radio (measured: about 1 s)
        b.fldigi.waiting = b.fldigi.granted = 0
        b.fldigi.state = "RX"
        check(wait_for(lambda: b.logged("fldigi: transmission over (start "), 2.0) and
              not any("fldigi: transmission over" in l for l in b.log_lines()[:[i for i, l in enumerate(b.log_lines()) if "fldigi: granted" in l][0]]),
              "the end of fldigi's granted transmission is logged, after its grant", fails)
        check(wait_for(lambda: b.subspace.wait_reasons[-1] == "radio keyed", 2.0), "fldigi reports RX, radio still keyed: Subspace told 'radio keyed'", fails)
        time.sleep(0.4)
        check(len(b.subspace.sends) == 1, "no grant onto a keyed radio", fails)
        b.flrig.key(0, "fldigi")
        check(wait_for(lambda: len(b.subspace.sends) == 2, 2.0) and b.subspace.sends[-1][1] == "granted",
              "granted when the radio is un-keyed", fails)
        t.join()
        check(wait_for(lambda: b.logged("Subspace: transmission over (message 2"), 2.0), "Subspace's end report arrives", fails)
        check(not b.set_enabled_requests() and not b.fldigi.rx_only and not b.logged("fldigi: disabled"),
              "neither program's own switch was thrown", fails)
    finally:
        b.stop()
    return fails


def scenario_18():
    """The layer and a Subspace that asks: a message waits for a VARA link to end; VarAC is held
    with BUSY ON / LISTEN OFF during Subspace's message; VarAC's kept CONNECT goes on after it."""
    if not BIN:
        return _study_skip()
    b = Bench(subspace_ask=True, fldigi_ask=True, layer=True, extra_ini="vara_burst_hold_s = 1.0\n")
    fails = []
    try:
        b.start()
        check(b.connect_varac(), "VarAC reaches VARA through the layer, its LISTEN lines passed on", fails)
        check(_asking(b), "the arbiter finds that both programs ask", fails)
        b.vara.say("CONNECTED WM8Q K9AVT 500")
        check(wait_for(lambda: b.logged("VarAC: link open"), 2.0), "VARA link seen in the layer's traffic", fails)
        t = _send(b, frames=1, frame_s=0.8)
        check(wait_for(lambda: b.subspace.wait_reasons == ["VARA link"], 2.0), "Subspace's message waits, told 'VARA link'", fails)
        time.sleep(0.4)
        check(not b.subspace.sends and b.flrig.ptt == 0, "nothing keyed during the link", fails)
        check(not b.set_enabled_requests() and not b.fldigi.rx_only, "no switch thrown in programs that ask", fails)
        b.vara.say("DISCONNECTED")
        t0 = time.time()
        check(wait_for(lambda: len(b.subspace.sends) == 1, 2.0), "granted after DISCONNECTED", fails)
        if b.subspace.sends:
            check(b.subspace.sends[0][2] - t0 < 0.3, "within %.2f s of it" % (b.subspace.sends[0][2] - t0), fails)
        check(wait_for(lambda: b.varac.got("BUSY ON") and b.vara.got("LISTEN OFF"), 1.0),
              "during Subspace's message VarAC is told BUSY ON, VARA LISTEN OFF", fails)
        b.varac.say("CONNECT WM8Q K9AVT")
        time.sleep(0.3)
        check(not b.vara.got("CONNECT WM8Q K9AVT") and b.logged("kept back until the release: CONNECT WM8Q K9AVT"),
              "VarAC's CONNECT is kept back", fails)
        t.join()
        check(wait_for(lambda: b.vara.got("CONNECT WM8Q K9AVT"), 3.0), "passed on to VARA after Subspace's message", fails)
        check(wait_for(lambda: len(b.varac.got("BUSY OFF")) == 1, 1.0) and len(b.vara.got("LISTEN ON")) == 2
              and len(b.vara.got("LISTEN CQ")) == 2, "VarAC told BUSY OFF, VARA given VarAC's LISTEN lines again", fails)
        t = _send(b, frames=1, frame_s=0.3)
        check(wait_for(lambda: b.subspace.wait_reasons[-1:] == ["VARA TX"], 2.0), "the passed CONNECT makes Subspace's next message wait ('VARA TX')", fails)
        b.vara.say("DISCONNECTED")
        check(wait_for(lambda: len(b.subspace.sends) == 2, 2.0), "granted when VARA says DISCONNECTED", fails)
        t.join()
        b.stop_arbiter()
        check(wait_for(lambda: not b.varac.cmd.open, 3.0), "arbiter closed: VarAC's connection to VARA is closed", fails)
    finally:
        b.stop()
    return fails


def scenario_19():
    """The study arbiter with programs that cannot ask: said once each, held as before."""
    if not BIN:
        return _study_skip()
    b = Bench()
    fails = []
    try:
        b.start()
        check(wait_for(lambda: b.logged("Subspace: this Subspace has no ask / grant"), 4.0), "Subspace without ask / grant found (no answer)", fails)
        check(b.logged("fldigi: this fldigi has no ask / grant"), "fldigi without ask / grant found (a fault)", fails)
        b.varalog.line("CONNECTED")
        check(wait_for(lambda: b.held("Subspace") and b.subspace.tx_enabled is False, 3.0), "Subspace held with its TX switch", fails)
        check(wait_for(lambda: b.held("fldigi") and b.fldigi.rx_only, 2.0), "fldigi held with receive-only", fails)
        time.sleep(1.5)
        b.varalog.line("DISCONNECTED")
        check(wait_for(lambda: b.subspace.tx_enabled and not b.fldigi.rx_only, 3.0), "both released after the link", fails)
        said = [l for l in b.log_lines() if "has no ask / grant" in l]
        check(len(said) == 2 and not b.logged("API.ERROR"), "each said once, no error lines", fails)
    finally:
        b.stop()
    return fails


def scenario_20():
    """Subspace asks, fldigi cannot: Subspace's granted message holds fldigi with receive-only."""
    if not BIN:
        return _study_skip()
    b = Bench(subspace_ask=True)
    fails = []
    try:
        b.start()
        check(_asking(b, fldigi=False), "the arbiter finds that Subspace asks", fails)
        t = _send(b, frames=2, frame_s=0.6, gap_s=0.3)
        check(wait_for(lambda: len(b.subspace.sends) == 1, 2.0), "radio free: Subspace's message is granted", fails)
        check(wait_for(lambda: b.fldigi.rx_only, 1.0), "fldigi held with receive-only during it", fails)
        t.join()
        check(wait_for(lambda: not b.fldigi.rx_only, 3.0), "fldigi released after it", fails)
        b.fldigi._tx()
        time.sleep(0.3)
        t = _send(b, frames=1, frame_s=0.4)
        check(wait_for(lambda: b.subspace.wait_reasons == ["fldigi TX"], 2.0), "Subspace's message waits for an fldigi that cannot ask", fails)
        b.fldigi._rx()
        check(wait_for(lambda: len(b.subspace.sends) == 2, 2.0), "granted when fldigi is back in receive", fails)
        t.join()
        check(not b.set_enabled_requests(), "Subspace's TX switch was never thrown", fails)
    finally:
        b.stop()
    return fails


def scenario_21():
    """Subspace stops without a word in the middle of its granted message."""
    if not BIN:
        return _study_skip()
    b = Bench(subspace_ask=True, fldigi_ask=True)
    fails = []
    try:
        b.start()
        check(_asking(b), "the arbiter finds that both programs ask", fails)
        _send(b, frames=1, frame_s=4.0)
        check(wait_for(lambda: b.flrig.ptt == 1, 2.0), "Subspace's message is granted and keys", fails)
        time.sleep(0.3)
        check(b.fldigi.start() == "waiting" and wait_for(lambda: b.logged("fldigi: waits (Subspace TX)"), 2.0),
              "fldigi's start waits for it", fails)
        b.subspace.drop()
        t0 = time.time()
        check(wait_for(lambda: b.logged("Subspace: grant over (Subspace is not connected)"), 2.0), "the grant ends with Subspace's connection", fails)
        check(b.logged("Subspace: not connected; no longer counted as transmitting"), "Subspace no longer counted as transmitting", fails)
        check(wait_for(lambda: b.fldigi.state == "TX", 2.0), "fldigi is granted %.2f s after Subspace stopped" % (time.time() - t0), fails)
        b.fldigi.end()
    finally:
        b.stop()
    return fails


def scenario_22():
    """A waiting message given up by the operator; a grant that no key-up follows."""
    if not BIN:
        return _study_skip()
    b = Bench(subspace_ask=True)
    fails = []
    try:
        b.start()
        check(_asking(b, fldigi=False), "the arbiter finds that Subspace asks", fails)
        b.varalog.line("CONNECTED")
        check(wait_for(lambda: b.logged("VarAC: link open"), 3.0), "VARA link open", fails)
        check(b.subspace.send(give_up_s=0.6) == "given up", "the operator halts the waiting message", fails)
        check(wait_for(lambda: b.logged("Subspace: no longer asks (message 1)"), 2.0), "the arbiter drops the ask", fails)
        b.varalog.line("DISCONNECTED")
        time.sleep(1.0)
        check(not b.logged("Subspace: granted"), "nothing is granted after the link", fails)
        check(b.subspace.send(silent_after_grant=True) == "granted", "a new message is granted", fails)
        check(wait_for(lambda: b.fldigi.rx_only, 1.0), "fldigi held for it", fails)
        t0 = time.time()
        check(wait_for(lambda: b.logged("Subspace: grant over (no key-up followed it)"), 3.0),
              "no key-up follows: the grant ends by itself after %.1f s" % (time.time() - t0), fails)
        check(wait_for(lambda: not b.fldigi.rx_only, 3.0), "fldigi released", fails)
    finally:
        b.stop()
    return fails


def scenario_23():
    """The arbiter stops: the programs' asking ends and they transmit as without it."""
    if not BIN:
        return _study_skip()
    ask = "\n[ask]\nsubspace_lease_ms = 1500\nsubspace_renew_s = 0.5\nfldigi_lease_ms = 600\n"
    fails = []
    b = Bench(subspace_ask=True, fldigi_ask=True, extra_ini=ask)
    try:
        b.start()
        check(_asking(b), "the arbiter finds that both programs ask", fails)
        b.stop_arbiter(hard=True)
        t0 = time.time()
        check(b.fldigi.start() == "waiting", "arbiter killed: fldigi's start still waits (its asking lasts 0.6 s)", fails)
        check(wait_for(lambda: b.fldigi.state == "TX", 2.0) and b.fldigi.starts[-1][1] == "lease lapsed",
              "it goes ahead by itself %.1f s later" % (time.time() - t0), fails)
        b.fldigi.end()
        how = b.subspace.send(frames=1, frame_s=0.3)
        check(how in ("lease lapsed", "not asked"), "Subspace's message goes out %.1f s after the kill (%s)" % (time.time() - t0, how), fails)
    finally:
        b.stop()
    b = Bench(subspace_ask=True, fldigi_ask=True)
    try:
        b.start()
        check(_asking(b), "second arbiter: both programs ask", fails)
        b.stop_arbiter()
        check(wait_for(lambda: b.subspace.ask_until == 0.0, 2.0), "clean close: Subspace told to stop asking", fails)
        check(b.subspace.send(frames=1, frame_s=0.3) == "not asked", "its message goes out at once", fails)
    finally:
        b.stop()
    return fails


def scenario_24():
    """The layer: a VarAC start command while the radio is free makes the others wait at once;
    of two programs that wait, the one that asked first is granted first."""
    if not BIN:
        return _study_skip()
    b = Bench(subspace_ask=True, fldigi_ask=True, layer=True, extra_ini="vara_burst_hold_s = 1.0\n")
    fails = []
    try:
        b.start()
        check(b.connect_varac() and _asking(b), "VarAC connected through the layer; both programs ask", fails)
        b.varac.say("CQFRAME WM8Q 500")
        check(wait_for(lambda: b.vara.got("CQFRAME WM8Q 500"), 1.0) and not b.varac.got("BUSY ON"),
              "radio free: VarAC's CQ goes to VARA at once", fails)
        t = _send(b, frames=1, frame_s=0.6)
        time.sleep(0.15)
        check(b.fldigi.start() == "waiting", "fldigi's start waits", fails)
        check(wait_for(lambda: b.subspace.wait_reasons == ["VARA TX"] and b.logged("fldigi: waits (VARA TX)"), 1.0),
              "both wait for VARA's transmission, before any PTT ON", fails)
        b.vara.say("PTT ON")
        time.sleep(0.5)
        b.vara.say("PTT OFF")
        check(not b.subspace.sends and b.fldigi.state == "RX", "nothing granted during VARA's transmission", fails)
        check(wait_for(lambda: len(b.subspace.sends) == 1, 3.0), "Subspace (asked first) is granted first", fails)
        check(b.fldigi.state == "RX" and wait_for(lambda: b.logged("fldigi: waits (Subspace TX)"), 1.0), "fldigi now waits for Subspace", fails)
        t.join()
        check(wait_for(lambda: b.fldigi.state == "TX", 3.0), "then fldigi is granted", fails)
        check(len(b.varac.got("BUSY ON")) == 1, "VarAC told BUSY ON once for both transmissions", fails)
        b.fldigi.end()
        check(wait_for(lambda: len(b.varac.got("BUSY OFF")) == 1, 2.0), "and BUSY OFF after them", fails)
    finally:
        b.stop()
    return fails


def scenario_25():
    """The layer while VarAC is held: a CQ's two identical lines kept once, VARA's own BUSY
    lines kept back, a kept CONNECT taken back by VarAC, VarAC closing during the hold."""
    if not BIN:
        return _study_skip()
    b = Bench(fldigi_ask=True, layer=True, extra_ini="vara_burst_hold_s = 1.0\n")
    fails = []
    try:
        b.start()
        check(b.connect_varac() and _asking(b, subspace=False), "VarAC connected through the layer; fldigi asks", fails)
        b.fldigi.start()
        check(wait_for(lambda: b.fldigi.state == "TX", 2.0), "radio free: fldigi's start is granted", fails)
        check(wait_for(lambda: b.varac.got("BUSY ON") and b.vara.got("LISTEN OFF"), 1.0), "VarAC told BUSY ON, VARA LISTEN OFF", fails)
        b.vara.say("BUSY ON")
        b.vara.say("BUSY OFF")
        b.varac.say("CQFRAME WM8Q 500")
        time.sleep(0.3)
        b.varac.say("CQFRAME WM8Q 500")
        time.sleep(0.3)
        check(len(b.varac.got("BUSY ON")) == 1 and not b.varac.got("BUSY OFF"), "VARA's own BUSY lines do not reach VarAC during the hold", fails)
        check(not b.vara.got("CQFRAME WM8Q 500") and b.logged("the same line is already kept"), "the CQ's second line is not kept twice", fails)
        b.fldigi.end()
        check(wait_for(lambda: b.varac.got("BUSY OFF"), 2.0), "fldigi over: VarAC told BUSY OFF", fails)
        time.sleep(0.3)
        check(len(b.vara.got("CQFRAME WM8Q 500")) == 1, "VARA gets the CQ line once", fails)
        check(len(b.varac.got("OK")) == 3, "VarAC sees VARA's OK for its own three lines only (%d)" % len(b.varac.got("OK")), fails)
        time.sleep(1.2)                       # the passed CQ counts as VARA transmitting for vara_burst_hold_s
        b.fldigi.start()
        check(wait_for(lambda: b.fldigi.state == "TX" and len(b.varac.got("BUSY ON")) == 2, 2.0), "second fldigi transmission: VarAC held again", fails)
        b.varac.say("CONNECT WM8Q K9AVT")
        time.sleep(0.2)
        b.varac.say("ABORT")
        check(wait_for(lambda: b.varac.got("DISCONNECTED"), 1.0) and b.vara.got("ABORT") and not b.vara.got("CONNECT WM8Q K9AVT"),
              "VarAC takes its kept CONNECT back: VARA never gets it, VarAC is told DISCONNECTED", fails)
        b.varac.close()
        check(wait_for(lambda: b.logged("VarAC: hold over (fldigi TX): VarAC closed its connection"), 2.0), "VarAC closes during the hold: the hold ends", fails)
        b.fldigi.end()
    finally:
        b.stop()
    return fails


def _spin(s):
    """A wait exact to well under a millisecond (sleep() is not)."""
    t = time.perf_counter() + s
    while time.perf_counter() < t:
        pass


def scenario_26():
    """The layer: a VarAC start command and another program's ask in the same instant, each
    kind of start command, the ask from 3 ms before the command to 3 ms after it (fldigi:
    anywhere in the arbiter's poll period). In every round one of the two gets the radio:
    the command goes to VARA and the program waits, or the program is granted and the
    command is kept until the release. Never both, never neither."""
    if not BIN:
        return _study_skip()
    b = Bench(subspace_ask=True, fldigi_ask=True, layer=True, extra_ini="vara_burst_hold_s = 1.0\n")
    fails = []
    rounds = []          # (program, kind, the ask that many ms after the command, who got the radio)
    wrong = []           # rounds where both or neither got it
    not_kept = []        # the program won and the command was not kept, or not passed on later
    not_waited = []      # VarAC won and the program was granted inside VARA's transmission
    latest = [0.0]       # the longest a kept command waited
    soonest = [99.0]     # the shortest time from a passed command to the loser's grant
    switch = sys.getswitchinterval()
    sys.setswitchinterval(0.0005)

    def command(n, kind):
        """The start command of round n: (name, send it, when VARA got it or None)."""
        if kind == "KISS":
            frame = b"\xc0\x00" + ("round %02d" % n).encode() + b"\xc0"
            seen = []

            def at_vara():
                if not seen and frame in b.vara.kiss.raw:
                    seen.append(time.time())
                return seen[0] if seen else None
            return "KISS frame", lambda: b.varac.kiss.sock.sendall(frame), at_vara
        line = "CQFRAME WM8Q-%d 500" % n if kind == "CQFRAME" else "CONNECT WM8Q K9R%02d" % n
        return line, lambda: b.varac.say(line), lambda: next((t for x, t in b.vara.cmd.lines if x == line), None)

    def kept_lines():
        return len([l for l in b.log_lines() if "kept back until the release" in l])

    def settle(holds, program_won):
        wait_for(lambda: len(b.varac.got("BUSY ON")) == holds and len(b.varac.got("BUSY OFF")) == holds, 5.0)
        time.sleep(1.2 if program_won else 0.2)     # a command passed on late counts as VARA transmitting

    def judge(who, kind, d_ms, a, g, kept0, at_vara, sent_t, grant_t, end_program):
        """a: the command is at VARA; g: the program is granted; both read 0.2 s after the first of them."""
        tag = "%s, %s, ask %+.1f ms" % (who, kind, d_ms)
        if a == g:
            wrong.append(tag + (": BOTH" if a else ": NEITHER"))
            end_program()
            rounds.append((who, kind, d_ms, "both" if a else "neither"))
            return False
        if g:
            ok = kept_lines() == kept0 + 1
            end_program()
            ok = wait_for(lambda: at_vara() is not None, 4.0, step=0.005) and ok
            if ok:
                latest[0] = max(latest[0], at_vara() - sent_t)
            else:
                not_kept.append(tag)
        else:
            ok = wait_for(lambda: grant_t() is not None, 4.0, step=0.005)
            gap = grant_t() - at_vara() if ok else 0.0
            if ok and gap >= 0.9:
                soonest[0] = min(soonest[0], gap)
            else:
                not_waited.append(tag + (": granted %.2f s after the command" % gap if ok else ": never granted"))
            end_program()
        rounds.append((who, kind, d_ms, who if g else "VarAC"))
        return g

    try:
        b.start()
        check(b.connect_varac() and _asking(b), "VarAC connected through the layer; both programs ask", fails)
        n = 0
        for kind in ("CQFRAME", "CONNECT", "KISS"):
            for d_ms in (-3.0, -1.0, -0.3, -0.1, -0.05, -0.02, 0.0, 0.0, 0.1, 0.3, 1.0, 3.0):
                n += 1
                name, start, at_vara = command(n, kind)
                r0, kept0, sent = len(b.subspace.requests), kept_lines(), []

                def both(ask, start=start, d_ms=d_ms, sent=sent):
                    if d_ms >= 0:
                        sent.append(time.time())
                        start()
                        _spin(d_ms / 1000.0)
                        ask()
                    else:
                        ask()
                        _spin(-d_ms / 1000.0)
                        sent.append(time.time())
                        start()

                def grant_t(r0=r0):
                    return next((t for q, p, t in b.subspace.requests[r0:] if q == "TX.GRANT" and p.get("GRANT")), None)
                t = _send(b, frames=1, frame_s=0.5, at_ask=both)
                wait_for(lambda: at_vara() is not None or grant_t() is not None, 1.0, step=0.002)
                time.sleep(0.2)
                won = judge("Subspace", kind, d_ms, at_vara() is not None, grant_t() is not None, kept0,
                            at_vara, sent[0] if sent else time.time(), grant_t, lambda t=t: t.join(8.0))
                settle(n, won)
        for d_ms in (0, 5, 10, 15, 20, 25, 30, 35, 40, 45, 50, 55):
            n += 1
            name, start, at_vara = command(n, "CQFRAME")
            s0, kept0 = len(b.fldigi.starts), kept_lines()
            b.fldigi.start()                  # waits; the arbiter learns of it at its next poll
            _spin(d_ms / 1000.0)
            sent_t = time.time()
            start()

            def grant_t(s0=s0):
                return next((t for _, how, t in b.fldigi.starts[s0:] if how == "granted"), None)

            def end_fldigi():
                time.sleep(0.4)
                b.fldigi.end()
            wait_for(lambda: at_vara() is not None or grant_t() is not None, 1.0, step=0.002)
            time.sleep(0.2)
            won = judge("fldigi", "CQFRAME", -d_ms, at_vara() is not None, grant_t() is not None, kept0,
                        at_vara, sent_t, grant_t, end_fldigi)
            settle(n, won)

        for who in ("Subspace", "fldigi"):
            mine = [r for r in rounds if r[0] == who]
            print("        %s against VarAC, %d rounds (ask that many ms after the command: who got the radio)" % (who, len(mine)))
            for kind in ("CQFRAME", "CONNECT", "KISS"):
                row = ["%+g: %s" % (d, w) for _, k, d, w in mine if k == kind]
                if row:
                    print("          %-8s %s" % (kind, "   ".join(row)))
        check(not wrong, "%d rounds: one of the two got the radio in each, never both, never neither%s"
              % (len(rounds), "" if not wrong else " - " + "; ".join(wrong)), fails)
        winners = set(r[3] for r in rounds if r[0] == "Subspace")
        check(winners == {"Subspace", "VarAC"}, "both orders happened against Subspace (%s)" % ", ".join(sorted(winners)), fails)
        check(not not_kept, "a command that lost was kept and passed on to VARA after the program's transmission "
              "(at most %.1f s after VarAC sent it)%s" % (latest[0], "" if not not_kept else " - " + "; ".join(not_kept)), fails)
        check(not not_waited, "a program that lost was granted after VARA's transmission (soonest %.2f s after the command)%s"
              % (soonest[0], "" if not not_waited else " - " + "; ".join(not_waited)), fails)
        check(len(b.varac.got("BUSY ON")) == n and len(b.varac.got("BUSY OFF")) == n,
              "VarAC told BUSY ON and BUSY OFF once per round (%d / %d in %d rounds)"
              % (len(b.varac.got("BUSY ON")), len(b.varac.got("BUSY OFF")), n), fails)
    finally:
        sys.setswitchinterval(switch)
        b.stop()
    return fails


def scenario_27():
    """A granted message halted by the operator before its key-up: Subspace sends its end
    report and, 0.2 s later, a PTT off although nothing was keyed. That PTT off is not the
    end of a frame: VarAC is released once and not held again (station 2026-11-20 19:14:56:
    released, held again 0.2 s later for 2.6 s)."""
    if not BIN:
        return _study_skip()
    b = Bench(subspace_ask=True, fldigi_ask=True, layer=True, extra_ini="vara_burst_hold_s = 1.0\n")
    fails = []
    try:
        b.start()
        check(b.connect_varac() and _asking(b), "VarAC connected through the layer; both programs ask", fails)
        t = _send(b, halt_after_grant_s=0.4)
        check(wait_for(lambda: b.varac.got("BUSY ON"), 2.0), "the message is granted, VarAC told BUSY ON", fails)
        t.join()
        check(wait_for(lambda: b.logged("Subspace: transmission over (message 1"), 1.0) and
              wait_for(lambda: len(b.varac.got("BUSY OFF")) == 1, 1.0),
              "halted before its key-up: VarAC told BUSY OFF with the end report", fails)
        time.sleep(0.3)                       # the PTT off has come by now
        t0 = time.time()
        check(b.fldigi.start() == "waiting" and wait_for(lambda: b.fldigi.state == "TX", 0.35),
              "fldigi's start right after it is granted in %.2f s, not after the frame gap (%.1f s)"
              % (time.time() - t0, FRAME_GAP_S), fails)
        b.fldigi.end()
        time.sleep(0.8)
        holds = len([l for l in b.log_lines() if "VarAC: disabled (Subspace TX)" in l])
        check(holds == 1, "the PTT off that follows no PTT on does not hold VarAC again for Subspace (%d holds)" % holds, fails)
    finally:
        b.stop()
    return fails


def scenario_28():
    """The reply wait. A message that expects a reply keeps the radio after its end: fldigi's
    start waits, VarAC stays told BUSY ON while VARA listens again. The wait ends when
    Subspace has its reply, when the time runs out, or when Subspace transmits again. A
    message that expects none frees the radio at its PTT off, without the frame gap."""
    if not BIN:
        return _study_skip()
    b = Bench(subspace_ask=True, fldigi_ask=True, layer=True, extra_ini="vara_burst_hold_s = 1.0\n")
    fails = []
    try:
        b.start()
        check(b.connect_varac() and _asking(b), "VarAC connected through the layer; both programs ask", fails)
        listens = len(b.vara.got("LISTEN ON"))
        _send(b, reply_ms=4000).join()
        check(wait_for(lambda: b.logged("Subspace: keeps the radio for the reply it expects (message 1, up to 4.0 s)"), 1.0),
              "the end report names 4 s for the reply: Subspace keeps the radio", fails)
        check(wait_for(lambda: len(b.vara.got("LISTEN ON")) == listens + 1, 1.0) and not b.varac.got("BUSY OFF"),
              "VarAC stays told BUSY ON; VARA is told to listen again", fails)
        check(b.fldigi.start() == "waiting" and wait_for(lambda: b.logged("fldigi: waits (Subspace reply)"), 1.0),
              "fldigi's start waits, told 'Subspace reply'", fails)
        time.sleep(0.5)
        check(b.fldigi.state == "RX", "fldigi does not start during the reply wait", fails)
        b.subspace.reply(3000)
        check(wait_for(lambda: b.logged("Subspace: a reply is coming in"), 1.0), "a reply's frame comes in: the wait goes on", fails)
        t0 = time.time()
        b.subspace.reply(0)
        check(wait_for(lambda: b.fldigi.state == "TX", 0.35) and b.logged("Subspace: reply wait over (Subspace has its reply)"),
              "the reply is complete: fldigi is granted %.2f s later" % (time.time() - t0), fails)
        b.fldigi.end()
        b.flrig.key(0, "fldigi")
        check(wait_for(lambda: len(b.varac.got("BUSY OFF")) == 1, 2.0), "all over: VarAC told BUSY OFF", fails)

        _send(b, reply_ms=1200).join()
        t0 = time.time()
        check(wait_for(lambda: b.logged("Subspace: reply wait over (no reply came in the time allowed)"), 2.5),
              "no reply: the wait ends by itself after %.2f s (1.2 s named)" % (time.time() - t0), fails)
        check(time.time() - t0 >= 1.0, "not before the time named", fails)

        _send(b, reply_ms=5000).join()
        check(wait_for(lambda: b.logged("(message 3, up to 5.0 s)"), 1.0), "a third message keeps the radio for its reply", fails)
        t = _send(b, reply_ms=0)
        check(wait_for(lambda: b.logged("Subspace: reply wait over (Subspace transmits again)"), 1.0) and
              wait_for(lambda: len(b.subspace.sends) == 4 and b.subspace.sends[-1][1] == "granted", 1.0),
              "Subspace sends again during its own reply wait: granted at once", fails)
        t.join()
        time.sleep(0.3)
        t0 = time.time()
        check(b.fldigi.start() == "waiting" and wait_for(lambda: b.fldigi.state == "TX", 0.35),
              "a message that expects no reply: fldigi granted in %.2f s, not after the frame gap (%.1f s)"
              % (time.time() - t0, FRAME_GAP_S), fails)
        b.fldigi.end()
    finally:
        b.stop()
    return fails


def scenario_29():
    """Every transmit attempt has its end in the log: an fldigi start given up while it waits,
    and each of VarAC's start commands by its number (a CQ, a CONNECT that gets its link, one
    that gets none, one that VARA never keys for)."""
    if not BIN:
        return _study_skip()
    b = Bench(subspace_ask=True, fldigi_ask=True, layer=True, extra_ini="vara_burst_hold_s = 1.0\n")
    fails = []
    try:
        b.start()
        check(b.connect_varac() and _asking(b), "VarAC connected through the layer; both programs ask", fails)
        t = _send(b, frames=1, frame_s=1.0)
        check(wait_for(lambda: b.flrig.ptt == 1, 2.0), "Subspace's message keys", fails)
        check(b.fldigi.start() == "waiting" and wait_for(lambda: b.logged("fldigi: asks to transmit (start 1)"), 1.0),
              "fldigi's start waits", fails)
        b.fldigi.waiting = 0
        check(wait_for(lambda: b.logged("fldigi: no longer asks (start 1)"), 1.0), "given up while it waits: the log says so", fails)
        t.join()
        check(wait_for(lambda: b.varac.got("BUSY OFF"), FRAME_GAP_S + 2.0), "Subspace's message over: VarAC told BUSY OFF", fails)
        check(not b.logged("fldigi: granted") and b.fldigi.state == "RX", "the start given up is never granted", fails)

        b.varac.say("CQFRAME WM8Q 500")
        check(wait_for(lambda: b.logged("VarAC: start 1 given to VARA at once: CQFRAME WM8Q 500"), 1.0), "a CQ is start 1", fails)
        for line in ("PENDING", "PTT ON"):
            b.vara.say(line)
        time.sleep(0.3)
        check(not b.logged("VarAC: transmission over"), "no end while VARA is keyed", fails)
        b.vara.say("PTT OFF")
        check(wait_for(lambda: b.logged("VarAC: transmission over (start 1, "), 1.0), "its end at VARA's PTT OFF", fails)
        t0 = time.time()
        t = _send(b, frames=1, frame_s=0.3)
        check(wait_for(lambda: len(b.subspace.sends) == 2 and b.subspace.sends[-1][1] == "granted", 0.5),
              "no burst hold after a one-shot transmission: Subspace granted %.2f s later (hold 1.0 s)" % (time.time() - t0), fails)
        t.join()
        check(wait_for(lambda: len(b.varac.got("BUSY OFF")) == 2, FRAME_GAP_S + 2.0), "Subspace's message over: VarAC released", fails)

        b.varac.say("CONNECT WM8Q K9AVT")
        check(wait_for(lambda: b.logged("VarAC: start 2 given to VARA at once: CONNECT WM8Q K9AVT"), 1.0), "a CONNECT is start 2", fails)
        for line in ("PTT ON", "PTT OFF", "PTT ON", "PTT OFF"):
            b.vara.say(line)
            time.sleep(0.1)
        check(not b.logged("VarAC: start 2 over") and not b.logged("(start 2, "), "a CONNECT does not end at a PTT OFF", fails)
        b.vara.say("CONNECTED WM8Q K9AVT 500")
        check(wait_for(lambda: b.logged("VarAC: start 2 over: link open (") and b.logged("VARA keyed 2 times)"), 1.0),
              "it ends with the link, VARA's key-ups counted", fails)
        b.vara.say("DISCONNECTED")
        check(wait_for(lambda: b.logged("VarAC: link closed"), 1.0), "the link closes", fails)

        b.varac.say("CONNECT WM8Q K9XYZ")
        b.vara.say("PTT ON")
        time.sleep(0.1)
        b.vara.say("PTT OFF")
        b.vara.say("DISCONNECTED")
        check(wait_for(lambda: b.logged("VarAC: start 3 over: no link (") and b.logged("VARA keyed 1 time)"), 1.0),
              "a CONNECT nobody answers ends with 'no link'", fails)

        b.varac.say("CQFRAME WM8Q 500")
        time.sleep(0.2)
        b.varac.say("CQFRAME WM8Q-1 500")
        check(wait_for(lambda: b.logged("VarAC: start 4: no end seen (start 5 followed it; VARA keyed 0 times)"), 1.0),
              "a start VARA never keys for is said so when the next one comes", fails)
        b.varac.close()
        check(wait_for(lambda: b.logged("VarAC: start 5: no end seen (the command connection closed; VARA keyed 0 times)"), 2.0),
              "and when VarAC closes its connection", fails)
        # the one line per attempt that "Show transmissions" shows
        tx = [l.split("  TX: ", 1)[1] for l in b.log_lines() if "  TX: " in l]
        want = ["Subspace message 1: SENT, completed (cleared to send ", "Subspace message 2: SENT, completed (cleared to send ",
                "fldigi start 1: NOT SENT (given up ", "VarAC start 1: SENT, completed (key-up to end ",
                "VarAC start 2: SENT, a connect: link open (VARA keyed 2 times)",
                "VarAC start 3: SENT, a connect: no link (VARA keyed 1 time)",
                "VarAC start 4: NOT SENT (VARA never keyed; start 5 followed it)",
                "VarAC start 5: NOT SENT (VARA never keyed; the command connection closed)"]
        for w in want:
            check(len([l for l in tx if l.startswith(w)]) == 1, "one line: " + w, fails)
        check(len(tx) == len(want), "and no other line (%d lines for %d attempts)" % (len(tx), len(want)), fails)
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
    9: ("fldigi release on the arbiter's own link; refused rx_tx sent again", scenario_9),
    10: ("Arbiter killed while holding fldigi; the next one restores it", scenario_10),
    11: ("A failed fldigi state query during a hold; the release still sends rx_tx", scenario_11),
    12: ("A waiting Subspace frame holds the others before the key-up", scenario_12),
    13: ("A held VarAC while VARA keeps asking: no VARA TX, fresh hold, un-keys blocked", scenario_13),
    14: ("A flrig without the un-key block", scenario_14),
    15: ("fldigi answered, then stopped: a skipped hold is logged", scenario_15),
    16: ("Dial change credited to Subspace with RIG.FREQ before or after", scenario_16),
    17: ("[study] Both ask: each waits for the other's transmission and for the radio to un-key", scenario_17),
    18: ("[study] Layer: Subspace waits for a VARA link; VarAC held during Subspace's message", scenario_18),
    19: ("[study] Programs that cannot ask are held as before", scenario_19),
    20: ("[study] Subspace asks, fldigi cannot", scenario_20),
    21: ("[study] Subspace stops in the middle of its granted message", scenario_21),
    22: ("[study] A waiting message given up; a grant no key-up follows", scenario_22),
    23: ("[study] The arbiter stops: asking ends, the programs transmit as without it", scenario_23),
    24: ("[study] Layer: a VarAC start makes the others wait at once; first asked, first granted", scenario_24),
    25: ("[study] Layer: what is kept and kept back while VarAC is held", scenario_25),
    26: ("[study] Layer: a VarAC start command and an ask in the same instant", scenario_26),
    27: ("[study] A granted message halted before its key-up: its PTT off holds nobody", scenario_27),
    28: ("[study] Reply wait: Subspace keeps the radio for the reply its message expects", scenario_28),
    29: ("[study] Every attempt has its end: an fldigi start given up, VarAC's numbered starts", scenario_29),
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
