"""Bench fakes for the arbiter: no radio, no real program.

FakeFlrig     XML-RPC: rig.get_ptt, rig.get_vfo, rig.set_ptt, rig.set_vfo, main.get_version
FakeFldigi    XML-RPC: main.get_trx_state, main.rx_only, main.rx_tx, main.tx, main.rx
FakeSubspace  JSON lines on TCP: STATION.GET_BUSY, TX.GET_ENABLED, TX.SET_ENABLED (with a
              lease), EVENTS.KEEPALIVE; pushes RIG.PTT / TX.COMPLETE / TX.ENABLED;
              transmit(frames) drives flrig's PTT the way Subspace does; auto_reply_due()
              is refused while TX is off, exactly as the station refuses a send.
VaracLog      writes VarAC's command-log lines ("27/10/2026 12:47:24 - PTT ON").

Each fake records what the arbiter sent to it, so a scenario can assert on it.
"""

import json
import os
import socket
import socketserver
import sqlite3
import threading
import time
from xmlrpc.server import SimpleXMLRPCServer


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


class _Rpc(SimpleXMLRPCServer):
    allow_reuse_address = True

    def __init__(self, port):
        super().__init__(("127.0.0.1", port), logRequests=False, allow_none=True)


class FakeFlrig:
    def __init__(self):
        self.port = free_port()
        self.ptt = 0
        self.vfo = 7115000
        self.calls = []
        self.srv = _Rpc(self.port)
        self.srv.register_function(lambda: int(self.ptt), "rig.get_ptt")
        self.srv.register_function(lambda: str(self.vfo), "rig.get_vfo")
        self.srv.register_function(self._set_ptt, "rig.set_ptt")
        self.srv.register_function(self._set_vfo, "rig.set_vfo")
        self.srv.register_function(lambda: "fake 1.0", "main.get_version")
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()

    def _set_ptt(self, v):
        self.calls.append(("set_ptt", int(v), time.time()))
        self.ptt = int(v)
        return 0

    def _set_vfo(self, v):
        self.calls.append(("set_vfo", int(v), time.time()))
        self.vfo = int(v)
        return 0

    def close(self):
        self.srv.shutdown()
        self.srv.server_close()


class FakeFldigi:
    def __init__(self, flrig=None):
        self.port = free_port()
        self.flrig = flrig
        self.state = "RX"
        self.rx_only = False
        self.calls = []
        self.srv = _Rpc(self.port)
        self.srv.register_function(lambda: self.state, "main.get_trx_state")
        self.srv.register_function(self._rx_only, "main.rx_only")
        self.srv.register_function(self._rx_tx, "main.rx_tx")
        self.srv.register_function(self._tx, "main.tx")
        self.srv.register_function(self._rx, "main.rx")
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()

    def _rx_only(self):
        self.calls.append(("rx_only", time.time()))
        self.rx_only = True
        self._rx()
        return ""

    def _rx_tx(self):
        self.calls.append(("rx_tx", time.time()))
        self.rx_only = False
        self._rx()
        return ""

    def _tx(self):
        """A macro or the T/R button: refused while rx_only, as in stock fldigi."""
        if self.rx_only:
            self.calls.append(("tx_refused", time.time()))
            return ""
        self.state = "TX"
        if self.flrig:
            self.flrig.ptt = 1
        return ""

    def _rx(self):
        self.state = "RX"
        if self.flrig:
            self.flrig.ptt = 0
        return ""

    def close(self):
        self.srv.shutdown()
        self.srv.server_close()


