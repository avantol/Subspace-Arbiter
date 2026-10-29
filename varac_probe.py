#!/usr/bin/env python3
"""Check VarAC's cluster PTT lock on this station, the lever the arbiter uses.

How the lock works (found on a live VarAC 16.1, 2026-10-29): with Cluster
enabled and PTT Lock ON, VarAC keeps one row per cluster instance in
VarAC.db, table `instance`. While another instance's row says busy and its
keepalive is less than 30 s old, VarAC disables its own PTT and shows
"PTT is disabled" (about 1 s after the row changes). The arbiter is such an
instance, named "Arbiter".

This tool writes that row for 25 s, asks whether VarAC shows "PTT is
disabled", then removes the row. Run it beside a running VarAC.

    python3 varac_probe.py                 finds VarAC.db itself
    python3 varac_probe.py /path/VarAC.db  or name it
"""

import glob
import os
import sqlite3
import sys
import time

IS_WIN = sys.platform.startswith("win")
HOME = os.path.expanduser("~")
GUID = "00000000-0000-4000-8000-00000000a7b1"   # the arbiter's fixed instance guid


def find_db():
    roots = [os.environ.get("SystemDrive", "C:") + "\\"] if IS_WIN else \
        glob.glob(os.path.join(HOME, ".wine*", "drive_c")) + \
        glob.glob(os.path.join(HOME, ".local", "share", "wineprefixes", "*", "drive_c"))
    found = []
    for r in roots:
        found += glob.glob(os.path.join(r, "VarAC*", "VarAC.db"))
    return sorted(set(found), key=os.path.getmtime)


def stamp():
    t = time.time()
    return time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime(t)) + ".%07dZ" % int((t % 1) * 1e7)


def write(db, what):
    con = sqlite3.connect(db, timeout=2.0)
    try:
        ts = stamp()
        if what == "hold":
            if con.execute("SELECT id FROM instance WHERE guid=?", (GUID,)).fetchone():
                con.execute("UPDATE instance SET busy=1, busy_last_time=?, last_keepalive_time=?, is_deleted=0 "
                            "WHERE guid=?", (ts, ts, GUID))
            else:
                con.execute("INSERT INTO instance (guid, name, comments, busy, busy_last_time, last_keepalive_time, "
                            "creation_time, is_deleted, email_gateway_active, email_gateway_sender_node, frequency, "
                            "band, mode) VALUES (?, 'Arbiter', 'probe', 1, ?, ?, ?, 0, 0, 0, 0, '', 'VARA')",
                            (GUID, ts, ts, ts))
        else:
            con.execute("DELETE FROM instance WHERE guid=?", (GUID,))
        con.commit()
    finally:
        con.close()


def ask(prompt):
    try:
        return input(prompt).strip().lower()
    except EOFError:
        return ""


def main(argv):
    path = argv[0] if argv else None
    if not path:
        dbs = find_db()
        if not dbs:
            print("No VarAC.db found under C:\\VarAC* (or a Wine prefix). Name it: varac_probe.py <file>")
            return 1
        path = dbs[-1]
    print("VarAC database: %s" % path)
    con = sqlite3.connect("file:%s?mode=ro" % path, uri=True, timeout=2.0)
    try:
        if not con.execute("SELECT name FROM sqlite_master WHERE type='table' AND name='instance'").fetchone():
            print("No `instance` table: this VarAC has never run with Cluster enabled. In VarAC: Settings >")
            print("Cluster: enable, PTT Lock ON, then restart VarAC and run this again.")
            return 1
        rows = con.execute("SELECT id, name, busy, last_keepalive_time FROM instance").fetchall()
    finally:
        con.close()
    print("instances now: %s" % rows)
    print("\nWriting the Arbiter row, busy, for 25 s. Watch VarAC: it should show 'PTT is disabled' within a")
    print("second or two, and its PTT, Tune, CQ and Beacon should refuse.")
    try:
        t0 = time.time()
        while time.time() - t0 < 25:
            write(path, "hold")
            time.sleep(5)
    finally:
        write(path, "remove")
    print("Row removed. VarAC should show its PTT enabled again within about 30 s.")
    r = ask("Did VarAC show 'PTT is disabled' while the row was there? [y/N] ")
    if r == "y":
        print("\nPROVEN on this station. arbiter.ini needs only:\n[varac]\nlever = cluster_lock")
        print("(db_path may stay blank: the arbiter finds VarAC.db beside the command log.)")
    else:
        print("\nNot proven here. Check in VarAC: Settings > Cluster enabled and PTT Lock ON, VarAC restarted.")
        print("Until it passes, set lever = none (observe only) in arbiter.ini.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
