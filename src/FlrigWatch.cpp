#include "FlrigWatch.h"
#include "Settings.h"
#include "Log.h"

#include <QDateTime>

static double nowS() { return QDateTime::currentMSecsSinceEpoch() / 1000.0; }

FlrigWatch::FlrigWatch(const Settings &s, Log &log, QObject *parent)
    : QObject(parent),
      m_link("flrig", s.flrigHost, s.flrigPort, int(s.xmlrpcTimeoutS * 1000), s.reconnectS, log),
      m_log(log), m_blockLabel(s.unkeyBlockLabel), m_blockMs(s.unkeyBlockMs),
      m_blockRenewS(s.unkeyBlockRenewS) {}

// [unkey-block] One instruction per call: start or renew the block while the arbiter
// holds VarAC, end it once when the hold ends. A flrig without the command (stock,
// or flrig-subspace before 8) answers with a fault: said once, then not asked again
// until flrig reconnects. Returns true when it sent something.
bool FlrigWatch::sendBlockIfDue() {
    if (m_blockLabel.isEmpty() || m_blockSupport == 0 || !m_link.up() || m_link.busy()) return false;
    double now = nowS();
    bool due = m_blockWant ? (!m_blockSent || now - m_blockSentT >= m_blockRenewS) : m_blockSent;
    if (!due) return false;
    bool want = m_blockWant;
    bool sent = m_link.call("rig.block_unkey", {m_blockLabel, want ? m_blockMs : 0},
        [this, want](bool ok, const QVariant &v) {
            if (ok) {
                if (m_blockSupport != 1) m_blockSupport = 1;
                if (want && !m_blockSent) m_log("flrig: un-keys from " + m_blockLabel + " blocked while VarAC is held");
                if (!want && m_blockSent) m_log("flrig: un-key block ended");
                m_blockSent = want;
                return;
            }
            if (v.toString().startsWith("fault")) {
                m_blockSupport = 0;
                m_blockSent = false;
                m_log("flrig: this flrig has no un-key block (stock, or flrig-subspace before 8): "
                      "a held VarAC's un-keys still reach the radio");
            }
            // no answer: flrig is down; asked again once it answers
        }, true);
    if (sent) m_blockSentT = now;
    return sent;
}

void FlrigWatch::poll() {
    if (!m_link.up()) {
        m_blockSupport = -1;    // flrig may come back as another version
        m_blockSent = false;
    }
    if (m_link.busy()) return;
    if (sendBlockIfDue()) return;
    bool sent = m_link.call("rig.get_ptt", {}, [this](bool ok, const QVariant &v) {
        double now = nowS();
        if (!ok) { ptt = -1; return; }
        int p = v.toLongLong() != 0 ? 1 : 0;
        if (p != ptt) { ptt = p; pttChangedT = now; }
        if (now - m_vfoPolledT < VFO_POLL_S) return;
        m_vfoPolledT = now;
        m_link.call("rig.get_vfo", {}, [this](bool ok2, const QVariant &v2) {
            if (!ok2) return;
            bool okNum = false;
            qint64 f = qint64(v2.toString().trimmed().toDouble(&okNum));
            if (!okNum) f = v2.toLongLong(&okNum);
            if (!okNum) return;
            if (f != vfo) {
                if (vfo >= 0) vfoChangedT = nowS();   // a change, not the first reading
                vfo = f;
            }
        });
    });
    if (!sent && !m_link.up()) ptt = -1;
}
