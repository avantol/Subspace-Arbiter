#pragma once
// [study] The layer between VarAC and the VARA modem (varac/lever = layer).
// VarAC connects here on its usual ports; the layer connects on to VARA, which
// has been moved to other ports in VARA.ini. Everything is passed on unchanged,
// except while the arbiter holds VarAC:
//   - VarAC is told "BUSY ON" (VarAC then waits by itself with a CQ, a connect,
//     a beacon or a broadcast, and sends it in full after "BUSY OFF"); VARA's
//     own BUSY ON / BUSY OFF are kept back and their last value remembered
//   - VARA is told "LISTEN OFF" (it answers no caller; what it receives still
//     arrives), and VarAC's own LISTEN lines are sent again at the release
//   - a start command that VarAC sent in the same instant (CONNECT, CQFRAME, a
//     frame on the KISS port) is kept back and passed on at the release
// All of it measured with the real VarAC 16.1.8 and VARA HF 4.9.0, 2026-11-20.
// The layer never makes up a "PTT ON" and never sends VARA a command that
// transmits by itself: its only commands of its own are LISTEN lines.
#include "VaracWatch.h"

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTcpServer>
#include <QTcpSocket>

struct Settings;
class Log;

class VaraLayer : public QObject {
    Q_OBJECT
public:
    VaraLayer(const Settings &s, Log &log, QObject *parent = nullptr);
    bool listening() const { return m_listening; }
    bool connected() const { return m_cmd.varac && m_cmd.varaOpen; }   // VarAC and VARA both
    QString status() const;
    VaracWatch::Snapshot snapshot() const;   // the same four facts the log watch gives, to the ms
    bool hold();      // false while VarAC or VARA is not connected: nothing to hold
    bool release();   // the kept start commands go to VARA, then VarAC's own LISTEN lines, then BUSY OFF
    bool held() const { return m_held; }   // ends by itself when VarAC closes its connection
    int kept() const { return int(m_kept.size()); }
    void close();

signals:
    void changed();   // VarAC started something, or VARA said something that changes who has the radio

private:
    struct Pipe {
        QString name;
        QTcpServer server;
        QTcpSocket *varac = nullptr, *vara = nullptr;
        bool varaOpen = false;
        int listenPort = 0, varaPort = 0;
        double dialUntil = 0;
        bool waitLogged = false;
        int gen = 0;                      // counts VarAC's connections, so a late timer finds its own gone
        QByteArray fromVarac, fromVara;   // command pipe: a line not yet complete
    };
    struct Kept {
        bool kiss;
        QByteArray payload;
        double t;
    };

    void open(Pipe &p, const QString &name, int listenPort, int varaPort);
    void accepted(Pipe &p);
    void dial(Pipe &p);
    void closePipe(Pipe &p, const QString &why);
    void fromVaracSide(Pipe &p);
    void fromVaraSide(Pipe &p);
    void varacLine(const QString &line);
    void varaLine(const QString &line);
    void toVarac(const QString &line);
    void toVara(const QString &line, bool own);
    void started(const QString &what);
    void keep(bool kiss, const QByteArray &payload, const QString &shown);
    bool dropKept(const QString &why);   // true when a CONNECT was among them
    void passKept();
    void say(const QString &text);       // one traffic line in the log, when layer_log_lines is on

    Log &m_log;
    Pipe m_cmd, m_data, m_kiss;
    double m_varaWaitS;
    bool m_logLines;
    bool m_listening = false, m_closing = false;
    QString m_portTrouble;

    bool m_held = false;
    bool m_varaBusy = false;         // VARA's own last BUSY ON / BUSY OFF
    int m_ownOk = 0;                 // answers still to come to the layer's own LISTEN lines
    bool m_oweDisconnected = false;  // a kept CONNECT was thrown away: VarAC is told DISCONNECTED
    QStringList m_varacListen;       // VarAC's own LISTEN lines, in the order it sent them
    QList<Kept> m_kept;

    bool m_link = false, m_ptt = false;
    double m_pttT = 0, m_burstT = 0;
};
