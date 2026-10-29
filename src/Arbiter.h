#pragma once
// The arbiter: one tick, one hold table. Whatever is already transmitting
// finishes; the arbiter holds only what has not started, by throwing each
// program's own switch, and never refuses, swallows or delays a PTT.
#include "Settings.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

class Log;
class FlrigWatch;
class FldigiLink;
class SubspaceLink;
class VaracWatch;
class VaracLever;

// Reasons shown inside the held program (Subspace status bar: "TX disabled by <reason>")
namespace Reason {
inline const QString VARA = QStringLiteral("VARA link");
inline const QString VARA_TX = QStringLiteral("VARA TX");
inline const QString FLDIGI = QStringLiteral("fldigi TX");
inline const QString SUBSPACE = QStringLiteral("Subspace TX");
inline const QString UNKNOWN = QStringLiteral("radio keyed");
inline const QStringList ORDER = {VARA, VARA_TX, FLDIGI, SUBSPACE, UNKNOWN};
}

struct Program {
    QString name;
    bool enabled = true;       // hold allowed (settings, window checkbox)
    bool connected = false;
    bool transmitting = false;
    bool held = false;
    QString reason;
    double heldT = 0, renewedT = 0, renewS = 60;
    bool overridden = false;   // operator released it during this busy period
    QString status;
    QStringList own;           // the reasons that are this program's own transmitting
};

class Arbiter : public QObject {
    Q_OBJECT
public:
    Arbiter(const Settings &s, Log &log, QObject *parent = nullptr);
    ~Arbiter() override;
    void start();
    void stop();                       // every exit path: releases every hold, removes VarAC's row
    void operatorRelease(Program &p);
    void setEnabled(Program &p, bool on);

    Program subspace, fldigi, varac;
    QList<Program *> programs() { return {&subspace, &fldigi, &varac}; }
    bool setupComplete() const;        // all three programs reachable and VarAC's lock ready
    static QString setupText();        // the one-time settings in the other programs, plain words
    const QHash<QString, bool> &sources() const { return m_sources; }
    FlrigWatch &flrig() { return *m_flrig; }
    Log &log() { return m_log; }

private:
    void tick();
    void subspaceEvents(double now);
    void subspaceStatePoll(double now);
    void apply(Program &p, bool want, const QString &reason, double now);
    bool hold(Program &p, const QString &reason);
    void renew(Program &p, const QString &reason);
    void release(Program &p);
    void releaseAll();

    Settings m_s;
    Log &m_log;
    double m_pollS;
    FlrigWatch *m_flrig;
    FldigiLink *m_fldigi;
    SubspaceLink *m_subspace;
    VaracWatch *m_varac;
    VaracLever *m_lever;
    QTimer m_tick;
    bool m_stopped = false;

    // Subspace message tracking (nothing pushed spans the gap between frames)
    bool m_ssPtt = false, m_ssMsg = false, m_ssBusy = false, m_ssHoldOwned = false;
    double m_ssLastOffT = 0, m_ssBusyPolledT = 0, m_ssDialT = 0;
    int m_ssTxEnabled = -1;
    double m_flLastTxT = 0;
    double m_unknownKeyedUntil = 0;
    bool m_busyPeriod = false;
    QHash<QString, bool> m_sources;
    qint64 m_loggedVfo = -1;
};
