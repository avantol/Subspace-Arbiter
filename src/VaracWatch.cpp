#include "VaracWatch.h"
#include "Settings.h"
#include "Log.h"
#include "VaracLever.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSqlError>
#include <QSqlQuery>
#include <QTextStream>
#include <QTimeZone>
#include <algorithm>

static double nowS() { return QDateTime::currentMSecsSinceEpoch() / 1000.0; }

static const QRegularExpression RE_VARA_LINE(
    R"(^(\d{1,2})/(\d{1,2})/(\d{4})\s+(\d\d):(\d\d):(\d\d)\s*-\s*(.*))");

QStringList VaracWatch::wineRoots() {
    QStringList out;
#ifndef Q_OS_WIN
    QDir home(QDir::homePath());
    for (const QString &d : home.entryList({".wine*"}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot))
        if (QFileInfo::exists(home.filePath(d + "/drive_c"))) out << home.filePath(d + "/drive_c");
    QDir prefixes(home.filePath(".local/share/wineprefixes"));
    for (const QString &d : prefixes.entryList(QDir::Dirs | QDir::NoDotAndDotDot))
        if (QFileInfo::exists(prefixes.filePath(d + "/drive_c"))) out << prefixes.filePath(d + "/drive_c");
#endif
    return out;
}

QStringList VaracWatch::winToHostPath(const QString &pIn) {
    QString p = pIn.trimmed();
    if (p.startsWith('"') && p.endsWith('"') && p.size() >= 2) p = p.mid(1, p.size() - 2);
    if (p.isEmpty()) return {};
#ifdef Q_OS_WIN
    return {p};
#else
    QStringList out;
    static const QRegularExpression drive(R"(^[A-Za-z]:[\\/](.*)$)");
    auto m = drive.match(p);
    QString rel = (m.hasMatch() ? m.captured(1) : p);
    rel.replace('\\', '/');
    for (const QString &c : wineRoots()) out << c + "/" + rel;
    return out;
#endif
}

QStringList VaracWatch::logFiles() {
    QStringList files;
    QStringList roots;
#ifdef Q_OS_WIN
    roots << qEnvironmentVariable("SystemDrive", "C:") + "\\";
#else
    roots = wineRoots();
#endif
    for (const QString &r : roots) {
        QDir root(r);
        for (const QString &vd : root.entryList({"VarAC*"}, QDir::Dirs | QDir::NoDotAndDotDot)) {
            QDir d(root.filePath(vd));
            for (const QString &ini : d.entryList({"VarAC*.ini"}, QDir::Files)) {
                QFile f(d.filePath(ini));
                if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
                QTextStream in(&f);
                while (!in.atEnd()) {
                    QString line = in.readLine();
                    if (line.startsWith("LogVARACommandsPath="))
                        files << winToHostPath(line.section('=', 1));
                }
            }
            for (const QString &lg : d.entryList({"VarAC_vara_commands*.log"}, QDir::Files))
                files << d.filePath(lg);
        }
    }
    QStringList found;
    for (const QString &f : files)
        if (!f.isEmpty() && QFileInfo::exists(f) && !found.contains(f)) found << f;
    std::sort(found.begin(), found.end(), [](const QString &a, const QString &b) {
        return QFileInfo(a).lastModified() < QFileInfo(b).lastModified();
    });
    return found;
}

VaracWatch::VaracWatch(const Settings &s, Log &log, QObject *parent)
    : QObject(parent), m_log(log), m_fixedPath(s.varacLogPath), m_db(s.varacDbPath),
      m_staleS(s.varaStaleS), m_deadS(s.varacDeadS) {
    m_ro = QSqlDatabase::addDatabase("QSQLITE", "varac-ro");
    m_ro.setConnectOptions("QSQLITE_OPEN_READONLY");
    m_timer.setInterval(250);
    connect(&m_timer, &QTimer::timeout, this, &VaracWatch::step);
    m_timer.start();
    QTimer::singleShot(0, this, &VaracWatch::step);
}

