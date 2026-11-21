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
    // [study] a granted start counts from the grant, before fldigi reports TX
    bool transmitting() const { return m_state == "TX" || m_granted != 0 || m_grantSent != 0; }
    QString state() const { return m_state; }   // "TX" / "RX" / "" (not answering)
    bool held() const { return m_held; }
    bool hold();      // only while fldigi is in RX: rx_only aborts an over in progress
    bool release(bool closing = false);   // rx_tx; closing = the arbiter's exit path

    // [study] ask / grant (fldigi-subspace, branch study-ask-grant). The state query
    // main.get_tx_ask(lease_ms) replaces main.get_trx_state: it answers
    // "<state> <waiting> <granted>" and each one renews fldigi's asking for lease_ms,
    // so a stopped arbiter's asking ends by itself and fldigi starts as before. While
    // fldigi asks, a start waits inside fldigi (text kept) until main.tx_grant(serial).
    // An fldigi without the method answers with a fault: said once, held with
    // receive-only as before, asked again when fldigi reconnects.
    bool asks() const { return m_askSupport == 1 && m_askWanted; }
    void setAsking(bool on) { m_askWanted = on; }   // off: no more renewals, fldigi's asking lapses
    bool askWanted() const { return m_askWanted; }
    int askSupport() const { return m_askSupport; }  // -1 not known yet, 0 not in this fldigi, 1 yes
    int waiting() const { return m_grantSent ? 0 : m_waiting; }   // serial of a start that waits, 0 = none
    int granted() const { return m_granted; }
    void grant(int serial);

signals:
    void changed();   // a start began to wait, or a granted one ended

private:
    bool sendCmd(const QString &cmd, const QVariantList &args = {});
    void askAnswer(bool ok, const QVariant &v);
    void rxTxResult(bool ok);
    void mark(bool on);             // [fail-open] the "rx_only sent, no rx_tx accepted since" marker

    XmlRpcClient m_link;
    Log &m_log;
    QString m_host;
    int m_port, m_timeoutMs;
    QString m_state, m_pending;
    int m_askLeaseMs;
    bool m_askWanted;
    int m_askSupport = -1;
    int m_waiting = 0, m_granted = 0;
    int m_grantSent = 0;            // main.tx_grant sent, the state query has not shown it yet
    int m_grantQueued = 0;          // main.tx_grant waits for the request in flight
    bool m_grantAnswered = false;
    bool m_wasUp = false;
    bool m_held = false;
    bool m_releaseFailed = false;   // the last rx_tx was not accepted (logged once)
    QString m_marker;               // [fail-open] beside the log file
    bool m_marked = false;          // the marker stands
    bool m_leftover = false;        // the marker was found at start-up
};
