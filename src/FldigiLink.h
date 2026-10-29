#pragma once
// main.get_trx_state ("TX" covers TX and TUNE, else "RX"), main.rx_only,
// main.rx_tx: stock fldigi since 2014. One request at a time on the link:
// a hold or release asked for while the state query is in flight goes out
// at the next tick instead of the query.
#include "XmlRpcClient.h"

struct Settings;
class Log;

class FldigiLink : public QObject {
    Q_OBJECT
public:
    FldigiLink(const Settings &s, Log &log, QObject *parent = nullptr);
    void poll();
    bool up() const { return m_link.up(); }
    bool transmitting() const { return m_state == "TX"; }
    QString state() const { return m_state; }   // "TX" / "RX" / "" (not answering)
    bool held() const { return m_held; }
    bool hold();      // only while fldigi is in RX: rx_only aborts an over in progress
    bool release();   // rx_tx; also used on every exit path

private:
    void sendCmd(const QString &cmd);

    XmlRpcClient m_link;
    Log &m_log;
    QString m_host;
    int m_port, m_timeoutMs;
    QString m_state, m_pending;
    bool m_held = false;
};