class FakeSubspace:
    """Enough of the Subspace TCP API for the arbiter, with the Build 517 TX enable rules."""

    def __init__(self, flrig=None, lease_s=300.0, max_connections=1):
        self.port = free_port()
        self.flrig = flrig
        self.lease_s = lease_s
        self.max_connections = max_connections
        self.tx_enabled = True
        self.remote = False
        self.reason = ""
        self.lease_t = None
        self.transmitting = False
        self.busy_reason = ""
        self.clients = []
        self.requests = []      # (type, params, time)
        self.refused_sends = 0
        self.sent = []          # messages that went on the air
        self.lock = threading.Lock()
        fake = self

        class Handler(socketserver.StreamRequestHandler):
            def handle(self):
                with fake.lock:
                    if len(fake.clients) >= fake.max_connections:
                        self.wfile.write(b'{"type":"API.ERROR","value":"Connections Full"}\n')
                        return
                    fake.clients.append(self)
                try:
                    for raw in self.rfile:
                        line = raw.strip()
                        if not line:
                            continue
                        try:
                            msg = json.loads(line.decode("utf-8"))
                        except ValueError:
                            continue
                        fake._handle(self, msg)
                finally:
                    with fake.lock:
                        if self in fake.clients:
                            fake.clients.remove(self)

        class Server(socketserver.ThreadingTCPServer):
            allow_reuse_address = True
            daemon_threads = True

        self.srv = Server(("127.0.0.1", self.port), Handler)
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()
        threading.Thread(target=self._lease_loop, daemon=True).start()

    # -- wire ---------------------------------------------------------------
    def _send_to(self, h, obj):
        try:
            h.wfile.write((json.dumps(obj) + "\n").encode("utf-8"))
            h.wfile.flush()
        except OSError:
            pass

    def push(self, mtype, value="", **params):
        params["_ID"] = -1
        with self.lock:
            cs = list(self.clients)
        for h in cs:
            self._send_to(h, {"type": mtype, "value": value, "params": params})

    def _handle(self, h, msg):
        t = msg.get("type", "")
        p = msg.get("params", {}) or {}
        rid = p.get("_ID", 0)
        self.requests.append((t, dict(p), time.time()))
        if t == "EVENTS.KEEPALIVE":
            self._send_to(h, {"type": "EVENTS.PONG", "params": {"_ID": rid, "UTC": int(time.time() * 1000)}})
        elif t == "STATION.GET_BUSY":
            busy = self.transmitting or bool(self.busy_reason)
            self._send_to(h, {"type": "STATION.BUSY", "value": self.busy_reason, "params": {
                "_ID": rid, "BUSY": busy, "BUSY_REASON": self.busy_reason or ("transmitting" if self.transmitting else ""),
                "PTT": self.transmitting, "ARQ_TX_ACTIVE": self.busy_reason == "arq_tx", "ARQ_RX_ACTIVE": False}})
        elif t == "TX.GET_ENABLED":
            self._send_to(h, {"type": "TX.ENABLED", "params": {
                "_ID": rid, "ENABLED": self.tx_enabled, "REMOTE": self.remote, "REASON": self.reason}})
        elif t == "TX.SET_ENABLED":
            self._set_enabled(h, rid, p)
        else:
            self._send_to(h, {"type": "API.ERROR", "value": "unknown " + t, "params": {"_ID": rid}})

    def _set_enabled(self, h, rid, p):
        if "ENABLED" not in p:
            self._send_to(h, {"type": "TX.ENABLED", "params": {"_ID": rid, "OK": False, "REASON": "missing",
                                                                "ENABLED": self.tx_enabled, "REMOTE": self.remote}})
            return
        want = bool(p["ENABLED"])
        reason = str(p.get("REASON", "") or "remote command")[:40]
        changed = want != self.tx_enabled
        if changed:
            self._button(want, by_remote=True, reason=reason)
        held = self.remote and not self.tx_enabled
        if held and not want:
            self.reason = reason
            self.lease_t = time.time() + self.lease_s
        self._send_to(h, {"type": "TX.ENABLED", "params": {
            "_ID": rid, "ENABLED": self.tx_enabled, "REMOTE": self.remote, "REASON": self.reason if held else "",
            "HOLD_S": int(self.lease_s) if held else 0, "CHANGED": changed, "OK": True}})

    def _button(self, on, by_remote, reason="", override=False, expired=False):
        """The TX enable button toggled: the one place the event comes from."""
        self.tx_enabled = on
        self.remote = (not on) and by_remote
        self.reason = reason if self.remote else ""
        if on:
            self.lease_t = None
        if not on and self.transmitting:
            self.transmitting = False       # the full halt cuts the message
            if self.flrig:
                self.flrig.ptt = 0
        self.push("TX.ENABLED", ENABLED=on, REMOTE=self.remote, REASON=self.reason,
                  OVERRIDE=override, EXPIRED=expired)

    def _lease_loop(self):
        while True:
            time.sleep(0.1)
            if self.lease_t and time.time() > self.lease_t and not self.tx_enabled:
                self._button(True, by_remote=False, expired=True)

    # -- the station's behaviour the scenarios drive -----------------------------
    def operator_click(self, on):
        override = on and self.remote and not self.tx_enabled
        self._button(on, by_remote=False, override=override)

    def transmit(self, frames=2, frame_s=0.6, gap_s=0.3, text="WM8Q: K7RIE SNR +05"):
        """A message of N frames: PTT on/off per frame on flrig, TX.COMPLETE at the end.
        Refused (as the station refuses it) while TX enable is off."""
        if not self.tx_enabled:
            self.refused_sends += 1
            return False
        self.transmitting = True
        for i in range(frames):
            if not self.transmitting:
                return False
            if self.flrig:
                self.flrig.ptt = 1
            self.push("RIG.PTT", "on", PTT=True, UTC=int(time.time() * 1000))
            time.sleep(frame_s)
            if i == frames - 1:
                self.push("TX.COMPLETE", text, SUBMODE=0, UTC=int(time.time() * 1000))
            if self.flrig:
                self.flrig.ptt = 0
            self.push("RIG.PTT", "off", PTT=False, UTC=int(time.time() * 1000))
            if i < frames - 1:
                time.sleep(gap_s)
        self.transmitting = False
        self.sent.append(text)
        return True

    def auto_reply_due(self):
        return self.transmit(frames=1, frame_s=0.4, text="auto-reply")

    def close(self):
        self.srv.shutdown()
        self.srv.server_close()


