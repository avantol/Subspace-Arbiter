#!/usr/bin/env python3
"""Arbiter for a shared radio.

One radio, several programs (Subspace, fldigi, VarAC) through flrig.
Whatever is already transmitting finishes. The arbiter holds only what
has not started, by throwing each program's own switch, and it never
refuses, swallows or delays a PTT. It is a client of flrig, never a link
in the CAT chain: stop it, kill it or never start it, and every program
transmits as it does today.

Switches used:
  Subspace  TX.SET_ENABLED on its TCP API (a 300 s lease the arbiter renews)
  fldigi    main.rx_only / main.rx_tx on its XML-RPC port (stock since 2014)
  VarAC     the lever proven on this station by varac_probe.py (else observe only)

Python 3.8+, standard library only. Tk window when tkinter is present,
else --console. Settings in arbiter.ini beside this file (written with
defaults on first run); every number is named there with its basis.
"""

import argparse
import atexit
import calendar
import configparser
import glob
import json
import logging
import os
import random
import re
import signal
import socket
import sqlite3
import sys
import threading
import time
import xmlrpc.client
from collections import deque

VERSION = "0.1"
HERE = os.path.dirname(os.path.abspath(__file__))
IS_WIN = sys.platform.startswith("win")
HOME = os.path.expanduser("~")

# Reasons shown inside the held program (Subspace status bar: "TX disabled by <reason>")
R_VARA = "VARA link"
R_VARA_TX = "VARA TX"
R_FLDIGI = "fldigi TX"
R_SUBSPACE = "Subspace TX"
R_UNKNOWN = "radio keyed"
REASON_ORDER = (R_VARA, R_VARA_TX, R_FLDIGI, R_SUBSPACE, R_UNKNOWN)

# Every number, with its basis. These are the defaults written to arbiter.ini.
DEFAULT_INI = """\
# Arbiter settings. Every number has its basis stated: "measured" on a real
# station, or "chosen". Change a value, restart the arbiter.

[ports]
flrig_host = 127.0.0.1
flrig_port = 12345
subspace_host = 127.0.0.1
subspace_port = 2442
fldigi_host = 127.0.0.1
fldigi_port = 7362

[holds]
# on / off per program. Off = that program is observed but never held.
subspace = on
fldigi = on
varac = on

[timing]
# poll_ms: how often flrig and fldigi are asked. chosen. flrig answers
# rig.get_ptt from its cache in about 0 ms; one poll is the reaction time.
poll_ms = 200
# state_poll_s: how often Subspace is asked STATION.GET_BUSY (ARQ state). chosen.
state_poll_s = 2
# renew_s: how often a hold is renewed. chosen; Subspace's own lease is 300 s.
renew_s = 60
# subspace_frame_gap_s: a Subspace message is still in progress this long
# after its PTT drops. measured: PTT drops 1.2 to 1.6 s between frames
# (N1ROG, WM8Q); must stay under the 3.75 s Subspace slot.
subspace_frame_gap_s = 2.5
# varac_dead_s: VarAC refreshes its own instance row's keepalive in VarAC.db
# about every 10 to 20 s (measured 2026-10-29); when that stops this long,
# VarAC is gone and an open link with it. chosen: 90 s.
varac_dead_s = 90
# vara_stale_s: far fallback when VarAC.db is not readable: a link whose
# VARA log has been silent this long is treated as gone. chosen: 600 s,
# longer than any idle link (VarAC's own idle disconnect is 5 min).
vara_stale_s = 600
# vara_ptt_match_s: VarAC's log stamps are whole seconds. measured.
vara_ptt_match_s = 2
# vara_burst_hold_s: after a VARA PTT OFF (or a PENDING connect request) the
# hold stays this long, so the gaps between VARA's bursts do not release and
# re-hold the other programs every few seconds. measured: VARA pauses up to
# 11.5 s between frames inside a link (K7RIE), 3 s between connect requests
# (WM8Q 2026-10-29).
vara_burst_hold_s = 12
# unknown_guard_s: keying no program claims counts as busy this long after
# the unkey. measured: VARA bursts pause up to 11.5 s (K7RIE).
unknown_guard_s = 15
# unkey_claim_s: a program that reported itself transmitting this recently
# still owns the keyed radio. measured 2026-10-29 (WM8Q): fldigi reports RX
# about 1 s before its PTT off reaches flrig, and flrig's cache is read one
# poll later; without this the arbiter saw "keyed, program unknown" at the
# end of every fldigi over and held everyone for unknown_guard_s.
unkey_claim_s = 3
# xmlrpc_timeout_s: a stalled flrig or fldigi must not stall the arbiter. chosen.
xmlrpc_timeout_s = 1
# reconnect_s: retry interval for a program that is not answering. chosen.
reconnect_s = 5

[varac]
# log_path: VarAC's VARA command log (VarAC: LogVARACommands=ON). Blank =
# found the way CAT Watch finds it (LogVARACommandsPath= in VarAC*.ini, or
# VarAC*/VarAC_vara_commands*.log under C:\\ or each Wine prefix).
log_path =
# lever: how VarAC is held. none = observe only, nothing is written.
# cluster_lock = VarAC's own cluster PTT lock (VarAC: Cluster enabled, PTT
# Lock ON): the arbiter is a second cluster instance named "Arbiter" in
# VarAC.db, marked busy while the hold lasts; VarAC then shows "PTT is
# disabled" and will not key. Proven on WM8Q's VarAC 16.1, 2026-10-29.
lever = cluster_lock
# db_path: VarAC.db. Blank = the VarAC.db beside the command log.
db_path =
# varac_renew_s: how often the Arbiter row's keepalive is refreshed while
# holding. measured 2026-10-29: VarAC 16.1 drops a lock whose keepalive is
# about 30 s old (PTT re-enabled 32 s after the last refresh), so a dead
# arbiter frees VarAC in half a minute; 10 s keeps a live hold well inside.
varac_renew_s = 10

[log]
file = arbiter.log
"""


