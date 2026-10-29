#include "FldigiLink.h"
#include "Settings.h"
#include "Log.h"

FldigiLink::FldigiLink(const Settings &s, Log &log, QObject *parent)
    : QObject(parent),
      m_link("fldigi", s.fldigiHost, s.fldigiPort, int(s.xmlrpcTimeoutS * 1000), s.reconnectS, log),
      m_log(log), m_host(s.fldigiHost), m_port(s.fldigiPort), m_timeoutMs(int(s.xmlrpcTimeoutS * 1000)) {}

void FldigiLink::sendCmd(const QString &cmd) {
    m_link.call(cmd, {}, [this, cmd](bool ok, const QVariant &) {
        if (!ok && cmd == "main.rx_only") {
            m_held = false;
            m_log("fldigi: rx_only not accepted");
        }
    });
}

void FldigiLink::poll() {
    if (m_link.busy()) return;
    if (!m_pending.isEmpty()) {          // a hold or release waits: it goes before the next query
        QString cmd = m_pending;
        m_pending.clear();
        sendCmd(cmd);
        return;
    }
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
    if (m_link.busy()) m_pending = "main.rx_only";
    else sendCmd("main.rx_only");
    return true;
}

bool FldigiLink::release() {
    if (!m_held) return false;
    m_held = false;
    m_pending.clear();
    if (m_link.busy()) {
        // the exit path cannot wait for the query in flight: a fresh client sends it now
        auto *tmp = new XmlRpcClient("fldigi", m_host, m_port, m_timeoutMs, 0, m_log, this);
        tmp->call("main.rx_tx", {}, [tmp](bool, const QVariant &) { tmp->deleteLater(); });
        return true;
    }
    sendCmd("main.rx_tx");
    return true;
}