class VaracLog:
    """Writes VarAC's VARA command log, one line per event, the station's format,
    and a VarAC.db beside it with the `instance` table as VarAC 16.1 creates it
    (one row: VarAC's own instance)."""

    def __init__(self, path):
        self.path = path
        open(path, "w").close()
        self.db = os.path.join(os.path.dirname(path), "VarAC.db")
        con = sqlite3.connect(self.db)
        con.execute("CREATE TABLE instance (id INTEGER PRIMARY KEY AUTOINCREMENT, guid TEXT, name TEXT, "
                    "comments TEXT, busy INTEGER, busy_last_time TEXT, last_keepalive_time TEXT, "
                    "creation_time TEXT, is_deleted INTEGER, email_gateway_active INTEGER, "
                    "email_gateway_sender_node INTEGER, frequency INTEGER, band TEXT, mode TEXT, gps_nmea TEXT)")
        con.execute("INSERT INTO instance (guid, name, busy, busy_last_time, last_keepalive_time, creation_time, "
                    "is_deleted, email_gateway_active, email_gateway_sender_node, frequency, band, mode) VALUES "
                    "('a1bff5cf-4fb8-44dd-b331-f5dbf941212a', '', 0, '', '', '', 0, 0, 0, 14105000, '20m', 'VARA')")
        con.commit()
        con.close()

    def arbiter_row(self):
        """(busy, last_keepalive_time) of the Arbiter instance row, or None when absent."""
        con = sqlite3.connect(self.db)
        try:
            r = con.execute("SELECT busy, last_keepalive_time FROM instance WHERE name='Arbiter'").fetchone()
        finally:
            con.close()
        return r

    def line(self, kind):
        lt = time.localtime()
        stamp = time.strftime("%d/%m/%Y %H:%M:%S", lt)
        with open(self.path, "a", encoding="utf-8") as f:
            f.write("%s - %s\n" % (stamp, kind))