# ----------------------------------------------------------------------------
# settings and log
# ----------------------------------------------------------------------------

class Settings:
    def __init__(self, path):
        self.path = path
        if not os.path.exists(path):
            with open(path, "w", encoding="utf-8") as f:
                f.write(DEFAULT_INI)
        self.cp = configparser.ConfigParser(interpolation=None)
        self.cp.read_string(DEFAULT_INI)      # defaults first
        self.cp.read(path, encoding="utf-8")  # then the station's file

    def get(self, sec, key):
        return self.cp.get(sec, key).strip()

    def getint(self, sec, key):
        return self.cp.getint(sec, key)

    def getfloat(self, sec, key):
        return self.cp.getfloat(sec, key)

    def getbool(self, sec, key):
        return self.cp.getboolean(sec, key)


def tstamp(t=None):
    """Local time with UTC offset, the same shape CAT Watch writes."""
    t = time.time() if t is None else t
    lt = time.localtime(t)
    return time.strftime("%Y-%m-%d %H:%M:%S", lt) + ".%03d %s" % (int(t * 1000) % 1000, time.strftime("%z", lt))


class Log:
    """One line per state change: to the file, to the window, to the console."""

    def __init__(self, path, echo):
        self.lines = deque(maxlen=500)
        self.echo = echo
        self.lock = threading.Lock()
        self.logger = logging.getLogger("arbiter")
        self.logger.setLevel(logging.INFO)
        self.logger.propagate = False
        h = logging.FileHandler(path, encoding="utf-8")
        h.setFormatter(logging.Formatter("%(message)s"))
        self.logger.addHandler(h)

    def __call__(self, msg):
        line = "%s  %s" % (tstamp(), msg)
        with self.lock:
            self.lines.append(line)
        self.logger.info(line)
        if self.echo:
            print(line, flush=True)


# ----------------------------------------------------------------------------
# flrig and fldigi: XML-RPC clients with a timeout
# ----------------------------------------------------------------------------

class _TimeoutTransport(xmlrpc.client.Transport):
    def __init__(self, timeout):
        super().__init__()
        self._timeout = timeout

    def make_connection(self, host):
        conn = super().make_connection(host)
        conn.timeout = self._timeout
        return conn


class XmlRpcLink:
    """A client that forgets its connection on any error and says so once."""

    def __init__(self, name, host, port, timeout, log):
        self.name, self.host, self.port, self.timeout, self.log = name, host, port, timeout, log
        self.proxy = None
        self.up = False
        self.last_try = 0.0

    def _get(self, reconnect_s):
        if self.proxy is None:
            if time.time() - self.last_try < reconnect_s:
                return None
            self.last_try = time.time()
            self.proxy = xmlrpc.client.ServerProxy(
                "http://%s:%d" % (self.host, self.port),
                transport=_TimeoutTransport(self.timeout), allow_none=True)
        return self.proxy

    def call(self, method, *args, reconnect_s=5.0):
        """Returns the result, or None when the program did not answer."""
        p = self._get(reconnect_s)
        if p is None:
            return None
        try:
            r = getattr(p, method)(*args)
            if not self.up:
                self.up = True
                self.log("%s: connected on %s:%d" % (self.name, self.host, self.port))
            return r
        except (OSError, xmlrpc.client.ProtocolError, xmlrpc.client.Fault, ValueError) as e:
            if self.up or self.proxy is not None and self.last_try == 0.0:
                pass
            if self.up:
                self.log("%s: not answering (%s)" % (self.name, str(e).strip()[:80]))
            self.up = False
            self.proxy = None
            return None


class FlrigWatch:
    """Reads PTT and the dial from flrig's cache. Never sends set_ptt."""

    def __init__(self, s, log):
        self.link = XmlRpcLink("flrig", s.get("ports", "flrig_host"), s.getint("ports", "flrig_port"),
                               s.getfloat("timing", "xmlrpc_timeout_s"), log)
        self.reconnect_s = s.getfloat("timing", "reconnect_s")
        self.ptt = None       # True / False / None (unknown)
        self.vfo = None
        self.ptt_changed_t = 0.0
        self.vfo_changed_t = 0.0
        self.vfo_polled_t = 0.0
        self.VFO_POLL_S = 1.0  # chosen: the dial is only logged; PTT is what every tick needs

    def poll(self):
        r = self.link.call("rig.get_ptt", reconnect_s=self.reconnect_s)
        now = time.time()
        if r is None:
            self.ptt = None
            return
        ptt = bool(int(r))
        if ptt != self.ptt:
            self.ptt = ptt
            self.ptt_changed_t = now
        if now - self.vfo_polled_t < self.VFO_POLL_S:
            return
        self.vfo_polled_t = now
        v = self.link.call("rig.get_vfo", reconnect_s=self.reconnect_s)
        if v is not None:
            try:
                v = int(float(v))
            except (TypeError, ValueError):
                v = None
            if v is not None and v != self.vfo:
                if self.vfo is not None:
                    self.vfo_changed_t = now   # a change, not the first reading
                self.vfo = v


class FldigiLink:
    """main.get_trx_state ("TX" covers TX and TUNE, else "RX"), main.rx_only, main.rx_tx."""

    def __init__(self, s, log):
        self.log = log
        self.link = XmlRpcLink("fldigi", s.get("ports", "fldigi_host"), s.getint("ports", "fldigi_port"),
                               s.getfloat("timing", "xmlrpc_timeout_s"), log)
        self.reconnect_s = s.getfloat("timing", "reconnect_s")
        self.state = None     # "TX" / "RX" / None
        self.held = False     # we sent rx_only and have not sent rx_tx

    def poll(self):
        r = self.link.call("main.get_trx_state", reconnect_s=self.reconnect_s)
        self.state = None if r is None else str(r).upper()
        if r is None and self.held:
            self.held = False  # fldigi went away: nothing of ours survives its restart

    @property
    def transmitting(self):
        return self.state == "TX"

    def hold(self):
        """Only while fldigi is in RX: rx_only aborts an over in progress."""
        if self.state != "RX" or self.held:
            return False
        if self.link.call("main.rx_only", reconnect_s=self.reconnect_s) is None and not self.link.up:
            return False
        self.held = True
        return True

    def release(self):
        if not self.held:
            return False
        self.held = False
        self.link.call("main.rx_tx", reconnect_s=0.0)
        return True


