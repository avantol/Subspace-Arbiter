#include "XmlRpcClient.h"
#include "Log.h"

#include <QDateTime>
#include <QNetworkRequest>
#include <QUrl>
#include <QXmlStreamReader>

static double nowS() { return QDateTime::currentMSecsSinceEpoch() / 1000.0; }

XmlRpcClient::XmlRpcClient(const QString &name, const QString &host, int port, int timeoutMs,
                           double reconnectS, Log &log, QObject *parent)
    : QObject(parent), m_name(name), m_host(host), m_port(port), m_timeoutMs(timeoutMs),
      m_reconnectS(reconnectS), m_log(log) {}

QByteArray XmlRpcClient::build(const QString &method, const QVariantList &args) {
    QByteArray b = "<?xml version=\"1.0\"?><methodCall><methodName>";
    b += method.toUtf8() + "</methodName><params>";
    for (const QVariant &a : args) {
        b += "<param><value>";
        switch (a.typeId()) {
        case QMetaType::Bool: b += QByteArray("<boolean>") + (a.toBool() ? "1" : "0") + "</boolean>"; break;
        case QMetaType::Int: case QMetaType::LongLong: case QMetaType::UInt:
            b += "<i4>" + QByteArray::number(a.toLongLong()) + "</i4>"; break;
        case QMetaType::Double: b += "<double>" + QByteArray::number(a.toDouble()) + "</double>"; break;
        default: b += "<string>" + a.toString().toHtmlEscaped().toUtf8() + "</string>"; break;
        }
        b += "</value></param>";
    }
    b += "</params></methodCall>";
    return b;
}

bool XmlRpcClient::parse(const QByteArray &xml, QVariant &value, QString &fault) {
    QXmlStreamReader x(xml);
    bool inFault = false, inValue = false;
    while (!x.atEnd()) {
        x.readNext();
        if (x.isStartElement()) {
            QString n = x.name().toString();
            if (n == "fault") inFault = true;
            else if (n == "value" && !inValue) {
                inValue = true;
                // the typed child, or bare text (= string)
                QString text;
                QString type = "string";
                while (!x.atEnd()) {
                    x.readNext();
                    if (x.isStartElement()) {
                        type = x.name().toString();
                        text = x.readElementText(QXmlStreamReader::IncludeChildElements);
                        break;
                    }
                    if (x.isCharacters() && !x.isWhitespace()) { text = x.text().toString(); }
                    if (x.isEndElement()) break;
                }
                if (inFault) { fault = text; return false; }
                if (type == "i4" || type == "int") value = text.trimmed().toLongLong();
                else if (type == "boolean") value = (text.trimmed() == "1" || text.trimmed().toLower() == "true");
                else if (type == "double") value = text.trimmed().toDouble();
                else value = text;
                return true;
            }
        }
    }
    if (inFault) { fault = "fault"; return false; }
    value = QString();   // an empty <params/> response (fldigi's rx_only answers nothing)
    return !x.hasError();
}

bool XmlRpcClient::call(const QString &method, const QVariantList &args, Reply cb) {
    if (m_inflight) return false;
    double now = nowS();
    if (!m_up && now < m_downUntil) return false;
    QNetworkRequest req(QUrl(QString("http://%1:%2/RPC2").arg(m_host).arg(m_port)));
    req.setHeader(QNetworkRequest::ContentTypeHeader, "text/xml");
    req.setTransferTimeout(m_timeoutMs);
    m_inflight = m_nam.post(req, build(method, args));
    connect(m_inflight, &QNetworkReply::finished, this, [this, cb]() { finished(m_inflight, cb); });
    return true;
}

void XmlRpcClient::finished(QNetworkReply *r, Reply cb) {
    m_inflight = nullptr;
    r->deleteLater();
    QVariant value;
    QString fault;
    bool ok = r->error() == QNetworkReply::NoError && parse(r->readAll(), value, fault);
    if (ok) {
        if (!m_up) {
            m_up = true;
            m_log(QString("%1: connected on %2:%3").arg(m_name, m_host).arg(m_port));
        }
    } else {
        QString why = r->error() != QNetworkReply::NoError ? r->errorString() : ("fault " + fault);
        if (m_up) m_log(QString("%1: not answering (%2)").arg(m_name, why.left(80)));
        m_up = false;
        m_downUntil = nowS() + m_reconnectS;
    }
    if (cb) cb(ok, value);
}
