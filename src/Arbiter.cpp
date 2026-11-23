#include "Arbiter.h"
#include "FldigiLink.h"
#include "FlrigWatch.h"
#include "Log.h"
#include "SubspaceLink.h"
#include "VaraLayer.h"
#include "VaracLever.h"
#include "VaracWatch.h"
#include "config.h"

#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QJsonObject>

static double nowS() { return QDateTime::currentMSecsSinceEpoch() / 1000.0; }

// Subspace's RIG.FREQ and flrig's dial reading (once a second) arrive in either order; a dial
// change is Subspace's when its RIG.FREQ came within this many seconds either side. chosen.
static constexpr double kDialMatchS = 2.0;

static QString clockText(double t) {
    return QDateTime::fromMSecsSinceEpoch(qint64(t * 1000)).toString("HH:mm:ss");
}

Arbiter::Arbiter(const Settings &s, Log &log, QObject *parent)
    : QObject(parent), m_s(s), m_log(log), m_pollS(s.pollMs / 1000.0) {
    m_flrig = new FlrigWatch(s, log, this);
    m_fldigi = new FldigiLink(s, log, this);
    m_subspace = new SubspaceLink(s, log, this);
    if (s.varacLever == "layer") {
        m_layer = new VaraLayer(s, log, this);
        connect(m_layer, &VaraLayer::changed, this, &Arbiter::decideSoon);
    } else {
        m_varac = new VaracWatch(s, log, this);
        m_lever = new VaracLever(s, log, this);
    }
    connect(m_fldigi, &FldigiLink::changed, this, &Arbiter::decideSoon);
    connect(m_subspace, &SubspaceLink::eventQueued, this, &Arbiter::decideSoon);

    subspace.name = "Subspace"; subspace.enabled = s.holdSubspace; subspace.renewS = s.renewS;
    subspace.own = {Reason::SUBSPACE, Reason::SUBSPACE_REPLY};
    fldigi.name = "fldigi"; fldigi.enabled = s.holdFldigi; fldigi.renewS = s.renewS;
    fldigi.own = {Reason::FLDIGI};
    fldigi.leased = false;        // rx_only has no lease
    varac.name = "VarAC"; varac.enabled = s.holdVarac; varac.renewS = s.varacRenewS;
    varac.own = {Reason::VARA, Reason::VARA_TX};
    varac.leased = !m_layer;      // the layer's hold ends with the layer, not with a lease
    for (const QString &r : Reason::ORDER) m_sources[r] = false;
    m_fldigi->setAsking(s.askFldigi && fldigi.enabled);

    m_tick.setInterval(s.pollMs);
    connect(&m_tick, &QTimer::timeout, this, &Arbiter::tick);
}

Arbiter::~Arbiter() { stop(); }

void Arbiter::start() {
    m_log(QString("arbiter %1 started; holds: Subspace %2, fldigi %3, VarAC %4 (lever: %5)%6")
              .arg(ARBITER_VERSION, subspace.enabled ? "on" : "off", fldigi.enabled ? "on" : "off",
                   varac.enabled ? "on" : "off", m_layer ? QString("layer") : m_lever->kind(),
                   m_s.iniPath.isEmpty() ? QString() : "; settings from " + m_s.iniPath));
    m_log(QString("study build: ask / grant: Subspace %1, fldigi %2")
              .arg(m_s.askSubspace ? "on" : "off", m_s.askFldigi ? "on" : "off"));
    m_tick.start();
    QTimer::singleShot(0, this, &Arbiter::tick);
}

void Arbiter::stop() {
    if (m_stopped) return;
    m_stopped = true;
    m_tick.stop();
    if (m_dialVfo >= 0) logDial();
    releaseAll();
    // [study] Subspace stops asking now instead of when its lease lapses; fldigi's lease
    // (ask/fldigi_lease_ms) lapses by itself
    if (m_ssAskOn && m_subspace->up()) m_subspace->request("TX.SET_ASK", {{"ASK", false}}, nullptr);
    m_ssAskOn = false;
    if (m_layer) m_layer->close();   // VarAC's and VARA's connections end with the arbiter
    else m_lever->remove();
    m_flrig->setUnkeyBlock(false);
    // the releases are network sends: let them go out before the sockets close
    // (kExitFlushMs = 600, chosen: fldigi and Subspace answer in well under that).
    // The un-key block's end is sent as soon as flrig's link is free; unsent, the
    // block lapses by itself within unkey_block_ms.
    QEventLoop flush;
    QTimer pump;
    connect(&pump, &QTimer::timeout, this, [this]() { m_flrig->sendBlockIfDue(); });
    pump.start(50);
    m_flrig->sendBlockIfDue();
    QTimer::singleShot(600, &flush, &QEventLoop::quit);
    flush.exec();
    m_subspace->close();
    if (m_varac) m_varac->close();
}