# ----------------------------------------------------------------------------
# Subspace: persistent JSON-lines client
# ----------------------------------------------------------------------------

class SubspaceLink:
    """Events RIG.PTT / TX.COMPLETE / TX.ENABLED / STATION.CLOSING, queries with
    _ID correlation, EVENTS.KEEPALIVE heartbeat, reconnect on loss."""

    KEEPALIVE_S = 10.0   # chosen

    def __init__(self, s, log):
        self.log = log
        self.host, self.port = s.get("ports", "subspace_host"), s.getint("ports", "subspace_port")
        self.reconnect_s = s.getfloat("timing", "reconnect_s")
        self.sock = None
        self.up = False
        self.refused = False
        self.status = "not connected"
        self.lock = threading.Lock()
        self.pending = {}            # _ID -> [event, reply]
        self.events = deque()        # unsolicited messages, consumed by the arbiter tick
        self.next_id = random.randint(10_000_000, 90_000_000)  # never SpotterX's small ids
        self.last_try = 0.0
        self.last_rx = 0.0
        self.last_keepalive = 0.0
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self._run, name="subspace", daemon=True)
        self.thread.start()

    # -- wire ---------------------------------------------------------------
    def _connect(self):
        if time.time() - self.last_try < self.reconnect_s:
            return False
        self.last_try = time.time()
        try:
            self.sock = socket.create_connection((self.host, self.port), timeout=2.0)
            self.sock.settimeout(0.5)
        except OSError as e:
            self.sock = None
            self.status = "not connected (%s)" % str(e).strip()[:60]
            return False
        self.up = True
        self.refused = False
        self.last_rx = time.time()
        self.status = "connected"
        self.log("Subspace: connected on %s:%d" % (self.host, self.port))
        return True

    def _drop(self, why):
        if self.sock is not None:
            try:
                self.sock.close()
            except OSError:
                pass
        self.sock = None
        if self.up and not self.refused:
            self.log("Subspace: connection lost (%s)" % why)
        self.up = False
        if not self.refused:   # a refusal's plain-words status stays until a connection succeeds
            self.status = "not connected (%s)" % why
        with self.lock:
            for ev, _ in self.pending.values():
                ev.set()
            self.pending.clear()

    def _run(self):
        buf = b""
        while not self.stop.is_set():
            if self.sock is None:
                if not self._connect():
                    time.sleep(0.2)
                    continue
                buf = b""
            try:
                chunk = self.sock.recv(65536)
            except socket.timeout:
                chunk = None
            except OSError as e:
                self._drop(str(e).strip()[:60])
                continue
            if chunk == b"":
                self._drop("closed by Subspace")
                continue
            if chunk:
                self.last_rx = time.time()
                buf += chunk
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    if line.strip():
                        self._line(line)
            now = time.time()
            if now - self.last_keepalive > self.KEEPALIVE_S:
                self.last_keepalive = now
                self.send("EVENTS.KEEPALIVE")
            if now - self.last_rx > 3 * self.KEEPALIVE_S:
                self._drop("no answer to keepalive")

    def _line(self, line):
        try:
            msg = json.loads(line.decode("utf-8", "replace"))
        except ValueError:
            return
        t = msg.get("type", "")
        params = msg.get("params", {}) or {}
        if t == "API.ERROR":
            v = str(msg.get("value", ""))
            if "Connections Full" in v:
                if not self.refused:
                    self.log("Subspace: refused, raise 'TCP max connections' in Subspace Settings")
                self.refused = True
                self.status = "refused: raise 'TCP max connections' in Subspace Settings"
            else:
                self.log("Subspace: API.ERROR %s" % v)
            return
        rid = params.get("_ID", -1)
        with self.lock:
            slot = self.pending.pop(rid, None) if isinstance(rid, int) and rid > 0 else None
        if slot is not None:
            slot[1] = msg
            slot[0].set()
            return
        if t == "EVENTS.PONG":
            return
        self.events.append(msg)

    def send(self, mtype, value="", **params):
        if self.sock is None:
            return None
        rid = self.next_id
        self.next_id += 1
        params["_ID"] = rid
        data = (json.dumps({"type": mtype, "value": value, "params": params}) + "\n").encode("utf-8")
        try:
            self.sock.sendall(data)
        except OSError as e:
            self._drop(str(e).strip()[:60])
            return None
        return rid

    def request(self, mtype, timeout=2.0, **params):
        """Sends and waits for the reply with the same _ID. None if no answer."""
        ev = threading.Event()
        slot = [ev, None]
        with self.lock:
            rid = self.next_id
            self.pending[rid] = slot
        if self.send(mtype, **params) is None:
            with self.lock:
                self.pending.pop(rid, None)
            return None
        ev.wait(timeout)
        with self.lock:
            self.pending.pop(rid, None)
        return slot[1]

    def close(self):
        self.stop.set()
        self._drop("arbiter closing")


# ----------------------------------------------------------------------------
# VarAC: its VARA command log, and the lever proven on this station
# ----------------------------------------------------------------------------

RE_VARA_LINE = re.compile(r"^(\d{1,2})/(\d{1,2})/(\d{4})\s+(\d\d):(\d\d):(\d\d)\s*-\s*(.*)")


