#include "VaraLayer.h"
#include "Log.h"
#include "Settings.h"

#include <QDateTime>
#include <QHostAddress>
#include <QTimer>

static double nowS() { return QDateTime::currentMSecsSinceEpoch() / 1000.0; }

// chosen: how often VARA's port is tried again while VARA is not listening yet
static constexpr int kDialRetryMs = 250;

VaraLayer::VaraLayer(const Settings &s, Log &log, QObject *parent)
    : QObject(parent), m_log(log), m_varaWaitS(s.layerVaraWaitS), m_logLines(s.layerLogLines) {
    open(m_cmd, "command", s.layerCmdPort, s.layerVaraCmdPort);
    open(m_data, "data", s.layerCmdPort + 1, s.layerVaraCmdPort + 1);
    open(m_kiss, "KISS", s.layerKissPort, s.layerVaraKissPort);
    m_listening = m_portTrouble.isEmpty();
    if (m_listening)
        m_log(QString("layer: VarAC side %1 / %2 / KISS %3, VARA side %4 / %5 / KISS %6")
                  .arg(m_cmd.listenPort).arg(m_data.listenPort).arg(m_kiss.listenPort)
                  .arg(m_cmd.varaPort).arg(m_data.varaPort).arg(m_kiss.varaPort));
    else
        m_log("layer: cannot listen (" + m_portTrouble + "); VarAC cannot reach VARA through the arbiter");
}

void VaraLayer::open(Pipe &p, const QString &name, int listenPort, int varaPort) {
    p.name = name;
    p.listenPort = listenPort;
    p.varaPort = varaPort;
    connect(&p.server, &QTcpServer::newConnection, this, [this, &p]() { accepted(p); });
    if (!p.server.listen(QHostAddress::LocalHost, quint16(listenPort))) {
        if (!m_portTrouble.isEmpty()) m_portTrouble += "; ";
        m_portTrouble += QString("port %1: %2").arg(listenPort).arg(p.server.errorString());
    }
}

void VaraLayer::accepted(Pipe &p) {
    while (QTcpSocket *sock = p.server.nextPendingConnection()) {
        if (p.varac || p.vara) closePipe(p, "VarAC connected again");
        ++p.gen;
        p.varac = sock;
        p.waitLogged = false;
        sock->setSocketOption(QAbstractSocket::LowDelayOption, 1);
        connect(sock, &QTcpSocket::readyRead, this, [this, &p, sock]() {
            if (p.varac == sock) fromVaracSide(p);
        });
        connect(sock, &QTcpSocket::disconnected, this, [this, &p, sock]() {
            if (p.varac == sock) closePipe(p, "VarAC closed it");
        });
        m_log(QString("layer: %1: VarAC connected").arg(p.name));
        p.dialUntil = nowS() + m_varaWaitS;
        dial(p);
    }
}

void VaraLayer::dial(Pipe &p) {
    auto *sock = new QTcpSocket(this);
    int gen = p.gen;
    p.vara = sock;
    p.varaOpen = false;
    connect(sock, &QTcpSocket::connected, this, [this, &p, sock]() {
        if (p.vara != sock) return;
        sock->setSocketOption(QAbstractSocket::LowDelayOption, 1);
        p.varaOpen = true;
        m_log(QString("layer: %1: connected to VARA on %2").arg(p.name).arg(p.varaPort));
        if (&p == &m_cmd) emit changed();
        fromVaracSide(p);   // what VarAC sent while VARA's side was not open yet
    });
    connect(sock, &QTcpSocket::readyRead, this, [this, &p, sock]() {
        if (p.vara == sock) fromVaraSide(p);
    });
    connect(sock, &QTcpSocket::disconnected, this, [this, &p, sock]() {
        if (p.vara == sock && p.varaOpen) closePipe(p, "VARA closed it");
    });
    connect(sock, &QTcpSocket::errorOccurred, this, [this, &p, sock, gen](QAbstractSocket::SocketError) {
        if (p.vara != sock) return;
        if (p.varaOpen) {
            closePipe(p, "VARA side: " + sock->errorString().left(60));
            return;
        }
        // VARA is not listening yet: VarAC's side stays open and unread meanwhile
        sock->disconnect(this);
        sock->deleteLater();
        p.vara = nullptr;
        if (nowS() >= p.dialUntil) {
            closePipe(p, QString("VARA did not listen on %1 within %2 s").arg(p.varaPort).arg(int(m_varaWaitS)));
            return;
        }
        if (!p.waitLogged) m_log(QString("layer: %1: VARA not listening on %2 yet; waiting").arg(p.name).arg(p.varaPort));
        p.waitLogged = true;
        QTimer::singleShot(kDialRetryMs, this, [this, &p, gen]() {
            if (p.gen == gen && p.varac && !p.vara) dial(p);
        });
    });
    sock->connectToHost(QHostAddress::LocalHost, quint16(p.varaPort));
}

