#include "Settings.h"

#include <QFileInfo>
#include <QSettings>

static bool onOff(const QVariant &v, bool dflt) {
    if (!v.isValid()) return dflt;
    QString s = v.toString().trimmed().toLower();
    if (s == "on" || s == "true" || s == "yes" || s == "1") return true;
    if (s == "off" || s == "false" || s == "no" || s == "0") return false;
    return dflt;
}

Settings Settings::load(const QString &iniPath) {
    Settings s;
    if (iniPath.isEmpty() || !QFileInfo::exists(iniPath))
        return s;
    QSettings ini(iniPath, QSettings::IniFormat);
    s.iniPath = iniPath;
    auto str = [&](const char *k, const QString &d) { return ini.value(k, d).toString().trimmed(); };
    auto num = [&](const char *k, double d) { bool ok; double v = ini.value(k, d).toDouble(&ok); return ok ? v : d; };
    s.flrigHost = str("ports/flrig_host", s.flrigHost);
    s.flrigPort = int(num("ports/flrig_port", s.flrigPort));
    s.subspaceHost = str("ports/subspace_host", s.subspaceHost);
    s.subspacePort = int(num("ports/subspace_port", s.subspacePort));
    s.fldigiHost = str("ports/fldigi_host", s.fldigiHost);
    s.fldigiPort = int(num("ports/fldigi_port", s.fldigiPort));
    s.holdSubspace = onOff(ini.value("holds/subspace"), s.holdSubspace);
    s.holdFldigi = onOff(ini.value("holds/fldigi"), s.holdFldigi);
    s.holdVarac = onOff(ini.value("holds/varac"), s.holdVarac);
    s.pollMs = int(num("timing/poll_ms", s.pollMs));
    s.statePollS = num("timing/state_poll_s", s.statePollS);
    s.renewS = num("timing/renew_s", s.renewS);
    s.subspaceFrameGapS = num("timing/subspace_frame_gap_s", s.subspaceFrameGapS);
    s.varacDeadS = num("timing/varac_dead_s", s.varacDeadS);
    s.varaStaleS = num("timing/vara_stale_s", s.varaStaleS);
    s.varaPttMatchS = num("timing/vara_ptt_match_s", s.varaPttMatchS);
    s.varaBurstHoldS = num("timing/vara_burst_hold_s", s.varaBurstHoldS);
    s.unknownGuardS = num("timing/unknown_guard_s", s.unknownGuardS);
    s.unkeyClaimS = num("timing/unkey_claim_s", s.unkeyClaimS);
    s.xmlrpcTimeoutS = num("timing/xmlrpc_timeout_s", s.xmlrpcTimeoutS);
    s.reconnectS = num("timing/reconnect_s", s.reconnectS);
    s.varacLogPath = str("varac/log_path", s.varacLogPath);
    s.varacLever = str("varac/lever", s.varacLever).toLower();
    s.varacDbPath = str("varac/db_path", s.varacDbPath);
    s.varacRenewS = num("varac/varac_renew_s", s.varacRenewS);
    s.logFile = str("log/file", s.logFile);
    if (s.pollMs < 50) s.pollMs = 50;
    return s;
}