def win_to_host_path(p):
    """C:\\VarAC\\x.log -> the same file on Windows, or inside a Wine prefix on Linux."""
    p = p.strip().strip('"')
    if not p:
        return []
    if IS_WIN:
        return [p]
    out = []
    m = re.match(r"^[A-Za-z]:[\\/](.*)$", p)
    rel = (m.group(1) if m else p).replace("\\", "/")
    for c in wine_roots():
        out.append(os.path.join(c, rel))
    return out


def wine_roots():
    return glob.glob(os.path.join(HOME, ".wine*", "drive_c")) + \
        glob.glob(os.path.join(HOME, ".local", "share", "wineprefixes", "*", "drive_c"))


def varac_log_files():
    """VarAC's VARA command log(s), found the way CAT Watch finds them."""
    files = []
    roots = [os.environ.get("SystemDrive", "C:") + "\\"] if IS_WIN else wine_roots()
    for r in roots:
        for ini in glob.glob(os.path.join(r, "VarAC*", "VarAC*.ini")):
            try:
                with open(ini, encoding="utf-8", errors="replace") as f:
                    for line in f:
                        if line.startswith("LogVARACommandsPath="):
                            files += win_to_host_path(line.split("=", 1)[1])
            except OSError:
                pass
        files += glob.glob(os.path.join(r, "VarAC*", "VarAC_vara_commands*.log"))
    return sorted({f for f in files if f and os.path.exists(f)}, key=os.path.getmtime)


class VaracWatch:
    """Follows VarAC's VARA command log. Lines look like
    "27/10/2026 12:47:24 - PTT ON". Only the kind of event is kept.
    Arrival time, not the one-second stamp, is what the arbiter uses."""

    LOOK_S = 30.0   # chosen: how often the log file is looked for again

    def __init__(self, s, log):
        self.log = log
        self.fixed_path = s.get("varac", "log_path")
        self.stale_s = s.getfloat("timing", "vara_stale_s")
        self.dead_s = s.getfloat("timing", "varac_dead_s")
        self.db = s.get("varac", "db_path")
        self.db_checked_t = 0.0
        self.varac_alive = None      # None = cannot tell (no database)
        self.varac_keepalive_t = 0.0
        self.path = None
        self.pos = None
        self.looked = 0.0
        self.link_open = False
        self.last_line_t = 0.0
        self.ptt = False
        self.ptt_t = 0.0          # last PTT ON / OFF / PENDING line, arrival time
        self.burst_t = 0.0        # last moment VARA was transmitting or about to
        self.status = "log not found"
        self.lock = threading.Lock()
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self._run, name="varac", daemon=True)
        self.thread.start()

    def _find(self):
        if self.fixed_path:
            return self.fixed_path if os.path.exists(self.fixed_path) else None
        files = varac_log_files()
        return files[-1] if files else None

    def _run(self):
        while not self.stop.is_set():
            try:
                self._step()
            except OSError:
                pass
            time.sleep(0.25)

    def _step(self):
        now = time.time()
        if now - self.looked > self.LOOK_S or self.path is None:
            self.looked = now
            p = self._find()
            if p != self.path:
                self.path, self.pos = p, None
                with self.lock:
                    self.link_open = False
                    self.ptt = False
                self.status = ("following %s" % p) if p else \
                    "log not found (VarAC: turn on 'Log VARA commands', restart VarAC)"
                self.log("VarAC: %s" % self.status)
        if not self.path:
            return
        size = os.path.getsize(self.path)
        if self.pos is None:
            # start from the tail: the last CONNECTED / DISCONNECTED decides the link state
            self.pos = max(0, size - 65536)
            with open(self.path, encoding="utf-8", errors="replace") as f:
                f.seek(self.pos)
                chunk = f.read()
                self.pos = f.tell()
            self._lines(chunk, live=False)
            self.last_line_t = os.path.getmtime(self.path)
            return
        if size < self.pos:
            self.pos = 0
            return
        if size == self.pos:
            return
        with open(self.path, encoding="utf-8", errors="replace") as f:
            f.seek(self.pos)
            chunk = f.read(1_000_000)
            self.pos = f.tell()
        self._lines(chunk, live=True)

    def _lines(self, chunk, live):
        now = time.time()
        for line in chunk.splitlines():
            m = RE_VARA_LINE.match(line.strip())
            if not m:
                continue
            msg = m.group(7).strip().upper()
            with self.lock:
                if live:
                    self.last_line_t = now
                if msg.startswith("DISCONNECTED"):
                    if self.link_open and live:
                        self.log("VarAC: link closed")
                    self.link_open = False
                    if live:
                        self.burst_t = 0.0   # the link is over: no burst hold lingers after it
                elif msg.startswith("CONNECTED"):
                    if not self.link_open and live:
                        self.log("VarAC: link open")
                    self.link_open = True
                elif not live:
                    pass                 # old PTT lines in the tail at start-up are history
                elif msg.startswith("PTT ON"):
                    self.ptt, self.ptt_t, self.burst_t = True, now, now
                elif msg.startswith("PTT OFF"):
                    self.ptt, self.ptt_t, self.burst_t = False, now, now
                elif msg.startswith("PENDING"):
                    self.burst_t = now   # a connect request: a PTT follows within a second

    def _check_alive(self, now):
        """VarAC's own instance row (not ours) carries a keepalive VarAC refreshes itself."""
        if now - self.db_checked_t < 5.0:
            return
        self.db_checked_t = now
        if not self.db and self.path:
            cand = os.path.join(os.path.dirname(self.path), "VarAC.db")
            if os.path.exists(cand):
                self.db = cand
        if not self.db or not os.path.exists(self.db):
            self.varac_alive = None
            return
        try:
            con = sqlite3.connect("file:%s?mode=ro" % self.db, uri=True, timeout=1.0)
            try:
                row = con.execute("SELECT MAX(last_keepalive_time) FROM instance WHERE guid != ?",
                                  (VaracLever.GUID,)).fetchone()
            finally:
                con.close()
        except sqlite3.Error:
            self.varac_alive = None
            return
        if not row or not row[0]:
            self.varac_alive = None
            return
        try:
            # 2026-10-29 20:58:28.9007671Z -> epoch; the stamp is UTC, so timegm, not mktime
            t = calendar.timegm(time.strptime(row[0][:19], "%Y-%m-%d %H:%M:%S"))
        except ValueError:
            self.varac_alive = None
            return
        self.varac_keepalive_t = t
        alive = now - t < self.dead_s
        if self.varac_alive is True and not alive:
            self.log("VarAC: its keepalive stopped %d s ago, treating VarAC as gone" % int(now - t))
        self.varac_alive = alive

    def snapshot(self):
        """(link open, PTT on now, time of the last PTT line, time of the last burst activity)."""
        with self.lock:
            now = time.time()
            self._check_alive(now)
            # VarAC's own keepalive decides whether an open link is real; the log-silence rule
            # is the fallback only when the database cannot be read
            if self.varac_alive is not None:
                link = self.link_open and self.varac_alive
            else:
                link = self.link_open and (now - self.last_line_t) < self.stale_s
            if self.link_open and not link:
                self.link_open = False
                self.log("VarAC: link treated as gone (%s)" % (
                    "VarAC's keepalive stopped" if self.varac_alive is False else "log silent %d s" % int(self.stale_s)))
            return link, self.ptt, self.ptt_t, self.burst_t

    def close(self):
        self.stop.set()