void VaraLayer::closePipe(Pipe &p, const QString &why) {
    if (!p.varac && !p.vara) return;
    ++p.gen;
    QTcpSocket *both[2] = {p.varac, p.vara};
    p.varac = p.vara = nullptr;
    p.varaOpen = false;
    p.fromVarac.clear();
    p.fromVara.clear();
    for (QTcpSocket *s : both) {
        if (!s) continue;
        s->disconnect(this);
        s->flush();
        s->abort();
        s->deleteLater();
    }
    m_log(QString("layer: %1: closed (%2)").arg(p.name, why));
    if (&p != &m_cmd) return;
    // VarAC gave up everything it had asked for; a hold ends with its connection
    dropKept("the command connection closed");
    noEnd("the command connection closed");
    m_held = m_deaf = m_varaBusy = m_oweDisconnected = false;
    m_ownOk = 0;
    m_varacListen.clear();
    m_link = m_ptt = false;
    m_burstT = 0;
    if (!m_closing) emit changed();
}

// -- bytes in ------------------------------------------------------------------
void VaraLayer::fromVaracSide(Pipe &p) {
    if (!p.varac || !p.varaOpen) return;   // nothing is read from VarAC until VARA's side is open
    QByteArray data = p.varac->readAll();
    if (data.isEmpty()) return;
    if (&p == &m_cmd) {
        int gen = p.gen;
        p.fromVarac += data.replace('\n', '\r');
        int i;
        while (p.gen == gen && (i = p.fromVarac.indexOf('\r')) >= 0) {
            QString line = QString::fromLatin1(p.fromVarac.left(i)).trimmed();
            p.fromVarac.remove(0, i + 1);
            if (!line.isEmpty()) varacLine(line);
        }
        return;
    }
    if (&p == &m_kiss) {
        // VarAC sends on the KISS port only what VARA is to transmit (a broadcast)
        QString shown = QString("%1 bytes %2").arg(data.size()).arg(QString::fromLatin1(data.left(30).toHex()));
        if (m_held) {
            keep(true, data, "KISS frame, " + shown);
            return;
        }
        say("KISS VarAC -> VARA: " + shown);
        int n = ++m_startN;
        m_log(QString("VarAC: start %1 given to VARA at once: KISS frame, %2 bytes").arg(n).arg(data.size()));
        passed(n, false);
        started("a frame on the KISS port");
    }
    p.vara->write(data);
    p.vara->flush();
}

void VaraLayer::fromVaraSide(Pipe &p) {
    if (!p.vara || !p.varac) return;
    QByteArray data = p.vara->readAll();
    if (data.isEmpty()) return;
    if (&p == &m_cmd) {
        int gen = p.gen;
        p.fromVara += data.replace('\n', '\r');
        int i;
        while (p.gen == gen && (i = p.fromVara.indexOf('\r')) >= 0) {
            QString line = QString::fromLatin1(p.fromVara.left(i)).trimmed();
            p.fromVara.remove(0, i + 1);
            if (!line.isEmpty()) varaLine(line);
        }
        return;
    }
    p.varac->write(data);
    p.varac->flush();
}

