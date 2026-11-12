# Arbiter for a shared radio

One radio, several programs (Subspace, fldigi, VarAC) through flrig. The
Arbiter is a small helper that keeps them from keying over each other.

## The one rule

Whatever is already transmitting finishes. The Arbiter holds only what
has not started, by throwing each program's own switch. Where a switch
holds a program only in part, flrig-subspace carries out the rest of the
hold on the Arbiter's instruction: today that is VarAC's un-key (item 9).
The Arbiter never refuses or delays a key.

## What it does, in plain words

1. It watches flrig (is the radio keyed, where is the dial), Subspace (its API), fldigi (its XML-RPC port) and VarAC (its command log and database).
2. When one program is on the air, or a VARA link is open, it tells the others to wait, using each program's own switch.
3. Subspace: its TX enable button goes off with the reason shown ("TX disabled by VARA link"); a Send is refused with that reason. The hold is a lease: if the Arbiter dies, Subspace turns TX back on by itself after 300 s.
4. fldigi: receive-only (the stock `main.rx_only`), set only while fldigi is idle; restored with `main.rx_tx`.
5. VarAC: held through VarAC's own cluster PTT lock. The Arbiter appears in VarAC.db as a second cluster instance named "Arbiter"; while that row says busy, VarAC shows "PTT is disabled" and will not key (about 1 s after the hold). VarAC drops a lock whose keepalive is 30 s old, so a dead Arbiter frees VarAC in half a minute.
6. When the radio is clear and no link is open, everything is released.
7. The operator always wins: re-enable TX in Subspace and the Arbiter leaves Subspace alone until the radio is next clear. The window has a Release button per program for the same purpose, and an "Enable Arb" box per program to take it out of arbitration.
8. Subspace (Build 523 and later) tells the Arbiter as soon as a frame is waiting for its slot, up to one slot before it keys. The Arbiter then holds VarAC and fldigi before Subspace keys, so VarAC (about 1 s to obey) is already held when the slot comes. A waiting frame never protects Subspace itself: a VARA link already on the air still holds it. Older Subspace builds send no such notice; the Arbiter then learns of a send when Subspace keys.
9. A held VarAC still un-keys the radio: VarAC drops VARA's "key" while held but passes VARA's "un-key" on to flrig, which cut whoever was transmitting (bench 11 Nov). With flrig-subspace 8 or later, the Arbiter tells flrig, for exactly as long as VarAC is held, to ignore un-keys labelled `XML-RPC.NET` (VarAC's label on every request, measured). A key VarAC itself made can always be ended. The instruction is renewed every 5 s and lapses within 15 s if the Arbiter stops. With stock flrig the Arbiter says once that this is not available and holds VarAC by its lock alone.
10. While VarAC is held, VARA's refused attempts to transmit do not count as VARA transmitting: they hold nobody else, and VarAC's hold is renewed and released on the normal rule (before 1.0.7 such attempts kept the hold from being renewed or released until VarAC's own 30 s timeout).
11. The dial is never touched, only logged.
12. Nothing is changed in flrig, fldigi, Subspace or VarAC settings. Stop the Arbiter and every program works as before.

## Install

- Windows: run `Arbiter_<version>_win64.exe` from the Releases page. It installs for the current user, no administrator rights, with a Start-menu entry and an optional "start when Windows starts" for unattended stations.
- Linux (Debian, Ubuntu, Mint): `sudo apt install ./arbiter_<version>_amd64.deb`. "Arbiter" appears in the applications menu.
- The Arbiter itself needs no settings. The other programs need these, once:
  - Subspace: Settings > Reporting: TCP API enabled, "Accept TCP requests" on, TCP max connections at least one more than you use now (SpotterX takes one or two). The Arbiter says so in its window if it is refused.
  - fldigi: nothing; its XML-RPC server is always on. To check it: Configure > Misc > TCP-IP sessions > XML, address 127.0.0.1, port 7362 (the defaults).
  - VarAC: Settings > "Log VARA commands" on; Settings > Cluster: enabled, "PTT Lock" on; then restart VarAC.
  - VarAC: in VarAC.ini (VarAC's folder, beside VarAC.exe; usually `C:\VarAC\VarAC.ini`), with VarAC closed, under `[RIG_CONTROL]` set `SafetyPTTOffEveryMinute=OFF`. VarAC has no screen for it (its release notes: "only available by manually editing the file"). ON, VarAC un-keys the radio once a minute, cutting off whichever program is transmitting (KR1FLE, 8 Nov: Subspace's first frame cut 0.6 s in).
- With CAT Watch in front of flrig (CAT Watch on 12345, flrig on 12346) nothing changes: the Arbiter talks to 12345 like every other program.

## Run

Start it from the menu. The window shows one row per program: connection,
state (RX, TX, or TX Disabled), the reason TX is disabled, the next
renew, an Enable box and a Release button; below, flrig's PTT and dial
and the transcript. Close the window to stop; every hold is released on
the way out.

One Arbiter per PC: a second copy started while one runs (from the menu
and from a start script, say) says "Arbiter is already running (pid N)"
and closes without touching anything. Two copies would release each
other's holds. The lock is `arbiter.lock` beside the log; a crashed
copy's lock is taken over at the next start.

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
| unkey_block_label | XML-RPC.NET | VarAC's label on its requests to flrig (measured); blank = never ask flrig to block un-keys |
| unkey_block_ms / unkey_block_renew_s | 15000 / 5 | how long one un-key block lasts at flrig, how often it is renewed while VarAC is held |

`[holds]` turns the hold on or off per program.

## Known limits

- One poll (200 ms) of reaction time: a program that keys inside that window collides as it does today (W8UFO, 10 Nov 19:48:44: VarAC keyed 185 ms before Subspace's slot). A Subspace frame announced ahead (Build 523 and later) closes that window for Subspace's slotted sends, as far as VarAC obeys in time; a VARA frame, an fldigi over or an un-slotted Subspace send gives no warning. No frame is ever lost to a refusal: the only thing ever refused is a held VarAC's stray un-key.
- Keying that bypasses flrig (VOX, a separate PTT line) is invisible.
- CommsPlan cannot be held: it has no switch the Arbiter can use. Its frequency change and its antenna tune (a transmission) are guarded only by CommsPlan's own check, just before it acts, that no PTT is asserted at that moment. The fix is CommsPlan obeying a hold itself.
- A held VarAC's un-keys are blocked only with flrig-subspace 8 or later. With stock flrig, VARA's un-key during a hold still reaches the radio. `XML-RPC.NET` is the default label of the library VarAC uses, not VarAC's own name: another program built on it would have its stray un-keys ignored too while VarAC is held (its own key can always be ended).
- A VARA exchange that starts while VarAC is held waits until the hold ends; the other station may give up first.
- If the Arbiter is killed outright (not closed) while holding fldigi, fldigi stays receive-only until the Arbiter is started again (or fldigi is restarted): while fldigi is held, a file `fldigi_held.flag` lies beside the Arbiter's log, and an Arbiter that finds it at start-up puts fldigi back to transmit first ("fldigi: restored to transmit" in the log). A normal close restores fldigi at once. fldigi's receive-only switch has no expiry of its own, so an Arbiter that is never started again leaves fldigi receive-only.
- VarAC's lock lives in its database; VarAC must have Cluster enabled with PTT Lock on, or there is nothing to hold. The command log must be on, or VarAC's links are invisible.
- Before its first write to VarAC.db in a run, the Arbiter keeps a copy of it beside it, `VarAC.db.arbiter-backup` (made by SQLite, refreshed once a week). If VarAC ever reports its database damaged: close VarAC and the Arbiter, rename the damaged VarAC.db, rename the copy to VarAC.db, start VarAC.
- The Arbiter asks flrig for PTT and fldigi for its state 5 times a second (poll_ms). On a station whose rig line is already busy, that is extra traffic; poll_ms can be raised in the ini at the cost of reaction time.
- Do not stop and restart the Arbiter while it is holding: a stop releases everything.

## For developers

- `src/` is the program (C++20, Qt 6: Core, Network, Sql, Widgets). `cmake -B build && cmake --build build`.
- `arbiter.py` is the reference implementation the C++ was ported from, kept runnable.
- `tests/scenarios.py` is the bench: fakes of flrig, fldigi, Subspace and VarAC's log and database, the eight acceptance checks from the requirements, and six more (refused fldigi release, an Arbiter killed while holding fldigi, a failed fldigi query during a hold, a waiting Subspace frame, a held VarAC while VARA keeps asking, a flrig without the un-key block). `python3 tests/scenarios.py` runs them against the Python version, `ARBITER_BIN=build/arbiter python3 tests/scenarios.py` against the binary. Both must pass.
- `varac_probe.py` checks VarAC's cluster lock on a station in 30 s.
- Packaging: `packaging/build-deb.sh <version>`; Windows: `installer/arbiter.iss` from `build/Arbiter/` (windeployqt output), as the workflows do.
