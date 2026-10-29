#pragma once
// Reads PTT and the dial from flrig's cache. Never sends set_ptt.
#include "XmlRpcClient.h"

struct Settings;
class Log;

class FlrigWatch : public QObject {
    Q_OBJECT
public:
    FlrigWatch(const Settings &s, Log &log, QObject *parent = nullptr);
    void poll();                    // every tick: rig.get_ptt; rig.get_vfo once a second
    bool up() const { return m_link.up(); }

    int ptt = -1;                   // 1 / 0 / -1 unknown
    qint64 vfo = -1;
    double pttChangedT = 0;
    double vfoChangedT = 0;

private:
    static constexpr double VFO_POLL_S = 1.0;   // chosen: the dial is only logged
    XmlRpcClient m_link;
    double m_vfoPolledT = 0;
};
