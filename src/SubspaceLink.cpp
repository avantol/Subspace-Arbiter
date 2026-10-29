#include "SubspaceLink.h"
#include "Settings.h"
#include "Log.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QRandomGenerator>

SubspaceLink::SubspaceLink(const Settings &s, Log &log, QObject *parent)
    : QObject(parent), m_host(s.subspaceHost), m_port(s.subspacePort), m_reconnectS(s.reconnectS), m_log(log),
      m_nextId(10'000'000 + QRandomGenerator::global()->bounded(80'000'000)) {   // never SpotterX's small ids
    connect(&m_sock, &QTcpSocket::connected, this, [this]() {
        // the socket is open, but "Connections Full" may be the first thing it says:
        // the connection is announced on the first real message instead
        m_up = true;
        m_announced = false;
        m_lastRxMs = QDateTime::currentMSecsSinceEpoch();
        if (!m_refused) m_status = "connected";
    });
    connect(&m_sock, &QTcpSocket::readyRead, this, &SubspaceLink::readLines);
    connect(&m_sock, &QTcpSocket::disconnected, this, [this]() { drop("closed by Subspace"); });
    connect(&m_sock, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        if (m_up) drop(m_sock.errorString().left(60));
        else if (!m_refused) m_status = "not connected (" + m_sock.errorString().left(60) + ")";
    });
    m_reconnect.setInterval(int(m_reconnectS * 1000));
    connect(&m_reconnect, &QTimer::timeout, this, &SubspaceLink::tryConnect);
    m_reconnect.start();
    m_keepalive.setInterval(KEEPALIVE_MS);
    connect(&m_keepalive, &QTimer::timeout, this, [this]() {
        if (!m_up) return;
        send("EVENTS.KEEPALIVE");
        if (QDateTime::currentMSecsSinceEpoch() - m_lastRxMs > 3 * KEEPALIVE_MS) drop("no answer to keepalive");
    });
    m_keepalive.start();
    QTimer::singleShot(0, this, &SubspaceLink::tryConnect);
}

void SubspaceLink::tryConnect() {
    if (m_closing || m_up || m_sock.state() != QAbstractSocket::UnconnectedState) return;
    m_buf.clear();
    m_sock.connectToHost(m_host, m_port);
}

void SubspaceLink::drop(const QString &why) {
    if (m_sock.state() != QAbstractSocket::UnconnectedState) m_sock.abort();
    if (m_up && m_announced && !m_refused) m_log("Subspace: connection lost (" + why + ")");
    m_up = false;
    if (!m_refused) m_status = "not connected (" + why + ")";   // a refusal's plain words stay
    for (auto it = m_pending.begin(); it != m_pending.end(); ++it)
        if (it.value()) it.value()(QJsonObject());
    m_pending.clear();
}

void SubspaceLink::readLines() {
    m_lastRxMs = QDateTime::currentMSecsSinceEpoch();
    while (m_sock.canReadLine()) {
        QByteArray line = m_sock.readLine().trimmed();
        if (!line.isEmpty()) handleLine(line);
    }
}

void SubspaceLink::handleLine(const QByteArray &line) {
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(line, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return;
    QJsonObject msg = doc.object();
    QString t = msg.value("type").toString();
    QJsonObject params = msg.value("params").toObject();
    if (t == "API.ERROR") {
        QString v = msg.value("value").toString();
        if (v.contains("Connections Full")) {
            if (!m_refused) m_log("Subspace: refused, raise 'TCP max connections' in Subspace Settings");
            m_refused = true;
            m_status = "refused: raise 'TCP max connections' in Subspace Settings";
        } else {
            m_log("Subspace: API.ERROR " + v);
        }
        return;
    }
    if (!m_announced) {
        m_announced = true;
        m_refused = false;
        m_status = "connected";
        m_log(QString("Subspace: connected on %1:%2").arg(m_host).arg(m_port));
    }
    qint64 rid = qint64(params.value("_ID").toDouble(-1));
    if (rid > 0 && m_pending.contains(rid)) {
        Reply cb = m_pending.take(rid);
        if (cb) cb(msg);
        return;
    }
    if (t == "EVENTS.PONG") return;
    m_events.append(msg);
}

qint64 SubspaceLink::send(const QString &type, const QString &value, QVariantMap params) {
    if (!m_up) return -1;
    qint64 rid = m_nextId++;
    params["_ID"] = double(rid);
    QJsonObject o;
    o["type"] = type;
    o["value"] = value;
    o["params"] = QJsonObject::fromVariantMap(params);
    m_sock.write(QJsonDocument(o).toJson(QJsonDocument::Compact) + "\n");
    m_sock.flush();
    return rid;
}

void SubspaceLink::request(const QString &type, const QVariantMap &params, Reply cb, int timeoutMs) {
    qint64 rid = send(type, QString(), params);
    if (rid < 0) { if (cb) cb(QJsonObject()); return; }
    m_pending[rid] = cb;
    QTimer::singleShot(timeoutMs, this, [this, rid]() {
        if (m_pending.contains(rid)) {
            Reply c = m_pending.take(rid);
            if (c) c(QJsonObject());
        }
    });
}

QList<QJsonObject> SubspaceLink::takeEvents() {
    QList<QJsonObject> e;
    e.swap(m_events);
    return e;
}

void SubspaceLink::close() {
    m_closing = true;
    m_reconnect.stop();
    m_keepalive.stop();
    drop("arbiter closing");
}
