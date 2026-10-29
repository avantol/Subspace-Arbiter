#pragma once
// One line per state change: to the file, to the window, to the console.
#include <QFile>
#include <QObject>
#include <QStringList>
#include <QTextStream>

class Log : public QObject {
    Q_OBJECT
public:
    Log(const QString &path, bool echo, QObject *parent = nullptr);
    void operator()(const QString &msg);
    const QStringList &lines() const { return m_lines; }
    QString path() const { return m_file.fileName(); }
    static QString stamp();   // local time with the UTC offset, the shape CAT Watch writes

signals:
    void line(const QString &text);

private:
    QFile m_file;
    QTextStream m_out;
    bool m_echo;
    QStringList m_lines;      // the last 500, for the window
};