// -- Subspace events ------------------------------------------------------------
void Arbiter::subspaceEvents(double now) {
    for (const QJsonObject &msg : m_subspace->takeEvents()) {
        QString t = msg.value("type").toString();
        QJsonObject p = msg.value("params").toObject();
        if (t == "RIG.PTT") {
            bool on = p.contains("PTT") ? p.value("PTT").toBool()
                                        : msg.value("value").toString().toLower() == "on";
            if (on) { m_ssPtt = true; m_ssMsg = true; m_ssNoGap = false; if (m_ssGrantId != 0 && !m_ssGrantKeyed) { m_ssGrantKeyed = true; m_ssGrantKeyT = now; } }
            // a PTT off that follows no PTT on ends no frame: a message halted before its
            // key-up sends one (station 2026-11-20 19:14:56: VarAC held again for 2.6 s)
            else {
                if (m_ssPtt) m_ssLastOffT = now;
                m_ssPtt = false;
                if (m_ssNoGap) m_ssMsg = false;   // its end was reported: no frame follows
                // the polled "transmitting" is older than this event (bench 2026-11-21:
                // it stood up to 1 s after the PTT off); the ARQ state is asked for anew
                if (m_ssBusyTx) m_ssBusy = m_ssBusyTx = false;
                m_ssBusyPolledT = 0;
            }
        } else if (t == "TX.ASK") {
            // [study] ASKING true: a message's first frame waits for a grant. false: that
            // ask is given up, or the granted message is over.
            qint64 id = qint64(p.value("ASK_ID").toDouble());
            if (p.value("ASKING").toBool()) {
                if (id != 0 && id != m_ssAskId && id != m_ssGrantId) {
                    m_ssAskId = id;
                    m_ssAskT = now;
                    m_ssAskTold.clear();
                    m_log(QString("Subspace: asks to transmit (message %1, key-up in %2 ms)")
                              .arg(id).arg(qint64(p.value("KEYUP_IN_MS").toDouble())));
                }
            } else if (id == m_ssAskId && id != 0) {
                m_ssAskId = 0;
                m_ssAskTold.clear();
                m_log(QString("Subspace: no longer asks (message %1)").arg(id));
                m_log(QString("TX: Subspace message %1: NOT SENT (given up %2 s after it asked)")
                          .arg(id).arg(now - m_ssAskT, 0, 'f', 1));
            } else if (id == m_ssGrantId && id != 0) {
                m_log(QString("Subspace: transmission over (message %1, %2 s after the grant)")
                          .arg(id).arg(now - m_ssGrantT, 0, 'f', 1));
                m_log(m_ssGrantKeyed
                          ? QString("TX: Subspace message %1: SENT, completed (cleared to send %2 s after it asked; key-up to end %3 s)")
                                .arg(id).arg(m_ssGrantWaitS, 0, 'f', 1).arg(now - m_ssGrantKeyT, 0, 'f', 1)
                          : QString("TX: Subspace message %1: NOT SENT (cleared to send, but ended without a key-up)").arg(id));
                m_ssGrantId = 0;
                m_ssGrantKeyed = false;
                if (p.contains("REPLY_MS")) {
                    m_ssNoGap = true;
                    if (!m_ssPtt) m_ssMsg = false;
                    double replyS = qMin(p.value("REPLY_MS").toDouble() / 1000.0, m_s.subspaceReplyMaxS);
                    if (replyS > 0) {
                        m_ssReplyId = id;
                        m_ssReplyUntil = now + replyS;
                        m_log(QString("Subspace: keeps the radio for the reply it expects (message %1, up to %2 s)")
                                  .arg(id).arg(replyS, 0, 'f', 1));
                    }
                }
            }
        } else if (t == "TX.REPLY") {
            // [study] the reply wait of the message just sent: a new time, or 0 = over
            qint64 id = qint64(p.value("ASK_ID").toDouble());
            if (id != 0 && id == m_ssReplyId) {
                double replyS = qMin(p.value("REPLY_MS").toDouble() / 1000.0, m_s.subspaceReplyMaxS);
                if (replyS > 0) {
                    m_ssReplyUntil = now + replyS;
                    m_log(QString("Subspace: a reply is coming in; the wait goes on (up to %1 s more)").arg(replyS, 0, 'f', 1));
                } else {
                    endSubspaceReply("Subspace has its reply");
                }
            }
        } else if (t == "TX.COMPLETE") {
            m_ssMsg = false;
        } else if (t == "TX.PENDING") {
            // a frame waits for its key-up, KEYUP_IN_MS from now (Subspace's clock is not
            // ours, so it is relative); lapses subspace_frame_gap_s after it if no PTT came
            m_ssPendingUntil = p.value("PENDING").toBool()
                ? now + p.value("KEYUP_IN_MS").toDouble() / 1000.0 + m_s.subspaceFrameGapS
                : 0;
        } else if (t == "TX.ENABLED") {
            bool en = p.value("ENABLED").toBool(true);
            m_ssTxEnabled = en ? 1 : 0;
            if (en && p.value("OVERRIDE").toBool()) {
                subspace.overridden = true;
                subspace.held = false;
                m_ssHoldOwned = false;
                m_log(QString("Subspace: operator re-enabled TX over the hold (%1); not held again this busy period")
                          .arg(subspace.reason));
            } else if (en && p.value("EXPIRED").toBool()) {
                subspace.held = false;
                m_ssHoldOwned = false;
                m_log("Subspace: hold lease expired inside Subspace (renew missed)");
            } else if (en && subspace.held && !p.value("REMOTE").toBool()) {
                subspace.held = false;   // released by something else (another API client)
                m_ssHoldOwned = false;
            }
        } else if (t == "RIG.FREQ") {
            m_ssDialT = now;
        } else if (t == "STATION.CLOSING") {
            subspace.held = false;
            m_ssHoldOwned = false;
            m_ssPendingUntil = 0;
            m_ssAskId = 0;
            if (m_ssGrantId != 0) endSubspaceGrant("Subspace is closing");
            if (m_ssReplyId != 0) endSubspaceReply("Subspace is closing");
            m_log("Subspace: closing");
        }
    }
}

