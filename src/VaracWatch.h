#pragma once
// Follows VarAC's VARA command log ("27/10/2026 12:47:24 - PTT ON"), found
// the way CAT Watch finds it, and reads VarAC's own keepalive from VarAC.db
// to know whether VarAC is alive. Arrival time, not the one-second stamp,
// is what the arbiter uses. Old lines in the tail at start-up are history.
#include <QObject>
#include <QSqlDatabase>
#include <QString>
#include <QTimer>

struct Settings;
class Log;

class VaracWatch : public QObject {
    Q_OBJECT
public:
    struct Snapshot {
        bool link = false;      // a CONNECTED link, VarAC alive
        bool ptt = false;       // PTT ON without OFF
        double pttT = 0;        // arrival of the last PTT line
        double burstT = 0;      // last moment VARA was transmitting or about to (PENDING)
    };
    VaracWatch(const Settings &s, Log &log, QObject *parent = nullptr);
    Snapshot snapshot();
    QString path() const { return m_path; }
    QString status() const { return m_status; }
    void close() { m_timer.stop(); }

    static QStringList logFiles();            // oldest first, newest last
    static QStringList wineRoots();           // Linux: each prefix's drive_c
    static QStringList winToHostPath(const QString &p);

private:
    static constexpr double LOOK_S = 30.0;    // chosen: how often the log file is looked for again
    void step();
    void lines(const QString &chunk, bool live);
    void checkAlive(double now);

    Log &m_log;
    QString m_fixedPath, m_path, m_db, m_status = "log not found";
    double m_staleS, m_deadS;
    QTimer m_timer;
    qint64 m_pos = -1;
    double m_looked = 0, m_lastLineT = 0, m_pttT = 0, m_burstT = 0, m_dbCheckedT = 0, m_keepaliveT = 0;
    bool m_linkOpen = false, m_ptt = false;
    int m_alive = -1;                         // 1 / 0 / -1 cannot tell
    QSqlDatabase m_ro;
};
