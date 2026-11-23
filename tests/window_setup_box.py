#!/usr/bin/env python3
"""[study] The arbiter's WINDOW: the setup box that opens by itself 8 s after the start
when a program is still missing. With the layer, VarAC is "connected" only once it has
really connected, which under Wine is later than those 8 s.

    case 1  VarAC connects 12 s after the start: the box opens by itself and says
            "Not reachable yet: VarAC."; it closes by itself once VarAC has connected
    case 2  VarAC connects 3 s after the start: no box at all

Needs a display (the windows are found with xwininfo). Run:
    ARBITER_BIN=build-study6/arbiter xvfb-run -a python3 tests/window_setup_box.py
"""
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import scenarios as S

BOX_OPENS_S = 8.0      # MainWindow.cpp: the arbiter looks 8 s after its start


def box_open():
    tree = subprocess.run(["xwininfo", "-root", "-tree"], capture_output=True, text=True).stdout
    return '"Arbiter setup"' in tree


def case(name, varac_after_s):
    b = S.Bench(subspace_ask=True, fldigi_ask=True, layer=True)
    fails = []
    try:
        t0 = time.time()
        b.proc = subprocess.Popen([S.BIN, "--ini", b.ini], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        S.wait_for(lambda: b.logged("Subspace: connected") and b.logged("fldigi: connected"), 6.0)
        if varac_after_s < BOX_OPENS_S:
            time.sleep(max(0.0, t0 + varac_after_s - time.time()))
            S.check(b.connect_varac(), "VarAC connects %.0f s after the start" % varac_after_s, fails)
            time.sleep(max(0.0, t0 + BOX_OPENS_S + 2.0 - time.time()))
            S.check(not box_open(), "no setup box 10 s after the start", fails)
        else:
            time.sleep(max(0.0, t0 + BOX_OPENS_S + 2.0 - time.time()))
            S.check(box_open(), "VarAC still missing 10 s after the start: the setup box opened by itself", fails)
            time.sleep(max(0.0, t0 + varac_after_s - time.time()))
            S.check(b.connect_varac(), "VarAC connects %.0f s after the start" % varac_after_s, fails)
            S.check(S.wait_for(lambda: not box_open(), 2.0), "the box closed by itself once VarAC had connected", fails)
    finally:
        b.stop()
    print("   => %s  (%s)" % ("PASS" if not fails else "FAIL", name))
    return not fails


if not S.BIN:
    sys.exit("set ARBITER_BIN to the C++ arbiter")
ok = [case("VarAC late: box opens and closes by itself", 12.0), case("VarAC early: no box", 3.0)]
print("ALL PASS" if all(ok) else "FAILED")
sys.exit(0 if all(ok) else 1)
