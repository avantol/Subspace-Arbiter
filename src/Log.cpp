#include "Log.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <cstdio>

Log::Log(const QString &path, bool echo, QObject *parent)
    : QObject(parent), m_file(path), m_echo(echo) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    m_file.open(QIODevice::Append | QIODevice::Text);
    m_out.setDevice(&m_file);
}

QString Log::stamp() {
    QDateTime now = QDateTime::currentDateTime();
    int off = now.offsetFromUtc();
    QString tz = QString("%1%2%3").arg(off < 0 ? '-' : '+')
                     .arg(qAbs(off) / 3600, 2, 10, QChar('0')).arg((qAbs(off) % 3600) / 60, 2, 10, QChar('0'));
    return now.toString("yyyy-MM-dd HH:mm:ss.zzz") + " " + tz;
}

void Log::operator()(const QString &msg) {
    QString text = stamp() + "  " + msg;
    m_lines.append(text);
    if (m_lines.size() > 500) m_lines.removeFirst();
    if (m_file.isOpen()) {
        m_out << text << '\n';
        m_out.flush();
    }
    if (m_echo) {
        std::fputs(qPrintable(text), stdout);
        std::fputc('\n', stdout);
        std::fflush(stdout);
    }
    emit line(text);
}
