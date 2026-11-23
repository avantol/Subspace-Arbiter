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
    s.iniLookedAt = iniPath;
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
    s.subspaceReplyMaxS = num("timing/subspace_reply_max_s", s.subspaceReplyMaxS);
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
    s.unkeyBlockLabel = str("varac/unkey_block_label", s.unkeyBlockLabel);
    s.unkeyBlockMs = int(num("varac/unkey_block_ms", s.unkeyBlockMs));
    s.unkeyBlockRenewS = num("varac/unkey_block_renew_s", s.unkeyBlockRenewS);
    s.layerCmdPort = int(num("varac/layer_port", s.layerCmdPort));
    s.layerKissPort = int(num("varac/layer_kiss_port", s.layerKissPort));
    s.layerVaraCmdPort = int(num("varac/layer_vara_port", s.layerVaraCmdPort));
    s.layerVaraKissPort = int(num("varac/layer_vara_kiss_port", s.layerVaraKissPort));
    s.layerVaraWaitS = num("varac/layer_vara_wait_s", s.layerVaraWaitS);
    s.layerLogLines = onOff(ini.value("varac/layer_log_lines"), s.layerLogLines);
    s.askSubspace = onOff(ini.value("ask/subspace"), s.askSubspace);
    s.askFldigi = onOff(ini.value("ask/fldigi"), s.askFldigi);
    s.fldigiAskLeaseMs = int(num("ask/fldigi_lease_ms", s.fldigiAskLeaseMs));
    s.subspaceAskLeaseMs = int(num("ask/subspace_lease_ms", s.subspaceAskLeaseMs));
    s.subspaceAskRenewS = num("ask/subspace_renew_s", s.subspaceAskRenewS);
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
        "# subspace_reply_max_s: [study] the longest time Subspace keeps the radio after a\n"
        "# message to hear the reply it expects, whatever time Subspace itself names.\n"
        "# chosen: 120 s, over Subspace's longest allowance (Slow speed, 96 s).\n"
        "subspace_reply_max_s = 120\n"
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
        "# unkey_block_label: the label (HTTP User-Agent) on VarAC's requests to flrig.\n"
        "# measured 2026-11-12 on WM8Q's VarAC: XML-RPC.NET on every request. A held VarAC\n"
        "# still sends VARA's PTT OFF (bench 2026-11-11), so while VarAC is held the arbiter\n"
        "# tells flrig-subspace 8 to ignore un-keys with this label. Blank = never ask.\n"
        "unkey_block_label = XML-RPC.NET\n"
        "# unkey_block_ms: how long one such instruction lasts. chosen: flrig-subspace's cap;\n"
        "# a stopped arbiter's instruction lapses within it.\n"
        "unkey_block_ms = 15000\n"
        "# unkey_block_renew_s: how often it is renewed while VarAC is held. chosen.\n"
        "unkey_block_renew_s = 5\n"
        "# [study] lever = layer: VarAC reaches VARA through the arbiter. VarAC connects to\n"
        "# layer_port, layer_port + 1 and layer_kiss_port (VARA HF's usual 8300 / 8301 /\n"
        "# 8100, so VarAC needs no change); the arbiter connects on to VARA, which is moved\n"
        "# to layer_vara_port (+ 1) and layer_vara_kiss_port in VARA.ini. While VarAC is\n"
        "# held it is told BUSY ON and VARA is told LISTEN OFF (measured with VarAC 16.1.8\n"
        "# and VARA HF 4.9.0, 2026-11-20). No cluster lock and no un-key block are used.\n"
        "# With lever = layer VarAC reaches VARA only while the arbiter runs.\n"
        "layer_port = 8300\nlayer_kiss_port = 8100\n"
        "layer_vara_port = 8310\nlayer_vara_kiss_port = 8110\n"
        "# layer_vara_wait_s: how long VarAC's connection waits for VARA to listen. chosen;\n"
        "# VARA under Wine took 12 to 20 s (measured).\n"
        "layer_vara_wait_s = 120\n"
        "# layer_log_lines: every command line between VarAC and VARA in the log.\n"
        "layer_log_lines = on\n"
        "\n[ask]\n"
        "# [study] A Subspace or fldigi-subspace of the study branch asks the arbiter before\n"
        "# it transmits and waits, inside the program, for the grant; such a program is not\n"
        "# held with its switch. A program without it is held as before. off = never asked\n"
        "# for, held as before.\n"
        "subspace = on\nfldigi = on\n"
        "# fldigi_lease_ms: each state query (every poll_ms) keeps fldigi asking this long;\n"
        "# a stopped arbiter's asking ends within it and fldigi starts as before. chosen.\n"
        "fldigi_lease_ms = 3000\n"
        "# subspace_lease_ms, subspace_renew_s: the same for Subspace (TX.SET_ASK). chosen.\n"
        "subspace_lease_ms = 15000\nsubspace_renew_s = 5\n"
        "\n[log]\n"
        "# file: relative to this ini's folder (or to the data folder without an ini).\n"
        "file = arbiter.log\n");
}
