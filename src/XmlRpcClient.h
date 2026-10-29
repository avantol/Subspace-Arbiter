#pragma once
// A small XML-RPC client over HTTP (flrig, fldigi): one request in flight,
// a transfer timeout, and a back-off after a failure. No library: the
// request is a hand-built <methodCall>, the reply's first <value> is parsed
// with QXmlStreamReader (i4, int, boolean, string, double; <fault>).
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QVariant>
#include <functional>

class Log;

class XmlRpcClient : public QObject {
    Q_OBJECT
public:
    // ok = false when the program did not answer (or answered with a fault)
    using Reply = std::function<void(bool ok, const QVariant &value)>;

    XmlRpcClient(const QString &name, const QString &host, int port, int timeoutMs,
                 double reconnectS, Log &log, QObject *parent = nullptr);

    // Returns false when nothing was sent: a call is in flight, or the program
    // failed less than reconnectS ago. The callback is then not invoked.
    bool call(const QString &method, const QVariantList &args, Reply cb);
    bool up() const { return m_up; }
    bool busy() const { return m_inflight != nullptr; }
    QString name() const { return m_name; }

private:
    void finished(QNetworkReply *r, Reply cb);
    static QByteArray build(const QString &method, const QVariantList &args);
    static bool parse(const QByteArray &xml, QVariant &value, QString &fault);

    QString m_name, m_host;
    int m_port, m_timeoutMs;
    double m_reconnectS;
    Log &m_log;
    QNetworkAccessManager m_nam;
    QNetworkReply *m_inflight = nullptr;
    bool m_up = false;
    double m_downUntil = 0;
};
