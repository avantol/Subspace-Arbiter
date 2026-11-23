#pragma once
// The arbiter: one tick, one hold table. Whatever is already transmitting
// finishes; the arbiter holds only what has not started, by throwing each
// program's own switch. Where a switch holds a program only in part (VarAC's
// lock stops VARA's keys, not its un-keys), flrig-subspace carries out the
// rest on the arbiter's renewed instruction. It never refuses or delays a key.
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
class VaraLayer;

// Reasons shown inside the held program (Subspace status bar: "TX disabled by <reason>")
namespace Reason {
inline const QString VARA = QStringLiteral("VARA link");
inline const QString VARA_TX = QStringLiteral("VARA TX");
inline const QString FLDIGI = QStringLiteral("fldigi TX");
inline const QString SUBSPACE = QStringLiteral("Subspace TX");
// [study] Subspace's message is over and it waits for the reply it expects: nobody here
// transmits, the radio stays Subspace's so the reply is heard
inline const QString SUBSPACE_REPLY = QStringLiteral("Subspace reply");
inline const QString UNKNOWN = QStringLiteral("radio keyed");
inline const QStringList ORDER = {VARA, VARA_TX, FLDIGI, SUBSPACE, SUBSPACE_REPLY, UNKNOWN};
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
    bool seenUp = false;       // answered at least once since the arbiter started
    double downT = 0;          // when it stopped answering after that (0 = answering)
    bool skipLogged = false;   // "not held (not answering)" said this busy period
    QString status;
    QStringList own;           // the reasons that are this program's own transmitting
    bool leased = true;        // its hold is renewed (the window counts down to the renewal)
    bool waiting = false;      // [study] it asked to transmit and waits for its grant
    QString waitReason;        // [study] what it waits for
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
    QString setupText() const;         // the one-time settings in the other programs, plain words,
                                       // with this PC's settings files
    const QHash<QString, bool> &sources() const { return m_sources; }
    FlrigWatch &flrig() { return *m_flrig; }
    Log &log() { return m_log; }

private:
    void tick();                       // asks the programs for their state, then decide()
    // Who has the radio, who is granted, who is held. Run by every tick and, [study], at
    // once when a program reports a change (a start that waits, a transmission over, a
    // VarAC start command), so a grant does not wait for the next tick.
    void decide();
    void decideSoon();
    void subspaceEvents(double now);
    void subspaceStatePoll(double now);
    // [study] ask / grant
    bool asks(const Program &p) const;         // it asks before it transmits: not held with its switch
    QStringList othersThan(const Program &p) const;
    QStringList inTheWayOf(const Program &p) const;   // othersThan, and a radio that is still keyed
    void subspaceAsk(double now);              // TX.SET_ASK, renewed
    void considerSubspace(double now);
    void considerFldigi(double now);
    void endSubspaceGrant(const QString &why);
    void apply(Program &p, bool want, const QString &reason, double now);
    bool hold(Program &p, const QString &reason);
    void renew(Program &p, const QString &reason);
    void release(Program &p);
    void releaseAll();
    bool dialFromSubspace() const;
    void logDial();

    Settings m_s;
    Log &m_log;
    double m_pollS;
    FlrigWatch *m_flrig;
    FldigiLink *m_fldigi;
    SubspaceLink *m_subspace;
    // varac/lever = layer: m_layer, and neither the log watch nor the cluster lock;
    // any other lever: the log watch and the lever, and no layer
    VaracWatch *m_varac = nullptr;
    VaracLever *m_lever = nullptr;
    VaraLayer *m_layer = nullptr;
    QTimer m_tick;
    bool m_stopped = false;
    bool m_decideQueued = false;

    // [study] ask / grant, Subspace. TX.SET_ASK {ASK, LEASE_MS} switches Subspace's asking
    // on for one lease; Subspace then reports TX.ASK {ASK_ID, ASKING} when a message's
    // first frame waits, and keys only after TX.GRANT {ASK_ID, GRANT: true}. One message
    // is granted at a time; TX.ASK {ASKING: false} ends the ask or the granted message.
    int m_ssAskSupport = -1;        // -1 not known yet, 0 this Subspace does not answer TX.SET_ASK, 1 yes
    bool m_ssAskOn = false;         // TX.SET_ASK {ASK: true} was sent and not taken back
    double m_ssAskSentT = 0;
    qint64 m_ssAskId = 0;           // the message that waits for a grant (0 = none)
    double m_ssAskT = 0;            // when it asked
    QString m_ssAskTold;            // the reason last told to the waiting Subspace
    qint64 m_ssGrantId = 0;         // the granted message (0 = none)
    double m_ssGrantT = 0;
    bool m_ssGrantKeyed = false;    // its first PTT on was seen
    // [study] the reply wait. Subspace's end report carries REPLY_MS: how long it waits
    // for the reply its message expects (0: none). The radio stays Subspace's until then;
    // TX.REPLY {ASK_ID, REPLY_MS} from Subspace sets a new time (a reply is coming in) or
    // ends the wait (0: the reply is complete). An end report that carries REPLY_MS also
    // says no frame follows, so the frame gap is not waited out after its PTT off.
    qint64 m_ssReplyId = 0;         // the message whose reply is waited for (0 = none)
    double m_ssReplyUntil = 0;
    bool m_ssNoGap = false;         // the message's end was reported: free at its PTT off
    void endSubspaceReply(const QString &why);
    // [study] ask / grant, fldigi (FldigiLink carries the protocol)
    int m_flWaitSerial = 0;         // the start that waits for a grant (0 = none)
    double m_flWaitT = 0;
    int m_flGrantSerial = 0;        // the granted start whose transmission has not ended yet (0 = none)
    double m_flGrantT = 0;
    bool m_flGrantOnAir = false;    // fldigi reported TX during this grant
    double m_flGrantOnAirT = 0;     // when it first did
    double m_flGrantWaitS = 0;      // from its ask to its grant
    double m_ssGrantKeyT = 0;       // Subspace's first key-up under its grant
    double m_ssGrantWaitS = 0;      // from Subspace's ask to its grant
    QString m_flWaitTold;

    // Subspace message tracking (nothing pushed spans the gap between frames)
    bool m_ssPtt = false, m_ssMsg = false, m_ssBusy = false, m_ssHoldOwned = false;
    bool m_ssBusyTx = false;        // m_ssBusy stands for the polled "transmitting", not for ARQ
    double m_ssLastOffT = 0, m_ssBusyPolledT = 0, m_ssDialT = 0;
    // TX.PENDING: a Subspace frame waits for its key-up, until this time (0 = none)
    double m_ssPendingUntil = 0;
    int m_ssTxEnabled = -1;
    double m_flLastTxT = 0;
    double m_unknownKeyedUntil = 0;
    bool m_busyPeriod = false;
    QHash<QString, bool> m_sources;
    qint64 m_seenVfo = -1;
    // a dial change waiting for its line (-1 = none): seen at m_dialT, during a VARA link or not
    qint64 m_dialVfo = -1;
    double m_dialT = 0;
    bool m_dialLink = false;
};