class VaracLever:
    """VarAC's own switch: the cluster PTT lock.

    VarAC (Cluster enabled, PTT Lock ON) keeps one row per cluster instance
    in VarAC.db table `instance` (busy, busy_last_time, last_keepalive_time).
    While any other instance's row says busy, VarAC disables its own PTT and
    shows "PTT is disabled". The arbiter is one such instance, named
    "Arbiter", with a fixed guid; hold = busy 1 with fresh times, renew =
    the same, release = busy 0, and the row is deleted when the arbiter
    exits. Proven on a live VarAC 16.1 under Wine, 2026-10-29 (WM8Q).
    lever = none: observe only, nothing is ever written."""

    GUID = "00000000-0000-4000-8000-00000000a7b1"
    NAME = "Arbiter"

    def __init__(self, s, log):
        self.log = log
        self.kind = s.get("varac", "lever").lower() or "none"
        self.db = s.get("varac", "db_path")
        self.renew_s = s.getfloat("varac", "varac_renew_s")
        self.held = False
        self.status = "observe only"

    def set_log_path(self, log_path):
        """VarAC.db lives beside the command log when db_path is not given."""
        if not self.db and log_path:
            cand = os.path.join(os.path.dirname(log_path), "VarAC.db")
            if os.path.exists(cand):
                self.db = cand

    @property
    def available(self):
        ok = self.kind == "cluster_lock" and bool(self.db) and os.path.exists(self.db)
        self.status = "cluster lock ready" if ok else (
            "observe only" if self.kind != "cluster_lock" else "cluster lock: VarAC.db not found")
        return ok

    @staticmethod
    def _stamp():
        """VarAC's own stamp shape: 2026-10-29 20:58:28.9007671Z (UTC, 7 digits)."""
        t = time.time()
        return time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime(t)) + ".%07dZ" % int((t % 1) * 1e7)

    def _write(self, what):
        try:
            con = sqlite3.connect(self.db, timeout=1.0)
            try:
                ts = self._stamp()
                if what == "hold":
                    row = con.execute("SELECT id FROM instance WHERE guid=?", (self.GUID,)).fetchone()
                    if row:
                        con.execute("UPDATE instance SET busy=1, busy_last_time=?, last_keepalive_time=?, "
                                    "is_deleted=0 WHERE guid=?", (ts, ts, self.GUID))
                    else:
                        con.execute(
                            "INSERT INTO instance (guid, name, comments, busy, busy_last_time, "
                            "last_keepalive_time, creation_time, is_deleted, email_gateway_active, "
                            "email_gateway_sender_node, frequency, band, mode) VALUES "
                            "(?, ?, 'hold placed by the arbiter', 1, ?, ?, ?, 0, 0, 0, 0, '', 'VARA')",
                            (self.GUID, self.NAME, ts, ts, ts))
                elif what == "release":
                    con.execute("UPDATE instance SET busy=0, busy_last_time=?, last_keepalive_time=? WHERE guid=?",
                                (ts, ts, self.GUID))
                elif what == "remove":
                    con.execute("DELETE FROM instance WHERE guid=?", (self.GUID,))
                con.commit()
            finally:
                con.close()
            return True
        except sqlite3.Error as e:
            self.log("VarAC: cluster lock %s failed (%s)" % (what, str(e)[:80]))
            return False

    def hold(self):
        if not self.available or self.held:
            return False
        if self._write("hold"):
            self.held = True
            return True
        return False

    def renew(self):
        if self.held:
            self._write("hold")

    def release(self):
        if not self.held:
            return False
        self.held = False
        return self._write("release")

    def remove(self):
        """On exit: leave no Arbiter row behind."""
        if self.kind == "cluster_lock" and self.db and os.path.exists(self.db):
            self._write("remove")


# ----------------------------------------------------------------------------
# the arbiter: one tick, one hold table
# ----------------------------------------------------------------------------

class Program:
    """One row of the hold table."""

    def __init__(self, name, enabled):
        self.name = name
        self.enabled = enabled       # hold allowed (settings, window checkbox)
        self.connected = False
        self.transmitting = False
        self.held = False
        self.reason = ""
        self.held_t = 0.0
        self.renewed_t = 0.0
        self.renew_s = 60.0          # set per program by the Arbiter
        self.overridden = False      # operator released it during this busy period
        self.status = ""


