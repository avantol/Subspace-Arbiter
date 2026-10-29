#pragma once
#include <QCheckBox>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
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
    void showSetup();

    Arbiter &m_arb;
    QList<Row> m_rows;
    QLabel *m_flrig;
    QPlainTextEdit *m_text;
    QTimer m_timer;
    bool m_setupShown = false;
};