// -- the command lines -----------------------------------------------------------
void VaraLayer::varacLine(const QString &line) {
    QString word = line.toUpper();
    if (word.startsWith("CONNECT ") || word.startsWith("CQFRAME ")) {
        if (m_held) {
            keep(false, line.toLatin1(), line);
            return;
        }
        int n = ++m_startN;
        m_log(QString("VarAC: start %1 given to VARA at once: %2").arg(n).arg(line));
        passed(n, word.startsWith("CONNECT "));
        started(line);
    } else if (word == "ABORT" || word == "DISCONNECT") {
        // VARA never saw a kept CONNECT, so it answers this line with OK and nothing more;
        // VarAC waits for DISCONNECTED (measured 2026-11-20 17:06). The layer says it
        // after VARA's OK.
        if (dropKept("VarAC sent " + line)) m_oweDisconnected = true;
    } else if (word.startsWith("LISTEN")) {
        // VarAC sends LISTEN ON and LISTEN CQ once, when it connects (8 starts, 2026-11-20)
        if (m_varacListen.isEmpty() || m_varacListen.last() != line) m_varacListen << line;
    }
    toVara(line, false);
    if (m_deaf && word.startsWith("LISTEN") && word != "LISTEN OFF") toVara("LISTEN OFF", true);
}

void VaraLayer::varaLine(const QString &line) {
    QString word = line.toUpper();
    if (word == "BUSY ON" || word == "BUSY OFF") {
        m_varaBusy = word == "BUSY ON";
        if (m_held) {
            say("VARA said, kept back during the hold: " + line);
            return;
        }
    }
    if (word == "OK" && m_ownOk > 0) {
        --m_ownOk;
        say("VARA said, kept back (answer to the layer's own line): OK");
        return;
    }
    double now = nowS();
    bool fact = true;
    if (word.startsWith("DISCONNECTED")) {
        if (m_open.n && m_open.connect) endOpen("no link");
        if (m_link) m_log("VarAC: link closed");
        m_link = false;
        m_burstT = 0;   // the link is over: no burst hold lingers after it
    } else if (word.startsWith("CONNECTED")) {
        if (m_open.n && m_open.connect) endOpen("link open");
        if (!m_link) m_log("VarAC: link open");
        m_link = true;
    } else if (word.startsWith("PTT ON")) {
        m_ptt = true; m_pttT = now; m_burstT = now;
        if (m_open.n && ++m_open.keyed == 1) m_open.onT = now;
    } else if (word.startsWith("PTT OFF")) {
        m_ptt = false; m_pttT = now; m_burstT = now;
        // a CQ, a beacon or a broadcast is one transmission: PENDING, PTT ON, PTT OFF
        // (measured 2026-11-22, 12 of 12 on the station). A CONNECT keys several times.
        if (m_open.n && !m_open.connect && m_open.keyed > 0) {
            endOpen("");
            // its end is known, so no burst hold lingers after it (the hold is for the pauses
            // inside a link and between connect requests). WM8Q 2026-11-22 16:37: a Subspace
            // message waited 12.1 s after a broadcast's PTT OFF and missed its slot.
            if (!m_link) m_burstT = 0;
        }
    } else if (word.startsWith("PENDING")) {
        m_burstT = now;
    } else {
        fact = false;
    }
    toVarac(line);
    if (word == "OK" && m_oweDisconnected) {
        m_oweDisconnected = false;
        toVarac("DISCONNECTED");
        m_log("layer: VarAC told DISCONNECTED (its kept CONNECT never reached VARA)");
    }
    if (fact) emit changed();
}

void VaraLayer::toVarac(const QString &line) {
    if (!m_cmd.varac) return;
    QString word = line.toUpper();
    if (word != "IAMALIVE" && !word.startsWith("BUFFER")) say("-> VarAC: " + line);
    m_cmd.varac->write(line.toLatin1() + '\r');
    m_cmd.varac->flush();
}

