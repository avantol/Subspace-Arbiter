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
#include <QRadioButton>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextCursor>
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
    connect(setup, &QPushButton::clicked, this, [this]() { showSetup(false); });
    line->addWidget(setup);
    outer->addLayout(line);
    auto *views = new QHBoxLayout;
    auto *showTx = new QRadioButton("Show transmissions");
    showTx->setToolTip("Each transmit attempt of the three programs and what became of it");
    auto *showAll = new QRadioButton("Show details");
    showAll->setToolTip("The whole log");
    showAll->setChecked(true);
    views->addWidget(showTx);
    views->addWidget(showAll);
    views->addStretch(1);
    outer->addLayout(views);
    // a run where something is still missing after a few seconds opens the setup
    // box by itself, once (kSetupCheckMs = 8000, chosen: every connect has had its retry)
    QTimer::singleShot(8000, this, [this]() {
        if (!m_setupShown && !m_arb.setupComplete()) showSetup(true);
    });
    m_text = new QPlainTextEdit;
    m_text->setReadOnly(true);
    m_text->setMaximumBlockCount(500);
    QFont mono("monospace");
    mono.setStyleHint(QFont::TypeWriter);
    m_text->setFont(mono);
    m_text->setMinimumSize(900, 260);
    m_tx = new QTextEdit;
    m_tx->setReadOnly(true);
    m_tx->setLineWrapMode(QTextEdit::NoWrap);
    m_tx->document()->setMaximumBlockCount(500);
    m_tx->setFont(mono);
    m_tx->setMinimumSize(900, 260);
    m_tx->hide();
    for (const QString &l : m_arb.log().lines()) {
        m_text->appendPlainText(l);
        if (aboutTransmission(l)) addTransmission(l);
    }
    connect(&m_arb.log(), &Log::line, m_text, &QPlainTextEdit::appendPlainText);
    connect(&m_arb.log(), &Log::line, this, [this](const QString &l) {
        if (aboutTransmission(l)) addTransmission(l);
    });
    connect(showTx, &QRadioButton::toggled, this, [this](bool tx) {
        m_tx->setVisible(tx);
        m_text->setVisible(!tx);
    });
    outer->addWidget(m_text, 1);
    outer->addWidget(m_tx, 1);

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
        QString st = p->transmitting ? "TX" : (p->held ? "TX Disabled" : (p->waiting ? "Waiting" : (p->connected ? "RX" : "-")));
        row.state->setText(st);
        row.state->setStyleSheet(st == "TX" ? "color:#b00" : (st == "TX Disabled" || st == "Waiting") ? "color:#a60" : "");
        row.reason->setText(p->held ? p->reason : p->waitReason);
        QString left;
        if (p->held && p->leased)
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
    // The open setup box says what is true now. With the layer VarAC counts as connected
    // only once it has really connected (about 12 s after the arbiter's start under Wine,
    // measured 2026-11-22), later than the moment the box opens by itself: that box
    // closes when nothing is missing any more. A box the operator opened stays.
    if (m_setupBox) {
        if (m_setupBoxByItself && m_arb.setupComplete()) m_setupBox->close();
        else if (m_setupBox->text() != setupHead()) m_setupBox->setText(setupHead());
    }
}

bool MainWindow::aboutTransmission(const QString &line) {
    // the lines written by Arbiter.cpp, FldigiLink.cpp and VaraLayer.cpp for a transmit
    // attempt: it asked or started, what it waited for, its grant, and its end
    // "TX: PROGRAM message / start N: SENT ..." / "NOT SENT ...": written once for every
    // attempt, where it ends (Arbiter.cpp, VaraLayer.cpp). Also a keyed radio nobody owns.
    return line.contains("  TX: ") || line.contains("flrig: radio keyed, program unknown");
}

void MainWindow::addTransmission(const QString &line) {
    // the log's stamp is "yyyy-MM-dd HH:mm:ss.zzz +hhmm", two spaces, then the text (Log.cpp)
    QString text = line.mid(31);
    if (text.startsWith("TX: ")) text.remove(0, 4);
    QDateTime t = QDateTime::fromString(line.left(23), "yyyy-MM-dd HH:mm:ss.zzz");
    QString off = line.mid(24, 5);
    int offS = (off.mid(1, 2).toInt() * 3600 + off.mid(3, 2).toInt() * 60) * (off.startsWith('-') ? -1 : 1);
    t.setOffsetFromUtc(offS);
    QString when = t.isValid() ? t.toUTC().toString("HH:mm:ss") : line.mid(11, 8);
    // The backgrounds (CHOSEN, all light enough for black text): by program for a transmission
    // that was sent, one colour for every one that was not, one for what the arbiter cannot tell.
    QColor back("#fff3b0");                                        // not known, or nobody's keying
    if (text.contains(": NOT SENT")) back = QColor("#ffc8c8");      // red
    else if (text.contains(": SENT")) {
        if (text.startsWith("Subspace")) back = QColor("#c8f0c8");      // green
        else if (text.startsWith("VarAC")) back = QColor("#c8dcff");    // blue
        else if (text.startsWith("fldigi")) back = QColor("#e4ccf5");   // purple
    }
    QScrollBar *bar = m_tx->verticalScrollBar();
    bool atEnd = bar->value() == bar->maximum();
    QTextCursor c(m_tx->document());
    c.movePosition(QTextCursor::End);
    QTextBlockFormat bf;
    bf.setBackground(back);
    QTextCharFormat cf;
    cf.setForeground(Qt::black);
    if (m_tx->document()->isEmpty()) c.setBlockFormat(bf);
    else c.insertBlock(bf);
    c.insertText(when + " UTC  " + text, cf);
    if (atEnd) bar->setValue(bar->maximum());
}

QString MainWindow::setupHead() const {
    QStringList missing;
    for (Program *p : m_arb.programs())
        if (!p->connected) missing << p->name;
    return missing.isEmpty() ? "All three programs are connected."
                             : "Not reachable yet: " + missing.join(", ") + ".";
}

void MainWindow::showSetup(bool byItself) {
    m_setupShown = true;
    if (m_setupBox) {                 // already open: the operator asked for it, so it stays
        m_setupBoxByItself = m_setupBoxByItself && byItself;
        m_setupBox->raise();
        return;
    }
    auto *box = new QMessageBox(QMessageBox::Information, "Arbiter setup", setupHead(), QMessageBox::Ok, this);
    box->setInformativeText(m_arb.setupText());
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setModal(false);
    m_setupBox = box;
    m_setupBoxByItself = byItself;
    box->show();
}

void MainWindow::closeEvent(QCloseEvent *e) {
    m_arb.stop();
    e->accept();
    QApplication::quit();
}