// -- [study] ask / grant ----------------------------------------------------------
bool Arbiter::asks(const Program &p) const {
    // Subspace not known yet (the first TX.SET_ASK is on its way, 2 s at most): not held
    // with its TX switch meanwhile; that switch throws a waiting message away
    if (&p == &subspace) return m_s.askSubspace && m_ssAskSupport != 0;
    if (&p == &fldigi) return m_fldigi->asks();
    return false;   // VarAC never asks: the layer holds it
}

QStringList Arbiter::othersThan(const Program &p) const {
    QStringList others;
    for (const QString &r : Reason::ORDER)
        if (m_sources.value(r) && !p.own.contains(r)) others << r;
    return others;
}

QStringList Arbiter::inTheWayOf(const Program &p) const {
    // what a grant waits for: the other programs, and the radio itself. fldigi reports RX
    // about 1 s before its PTT off reaches flrig (measured, see unkey_claim_s): a program
    // granted in that second would key a radio that the late PTT off then un-keys.
    QStringList others = othersThan(p);
    if (others.isEmpty() && m_flrig->ptt == 1) others << Reason::UNKNOWN;
    return others;
}

void Arbiter::subspaceAsk(double now) {
    bool want = m_s.askSubspace && subspace.enabled;
    if (!want) {
        if (m_ssAskOn) {
            m_subspace->request("TX.SET_ASK", {{"ASK", false}}, nullptr);
            m_log("Subspace: no longer asked to ask (its hold is switched off)");
        }
        m_ssAskOn = false;
        m_ssAskSentT = 0;
        if (m_ssAskId != 0)
            m_log(QString("TX: Subspace message %1: NOT KNOWN (asking was switched off while it waited)").arg(m_ssAskId));
        m_ssAskId = 0;
        return;
    }
    // sent again every subspace_renew_s, also to a Subspace that did not answer: one
    // missed answer must not leave a Subspace that can ask held with its TX switch
    if (m_ssAskSentT > 0 && now - m_ssAskSentT < m_s.subspaceAskRenewS) return;
    m_ssAskSentT = now;
    m_ssAskOn = true;
    m_subspace->request("TX.SET_ASK", {{"ASK", true}, {"LEASE_MS", m_s.subspaceAskLeaseMs}}, [this](const QJsonObject &r) {
        if (!m_subspace->up()) return;   // the link dropped: nothing is known
        if (r.isEmpty()) {
            // a Subspace without ask / grant gives no answer to a request it does not know.
            // One that answered before keeps its 1: its own lease ends its asking.
            if (m_ssAskSupport == -1) {
                m_ssAskSupport = 0;
                m_log("Subspace: this Subspace has no ask / grant (TX.SET_ASK not answered): held with its TX switch as before");
            }
            return;
        }
        if (m_ssAskSupport != 1) m_log("Subspace: asks before it transmits; not held with its TX switch");
        m_ssAskSupport = 1;
    });
}

void Arbiter::endSubspaceGrant(const QString &why) {
    m_log(QString("Subspace: grant over (%1)").arg(why));
    m_log(QString("TX: Subspace message %1: %2 (%3)").arg(m_ssGrantId)
              .arg(m_ssGrantKeyed ? "SENT, its end not reported" : "NOT SENT", why));
    m_ssGrantId = 0;
    m_ssGrantKeyed = false;
}

void Arbiter::endSubspaceReply(const QString &why) {
    m_log(QString("Subspace: reply wait over (%1)").arg(why));
    m_ssReplyId = 0;
    m_ssReplyUntil = 0;
}

void Arbiter::considerSubspace(double now) {
    if (m_ssAskId == 0) return;
    QStringList const others = inTheWayOf(subspace);
    if (!others.isEmpty() && !subspace.overridden) {
        subspace.waiting = true;
        subspace.waitReason = others.first();
        if (others.first() != m_ssAskTold) {   // Subspace shows what it waits for
            m_ssAskTold = others.first();
            m_subspace->request("TX.GRANT", {{"ASK_ID", double(m_ssAskId)}, {"GRANT", false}, {"REASON", m_ssAskTold}}, nullptr);
            m_log(QString("Subspace: waits (%1)").arg(m_ssAskTold));
        }
        return;
    }
    qint64 const id = m_ssAskId;
    m_ssAskId = 0;
    m_ssAskTold.clear();
    if (m_ssReplyId != 0) endSubspaceReply("Subspace transmits again");
    m_ssGrantId = id;
    m_ssGrantT = now;
    m_ssGrantWaitS = now - m_ssAskT;
    m_ssGrantKeyed = false;
    m_subspace->request("TX.GRANT", {{"ASK_ID", double(id)}, {"GRANT", true}}, [this, id](const QJsonObject &r) {
        // no answer: the rule in decide() ends a grant that is followed by no key-up
        if (m_ssGrantId != id || r.isEmpty()) return;
        if (!r.value("params").toObject().value("OK").toBool()) endSubspaceGrant("Subspace no longer waits for it");
    });
    m_log(QString("Subspace: granted (message %1, %2 s after it asked)").arg(id).arg(now - m_ssAskT, 0, 'f', 1));
    // it has the radio from this instant: whatever is decided next in this pass sees it
    m_sources[Reason::SUBSPACE] = true;
    subspace.transmitting = true;
}

