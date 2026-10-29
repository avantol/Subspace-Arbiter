# Arbiter for a shared radio

One radio, several programs (Subspace, fldigi, VarAC) through flrig. The
Arbiter is a small helper that keeps them from keying over each other.

## The one rule

Whatever is already transmitting finishes. The Arbiter holds only what
has not started, by throwing each program's own switch, and it never
refuses, swallows or delays a PTT.

## What it does, in plain words

1. It watches flrig (is the radio keyed, where is the dial), Subspace (its API), fldigi (its XML-RPC port) and VarAC (its command log and database).
2. When one program is on the air, or a VARA link is open, it tells the others to wait, using each program's own switch.
3. Subspace: its TX enable button goes off with the reason shown ("TX disabled by VARA link"); a Send is refused with that reason. The hold is a lease: if the Arbiter dies, Subspace turns TX back on by itself after 300 s.
4. fldigi: receive-only (the stock `main.rx_only`), set only while fldigi is idle; restored with `main.rx_tx`.
5. VarAC: held through VarAC's own cluster PTT lock. The Arbiter appears in VarAC.db as a second cluster instance named "Arbiter"; while that row says busy, VarAC shows "PTT is disabled" and will not key (about 1 s after the hold). VarAC drops a lock whose keepalive is 30 s old, so a dead Arbiter frees VarAC in half a minute.
6. When the radio is clear and no link is open, everything is released.
7. The operator always wins: re-enable TX in Subspace and the Arbiter leaves Subspace alone until the radio is next clear. The window has a Release button per program for the same purpose, and an "Enable Arb" box per program to take it out of arbitration.
8. The dial is never touched, only logged.
9. Nothing is changed in flrig, fldigi, Subspace or VarAC settings. Stop the Arbiter and every program works as before.

## Install

- Windows: run `Arbiter_<version>_win64.exe` from the Releases page. It installs for the current user, no administrator rights, with a Start-menu entry and an optional "start when Windows starts" for unattended stations.
- Linux (Debian, Ubuntu, Mint): `sudo apt install ./arbiter_<version>_amd64.deb`. "Arbiter" appears in the applications menu.
- The Arbiter itself needs no settings. The other programs need these, once:
  - Subspace: Settings > Reporting: TCP API enabled, "Accept TCP requests" on, TCP max connections at least one more than you use now (SpotterX takes one or two). The Arbiter says so in its window if it is refused.
  - fldigi: its XML-RPC server on (default port 7362; it is on by default).
  - VarAC: Settings > "Log VARA commands" on; Settings > Cluster: enabled, "PTT Lock" on; then restart VarAC.
- With CAT Watch in front of flrig (CAT Watch on 12345, flrig on 12346) nothing changes: the Arbiter talks to 12345 like every other program.

## Run

Start it from the menu. The window shows one row per program: connection,
state (RX, TX, or TX Disabled), the reason TX is disabled, the next
renew, an Enable box and a Release button; below, flrig's PTT and dial
and the transcript. Close the window to stop; every hold is released on
the way out.

Command line: `arbiter --console` (no window, log to the console),
`arbiter --ini <file>` (settings from that file), `arbiter --write-ini
<file>` (writes the documented defaults to edit from).

Log file: `%LOCALAPPDATA%\Arbiter\arbiter.log` on Windows,
`~/.local/share/Arbiter/arbiter.log` on Linux. Settings, only if you
create one: `%LOCALAPPDATA%\arbiter.ini` or `~/.config/arbiter.ini`.

## Settings (only for exceptions)

Every number is named in the ini template with its basis, measured or
chosen. The ones that matter:

| Setting | Default | Why |
|---|---|---|
| poll_ms | 200 | how often flrig and fldigi are asked; the reaction time |
| renew_s | 60 | how often a hold is renewed (Subspace's own lease is 300 s) |
| subspace_frame_gap_s | 2.5 | a Subspace message is still running this long after PTT drops between frames (measured 1.2 to 1.6 s, and 1.4 s between queued messages) |
| vara_burst_hold_s | 12 | a VARA burst or connect request keeps the hold this long, so gaps inside a link do not flicker the holds (measured up to 11.5 s) |
| varac_dead_s | 90 | VarAC's own keepalive in VarAC.db older than this = VarAC gone, link gone |
| unkey_claim_s | 3 | a program that reported transmitting this recently still owns the keyed radio (fldigi reports RX ~1 s before its PTT off reaches flrig) |
| unknown_guard_s | 15 | keying no program claims counts as busy this long after the unkey |

`[holds]` turns the hold on or off per program.

## Known limits

- One poll (200 ms) of reaction time: a program that keys inside that window collides as it does today. No frame is ever lost to a refusal, because there is none.
- Keying that bypasses flrig (VOX, a separate PTT line) is invisible.
- If the Arbiter is killed outright (not closed) while holding fldigi, fldigi stays receive-only until fldigi is restarted. A normal close restores it. This is the one case that is not fail-open, until fldigi has a hold with its own expiry.
- VarAC's lock lives in its database; VarAC must have Cluster enabled with PTT Lock on, or there is nothing to hold. The command log must be on, or VarAC's links are invisible.
- Do not stop and restart the Arbiter while it is holding: a stop releases everything.

## For developers

- `src/` is the program (C++20, Qt 6: Core, Network, Sql, Widgets). `cmake -B build && cmake --build build`.
- `arbiter.py` is the reference implementation the C++ was ported from, kept runnable.
- `tests/scenarios.py` is the bench: fakes of flrig, fldigi, Subspace and VarAC's log and database, and the eight acceptance checks from the requirements. `python3 tests/scenarios.py` runs them against the Python version, `ARBITER_BIN=build/arbiter python3 tests/scenarios.py` against the binary. Both must pass.
- `varac_probe.py` checks VarAC's cluster lock on a station in 30 s.
- Packaging: `packaging/build-deb.sh <version>`; Windows: `installer/arbiter.iss` from `build/Arbiter/` (windeployqt output), as the workflows do.
