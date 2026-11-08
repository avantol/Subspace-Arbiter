#pragma once
// main.get_trx_state ("TX" covers TX and TUNE, else "RX"), main.rx_only,
// main.rx_tx: stock fldigi since 2014. One request at a time on the link:
// a hold or release asked for while the state query is in flight goes out
// at the next tick instead of the query. From rx_only until fldigi accepts
// an rx_tx a marker file stands (fail-open across a kill); while it stands
// and fldigi is not to be held, rx_tx is sent again at every tick. Only the
// exit path, which cannot wait for the next tick, sends on a client of its own.
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
    bool release(bool closing = false);   // rx_tx; closing = the arbiter's exit path

private:
    bool sendCmd(const QString &cmd);
    void rxTxResult(bool ok);
    void mark(bool on);             // [fail-open] the "rx_only sent, no rx_tx accepted since" marker

    XmlRpcClient m_link;
    Log &m_log;
    QString m_host;
    int m_port, m_timeoutMs;
    QString m_state, m_pending;
    bool m_held = false;
    bool m_releaseFailed = false;   // the last rx_tx was not accepted (logged once)
    QString m_marker;               // [fail-open] beside the log file
    bool m_marked = false;          // the marker stands
    bool m_leftover = false;        // the marker was found at start-up
};