void VaracWatch::step() {
    double now = nowS();
    if (now - m_looked > LOOK_S || m_path.isEmpty()) {
        m_looked = now;
        QString p;
        if (!m_fixedPath.isEmpty()) p = QFileInfo::exists(m_fixedPath) ? m_fixedPath : QString();
        else { QStringList f = logFiles(); if (!f.isEmpty()) p = f.last(); }
        if (p != m_path) {
            m_path = p;
            m_pos = -1;
            m_linkOpen = false;
            m_ptt = false;
            m_status = p.isEmpty() ? "log not found (VarAC: turn on 'Log VARA commands', restart VarAC)"
                                   : "following " + p;
            m_log("VarAC: " + m_status);
        }
    }
    if (m_path.isEmpty()) return;
    QFile f(m_path);
    qint64 size = f.size();
    if (m_pos < 0) {
        // start from the tail: the last CONNECTED / DISCONNECTED decides the link state
        m_pos = std::max<qint64>(0, size - 65536);
        if (f.open(QIODevice::ReadOnly)) {
            f.seek(m_pos);
            QString chunk = QString::fromUtf8(f.readAll());
            m_pos = f.pos();
            lines(chunk, false);
        }
        m_lastLineT = QFileInfo(m_path).lastModified().toMSecsSinceEpoch() / 1000.0;
        return;
    }
    if (size < m_pos) { m_pos = 0; return; }
    if (size == m_pos) return;
    if (!f.open(QIODevice::ReadOnly)) return;
    f.seek(m_pos);
    QString chunk = QString::fromUtf8(f.read(1'000'000));
    m_pos = f.pos();
    lines(chunk, true);
}

void VaracWatch::lines(const QString &chunk, bool live) {
    double now = nowS();
    for (const QString &raw : chunk.split('\n')) {
        auto m = RE_VARA_LINE.match(raw.trimmed());
        if (!m.hasMatch()) continue;
        QString msg = m.captured(7).trimmed().toUpper();
        if (live) m_lastLineT = now;
        if (msg.startsWith("DISCONNECTED")) {
            if (m_linkOpen && live) m_log("VarAC: link closed");
            m_linkOpen = false;
            if (live) m_burstT = 0;            // the link is over: no burst hold lingers after it
        } else if (msg.startsWith("CONNECTED")) {
            if (!m_linkOpen && live) m_log("VarAC: link open");
            m_linkOpen = true;
        } else if (!live) {
            // old PTT lines in the tail at start-up are history
        } else if (msg.startsWith("PTT ON")) {
            m_ptt = true; m_pttT = now; m_burstT = now;
        } else if (msg.startsWith("PTT OFF")) {
            m_ptt = false; m_pttT = now; m_burstT = now;
        } else if (msg.startsWith("PENDING")) {
            m_burstT = now;                    // a connect request: a PTT follows within a second
        }
    }
}

void VaracWatch::checkAlive(double now) {
    if (now - m_dbCheckedT < 5.0) return;
    m_dbCheckedT = now;
    if (m_db.isEmpty() && !m_path.isEmpty()) {
        QString cand = QFileInfo(m_path).absolutePath() + "/VarAC.db";
        if (QFileInfo::exists(cand)) m_db = cand;
    }
    if (m_db.isEmpty() || !QFileInfo::exists(m_db)) { m_alive = -1; return; }
    if (m_ro.databaseName() != m_db) { if (m_ro.isOpen()) m_ro.close(); m_ro.setDatabaseName(m_db); }
    if (!m_ro.isOpen() && !m_ro.open()) { m_alive = -1; return; }
    QSqlQuery q(m_ro);
    q.prepare("SELECT MAX(last_keepalive_time) FROM instance WHERE guid != :g");
    q.bindValue(":g", VaracLever::GUID);
    if (!q.exec() || !q.next() || q.value(0).toString().isEmpty()) { m_alive = -1; return; }
    // 2026-10-29 20:58:28.9007671Z: UTC, seven fraction digits
    QDateTime t = QDateTime::fromString(q.value(0).toString().left(19), "yyyy-MM-dd HH:mm:ss");
    if (!t.isValid()) { m_alive = -1; return; }
    t.setTimeZone(QTimeZone::utc());
    m_keepaliveT = t.toMSecsSinceEpoch() / 1000.0;
    bool alive = now - m_keepaliveT < m_deadS;
    if (m_alive == 1 && !alive)
        m_log(QString("VarAC: its keepalive stopped %1 s ago, treating VarAC as gone").arg(int(now - m_keepaliveT)));
    m_alive = alive ? 1 : 0;
}

VaracWatch::Snapshot VaracWatch::snapshot() {
    double now = nowS();
    checkAlive(now);
    bool link;
    if (m_alive != -1) link = m_linkOpen && m_alive == 1;            // VarAC's own keepalive decides
    else link = m_linkOpen && (now - m_lastLineT) < m_staleS;         // fallback: log silence
    if (m_linkOpen && !link) {
        m_linkOpen = false;
        m_log(QString("VarAC: link treated as gone (%1)")
                  .arg(m_alive == 0 ? "VarAC's keepalive stopped" : QString("log silent %1 s").arg(int(m_staleS))));
    }
    Snapshot s;
    s.link = link;
    s.ptt = m_ptt;
    s.pttT = m_pttT;
    s.burstT = m_burstT;
    return s;
}
