#include "VaracLever.h"
#include "Settings.h"
#include "Log.h"

#include <QDateTime>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>

const QString VaracLever::GUID = QStringLiteral("00000000-0000-4000-8000-00000000a7b1");
const QString VaracLever::NAME = QStringLiteral("Arbiter");

VaracLever::VaracLever(const Settings &s, Log &log, QObject *parent)
    : QObject(parent), m_log(log), m_kind(s.varacLever.isEmpty() ? "none" : s.varacLever), m_db(s.varacDbPath) {
    m_rw = QSqlDatabase::addDatabase("QSQLITE", "varac-rw");
}

void VaracLever::setLogPath(const QString &logPath) {
    if (!m_db.isEmpty() || logPath.isEmpty()) return;
    QString cand = QFileInfo(logPath).absolutePath() + "/VarAC.db";
    if (QFileInfo::exists(cand)) m_db = cand;
}

bool VaracLever::available() {
    bool ok = m_kind == "cluster_lock" && !m_db.isEmpty() && QFileInfo::exists(m_db);
    m_status = ok ? "cluster lock ready"
                  : (m_kind != "cluster_lock" ? "observe only" : "cluster lock: VarAC.db not found");
    return ok;
}

QString VaracLever::stamp() {
    return QDateTime::currentDateTimeUtc().toString("yyyy-MM-dd HH:mm:ss.zzz") + "0000Z";
}

bool VaracLever::write(const QString &what) {
    if (m_rw.databaseName() != m_db) { if (m_rw.isOpen()) m_rw.close(); m_rw.setDatabaseName(m_db); }
    if (!m_rw.isOpen() && !m_rw.open()) {
        m_log("VarAC: cluster lock " + what + " failed (" + m_rw.lastError().text().left(80) + ")");
        return false;
    }
    QString ts = stamp();
    QSqlQuery q(m_rw);
    bool ok = true;
    if (what == "hold") {
        q.prepare("SELECT id FROM instance WHERE guid = :g");
        q.bindValue(":g", GUID);
        bool exists = q.exec() && q.next();
        if (exists) {
            q.prepare("UPDATE instance SET busy=1, busy_last_time=:t, last_keepalive_time=:t2, is_deleted=0 WHERE guid=:g");
        } else {
            q.prepare("INSERT INTO instance (guid, name, comments, busy, busy_last_time, last_keepalive_time, "
                      "creation_time, is_deleted, email_gateway_active, email_gateway_sender_node, frequency, band, mode) "
                      "VALUES (:g, :n, 'hold placed by the arbiter', 1, :t, :t2, :t3, 0, 0, 0, 0, '', 'VARA')");
            q.bindValue(":n", NAME);
            q.bindValue(":t3", ts);
        }
        q.bindValue(":g", GUID);
        q.bindValue(":t", ts);
        q.bindValue(":t2", ts);
        ok = q.exec();
    } else if (what == "release") {
        q.prepare("UPDATE instance SET busy=0, busy_last_time=:t, last_keepalive_time=:t2 WHERE guid=:g");
        q.bindValue(":t", ts);
        q.bindValue(":t2", ts);
        q.bindValue(":g", GUID);
        ok = q.exec();
    } else if (what == "remove") {
        q.prepare("DELETE FROM instance WHERE guid = :g");
        q.bindValue(":g", GUID);
        ok = q.exec();
    }
    if (!ok) m_log("VarAC: cluster lock " + what + " failed (" + q.lastError().text().left(80) + ")");
    return ok;
}

bool VaracLever::hold() {
    if (!available() || m_held) return false;
    if (write("hold")) { m_held = true; return true; }
    return false;
}

void VaracLever::renew() {
    if (m_held) write("hold");
}

bool VaracLever::release() {
    if (!m_held) return false;
    m_held = false;
    return write("release");
}

void VaracLever::remove() {
    if (m_kind == "cluster_lock" && !m_db.isEmpty() && QFileInfo::exists(m_db)) write("remove");
}