QString Settings::defaultIniText() {
    return QStringLiteral(
        "# Arbiter settings. The program needs none of this: these are its defaults.\n"
        "# Every number has its basis stated: \"measured\" on a real station, or \"chosen\".\n"
        "# Only the keys present here override the built-in values. Change, restart.\n"
        "\n[ports]\n"
        "flrig_host = 127.0.0.1\nflrig_port = 12345\n"
        "subspace_host = 127.0.0.1\nsubspace_port = 2442\n"
        "fldigi_host = 127.0.0.1\nfldigi_port = 7362\n"
        "\n[holds]\n# on / off per program. Off = that program is observed but never held.\n"
        "subspace = on\nfldigi = on\nvarac = on\n"
        "\n[timing]\n"
        "# poll_ms: how often flrig and fldigi are asked. chosen. flrig answers rig.get_ptt\n"
        "# from its cache in about 0 ms; one poll is the reaction time.\n"
        "poll_ms = 200\n"
        "# state_poll_s: how often Subspace is asked STATION.GET_BUSY (ARQ state). chosen.\n"
        "state_poll_s = 2\n"
        "# renew_s: how often a hold is renewed. chosen; Subspace's own lease is 300 s.\n"
        "renew_s = 60\n"
        "# subspace_frame_gap_s: a Subspace message is still in progress this long after\n"
        "# its PTT drops. measured: 1.2 to 1.6 s between frames, 1.4 s between two queued\n"
        "# messages (WM8Q 2026-10-29); must stay under the 3.75 s Subspace slot.\n"
        "subspace_frame_gap_s = 2.5\n"
        "# varac_dead_s: VarAC refreshes its own instance row's keepalive in VarAC.db about\n"
        "# every 10 to 20 s (measured); when that stops this long, VarAC is gone and an\n"
        "# open link with it. chosen: 90 s.\n"
        "varac_dead_s = 90\n"
        "# vara_stale_s: far fallback when VarAC.db is not readable: a link whose VARA log\n"
        "# has been silent this long is treated as gone. chosen: 600 s.\n"
        "vara_stale_s = 600\n"
        "# vara_ptt_match_s: VarAC's log stamps are whole seconds. measured.\n"
        "vara_ptt_match_s = 2\n"
        "# vara_burst_hold_s: after a VARA PTT OFF (or a PENDING connect request) the hold\n"
        "# stays this long, so the gaps between VARA's bursts do not release and re-hold\n"
        "# the other programs every few seconds. measured: up to 11.5 s between frames\n"
        "# inside a link (K7RIE), 3 s between connect requests (WM8Q).\n"
        "vara_burst_hold_s = 12\n"
        "# unknown_guard_s: keying no program claims counts as busy this long after the\n"
        "# unkey. measured: VARA bursts pause up to 11.5 s (K7RIE).\n"
        "unknown_guard_s = 15\n"
        "# unkey_claim_s: a program that reported itself transmitting this recently still\n"
        "# owns the keyed radio. measured: fldigi reports RX about 1 s before its PTT off\n"
        "# reaches flrig.\n"
        "unkey_claim_s = 3\n"
        "# xmlrpc_timeout_s: a stalled flrig or fldigi must not stall the arbiter. chosen.\n"
        "xmlrpc_timeout_s = 1\n"
        "# reconnect_s: retry interval for a program that is not answering. chosen.\n"
        "reconnect_s = 5\n"
        "\n[varac]\n"
        "# log_path: VarAC's VARA command log (VarAC: LogVARACommands=ON). Blank = found\n"
        "# the way CAT Watch finds it (LogVARACommandsPath= in VarAC*.ini, or\n"
        "# VarAC*/VarAC_vara_commands*.log under C:\\ or each Wine prefix).\n"
        "log_path =\n"
        "# lever: cluster_lock = VarAC's own cluster PTT lock (VarAC: Cluster enabled, PTT\n"
        "# Lock ON): the arbiter is a second cluster instance named \"Arbiter\" in VarAC.db,\n"
        "# marked busy while the hold lasts. Proven on VarAC 16.1, 2026-10-29.\n"
        "# none = observe only, nothing is written.\n"
        "lever = cluster_lock\n"
        "# db_path: VarAC.db. Blank = the VarAC.db beside the command log.\n"
        "db_path =\n"
        "# varac_renew_s: how often the Arbiter row's keepalive is refreshed. measured:\n"
        "# VarAC drops a lock whose keepalive is about 30 s old.\n"
        "varac_renew_s = 10\n"
        "\n[log]\n"
        "# file: relative to this ini's folder (or to the data folder without an ini).\n"
        "file = arbiter.log\n");
}