void VaraLayer::toVara(const QString &line, bool own) {
    if (!m_cmd.vara || !m_cmd.varaOpen) return;
    if (own) ++m_ownOk;
    say(QString(own ? "layer -> VARA: " : "VarAC -> VARA: ") + line);
    m_cmd.vara->write(line.toLatin1() + '\r');
    m_cmd.vara->flush();
}

void VaraLayer::say(const QString &text) {
    if (m_logLines) m_log("layer: " + text);
}

// -- start commands ----------------------------------------------------------------
void VaraLayer::started(const QString &) {
    // VARA transmits within a second of a start command (PENDING / PTT ON follow); the
    // other programs are held from this instant, not from VARA's first PTT ON
    m_burstT = nowS();
    emit changed();
}

void VaraLayer::passed(int n, bool connect) {
    noEnd(QString("start %1 followed it").arg(n));
    m_open = Open{};
    m_open.n = n;
    m_open.t = nowS();
    m_open.connect = connect;
}

void VaraLayer::endOpen(const QString &how) {
    QString keyed = QString("VARA keyed %1 time%2").arg(m_open.keyed).arg(m_open.keyed == 1 ? "" : "s");
    double s = nowS() - m_open.t;        // since VARA got the start command
    double air = nowS() - m_open.onT;    // since VARA's first PTT ON for it
    if (how.isEmpty())
        m_log(QString("VarAC: transmission over (start %1, on the air %2 s)").arg(m_open.n).arg(air, 0, 'f', 1));
    else
        m_log(QString("VarAC: start %1 over: %2 (%3 s after VARA got the connect command, %4)").arg(m_open.n).arg(how).arg(s, 0, 'f', 1).arg(keyed));
    // the one line for "Show transmissions": sent means VARA keyed for it
    if (how.isEmpty())
        m_log(QString("TX: VarAC start %1: SENT, completed (key-up to end %2 s)").arg(m_open.n).arg(air, 0, 'f', 1));
    else if (m_open.keyed > 0)
        m_log(QString("TX: VarAC start %1: SENT, a connect: %2 (%3)").arg(m_open.n).arg(how, keyed));
    else
        m_log(QString("TX: VarAC start %1: NOT SENT, a connect: %2 (VARA never keyed)").arg(m_open.n).arg(how));
    m_open = Open{};
}

void VaraLayer::noEnd(const QString &why) {
    if (!m_open.n) return;
    m_log(QString("VarAC: start %1: no end seen (%2; VARA keyed %3 time%4)")
              .arg(m_open.n).arg(why).arg(m_open.keyed).arg(m_open.keyed == 1 ? "" : "s"));
    m_log(m_open.keyed > 0
              ? QString("TX: VarAC start %1: SENT (VARA keyed; no end seen: %2)").arg(m_open.n).arg(why)
              : QString("TX: VarAC start %1: NOT SENT (VARA never keyed; %2)").arg(m_open.n).arg(why));
    m_open = Open{};
}

void VaraLayer::keep(bool kiss, const QByteArray &payload, const QString &shown) {
    // VarAC sends a CQ as two identical CQFRAME lines 10 s apart on its own clock, without
    // waiting for an answer (measured 2026-11-20 17:11). Two passed on at once: VARA sends
    // one and answers WRONG to the other. An identical line is kept once.
    int n = ++m_startN;
    if (!kiss) {
        for (const Kept &k : m_kept) {
            if (!k.kiss && k.payload == payload) {
                m_log(QString("VarAC: start %1: the same line is already kept (start %2), not kept twice: %3")
                          .arg(n).arg(k.n).arg(shown));
                m_log(QString("TX: VarAC start %1: NOT SENT (the same line as start %2, which is kept once)").arg(n).arg(k.n));
                return;
            }
        }
    }
    m_kept.append({kiss, payload, nowS(), n});
    m_log(QString("VarAC: start %1 kept back until the release: %2").arg(n).arg(shown));
}

