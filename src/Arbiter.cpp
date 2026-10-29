#include "Arbiter.h"
#include "FldigiLink.h"
#include "FlrigWatch.h"
#include "Log.h"
#include "SubspaceLink.h"
#include "VaracLever.h"
#include "VaracWatch.h"
#include "config.h"

#include <QDateTime>
#include <QEventLoop>
#include <QJsonObject>

static double nowS() { return QDateTime::currentMSecsSinceEpoch() / 1000.0; }

Arbiter::Arbiter(const Settings &s, Log &log, QObject *parent)
    : QObject(parent), m_s(s), m_log(log), m_pollS(s.pollMs / 1000.0) {
    m_flrig = new FlrigWatch(s, log, this);
    m_fldigi = new FldigiLink(s, log, this);
    m_subspace = new SubspaceLink(s, log, this);
    m_varac = new VaracWatch(s, log, this);
    m_lever = new VaracLever(s, log, this);

    subspace.name = "Subspace"; subspace.enabled = s.holdSubspace; subspace.renewS = s.renewS;
    subspace.own = {Reason::SUBSPACE};
    fldigi.name = "fldigi"; fldigi.enabled = s.holdFldigi; fldigi.renewS = s.renewS;
    fldigi.own = {Reason::FLDIGI};
    varac.name = "VarAC"; varac.enabled = s.holdVarac; varac.renewS = s.varacRenewS;
    varac.own = {Reason::VARA, Reason::VARA_TX};
    for (const QString &r : Reason::ORDER) m_sources[r] = false;

    m_tick.setInterval(s.pollMs);
    connect(&m_tick, &QTimer::timeout, this, &Arbiter::tick);
}

Arbiter::~Arbiter() { stop(); }

void Arbiter::start() {
    m_log(QString("arbiter %1 started; holds: Subspace %2, fldigi %3, VarAC %4 (lever: %5)%6")
              .arg(ARBITER_VERSION, subspace.enabled ? "on" : "off", fldigi.enabled ? "on" : "off",
                   varac.enabled ? "on" : "off", m_lever->kind(),
                   m_s.iniPath.isEmpty() ? QString() : "; settings from " + m_s.iniPath));
    m_tick.start();
    QTimer::singleShot(0, this, &Arbiter::tick);
}

void Arbiter::stop() {
    if (m_stopped) return;
    m_stopped = true;
    m_tick.stop();
    releaseAll();
    m_lever->remove();
    // the releases are network sends: let them go out before the sockets close
    // (kExitFlushMs = 600, chosen: fldigi and Subspace answer in well under that)
    QEventLoop flush;
    QTimer::singleShot(600, &flush, &QEventLoop::quit);
    flush.exec();
    m_subspace->close();
    m_varac->close();
}

// -- Subspace events ------------------------------------------------------------
void Arbiter::subspaceEvents(double now) {
    for (const QJsonObject &msg : m_subspace->takeEvents()) {
        QString t = msg.value("type").toString();
        QJsonObject p = msg.value("params").toObject();
        if (t == "RIG.PTT") {
            bool on = p.contains("PTT") ? p.value("PTT").toBool()
                                        : msg.value("value").toString().toLower() == "on";
            if (on) { m_ssPtt = true; m_ssMsg = true; }
            else { m_ssPtt = false; m_ssLastOffT = now; }
        } else if (t == "TX.COMPLETE") {
            m_ssMsg = false;
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
            m_log("Subspace: closing");
        }
    }
}

void Arbiter::subspaceStatePoll(double now) {
    if (now - m_ssBusyPolledT < m_s.statePollS) return;
    m_ssBusyPolledT = now;
    m_subspace->request("STATION.GET_BUSY", {}, [this](const QJsonObject &r) {
        if (r.isEmpty()) return;
        QJsonObject p = r.value("params").toObject();
        QString reason = p.value("BUSY_REASON").toString();
        m_ssBusy = p.value("BUSY").toBool() &&
                   (reason == "arq_tx" || reason == "arq_negotiating" || reason == "transmitting");
        if (p.value("PTT").toBool()) { m_ssPtt = true; m_ssMsg = true; }
    }, 1000);
}