void Arbiter::considerFldigi(double now) {
    if (m_flWaitSerial == 0) return;
    QStringList const others = inTheWayOf(fldigi);
    if (!others.isEmpty() && !fldigi.overridden) {
        fldigi.waiting = true;
        fldigi.waitReason = others.first();
        if (others.first() != m_flWaitTold) {
            m_flWaitTold = others.first();
            m_log(QString("fldigi: waits (%1)").arg(m_flWaitTold));
        }
        return;
    }
    m_fldigi->grant(m_flWaitSerial);
    m_log(QString("fldigi: granted (start %1, %2 s after it asked)").arg(m_flWaitSerial).arg(now - m_flWaitT, 0, 'f', 1));
    m_flGrantSerial = m_flWaitSerial;
    m_flGrantT = now;
    m_flGrantWaitS = now - m_flWaitT;
    m_flWaitSerial = 0;
    m_flWaitTold.clear();
    m_flLastTxT = now;
    m_sources[Reason::FLDIGI] = true;
    fldigi.transmitting = true;
}

void Arbiter::subspaceStatePoll(double now) {
    if (now - m_ssBusyPolledT < m_s.statePollS) return;
    m_ssBusyPolledT = now;
    m_subspace->request("STATION.GET_BUSY", {}, [this, now](const QJsonObject &r) {
        if (r.isEmpty()) return;
        QJsonObject p = r.value("params").toObject();
        QString reason = p.value("BUSY_REASON").toString();
        bool busy = p.value("BUSY").toBool();
        // "transmitting" and PTT are also pushed (RIG.PTT), at the instant: an answer
        // asked for before the last pushed PTT off is older than that event
        bool fresh = now > m_ssLastOffT;
        m_ssBusyTx = fresh && busy && reason == "transmitting";
        m_ssBusy = m_ssBusyTx || (busy && (reason == "arq_tx" || reason == "arq_negotiating"));
        if (fresh && p.value("PTT").toBool()) { m_ssPtt = true; m_ssMsg = true; }
    }, 1000);
}

// -- one tick -----------------------------------------------------------------------
void Arbiter::tick() {
    double now = nowS();
    m_flrig->poll();
    m_fldigi->poll();
    if (m_subspace->up()) {
        subspaceStatePoll(now);
        subspaceAsk(now);
    }
    decide();
}

void Arbiter::decideSoon() {
    // queued: the report that asks for it is handled in full first, and several reports
    // in one pass of the event loop give one decide()
    if (m_decideQueued || m_stopped) return;
    m_decideQueued = true;
    QMetaObject::invokeMethod(this, [this]() {
        m_decideQueued = false;
        if (!m_stopped) decide();
    }, Qt::QueuedConnection);
}

