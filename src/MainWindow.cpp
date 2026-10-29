#include "MainWindow.h"
#include "Arbiter.h"
#include "FlrigWatch.h"
#include "Log.h"
#include "config.h"

#include <QApplication>
#include <QCloseEvent>
#include <QDateTime>
#include <QFont>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QVBoxLayout>

MainWindow::MainWindow(Arbiter &arb, QWidget *parent) : QWidget(parent), m_arb(arb) {
    setWindowTitle(QString("Arbiter %1").arg(ARBITER_VERSION));
    auto *outer = new QVBoxLayout(this);
    auto *grid = new QGridLayout;
    outer->addLayout(grid);

    QFont bold = font();
    bold.setBold(true);
    const QStringList heads = {"Program", "Connection", "State", "Tx disable reason", "Renew in", "Enable Arb", ""};
    for (int c = 0; c < heads.size(); ++c) {
        auto *h = new QLabel(heads[c]);
        h->setFont(bold);
        grid->addWidget(h, 0, c);
    }
    int r = 1;
    for (Program *p : m_arb.programs()) {
        Row row;
        row.p = p;
        grid->addWidget(new QLabel(p->name), r, 0);
        row.conn = new QLabel; row.conn->setMinimumWidth(320);
        row.state = new QLabel; row.state->setMinimumWidth(90);
        row.reason = new QLabel; row.reason->setMinimumWidth(130);
        row.renew = new QLabel; row.renew->setMinimumWidth(60);
        grid->addWidget(row.conn, r, 1);
        grid->addWidget(row.state, r, 2);
        grid->addWidget(row.reason, r, 3);
        grid->addWidget(row.renew, r, 4);
        row.enable = new QCheckBox;
        row.enable->setChecked(p->enabled);
        connect(row.enable, &QCheckBox::toggled, this, [this, p](bool on) { m_arb.setEnabled(*p, on); });
        grid->addWidget(row.enable, r, 5, Qt::AlignCenter);
        auto *btn = new QPushButton("Release");
        connect(btn, &QPushButton::clicked, this, [this, p]() { m_arb.operatorRelease(*p); });
        grid->addWidget(btn, r, 6);
        m_rows.append(row);
        ++r;
    }
    grid->setColumnStretch(1, 1);
    auto *line = new QHBoxLayout;
    m_flrig = new QLabel;
    line->addWidget(m_flrig, 1);
    auto *setup = new QPushButton("Setup...");
    setup->setToolTip("What to set in Subspace, fldigi and VarAC, once");
    connect(setup, &QPushButton::clicked, this, &MainWindow::showSetup);
    line->addWidget(setup);
    outer->addLayout(line);
    // a run where something is still missing after a few seconds opens the setup
    // box by itself, once (kSetupCheckMs = 8000, chosen: every connect has had its retry)
    QTimer::singleShot(8000, this, [this]() {
        if (!m_setupShown && !m_arb.setupComplete()) showSetup();
    });
    m_text = new QPlainTextEdit;
    m_text->setReadOnly(true);
    m_text->setMaximumBlockCount(500);
    QFont mono("monospace");
    mono.setStyleHint(QFont::TypeWriter);
    m_text->setFont(mono);
    m_text->setMinimumSize(900, 260);
    for (const QString &l : m_arb.log().lines()) m_text->appendPlainText(l);
    connect(&m_arb.log(), &Log::line, m_text, &QPlainTextEdit::appendPlainText);
    outer->addWidget(m_text, 1);

    m_timer.setInterval(250);
    connect(&m_timer, &QTimer::timeout, this, &MainWindow::refresh);
    m_timer.start();
    refresh();
}

void MainWindow::refresh() {
    double now = QDateTime::currentMSecsSinceEpoch() / 1000.0;
    for (Row &row : m_rows) {
        Program *p = row.p;
        row.conn->setText(p->status.left(140));
        QString st = p->transmitting ? "TX" : (p->held ? "TX Disabled" : (p->connected ? "RX" : "-"));
        row.state->setText(st);
        row.state->setStyleSheet(st == "TX" ? "color:#b00" : st == "TX Disabled" ? "color:#a60" : "");
        row.reason->setText(p->reason);
        QString left;
        if (p->held && p != &m_arb.fldigi)
            left = QString("%1 s").arg(qMax(0, int(p->renewS - (now - p->renewedT))));
        row.renew->setText(left);
        if (row.enable->isChecked() != p->enabled) {
            QSignalBlocker b(row.enable);
            row.enable->setChecked(p->enabled);
        }
    }
    FlrigWatch &f = m_arb.flrig();
    QStringList busy;
    for (const QString &r : Reason::ORDER) if (m_arb.sources().value(r)) busy << r;
    m_flrig->setText(QString("flrig: %1   PTT %2   dial %3   busy: %4")
                         .arg(f.up() ? "connected" : "not answering",
                              f.ptt == 1 ? "ON" : (f.ptt == 0 ? "off" : "?"),
                              f.vfo >= 0 ? QString::number(f.vfo) : "?",
                              busy.isEmpty() ? "none" : busy.join(", ")));
}

void MainWindow::showSetup() {
    m_setupShown = true;
    QStringList missing;
    for (Program *p : m_arb.programs())
        if (!p->connected) missing << p->name;
    QString head = missing.isEmpty() ? "All three programs are connected."
                                     : "Not reachable yet: " + missing.join(", ") + ".";
    auto *box = new QMessageBox(QMessageBox::Information, "Arbiter setup", head, QMessageBox::Ok, this);
    box->setInformativeText(Arbiter::setupText());
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setModal(false);
    box->show();
}

void MainWindow::closeEvent(QCloseEvent *e) {
    m_arb.stop();
    e->accept();
    QApplication::quit();
}
