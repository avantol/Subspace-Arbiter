#include "FldigiLink.h"
#include "Settings.h"
#include "Log.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>

FldigiLink::FldigiLink(const Settings &s, Log &log, QObject *parent)
    : QObject(parent),
      m_link("fldigi", s.fldigiHost, s.fldigiPort, int(s.xmlrpcTimeoutS * 1000), s.reconnectS, log),
      m_log(log), m_host(s.fldigiHost), m_port(s.fldigiPort), m_timeoutMs(int(s.xmlrpcTimeoutS * 1000)) {
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

bool FldigiLink::sendCmd(const QString &cmd) {
    bool sent = m_link.call(cmd, {}, [this, cmd](bool ok, const QVariant &) {
        if (cmd == "main.rx_tx") {
            rxTxResult(ok);
        } else if (!ok && cmd == "main.rx_only") {
            m_held = false;   // the marker stays: the next poll sends rx_tx in case fldigi took it
            m_log("fldigi: rx_only not accepted");
        }
    });
    if (!sent && cmd == "main.rx_tx") rxTxResult(false);   // fldigi failed moments ago: not even sent
    return sent;
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
    if (m_link.busy()) return;
    if (!m_pending.isEmpty()) {          // a hold or release waits: it goes before the next query
        QString cmd = m_pending;
        m_pending.clear();
        sendCmd(cmd);
        return;
    }
    // rx_only went out and no rx_tx was accepted since, but fldigi is not to be held (a
    // release fldigi refused, or a marker left by an arbiter that did not close): rx_tx
    // again at every tick until fldigi takes it. Nothing else clears the marker, so a
    // failed state query or an fldigi restart can't make the arbiter forget its hold.
    if (m_marked && !m_held && sendCmd("main.rx_tx")) return;
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