void Arbiter::decide() {
    double now = nowS();
    subspaceEvents(now);
    if (!m_subspace->up()) {
        // [study] a program that stopped has nothing: its ask, its grant and its
        // transmitting end with its connection, and the next Subspace is asked anew
        if (m_ssGrantId != 0) endSubspaceGrant("Subspace is not connected");
        if (m_ssReplyId != 0) endSubspaceReply("Subspace is not connected");
        if (m_ssPtt || m_ssMsg || m_ssBusy) m_log("Subspace: not connected; no longer counted as transmitting");
        m_ssPtt = m_ssMsg = m_ssBusy = m_ssBusyTx = false;
        m_ssPendingUntil = 0;
        m_ssAskSupport = -1;
        m_ssAskOn = false;
        m_ssAskSentT = 0;
        if (m_ssAskId != 0) m_log(QString("TX: Subspace message %1: NOT SENT (Subspace is not connected)").arg(m_ssAskId));
        m_ssAskId = 0;
        m_ssAskTold.clear();
    }
    VaracWatch::Snapshot va = m_layer ? m_layer->snapshot() : m_varac->snapshot();
    if (m_layer && varac.held && !m_layer->held()) {
        // the layer's hold ends when VarAC closes its command connection
        varac.held = false;
        m_log("VarAC: hold over (" + varac.reason + "): VarAC closed its connection");
        varac.reason.clear();
    }

    // who is transmitting
    if (m_ssMsg && !m_ssPtt && now - m_ssLastOffT > m_s.subspaceFrameGapS)
        m_ssMsg = false;   // no next frame came: the message ended without TX.COMPLETE (halted)
    // [study] a granted Subspace message has the radio until Subspace reports its end
    // (TX.ASK, ASKING false). Two rules end a grant whose end report did not come:
    // no key-up and no frame waiting for its key-up (TX.PENDING) subspace_frame_gap_s
    // after the grant; or, after its key-up, no frame and none waiting for that long.
    if (m_ssGrantId != 0) {
        bool frameWaits = m_ssPendingUntil > 0 && now < m_ssPendingUntil;
        if (!m_ssGrantKeyed && !frameWaits && now - m_ssGrantT > m_s.subspaceFrameGapS)
            endSubspaceGrant("no key-up followed it");
        else if (m_ssGrantKeyed && !m_ssPtt && !m_ssMsg && !frameWaits && now - m_ssLastOffT > m_s.subspaceFrameGapS)
            endSubspaceGrant("the message ended without its end report");
    }
    // still transmitting for subspace_frame_gap_s after the last PTT off: covers the gap between
    // frames and the 1.4 s between two queued messages (measured 2026-10-29)
    // [study] not after a message whose end Subspace reported: no frame follows that one
    bool ssTx = m_ssPtt || m_ssMsg || m_ssBusy || (!m_ssNoGap && now - m_ssLastOffT < m_s.subspaceFrameGapS) ||
                m_ssGrantId != 0;
    if (m_ssReplyId != 0 && now >= m_ssReplyUntil) endSubspaceReply("no reply came in the time allowed");
    // a frame waiting for its key-up (TX.PENDING) holds the others before Subspace keys
    // (W8UFO 10 Nov: VarAC keyed 185 ms before Subspace's slot; VarAC takes ~1 s to obey
    // its lock), but does not shield Subspace itself: a program already on the air (a VARA
    // link that opens during the wait) still holds it, as before. A held Subspace sends
    // nothing, so its waiting frame claims nothing.
    bool ssPending = m_ssPendingUntil > 0 && now < m_ssPendingUntil && !subspace.held;
    bool flTx = m_fldigi->transmitting();
    if (flTx) m_flLastTxT = now;
    // [study] the end of a granted fldigi transmission, in the log as Subspace's is (CAT Watch
    // matches a key-up to its grant by these two lines). Also the end of a grant fldigi did
    // not take, or one cut short because fldigi stopped answering.
    if (m_flGrantSerial != 0 && !m_flGrantOnAir && m_fldigi->state() == "TX") { m_flGrantOnAir = true; m_flGrantOnAirT = now; }
    if (m_flGrantSerial != 0 && !flTx) {
        m_log(QString("fldigi: transmission over (start %1, %2 s after the grant)")
                  .arg(m_flGrantSerial).arg(now - m_flGrantT, 0, 'f', 1));
        m_log(m_flGrantOnAir
                  ? QString("TX: fldigi start %1: SENT, completed (cleared to send %2 s after it asked; key-up to end %3 s)")
                        .arg(m_flGrantSerial).arg(m_flGrantWaitS, 0, 'f', 1).arg(now - m_flGrantOnAirT, 0, 'f', 1)
                  : QString("TX: fldigi start %1: NOT SENT (cleared to send, but fldigi never transmitted)").arg(m_flGrantSerial));
        m_flGrantSerial = 0;
        m_flGrantOnAir = false;
    }
    // VARA transmitting: PTT on now (a PTT ON without OFF counts for a minute), or a burst or
    // connect request within vara_burst_hold_s, so the gaps between bursts keep the hold.
    // [unkey-block] Not while VarAC is held: a held VarAC puts nothing on the air (VarAC:
    // "Ignoring PTT request by the modem"). Counted, VARA's refused attempts held the others
    // for nothing and made VarAC "transmitting", so its hold was neither renewed nor released
    // until VarAC's own 30 s timeout (W8UFO 10 Nov 19:48:45 to 19:49:15).
    // [study] With the layer every PTT line counts: a held VarAC's VARA is told LISTEN OFF
    // and VarAC BUSY ON, so a PTT ON while held is a real transmission.
    bool vaBurst = (m_layer || !varac.held) &&
                   ((va.ptt && now - va.pttT < 60) || (now - va.burstT < m_s.varaBurstHoldS));
    bool vaTx = va.link || vaBurst;
    bool keyed = m_flrig->ptt == 1;
    // who owns the keyed radio: a program transmitting now, or one that stopped within
    // unkey_claim_s (its PTT off is still on its way), or VARA's PTT line, or a PTT change so
    // fresh that the owner's own report has not arrived yet
    bool claimed = m_ssPtt || now - m_ssLastOffT < m_s.unkeyClaimS ||
                   flTx || now - m_flLastTxT < m_s.unkeyClaimS ||
                   (va.ptt && qAbs(now - va.pttT) < 60) || qAbs(now - va.pttT) < m_s.unkeyClaimS ||
                   (now - m_flrig->pttChangedT) < m_s.varaPttMatchS;
    if (keyed && !claimed) {
        if (m_unknownKeyedUntil == 0) m_log("flrig: radio keyed, program unknown");
        m_unknownKeyedUntil = now + m_s.unknownGuardS;
    } else if (!keyed && m_unknownKeyedUntil > 0 && now > m_unknownKeyedUntil) {
        m_unknownKeyedUntil = 0;
    }
    bool unknown = m_unknownKeyedUntil > 0;

    m_sources[Reason::VARA] = va.link;
    m_sources[Reason::VARA_TX] = vaBurst && !va.link;
    m_sources[Reason::FLDIGI] = flTx;
    m_sources[Reason::SUBSPACE] = ssTx || ssPending;
    m_sources[Reason::SUBSPACE_REPLY] = m_ssReplyId != 0;
    m_sources[Reason::UNKNOWN] = unknown;

    subspace.connected = m_subspace->up();
    fldigi.connected = m_fldigi->up();
    varac.connected = m_layer ? m_layer->connected() : !m_varac->path().isEmpty();
    for (Program *p : programs()) {
        if (p->connected) { p->seenUp = true; p->downT = 0; }
        else if (p->seenUp && p->downT == 0) p->downT = now;
        p->waiting = false;
        p->waitReason.clear();
    }
    if (m_lever) {
        if (varac.connected) m_lever->setLogPath(m_varac->path());
        m_lever->available();   // refreshes its status text
    }
    subspace.transmitting = ssTx;
    fldigi.transmitting = flTx;
    varac.transmitting = vaTx;

    // [study] grants. A program that asks gets the radio when no other program has it
    // (or the operator released it in this window), one at a time, the one that asked
    // first before the other (chosen). VarAC does not ask: a start command it sends
    // while the radio is free is already in m_sources here (the layer reports it at once).
    int flWait = m_fldigi->asks() ? m_fldigi->waiting() : 0;
    if (flWait != m_flWaitSerial) {
        // a start that ends while it still waits (given up in fldigi, or fldigi gone): a
        // granted start has left m_flWaitSerial already, so this one never got its grant
        if (m_flWaitSerial != 0) {
            m_log(QString("fldigi: no longer asks (start %1)").arg(m_flWaitSerial));
            m_log(QString("TX: fldigi start %1: NOT SENT (given up %2 s after it asked)")
                      .arg(m_flWaitSerial).arg(now - m_flWaitT, 0, 'f', 1));
        }
        m_flWaitSerial = flWait;
        m_flWaitT = now;
        m_flWaitTold.clear();
        if (flWait != 0) m_log(QString("fldigi: asks to transmit (start %1)").arg(flWait));
    }
    if (m_ssAskId != 0 && (m_flWaitSerial == 0 || m_ssAskT <= m_flWaitT)) {
        considerSubspace(now);
        considerFldigi(now);
    } else {
        considerFldigi(now);
        considerSubspace(now);
    }

    bool busy = false;
    for (const QString &r : Reason::ORDER) busy = busy || m_sources[r];
    if (busy && !m_busyPeriod) {
        m_busyPeriod = true;
    } else if (!busy && m_busyPeriod) {
        m_busyPeriod = false;
        for (Program *p : programs()) {
            if (p->overridden) m_log(p->name + ": busy period over, override forgotten");
            p->overridden = false;
            p->skipLogged = false;
        }
    }

    for (Program *p : programs()) {
        QStringList const others = othersThan(*p);
        // [study] a program that asks waits inside itself for its grant: its switch is
        // not thrown (a hold standing from before it asked is released here)
        bool want = !others.isEmpty() && !p->transmitting && !p->overridden && p->enabled && !asks(*p);
        apply(*p, want, others.isEmpty() ? QString() : others.first(), now);
    }
    // [unkey-block] VarAC's lock holds its keys but not VARA's un-keys: while VarAC is held,
    // flrig-subspace ignores un-keys with VarAC's label (renewed; it lapses by itself).
    // [study] Not with the layer: BUSY ON and LISTEN OFF keep VARA from keying at all.
    m_flrig->setUnkeyBlock(!m_layer && varac.held);

    // the dial is never arbitrated, only logged, once per change. Subspace's RIG.FREQ may come
    // after the dial reading (W1NEM 12 Nov 21:00:29: read first, logged "program unknown"),
    // so without one yet the line waits up to kDialMatchS for it.
    if (m_flrig->vfo >= 0 && m_flrig->vfo != m_seenVfo) {
        if (m_dialVfo >= 0) logDial();   // an earlier change still waiting: decided now
        if (m_seenVfo >= 0) { m_dialVfo = m_flrig->vfo; m_dialT = now; m_dialLink = va.link; }
        m_seenVfo = m_flrig->vfo;
    }
    if (m_dialVfo >= 0 && (dialFromSubspace() || now - m_dialT >= kDialMatchS)) logDial();

    // the status texts carry the remedy, so a fresh station needs no manual; a program that
    // answered before and stopped (W1NEM 12 Nov 18:29: fldigi frozen) needs no setup, so it
    // gets the time it stopped instead
    subspace.status = m_subspace->status();
    if (!m_subspace->up() && !subspace.status.startsWith("refused"))
        subspace.status += subspace.seenUp
            ? "  since " + clockText(subspace.downT) + " (it was connected before: Subspace closed or stopped answering)"
            : "  (Subspace: Settings > Reporting: TCP API enabled, Accept TCP requests)";
    else if (m_s.askSubspace && subspace.enabled && m_ssAskSupport == 1)
        subspace.status += "; asks before it transmits";
    else if (m_s.askSubspace && subspace.enabled && m_ssAskSupport == 0)
        subspace.status += "; no ask / grant in this Subspace: held with its TX switch";
    fldigi.status = m_fldigi->up() ? "connected"
        : fldigi.seenUp ? "not answering since " + clockText(fldigi.downT) + "  (it answered before: fldigi may be frozen)"
                        : "not answering  (fldigi: Configure > Misc > TCP-IP sessions > XML: address 127.0.0.1, port 7362)";
    if (m_fldigi->up() && m_fldigi->asks()) fldigi.status += "; asks before it transmits";
    else if (m_fldigi->up() && m_fldigi->askWanted() && m_fldigi->askSupport() == 0)
        fldigi.status += "; no ask / grant in this fldigi: held with receive-only";
    if (m_layer) {
        varac.status = m_layer->status();
    } else {
        QString lever = m_lever->status();
        if (lever.startsWith("cluster lock: VarAC.db not found"))
            lever += "  (VarAC: Settings > Cluster enabled, PTT Lock ON, restart VarAC)";
        varac.status = m_varac->status() + "; " + lever;
        if (varac.held && m_flrig->blockStanding()) varac.status += "; un-keys blocked at flrig";
        else if (m_flrig->blockSupport() == 0) varac.status += "; un-key block not available with this flrig";
    }
}

