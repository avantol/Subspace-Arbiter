#pragma once
// VarAC's own switch: the cluster PTT lock. With Cluster enabled and PTT
// Lock ON, VarAC keeps one row per cluster instance in VarAC.db, table
// `instance` (busy, busy_last_time, last_keepalive_time). While any other
// instance's row says busy and its keepalive is under 30 s old, VarAC
// disables its own PTT ("PTT is disabled"). The arbiter is one such
// instance, named "Arbiter", with a fixed guid: hold = busy 1 with fresh
// times, renew = the same, release = busy 0, removed when the arbiter
// exits. Proven on VarAC 16.1 under Wine, 2026-10-29 (WM8Q).
#include <QObject>
#include <QSqlDatabase>
#include <QString>

struct Settings;
class Log;

class VaracLever : public QObject {
    Q_OBJECT
public:
    static const QString GUID;
    static const QString NAME;

    VaracLever(const Settings &s, Log &log, QObject *parent = nullptr);
    void setLogPath(const QString &logPath);   // VarAC.db lives beside the command log
    bool available();                          // refreshes status()
    QString status() const { return m_status; }
    QString kind() const { return m_kind; }
    bool held() const { return m_held; }
    bool hold();
    void renew();
    bool release();
    void remove();                             // on exit: leave no Arbiter row behind

private:
    static QString stamp();                    // VarAC's own shape: 2026-10-29 20:58:28.9007671Z
    bool write(const QString &what);

    Log &m_log;
    QString m_kind, m_db, m_status = "observe only";
    bool m_held = false;
    QSqlDatabase m_rw;
};
