#include "FlrigWatch.h"
#include "Settings.h"

#include <QDateTime>

static double nowS() { return QDateTime::currentMSecsSinceEpoch() / 1000.0; }

FlrigWatch::FlrigWatch(const Settings &s, Log &log, QObject *parent)
    : QObject(parent),
      m_link("flrig", s.flrigHost, s.flrigPort, int(s.xmlrpcTimeoutS * 1000), s.reconnectS, log) {}

void FlrigWatch::poll() {
    if (m_link.busy()) return;
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