bool Arbiter::setupComplete() const {
    if (m_layer) return m_subspace->up() && m_fldigi->up() && m_layer->connected();
    return m_subspace->up() && m_fldigi->up() && !m_varac->path().isEmpty() &&
           m_lever->status() == "cluster lock ready";
}

QString Arbiter::setupText() const {
    // this PC's own files, named for the OS it runs on
#ifdef Q_OS_WIN
    const QString exe = "Arbiter.exe";
    const QString varacUsual = "C:\\VarAC\\VarAC.ini";
#else
    const QString exe = "arbiter";
    const QString varacUsual = "<Wine prefix>/drive_c/VarAC/VarAC.ini";
#endif
    QString varacFiles;
    QString const logDir = !m_varac || m_varac->path().isEmpty() ? QString()
                                                                 : QFileInfo(m_varac->path()).absolutePath();
    for (const QString &f : VaracWatch::settingsFiles(logDir))
        varacFiles += "    " + QDir::toNativeSeparators(f) + "\n";
    if (varacFiles.isEmpty())
        varacFiles = "    none found; usually " + varacUsual + "\n"
                     "    (VarAC's folder, beside VarAC.exe)\n";
    QString const arbIni = QDir::toNativeSeparators(m_s.iniLookedAt);
    QString const arbLines = QFileInfo::exists(m_s.iniLookedAt)
        ? "    " + arbIni + "  (in use)\n"
        : "    " + arbIni + "\n"
          "    (not there; none is needed. To make one with every setting\n"
          "    explained: " + exe + " --write-ini \"" + arbIni + "\")\n";

    return QStringLiteral(
        "The Arbiter itself needs no settings. The other programs need these, once:\n\n"
        "Subspace\n"
        "  Settings > Reporting: TCP API enabled; \"Accept TCP requests\" on;\n"
        "  TCP max connections at least one more than you use now\n"
        "  (SpotterX takes one or two, CAT Watch one, the Arbiter one).\n\n"
        "fldigi\n"
        "  Nothing: its XML-RPC server is always on. To check it:\n"
        "  Configure > Misc > TCP-IP sessions > XML: address 127.0.0.1,\n"
        "  port 7362 (the defaults; Default, then Restart, puts them back).\n\n") +
        (m_layer ? QString(
        "VarAC and VARA  (study: varac/lever = layer)\n"
        "  VarAC reaches VARA through the Arbiter, and only while the Arbiter\n"
        "  runs. VARA is moved to command port %1 (data %2) and KISS port %3\n"
        "  (VARA: Settings > VARA Setup). VarAC keeps its usual ports %4 and\n"
        "  %5 and must not start VARA itself: when VarAC starts VARA it first\n"
        "  writes its own ports into VARA.ini (measured 2026-11-20). In VarAC's\n"
        "  settings file, with VarAC closed: VarahfLaunchOnModemConnect=OFF\n"
        "  (used at WM8Q since 2026-11-22). Start VARA and the Arbiter before\n"
        "  VarAC.\n"
        "  Neither \"Log VARA commands\" nor the cluster PTT lock is used.\n"
        "  In VarAC's settings file, with VarAC closed: under [RIG_CONTROL]\n")
            .arg(m_s.layerVaraCmdPort).arg(m_s.layerVaraCmdPort + 1).arg(m_s.layerVaraKissPort)
            .arg(m_s.layerCmdPort).arg(m_s.layerKissPort)
        : QStringLiteral(
        "VarAC\n"
        "  Settings: \"Log VARA commands\" on.\n"
        "  Settings > Cluster: enabled, \"PTT Lock\" on. Restart VarAC.\n"
        "  In VarAC's settings file, with VarAC closed: under [RIG_CONTROL]\n")) +
        QStringLiteral(
        "  set SafetyPTTOffEveryMinute=OFF (VarAC has no screen for it). ON,\n"
        "  VarAC un-keys the radio once a minute, cutting off whichever\n"
        "  program is transmitting. VarAC's settings files on this PC\n"
        "  (one per profile; set it in the one you use):\n") + varacFiles +
        QStringLiteral(
        "\n"
        "flrig\n"
        "  Nothing. The Arbiter talks to port 12345 like every other program,\n"
        "  with or without CAT Watch in front of flrig.\n\n"
        "Arbiter\n"
        "  Nothing. Its settings file, only for exceptions:\n") + arbLines +
        QStringLiteral(
        "\n"
        "Then: a Subspace or fldigi-subspace of the study branch waits with its\n"
        "own transmission until the Arbiter grants it, and says so. Any other\n"
        "Subspace shows \"TX disabled by ...\" while held, any other fldigi goes\n"
        "receive-only. ") +
        (m_layer ? QStringLiteral(
        "VarAC shows the channel busy while held. Close the\n"
        "Arbiter: Subspace and fldigi work as before; VarAC has no VARA until\n"
        "the Arbiter runs again.")
                 : QStringLiteral(
        "VarAC shows \"PTT is disabled\". Close the Arbiter and\n"
        "every program works as before."));
}

