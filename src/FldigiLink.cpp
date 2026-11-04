#include "FldigiLink.h"
#include "Settings.h"
#include "Log.h"

FldigiLink::FldigiLink(const Settings &s, Log &log, QObject *parent)
    : QObject(parent),
      m_link("fldigi", s.fldigiHost, s.fldigiPort, int(s.xmlrpcTimeoutS * 1000), s.reconnectS, log),
      m_log(log), m_host(s.fldigiHost), m_port(s.fldigiPort), m_timeoutMs(int(s.xmlrpcTimeoutS * 1000)) {}

bool FldigiLink::sendCmd(const QString &cmd) {
    bool sent = m_link.call(cmd, {}, [this, cmd](bool ok, const QVariant &) {
        if (cmd == "main.rx_tx") {
            rxTxResult(ok);
        } else if (!ok && cmd == "main.rx_only") {
            m_held = false;
            m_log("fldigi: rx_only not accepted");
        }
    });
    if (!sent && cmd == "main.rx_tx") rxTxResult(false);   // fldigi failed moments ago: not even sent
    return sent;
}

void FldigiLink::rxTxResult(bool ok) {
    if (ok && m_releaseFailed)
        m_log("fldigi: rx_tx accepted");
    else if (!ok && !m_releaseFailed)
        m_log("fldigi: rx_tx not accepted; sent again until fldigi answers");
    m_releaseFailed = !ok;
}

void FldigiLink::poll() {
    if (m_link.busy()) return;
    if (!m_pending.isEmpty()) {          // a hold or release waits: it goes before the next query
        QString cmd = m_pending;
        m_pending.clear();
        sendCmd(cmd);
        return;
    }
    if (m_releaseFailed && !m_held && sendCmd("main.rx_tx")) return;
    bool sent = m_link.call("main.get_trx_state", {}, [this](bool ok, const QVariant &v) {
        if (!ok) {
            m_state.clear();
            if (m_held) m_held = false;   // fldigi went away: nothing of ours survives its restart
            return;
        }
        m_state = v.toString().trimmed().toUpper();
    });
    if (!sent && !m_link.up()) { m_state.clear(); m_held = false; }
}

bool FldigiLink::hold() {
    if (m_state != "RX" || m_held) return false;
    m_held = true;
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
        if (!ok) m_log("fldigi: rx_tx not accepted (arbiter closing); fldigi may stay receive-only");
        tmp->deleteLater();
    });
    return true;
}