bool VaraLayer::dropKept(const QString &why) {
    bool hadConnect = false;
    double now = nowS();
    for (const Kept &k : m_kept) {
        if (isConnect(k)) hadConnect = true;
        m_log(QString("VarAC: start %1 thrown away after %2 s (%3): %4")
                  .arg(k.n).arg(now - k.t, 0, 'f', 1)
                  .arg(why, k.kiss ? QString("KISS frame, %1 bytes").arg(k.payload.size())
                                   : QString::fromLatin1(k.payload)));
        m_log(QString("TX: VarAC start %1: NOT SENT (kept back %2 s, then thrown away: %3)")
                  .arg(k.n).arg(now - k.t, 0, 'f', 1).arg(why));
    }
    m_kept.clear();
    return hadConnect;
}

void VaraLayer::passKept() {
    QList<Kept> kept;
    kept.swap(m_kept);
    double now = nowS();
    for (const Kept &k : kept) {
        QTcpSocket *to = k.kiss ? (m_kiss.varaOpen ? m_kiss.vara : nullptr)
                                : (m_cmd.varaOpen ? m_cmd.vara : nullptr);
        QString shown = k.kiss ? QString("KISS frame, %1 bytes").arg(k.payload.size())
                               : QString::fromLatin1(k.payload);
        if (!to) {
            m_log(QString("VarAC: start %1 not given to VARA (VARA's side is closed): %2").arg(k.n).arg(shown));
            m_log(QString("TX: VarAC start %1: NOT SENT (VARA's side is closed)").arg(k.n));
            continue;
        }
        to->write(k.kiss ? k.payload : k.payload + '\r');
        to->flush();
        m_log(QString("VarAC: start %1 given to VARA after %2 s: %3").arg(k.n).arg(now - k.t, 0, 'f', 1).arg(shown));
        passed(k.n, isConnect(k));
        m_burstT = now;
    }
}

// -- the hold ------------------------------------------------------------------------
bool VaraLayer::hold(bool listening) {
    if (!m_held) {
        if (!connected()) return false;
        m_held = true;
        toVarac("BUSY ON");
    }
    setDeaf(!listening);
    return true;
}

void VaraLayer::setDeaf(bool deaf) {
    if (deaf == m_deaf) return;
    m_deaf = deaf;
    if (deaf) toVara("LISTEN OFF", true);
    else for (const QString &line : m_varacListen) toVara(line, true);
}

bool VaraLayer::release() {
    if (!m_held) return false;
    m_held = false;
    passKept();
    setDeaf(false);
    if (m_varaBusy) say("release: VARA itself says busy; BUSY OFF left to VARA");
    else toVarac("BUSY OFF");
    return true;
}

VaracWatch::Snapshot VaraLayer::snapshot() const {
    VaracWatch::Snapshot s;
    s.link = m_link;
    s.ptt = m_ptt;
    s.pttT = m_pttT;
    s.burstT = m_burstT;
    return s;
}

QString VaraLayer::status() const {
    if (!m_listening) return "layer: cannot listen (" + m_portTrouble + ")";
    if (!m_cmd.varac) return QString("layer: waiting for VarAC on port %1").arg(m_cmd.listenPort);
    if (!m_cmd.varaOpen) return QString("layer: VarAC connected, VARA not listening on port %1").arg(m_cmd.varaPort);
    QString s = "layer: VarAC and VARA connected";
    if (m_held) s += m_deaf ? "; VarAC told BUSY, VARA told LISTEN OFF" : "; VarAC told BUSY, VARA listening";
    if (!m_kept.isEmpty()) s += QString("; %1 start command(s) kept").arg(m_kept.size());
    return s;
}

void VaraLayer::close() {
    if (m_closing) return;
    release();   // a closing arbiter leaves VarAC and VARA as they would be without it
    m_closing = true;
    for (Pipe *p : {&m_cmd, &m_data, &m_kiss}) {
        p->server.close();
        closePipe(*p, "arbiter closing");
    }
}