// -- one tick -----------------------------------------------------------------------
void Arbiter::tick() {
    double now = nowS();
    m_flrig->poll();
    m_fldigi->poll();
    subspaceEvents(now);
    if (m_subspace->up()) subspaceStatePoll(now);
    VaracWatch::Snapshot va = m_varac->snapshot();

    // who is transmitting
    if (m_ssMsg && !m_ssPtt && now - m_ssLastOffT > m_s.subspaceFrameGapS)
        m_ssMsg = false;   // no next frame came: the message ended without TX.COMPLETE (halted)
    // still transmitting for subspace_frame_gap_s after the last PTT off: covers the gap between
    // frames and the 1.4 s between two queued messages (measured 2026-10-29)
    bool ssTx = m_ssPtt || m_ssMsg || m_ssBusy || now - m_ssLastOffT < m_s.subspaceFrameGapS;
    bool flTx = m_fldigi->transmitting();
    if (flTx) m_flLastTxT = now;
    // VARA transmitting: PTT on now (a PTT ON without OFF counts for a minute), or a burst or
    // connect request within vara_burst_hold_s, so the gaps between bursts keep the hold
    bool vaBurst = (va.ptt && now - va.pttT < 60) || (now - va.burstT < m_s.varaBurstHoldS);
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
    m_sources[Reason::SUBSPACE] = ssTx;
    m_sources[Reason::UNKNOWN] = unknown;

    subspace.connected = m_subspace->up();
    fldigi.connected = m_fldigi->up();
    varac.connected = !m_varac->path().isEmpty();
    if (varac.connected) m_lever->setLogPath(m_varac->path());
    m_lever->available();   // refreshes its status text
    subspace.transmitting = ssTx;
    fldigi.transmitting = flTx;
    varac.transmitting = vaTx;

    bool busy = false;
    for (const QString &r : Reason::ORDER) busy = busy || m_sources[r];
    if (busy && !m_busyPeriod) {
        m_busyPeriod = true;
    } else if (!busy && m_busyPeriod) {
        m_busyPeriod = false;
        for (Program *p : programs()) {
            if (p->overridden) m_log(p->name + ": busy period over, override forgotten");
            p->overridden = false;
        }
    }

    for (Program *p : programs()) {
        QStringList others;
        for (const QString &r : Reason::ORDER)
            if (m_sources[r] && !p->own.contains(r)) others << r;
        bool want = !others.isEmpty() && !p->transmitting && !p->overridden && p->enabled;
        apply(*p, want, others.isEmpty() ? QString() : others.first(), now);
    }

    // the dial is never arbitrated, only logged, once per change
    if (m_flrig->vfo >= 0 && m_flrig->vfo != m_loggedVfo) {
        if (m_loggedVfo >= 0) {
            QString who = now - m_ssDialT < 2.0 ? "Subspace" : "program unknown";
            m_log(QString("flrig: dial %1 (%2)%3").arg(m_flrig->vfo).arg(who, va.link ? " during a VARA link" : ""));
        }
        m_loggedVfo = m_flrig->vfo;
    }

    // the status texts carry the remedy, so a fresh station needs no manual
    subspace.status = m_subspace->status();
    if (!m_subspace->up() && !subspace.status.startsWith("refused"))
        subspace.status += "  (Subspace: Settings > Reporting: TCP API enabled, Accept TCP requests)";
    fldigi.status = m_fldigi->up() ? "connected"
                                   : "not answering  (fldigi: Configure > Misc > XML-RPC server on, port 7362)";
    QString lever = m_lever->status();
    if (lever.startsWith("cluster lock: VarAC.db not found"))
        lever += "  (VarAC: Settings > Cluster enabled, PTT Lock ON, restart VarAC)";
    varac.status = m_varac->status() + "; " + lever;
}

bool Arbiter::setupComplete() const {
    return m_subspace->up() && m_fldigi->up() && !m_varac->path().isEmpty() &&
           m_lever->status() == "cluster lock ready";
}

QString Arbiter::setupText() {
    return QStringLiteral(
        "The Arbiter itself needs no settings. The other programs need these, once:\n\n"
        "Subspace\n"
        "  Settings > Reporting: TCP API enabled; \"Accept TCP requests\" on;\n"
        "  TCP max connections at least one more than you use now\n"
        "  (SpotterX takes one or two, CAT Watch one, the Arbiter one).\n\n"
        "fldigi\n"
        "  Configure > Misc > XML-RPC: server on, port 7362 (the default).\n\n"
        "VarAC\n"
        "  Settings: \"Log VARA commands\" on.\n"
        "  Settings > Cluster: enabled, \"PTT Lock\" on. Restart VarAC.\n\n"
        "flrig\n"
        "  Nothing. The Arbiter talks to port 12345 like every other program,\n"
        "  with or without CAT Watch in front of flrig.\n\n"
        "Then: Subspace shows \"TX disabled by ...\" while held, fldigi goes\n"
        "receive-only, VarAC shows \"PTT is disabled\". Close the Arbiter and\n"
        "every program works as before.");
}

void Arbiter::apply(Program &p, bool want, const QString &reason, double now) {
    if (want && !p.held) {
        if (hold(p, reason)) {
            p.held = true; p.reason = reason; p.heldT = now; p.renewedT = now;
            m_log(p.name + ": disabled (" + reason + ")");
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
        if (p.transmitting) return;   // never touch a program while it transmits
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
    if (&p == &varac) return m_lever->hold();
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
    } else if (&p == &varac) {
        m_lever->renew();
    }
    // fldigi: rx_only has no lease; nothing to renew
}

void Arbiter::release(Program &p) {
    if (&p == &subspace) {
        if (m_ssHoldOwned && m_subspace->up())
            m_subspace->request("TX.SET_ENABLED", {{"ENABLED", true}}, nullptr);
        m_ssHoldOwned = false;
    } else if (&p == &fldigi) {
        m_fldigi->release();
    } else if (&p == &varac) {
        m_lever->release();
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
