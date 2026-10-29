#pragma once
// Subspace's TCP API: JSON lines on a persistent socket. Events RIG.PTT /
// TX.COMPLETE / TX.ENABLED / STATION.CLOSING, queries with _ID correlation,
// EVENTS.KEEPALIVE heartbeat, reconnect on loss, "Connections Full" said in
// plain words.
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QTcpSocket>
#include <QTimer>
#include <QVariantMap>
#include <functional>

struct Settings;
class Log;

class SubspaceLink : public QObject {
    Q_OBJECT
public:
    // an empty object = no answer
    using Reply = std::function<void(const QJsonObject &)>;

    SubspaceLink(const Settings &s, Log &log, QObject *parent = nullptr);
    void request(const QString &type, const QVariantMap &params, Reply cb, int timeoutMs = 2000);
    qint64 send(const QString &type, const QString &value = QString(), QVariantMap params = {});
    QList<QJsonObject> takeEvents();
    bool up() const { return m_up; }
    QString status() const { return m_status; }
    void close();

private:
    static constexpr int KEEPALIVE_MS = 10000;   // chosen
    void tryConnect();
    void drop(const QString &why);
    void readLines();
    void handleLine(const QByteArray &line);

    QString m_host;
    int m_port;
    double m_reconnectS;
    Log &m_log;
    QTcpSocket m_sock;
    QTimer m_reconnect, m_keepalive;
    QByteArray m_buf;
    QHash<qint64, Reply> m_pending;
    QList<QJsonObject> m_events;
    qint64 m_nextId;
    bool m_up = false, m_refused = false, m_closing = false, m_announced = false;
    qint64 m_lastRxMs = 0;
    QString m_status = "not connected";
};