class Arbiter:
    def __init__(self, settings, log):
        self.s = settings
        self.log = log
        self.poll_s = settings.getint("timing", "poll_ms") / 1000.0
        self.state_poll_s = settings.getfloat("timing", "state_poll_s")
        self.renew_s = settings.getfloat("timing", "renew_s")
        self.frame_gap_s = settings.getfloat("timing", "subspace_frame_gap_s")
        self.ptt_match_s = settings.getfloat("timing", "vara_ptt_match_s")
        self.vara_burst_hold_s = settings.getfloat("timing", "vara_burst_hold_s")
        self.unknown_guard_s = settings.getfloat("timing", "unknown_guard_s")
        self.unkey_claim_s = settings.getfloat("timing", "unkey_claim_s")
        self.fl_last_tx_t = 0.0

        self.flrig = FlrigWatch(settings, log)
        self.fldigi = FldigiLink(settings, log)
        self.subspace = SubspaceLink(settings, log)
        self.varac = VaracWatch(settings, log)
        self.lever = VaracLever(settings, log)

        self.ss = Program("Subspace", settings.getbool("holds", "subspace"))
        self.fl = Program("fldigi", settings.getbool("holds", "fldigi"))
        self.va = Program("VarAC", settings.getbool("holds", "varac"))
        self.programs = (self.ss, self.fl, self.va)
        self.ss.renew_s = self.renew_s
        self.fl.renew_s = self.renew_s
        self.va.renew_s = self.lever.renew_s

        # Subspace message tracking (nothing pushed spans the gap between frames)
        self.ss_ptt = False
        self.ss_msg = False
        self.ss_last_off_t = 0.0
        self.ss_busy = False          # STATION.GET_BUSY: arq_tx / arq_negotiating / transmitting
        self.ss_busy_polled_t = 0.0
        self.ss_hold_owned = False    # our TX.SET_ENABLED false changed the button (else it was off already)
        self.ss_tx_enabled = None
        self.ss_dial_t = 0.0
        self.logged_vfo = None

        self.unknown_keyed_until = 0.0
        self.busy_period = False
        self.sources = {}             # reason -> active (bool), this tick
        self.lock = threading.Lock()
        self.stop = threading.Event()
        self.started_t = time.time()
        atexit.register(self.release_all)

    # -- Subspace events ----------------------------------------------------
    def _subspace_events(self, now):
        while self.subspace.events:
            msg = self.subspace.events.popleft()
            t = msg.get("type", "")
            p = msg.get("params", {}) or {}
            if t == "RIG.PTT":
                on = bool(p.get("PTT", str(msg.get("value", "")).lower() == "on"))
                if on:
                    self.ss_ptt, self.ss_msg = True, True
                else:
                    self.ss_ptt, self.ss_last_off_t = False, now
            elif t == "TX.COMPLETE":
                self.ss_msg = False
            elif t == "TX.ENABLED":
                en = bool(p.get("ENABLED", True))
                self.ss_tx_enabled = en
                if en and p.get("OVERRIDE"):
                    self.ss.overridden = True
                    self.ss.held = False
                    self.ss_hold_owned = False
                    self.log("Subspace: operator re-enabled TX over the hold (%s); not held again this busy period"
                             % self.ss.reason)
                elif en and p.get("EXPIRED"):
                    self.ss.held = False
                    self.ss_hold_owned = False
                    self.log("Subspace: hold lease expired inside Subspace (renew missed)")
                elif en and self.ss.held and not p.get("REMOTE"):
                    # released by something else (another API client, settings reload)
                    self.ss.held = False
                    self.ss_hold_owned = False
            elif t == "RIG.FREQ":
                self.ss_dial_t = now
            elif t == "STATION.CLOSING":
                self.ss.held = False
                self.ss_hold_owned = False
                self.log("Subspace: closing")

    def _subspace_state_poll(self, now):
        if now - self.ss_busy_polled_t < self.state_poll_s:
            return
        self.ss_busy_polled_t = now
        r = self.subspace.request("STATION.GET_BUSY", timeout=1.0)
        if r is None:
            return
        p = r.get("params", {}) or {}
        reason = str(p.get("BUSY_REASON", "") or "")
        self.ss_busy = bool(p.get("BUSY")) and reason in ("arq_tx", "arq_negotiating", "transmitting")
        if bool(p.get("PTT")):
            self.ss_ptt, self.ss_msg = True, True

    # -- one tick -------------------------------------------------------------
    def tick(self):
        now = time.time()
        self.flrig.poll()
        self.fldigi.poll()
        self._subspace_events(now)
        if self.subspace.up:
            self._subspace_state_poll(now)
        vara_link, vara_ptt, vara_ptt_t, vara_burst_t = self.varac.snapshot()

        # who is transmitting
        if self.ss_msg and not self.ss_ptt and now - self.ss_last_off_t > self.frame_gap_s:
            self.ss_msg = False   # no next frame came: the message ended without TX.COMPLETE (halted)
        # still transmitting for subspace_frame_gap_s after the last PTT off: covers the gap
        # between frames and the 1.4 s between two queued messages (measured 2026-10-29)
        ss_tx = self.ss_ptt or self.ss_msg or self.ss_busy or now - self.ss_last_off_t < self.frame_gap_s
        fl_tx = self.fldigi.transmitting
        if fl_tx:
            self.fl_last_tx_t = now
        # VARA transmitting: PTT on now (a PTT ON without OFF counts for a minute), or a burst or
        # connect request within vara_burst_hold_s, so the gaps between bursts keep the hold
        va_burst = (vara_ptt and now - vara_ptt_t < 60) or (now - vara_burst_t < self.vara_burst_hold_s)
        va_tx = vara_link or va_burst
        keyed = self.flrig.ptt is True
        # who owns the keyed radio: a program transmitting now, or one that stopped within
        # unkey_claim_s (its PTT off is still on its way), or VARA's PTT line, or a PTT change
        # so fresh that the owner's own report has not arrived yet
        claimed = self.ss_ptt or now - self.ss_last_off_t < self.unkey_claim_s or \
            fl_tx or now - self.fl_last_tx_t < self.unkey_claim_s or \
            (vara_ptt and abs(now - vara_ptt_t) < 60) or abs(now - vara_ptt_t) < self.unkey_claim_s or \
            (now - self.flrig.ptt_changed_t) < self.ptt_match_s
        if keyed and not claimed:
            if self.unknown_keyed_until == 0.0:
                self.log("flrig: radio keyed, program unknown")
            self.unknown_keyed_until = now + self.unknown_guard_s
        elif not keyed and self.unknown_keyed_until and now > self.unknown_keyed_until:
            self.unknown_keyed_until = 0.0
        unknown = self.unknown_keyed_until > 0.0

        sources = {R_VARA: vara_link, R_VARA_TX: va_burst and not vara_link,
                   R_FLDIGI: fl_tx, R_SUBSPACE: ss_tx, R_UNKNOWN: unknown}
        own = {self.ss: {R_SUBSPACE}, self.fl: {R_FLDIGI}, self.va: {R_VARA, R_VARA_TX}}
        with self.lock:
            self.sources = sources
            self.ss.connected = self.subspace.up
            self.fl.connected = self.fldigi.link.up
            self.va.connected = self.varac.path is not None
            if self.va.connected:
                self.lever.set_log_path(self.varac.path)
            self.lever.available   # refreshes its status text
            self.ss.transmitting, self.fl.transmitting, self.va.transmitting = ss_tx, fl_tx, va_tx

            busy = any(sources.values())
            if busy and not self.busy_period:
                self.busy_period = True
            elif not busy and self.busy_period:
                self.busy_period = False
                for p in self.programs:
                    if p.overridden:
                        self.log("%s: busy period over, override forgotten" % p.name)
                    p.overridden = False

            for p in self.programs:
                others = [r for r in REASON_ORDER if sources[r] and r not in own[p]]
                want = bool(others) and not p.transmitting and not p.overridden and p.enabled
                reason = others[0] if others else ""
                self._apply(p, want, reason, now)

            # the dial is never arbitrated, only logged, once per change
            if self.flrig.vfo is not None and self.flrig.vfo != self.logged_vfo:
                if self.logged_vfo is not None:
                    who = "Subspace" if now - self.ss_dial_t < 2.0 else "program unknown"
                    self.log("flrig: dial %d (%s)%s" % (self.flrig.vfo, who, " during a VARA link" if vara_link else ""))
                self.logged_vfo = self.flrig.vfo

            self.ss.status = self.subspace.status
            self.fl.status = "connected" if self.fldigi.link.up else "not answering"
            self.va.status = self.varac.status + "; " + self.lever.status

    def _apply(self, p, want, reason, now):
        if want and not p.held:
            if self._hold(p, reason, now):
                p.held, p.reason, p.held_t, p.renewed_t = True, reason, now, now
                self.log("%s: disabled (%s)" % (p.name, reason))
        elif want and p.held:
            if reason != p.reason:
                p.reason = reason
                self._renew(p, reason, now)
                p.renewed_t = now
                self.log("%s: hold reason now %s" % (p.name, reason))
            elif now - p.renewed_t >= p.renew_s:
                self._renew(p, reason, now)
                p.renewed_t = now
        elif not want and p.held:
            if p.transmitting:
                return   # never touch a program while it transmits
            self._release(p)
            p.held = False
            self.log("%s: released (%s over)" % (p.name, p.reason))
            p.reason = ""

    def _hold(self, p, reason, now):
        if p is self.ss:
            if not self.subspace.up:
                return False
            r = self.subspace.request("TX.SET_ENABLED", ENABLED=False, REASON=reason)
            if r is None:
                return False
            q = r.get("params", {}) or {}
            if not q.get("OK", False):
                self.log("Subspace: hold refused (%s)" % q.get("REASON", ""))
                return False
            self.ss_hold_owned = bool(q.get("CHANGED")) or bool(q.get("REMOTE"))
            if not self.ss_hold_owned:
                self.log("Subspace: TX was already off by the operator; nothing to hold")
            self.ss_tx_enabled = False
            return True
        if p is self.fl:
            return self.fldigi.hold()
        if p is self.va:
            return self.lever.hold()
        return False

    def _renew(self, p, reason, now):
        if p is self.ss:
            r = self.subspace.request("TX.SET_ENABLED", ENABLED=False, REASON=reason)
            if r is None:
                return
            q = r.get("params", {}) or {}
            if q.get("OK", False) and (q.get("CHANGED") or q.get("REMOTE")):
                self.ss_hold_owned = True
        elif p is self.va:
            self.lever.renew()
        # fldigi: rx_only has no lease; nothing to renew

    def _release(self, p):
        if p is self.ss:
            if self.ss_hold_owned and self.subspace.up:
                self.subspace.request("TX.SET_ENABLED", ENABLED=True)
            self.ss_hold_owned = False
        elif p is self.fl:
            self.fldigi.release()
        elif p is self.va:
            self.lever.release()

    # -- operator controls (window) -------------------------------------------
    def operator_release(self, p):
        with self.lock:
            if p.held:
                self._release(p)
                p.held = False
                self.log("%s: released by the operator (%s); not held again this busy period" % (p.name, p.reason))
                p.reason = ""
            p.overridden = True

    def set_enabled(self, p, on):
        with self.lock:
            p.enabled = on
            self.log("%s: hold %s by the operator" % (p.name, "enabled" if on else "disabled"))
            if not on and p.held:
                self._release(p)
                p.held = False
                p.reason = ""

    def release_all(self):
        """Every exit path we can see: window close, Ctrl-C, SIGTERM, atexit."""
        with self.lock:
            for p in self.programs:
                if p.held:
                    try:
                        self._release(p)
                    except Exception:
                        pass
                    p.held = False
                    self.log("%s: released (arbiter closing)" % p.name)

    def run(self):
        self.log("arbiter %s started; holds: Subspace %s, fldigi %s, VarAC %s (lever: %s)" % (
            VERSION, "on" if self.ss.enabled else "off", "on" if self.fl.enabled else "off",
            "on" if self.va.enabled else "off", self.lever.kind))
        while not self.stop.is_set():
            t0 = time.time()
            try:
                self.tick()
            except Exception as e:   # the tick must never die
                self.log("arbiter: tick error %s: %s" % (type(e).__name__, str(e)[:100]))
            dt = self.poll_s - (time.time() - t0)
            if dt > 0:
                self.stop.wait(dt)
        self.release_all()
        self.lever.remove()
        self.subspace.close()
        self.varac.close()


