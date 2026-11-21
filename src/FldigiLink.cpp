#include "FldigiLink.h"
#include "Settings.h"
#include "Log.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>

FldigiLink::FldigiLink(const Settings &s, Log &log, QObject *parent)
    : QObject(parent),
      m_link("fldigi", s.fldigiHost, s.fldigiPort, int(s.xmlrpcTimeoutS * 1000), s.reconnectS, log),
      m_log(log), m_host(s.fldigiHost), m_port(s.fldigiPort), m_timeoutMs(int(s.xmlrpcTimeoutS * 1000)),
      m_askLeaseMs(s.fldigiAskLeaseMs), m_askWanted(s.askFldigi) {
    // [fail-open] rx_only has no expiry in fldigi. The marker file beside the log says
    // "rx_only sent, no rx_tx accepted since": written before rx_only goes out, removed
    // only when fldigi accepts an rx_tx. One found at start-up means an arbiter was
    // killed (or crashed) while holding: this one restores fldigi before anything else.
    if (!log.path().isEmpty()) {
        m_marker = QFileInfo(log.path()).absolutePath() + "/fldigi_held.flag";
        m_marked = m_leftover = QFileInfo::exists(m_marker);
    }
}

void FldigiLink::mark(bool on) {
    if (on == m_marked) return;
    m_marked = on;
    if (m_marker.isEmpty()) return;
    if (on) {
        QFile f(m_marker);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
            f.write(("rx_only sent by the arbiter at " +
                     QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss") + "\n").toUtf8());
    } else {
        QFile::remove(m_marker);
    }
}

bool FldigiLink::sendCmd(const QString &cmd, const QVariantList &args) {
    bool sent = m_link.call(cmd, args, [this, cmd](bool ok, const QVariant &v) {
        if (cmd == "main.rx_tx") {
            rxTxResult(ok);
        } else if (cmd == "main.tx_grant") {
            // the next state query shows what fldigi did with it
            m_grantAnswered = true;
            if (!ok || v.toString().trimmed() != "granted")
                m_log("fldigi: grant not taken (the start no longer waits)");
        } else if (!ok && cmd == "main.rx_only") {
            m_held = false;   // the marker stays: the next poll sends rx_tx in case fldigi took it
            m_log("fldigi: rx_only not accepted");
        }
    }, cmd == "main.tx_grant");
    if (!sent && cmd == "main.rx_tx") rxTxResult(false);   // fldigi failed moments ago: not even sent
    if (!sent && cmd == "main.tx_grant") m_grantSent = 0;  // not sent: the start still waits, granted again
    return sent;
}

void FldigiLink::grant(int serial) {
    if (serial == 0 || m_grantSent != 0) return;
    m_grantSent = serial;
    m_grantAnswered = false;
    if (m_link.busy()) m_grantQueued = serial;
    else sendCmd("main.tx_grant", {serial});
}

void FldigiLink::askAnswer(bool ok, const QVariant &v) {
    QString const state = m_state;
    int const waiting = m_waiting, granted = m_granted;
    if (ok) {
        // "RX 7 0": state, the serial of a start that waits, the serial of the granted one
        QStringList const f = v.toString().trimmed().toUpper().split(' ', Qt::SkipEmptyParts);
        m_state = f.value(0);
        m_waiting = f.value(1).toInt();
        m_granted = f.value(2).toInt();
        if (m_askSupport != 1) m_log("fldigi: asks before it transmits; not held with receive-only");
        m_askSupport = 1;
        if (m_grantSent != 0 && m_grantAnswered) m_grantSent = 0;
    } else {
        if (v.toString().startsWith("fault")) {
            m_askSupport = 0;
            m_log("fldigi: this fldigi has no ask / grant (main.get_tx_ask): held with receive-only as before");
        } else {
            m_state.clear();   // not answering
        }
        m_waiting = m_granted = m_grantSent = m_grantQueued = 0;
    }
    if (state != m_state || waiting != m_waiting || granted != m_granted) emit changed();
}

void FldigiLink::rxTxResult(bool ok) {
    if (!ok) {
        if (!m_releaseFailed) m_log("fldigi: rx_tx not accepted; sent again until fldigi answers");
        m_releaseFailed = true;
        return;
    }
    if (m_held) return;   // a new hold began while this rx_tx was on its way: its marker stays
    if (m_leftover)
        m_log("fldigi: restored to transmit (left receive-only by an arbiter that did not close)");
    else if (m_releaseFailed)
        m_log("fldigi: rx_tx accepted");
    m_leftover = m_releaseFailed = false;
    mark(false);
}

void FldigiLink::poll() {
    if (m_wasUp && !m_link.up()) {       // fldigi may come back as another version
        m_askSupport = -1;
        m_waiting = m_granted = m_grantSent = m_grantQueued = 0;
    }
    m_wasUp = m_link.up();
    if (m_link.busy()) return;
    if (!m_pending.isEmpty()) {          // a hold or release waits: it goes before the next query
        QString cmd = m_pending;
        m_pending.clear();
        sendCmd(cmd);
        return;
    }
    if (m_grantQueued != 0) {            // [study] so does a grant
        int serial = m_grantQueued;
        m_grantQueued = 0;
        sendCmd("main.tx_grant", {serial});
        return;
    }
    // rx_only went out and no rx_tx was accepted since, but fldigi is not to be held (a
    // release fldigi refused, or a marker left by an arbiter that did not close): rx_tx
    // again at every tick until fldigi takes it. Nothing else clears the marker, so a
    // failed state query or an fldigi restart can't make the arbiter forget its hold.
    if (m_marked && !m_held && sendCmd("main.rx_tx")) return;
    if (m_askWanted && m_askSupport != 0) {
        bool sent = m_link.call("main.get_tx_ask", {m_askLeaseMs},
                                [this](bool ok, const QVariant &v) { askAnswer(ok, v); }, true);
        if (!sent && !m_link.up()) m_state.clear();
        return;
    }
    if (m_waiting != 0 || m_granted != 0 || m_grantSent != 0) {   // asking was switched off here
        m_waiting = m_granted = m_grantSent = m_grantQueued = 0;
        emit changed();
    }
    bool sent = m_link.call("main.get_trx_state", {}, [this](bool ok, const QVariant &v) {
        m_state = ok ? v.toString().trimmed().toUpper() : QString();
    });
    if (!sent && !m_link.up()) m_state.clear();
}

bool FldigiLink::hold() {
    if (m_state != "RX" || m_held) return false;
    m_held = true;
    mark(true);                // before rx_only goes out: a kill right after it still leaves the marker
    m_releaseFailed = false;   // the next release sends its own rx_tx
    if (m_link.busy()) m_pending = "main.rx_only";
    else sendCmd("main.rx_only");
    return true;
}

bool FldigiLink::release(bool closing) {
    if (!m_held) return false;
    m_held = false;
    m_pending.clear();
    if (!m_link.busy()) {
        sendCmd("main.rx_tx");
        return true;
    }
    if (!closing) {
        m_pending = "main.rx_tx";        // the query in flight answers within the timeout
        return true;
    }
    // the exit path cannot wait for the query in flight: a fresh client sends it now
    auto *tmp = new XmlRpcClient("fldigi (closing)", m_host, m_port, m_timeoutMs, 0, m_log, this);
    tmp->call("main.rx_tx", {}, [this, tmp](bool ok, const QVariant &) {
        if (!ok) m_log("fldigi: rx_tx not accepted (arbiter closing); the next arbiter to start restores fldigi");
        else mark(false);
        tmp->deleteLater();
    });
    return true;
}
