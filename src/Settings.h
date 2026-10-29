#pragma once
// Every number the arbiter uses, with its basis stated in defaultIniText().
// The defaults ARE the configuration: a station needs no ini file. One is
// read only if it exists (or --ini names it), and only the keys present
// override the defaults. Same sections and keys as the Python arbiter.ini.
#include <QString>

struct Settings {
    // [ports]
    QString flrigHost = "127.0.0.1";
    int flrigPort = 12345;
    QString subspaceHost = "127.0.0.1";
    int subspacePort = 2442;
    QString fldigiHost = "127.0.0.1";
    int fldigiPort = 7362;
    // [holds]
    bool holdSubspace = true;
    bool holdFldigi = true;
    bool holdVarac = true;
    // [timing]
    int pollMs = 200;              // chosen: flrig answers get_ptt from cache; one poll = reaction time
    double statePollS = 2;         // chosen: STATION.GET_BUSY backstop
    double renewS = 60;            // chosen: well under Subspace's 300 s lease
    double subspaceFrameGapS = 2.5;// measured: PTT drops 1.2-1.6 s between frames, 1.4 s between queued messages
    double varacDeadS = 90;        // chosen: VarAC refreshes its keepalive every 10-20 s (measured)
    double varaStaleS = 600;       // chosen: fallback when VarAC.db is unreadable
    double varaPttMatchS = 2;      // measured: VarAC's log stamps are whole seconds
    double varaBurstHoldS = 12;    // measured: VARA pauses up to 11.5 s inside a link, 3 s between connect requests
    double unknownGuardS = 15;     // measured: VARA bursts pause up to 11.5 s (K7RIE)
    double unkeyClaimS = 3;        // measured: fldigi reports RX ~1 s before its PTT off reaches flrig
    double xmlrpcTimeoutS = 1;     // chosen
    double reconnectS = 5;         // chosen
    // [varac]
    QString varacLogPath;          // blank = found the way CAT Watch finds it
    QString varacLever = "cluster_lock";  // or none = observe only
    QString varacDbPath;           // blank = VarAC.db beside the command log
    double varacRenewS = 10;       // measured: VarAC drops a lock 30 s after its last keepalive
    // [log]
    QString logFile = "arbiter.log";

    QString iniPath;               // the file read, if any (for the log line)

    // Reads the ini over the defaults. A missing file is fine.
    static Settings load(const QString &iniPath);
    // The documented template, the same text the Python version wrote.
    static QString defaultIniText();
};