# ----------------------------------------------------------------------------
# window (Tk) or console
# ----------------------------------------------------------------------------

def run_window(arb):
    import tkinter as tk
    from tkinter import ttk
    root = tk.Tk()
    root.title("Arbiter %s" % VERSION)
    frm = ttk.Frame(root, padding=8)
    frm.grid(sticky="nsew")
    root.columnconfigure(0, weight=1)
    root.rowconfigure(0, weight=1)
    cols = ("Program", "Connection", "State", "Tx disable reason", "Renew in", "Enable Arb")
    for c, h in enumerate(cols):
        ttk.Label(frm, text=h, font=("TkDefaultFont", 9, "bold")).grid(row=0, column=c, sticky="w", padx=4)
    ttk.Label(frm, text="").grid(row=0, column=6)
    rows = []
    for i, p in enumerate(arb.programs, start=1):
        labels = [ttk.Label(frm, text=p.name, width=10)]
        labels += [ttk.Label(frm, text="", width=w) for w in (44, 12, 18, 9)]
        for c, lbl in enumerate(labels):
            lbl.grid(row=i, column=c, sticky="w", padx=4)
        var = tk.BooleanVar(value=p.enabled)
        chk = ttk.Checkbutton(frm, variable=var, command=lambda p=p, v=var: arb.set_enabled(p, v.get()))
        chk.grid(row=i, column=5)
        btn = ttk.Button(frm, text="Release", command=lambda p=p: arb.operator_release(p))
        btn.grid(row=i, column=6, padx=4)
        rows.append((p, labels, var))
    flrig_lbl = ttk.Label(frm, text="")
    flrig_lbl.grid(row=4, column=0, columnspan=7, sticky="w", padx=4, pady=(6, 2))
    txt = tk.Text(frm, height=14, width=110, state="disabled", font=("TkFixedFont", 9))
    txt.grid(row=5, column=0, columnspan=7, sticky="nsew", pady=(4, 0))
    frm.rowconfigure(5, weight=1)
    frm.columnconfigure(1, weight=1)
    shown = [0]

    def refresh():
        with arb.lock:
            for p, labels, var in rows:
                labels[1].config(text=p.status[:60])
                st = "TX" if p.transmitting else ("TX Disabled" if p.held else ("RX" if p.connected else "-"))
                labels[2].config(text=st, foreground=("#b00" if st == "TX" else "#a60" if st == "TX Disabled" else "#000"))
                labels[3].config(text=p.reason)
                left = ""
                if p.held and p is not arb.fl:
                    left = "%d s" % max(0, int(p.renew_s - (time.time() - p.renewed_t)))
                labels[4].config(text=left)
            ptt = arb.flrig.ptt
            flrig_lbl.config(text="flrig: %s   PTT %s   dial %s   busy: %s" % (
                "connected" if arb.flrig.link.up else "not answering",
                "ON" if ptt else ("off" if ptt is False else "?"),
                arb.flrig.vfo if arb.flrig.vfo else "?",
                ", ".join(r for r in REASON_ORDER if arb.sources.get(r)) or "none"))
        with arb.log.lock:
            lines = list(arb.log.lines)
        if len(lines) != shown[0] or (lines and shown[0] and lines[-1] != getattr(refresh, "last", None)):
            txt.config(state="normal")
            txt.delete("1.0", "end")
            txt.insert("end", "\n".join(lines[-200:]))
            txt.see("end")
            txt.config(state="disabled")
            shown[0] = len(lines)
            refresh.last = lines[-1] if lines else None
        root.after(250, refresh)

    def on_close():
        arb.stop.set()
        root.after(300, root.destroy)

    root.protocol("WM_DELETE_WINDOW", on_close)
    th = threading.Thread(target=arb.run, name="arbiter", daemon=True)
    th.start()
    refresh()
    root.mainloop()
    arb.stop.set()
    th.join(3.0)


