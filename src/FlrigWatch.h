#pragma once
// Reads PTT and the dial from flrig's cache. Never sends set_ptt. While the
// arbiter holds VarAC it also keeps flrig-subspace's un-key block standing
// (rig.block_unkey): VarAC's lock holds its keys, not VARA's un-keys.
#include "XmlRpcClient.h"

struct Settings;
class Log;

class FlrigWatch : public QObject {
    Q_OBJECT
public:
    FlrigWatch(const Settings &s, Log &log, QObject *parent = nullptr);
    void poll();                    // every tick: rig.get_ptt; rig.get_vfo once a second
    bool up() const { return m_link.up(); }

    // the un-key block the arbiter wants now; poll() sends, renews and ends it
    void setUnkeyBlock(bool on) { m_blockWant = on; }
    bool sendBlockIfDue();          // also called while the arbiter closes
    int blockSupport() const { return m_blockSupport; }   // -1 unknown, 0 not in this flrig, 1 yes
    bool blockStanding() const { return m_blockSent && m_blockSupport == 1; }

    int ptt = -1;                   // 1 / 0 / -1 unknown
    qint64 vfo = -1;
    double pttChangedT = 0;
    double vfoChangedT = 0;

private:
    static constexpr double VFO_POLL_S = 1.0;   // chosen: the dial is only logged
    XmlRpcClient m_link;
    Log &m_log;
    double m_vfoPolledT = 0;

    QString m_blockLabel;
    int m_blockMs;
    double m_blockRenewS;
    bool m_blockWant = false;
    bool m_blockSent = false;       // the last instruction that reached flrig was "block"
    double m_blockSentT = 0;
    int m_blockSupport = -1;
};
