#pragma once
#include <QCheckBox>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QTextEdit>
#include <QTimer>
#include <QWidget>

class Arbiter;
struct Program;

class MainWindow : public QWidget {
    Q_OBJECT
public:
    explicit MainWindow(Arbiter &arb, QWidget *parent = nullptr);

protected:
    void closeEvent(QCloseEvent *e) override;

private:
    struct Row {
        Program *p;
        QLabel *conn, *state, *reason, *renew;
        QCheckBox *enable;
    };
    void refresh();
    void showSetup(bool byItself);
    QString setupHead() const;

    Arbiter &m_arb;
    // the open setup box: its first line follows the programs' state, and a box that
    // opened by itself closes when nothing is missing any more
    QPointer<QMessageBox> m_setupBox;
    bool m_setupBoxByItself = false;
    QList<Row> m_rows;
    QLabel *m_flrig;
    QPlainTextEdit *m_text;
    // [study] "Show transmissions": only the log lines that tell of a transmit attempt and
    // what became of it. The same lines as in the detailed log, nothing written twice.
    // One line per attempt, the time in UTC, the line's background by program and result.
    QTextEdit *m_tx;
    static bool aboutTransmission(const QString &line);
    void addTransmission(const QString &line);
    QTimer m_timer;
    bool m_setupShown = false;
};