def main(argv=None):
    ap = argparse.ArgumentParser(description="Arbiter for a shared radio (Subspace, fldigi, VarAC through flrig).")
    ap.add_argument("--ini", default=os.path.join(HERE, "arbiter.ini"), help="settings file (default: beside this script)")
    ap.add_argument("--console", action="store_true", help="no window; log to the console")
    ap.add_argument("--version", action="version", version="arbiter " + VERSION)
    args = ap.parse_args(argv)

    s = Settings(args.ini)
    logfile = s.get("log", "file")
    if not os.path.isabs(logfile):
        logfile = os.path.join(os.path.dirname(os.path.abspath(args.ini)), logfile)
    want_window = not args.console
    if want_window:
        try:
            import tkinter  # noqa: F401
        except ImportError:
            want_window = False
    log = Log(logfile, echo=not want_window)
    arb = Arbiter(s, log)

    def on_signal(signum, frame):
        arb.stop.set()
    for sig in ("SIGINT", "SIGTERM", "SIGBREAK", "SIGHUP"):
        if hasattr(signal, sig):
            try:
                signal.signal(getattr(signal, sig), on_signal)
            except (OSError, ValueError):
                pass

    if want_window:
        run_window(arb)
    else:
        arb.run()
    return 0


if __name__ == "__main__":
    sys.exit(main())