bool Arbiter::dialFromSubspace() const {
    return m_ssDialT > 0 && qAbs(m_ssDialT - m_dialT) < kDialMatchS;
}

void Arbiter::logDial() {
    m_log(QString("flrig: dial %1 (%2)%3")
              .arg(m_dialVfo)
              .arg(dialFromSubspace() ? "Subspace" : "program unknown", m_dialLink ? " during a VARA link" : ""));
    m_dialVfo = -1;
}

void Arbiter::apply(Program &p, bool want, const QString &reason, double now) {
    if (want && !p.held) {
        if (hold(p, reason)) {
            p.held = true; p.reason = reason; p.heldT = now; p.renewedT = now;
            m_log(p.name + ": disabled (" + reason + ")");
        } else if (!p.connected && p.seenUp && !p.skipLogged) {
            // it answered before and stopped (W1NEM 12 Nov 18:29, fldigi frozen): the hold
            // can't go out, and a skipped hold must not be silent. Once per busy period.
            m_log(p.name + ": not held (" + reason + "): not answering since " + clockText(p.downT));
            p.skipLogged = true;
        }
    } else if (want && p.held) {
        if (reason != p.reason) {
            p.reason = reason;
            renew(p, reason);
            p.renewedT = now;
            m_log(p.name + ": hold reason now " + reason);
        } else if (now - p.renewedT >= p.renewS) {
            renew(p, reason);
            p.renewedT = now;
        }
    } else if (!want && p.held) {
        // never touch a program while it transmits; a held VarAC puts nothing on the air,
        // so releasing it touches nothing
        if (p.transmitting && &p != &varac) return;
        release(p);
        p.held = false;
        m_log(p.name + ": released (" + p.reason + " over)");
        p.reason.clear();
    }
}

bool Arbiter::hold(Program &p, const QString &reason) {
    if (&p == &subspace) {
        if (!m_subspace->up()) return false;
        m_subspace->request("TX.SET_ENABLED", {{"ENABLED", false}, {"REASON", reason}}, [this](const QJsonObject &r) {
            if (r.isEmpty()) { subspace.held = false; return; }   // no answer: try again next tick
            QJsonObject q = r.value("params").toObject();
            if (!q.value("OK").toBool()) {
                m_log("Subspace: hold refused (" + q.value("REASON").toString() + ")");
                subspace.held = false;
                return;
            }
            m_ssHoldOwned = q.value("CHANGED").toBool() || q.value("REMOTE").toBool();
            if (!m_ssHoldOwned) m_log("Subspace: TX was already off by the operator; nothing to hold");
            m_ssTxEnabled = 0;
        });
        return true;
    }
    if (&p == &fldigi) return m_fldigi->hold();
    // [study] during Subspace's reply wait nobody here transmits: VarAC starts nothing
    // (BUSY ON), VARA still answers a caller (operator 2026-11-21)
    if (&p == &varac) return m_layer ? m_layer->hold(reason == Reason::SUBSPACE_REPLY) : m_lever->hold();
    return false;
}

void Arbiter::renew(Program &p, const QString &reason) {
    if (&p == &subspace) {
        m_subspace->request("TX.SET_ENABLED", {{"ENABLED", false}, {"REASON", reason}}, [this](const QJsonObject &r) {
            if (r.isEmpty()) return;
            QJsonObject q = r.value("params").toObject();
            if (q.value("OK").toBool() && (q.value("CHANGED").toBool() || q.value("REMOTE").toBool()))
                m_ssHoldOwned = true;
        });
    } else if (&p == &varac && m_layer) {
        m_layer->hold(reason == Reason::SUBSPACE_REPLY);   // the reason changed: listening or not
    } else if (&p == &varac && m_lever) {
        m_lever->renew();
    }
    // fldigi: rx_only has no lease; nothing to renew. Nor has the layer's hold.
}

void Arbiter::release(Program &p) {
    if (&p == &subspace) {
        if (m_ssHoldOwned && m_subspace->up())
            m_subspace->request("TX.SET_ENABLED", {{"ENABLED", true}}, nullptr);
        m_ssHoldOwned = false;
    } else if (&p == &fldigi) {
        m_fldigi->release(m_stopped);
    } else if (&p == &varac) {
        if (m_layer) m_layer->release();
        else m_lever->release();
    }
}

void Arbiter::operatorRelease(Program &p) {
    if (p.held) {
        release(p);
        p.held = false;
        m_log(p.name + ": released by the operator (" + p.reason + "); not held again this busy period");
        p.reason.clear();
    }
    p.overridden = true;
}

void Arbiter::setEnabled(Program &p, bool on) {
    p.enabled = on;
    m_log(p.name + ": hold " + (on ? "enabled" : "disabled") + " by the operator");
    // [study] a program whose hold is switched off is not asked to ask either (Subspace:
    // subspaceAsk() at the next tick)
    if (&p == &fldigi) m_fldigi->setAsking(on && m_s.askFldigi);
    if (!on && p.held) {
        release(p);
        p.held = false;
        p.reason.clear();
    }
}

void Arbiter::releaseAll() {
    for (Program *p : programs()) {
        if (p->held) {
            release(*p);
            p->held = false;
            m_log(p->name + ": released (arbiter closing)");
        }
    }
}
