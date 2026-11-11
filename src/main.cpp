// Arbiter for a shared radio: Subspace, fldigi and VarAC through flrig.
// Whatever is already transmitting finishes. The arbiter holds only what has
// not started, by throwing each program's own switch, and it never refuses,
// swallows or delays a PTT. A client of flrig, never a link in the CAT chain.
#include "Arbiter.h"
#include "Log.h"
#include "MainWindow.h"
#include "Settings.h"
#include "config.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QMessageBox>
#include <QStandardPaths>
#include <QTextStream>
#include <cstdio>
#include <memory>

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <QSocketNotifier>
#include <csignal>
#include <sys/socket.h>
#include <unistd.h>
static int sigFds[2] = {-1, -1};
static void onSignal(int) { char c = 1; if (::write(sigFds[0], &c, 1) < 0) {} }
#endif

int main(int argc, char *argv[]) {
    // --console needs no display: a plain core application then (headless Linux
    // boxes); so do --version, --help and --write-ini, which never open a window
    bool consoleMode = false;
    for (int i = 1; i < argc; ++i) {
        QString a = QString::fromLocal8Bit(argv[i]);
        if (a == "--console" || a == "--version" || a == "-v" || a == "--help" || a == "-h" ||
            a == "--write-ini")
            consoleMode = true;
    }
    std::unique_ptr<QCoreApplication> appPtr;
    if (consoleMode) appPtr = std::make_unique<QCoreApplication>(argc, argv);
    else appPtr = std::make_unique<QApplication>(argc, argv);
    QCoreApplication &app = *appPtr;
    app.setApplicationName("Arbiter");
    app.setApplicationVersion(ARBITER_VERSION);

    QCommandLineParser cli;
    cli.setApplicationDescription("Arbiter for a shared radio (Subspace, fldigi, VarAC through flrig).");
    cli.addHelpOption();
    cli.addVersionOption();
    QCommandLineOption iniOpt({"i", "ini"}, "settings file (default: the config folder's arbiter.ini, if it exists)", "file");
    QCommandLineOption consoleOpt("console", "no window; log to the console");
    QCommandLineOption writeIniOpt("write-ini", "write the documented default settings to <file> and exit", "file");
    cli.addOption(iniOpt);
    cli.addOption(consoleOpt);
    cli.addOption(writeIniOpt);
    cli.process(app);

    if (cli.isSet(writeIniOpt)) {
        QFile f(cli.value(writeIniOpt));
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) { std::fprintf(stderr, "cannot write %s\n", qPrintable(f.fileName())); return 1; }
        QTextStream(&f) << Settings::defaultIniText();
        std::printf("wrote %s\n", qPrintable(f.fileName()));
        return 0;
    }

    QString ini = cli.isSet(iniOpt) ? cli.value(iniOpt)
                                    : QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + "/arbiter.ini";
    Settings s = Settings::load(ini);
    QString logPath = s.logFile;
    if (QFileInfo(logPath).isRelative()) {
        QString base = cli.isSet(iniOpt) ? QFileInfo(ini).absolutePath()
                                         : QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        QDir().mkpath(base);
        logPath = base + "/" + logPath;
    }
    bool window = !cli.isSet(consoleOpt);

    // one Arbiter per PC (per --ini on the bench): two would release each
    // other's holds. Checked before the log is opened or anything is sent.
    // Stale time 0: a live copy's lock is never taken over by age; a crashed
    // copy's lock is (its process is gone).
    QString lockDir = QFileInfo(logPath).absolutePath();
    QDir().mkpath(lockDir);
    QLockFile lock(lockDir + "/arbiter.lock");
    lock.setStaleLockTime(0);
    if (!lock.tryLock(0) && lock.error() == QLockFile::LockFailedError) {
        qint64 pid = 0;
        QString host, appName;
        lock.getLockInfo(&pid, &host, &appName);
        QString msg = pid > 0 ? QString("Arbiter is already running (pid %1). This copy will close.").arg(pid)
                              : QString("Arbiter is already running. This copy will close.");
        std::fprintf(stderr, "%s\n", qPrintable(msg));
        if (window) QMessageBox::information(nullptr, "Arbiter", msg);
        return 2;
    }

    Log log(logPath, !window);
    if (!lock.isLocked())
        log(QString("arbiter: cannot create %1 (error %2): a second copy would not be stopped")
                .arg(QDir::toNativeSeparators(lockDir + "/arbiter.lock")).arg(int(lock.error())));
    Arbiter arb(s, log);

    // every exit path releases every hold
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &arb, &Arbiter::stop);
#ifdef Q_OS_WIN
    SetConsoleCtrlHandler([](DWORD) -> BOOL { QMetaObject::invokeMethod(qApp, &QCoreApplication::quit, Qt::QueuedConnection); return TRUE; }, TRUE);
#else
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sigFds) == 0) {
        auto *sn = new QSocketNotifier(sigFds[1], QSocketNotifier::Read, &app);
        QObject::connect(sn, &QSocketNotifier::activated, &app, [](int) {
            char c;
            if (::read(sigFds[1], &c, 1) < 0) {}
            QCoreApplication::quit();
        });
        for (int sig : {SIGINT, SIGTERM, SIGHUP}) std::signal(sig, onSignal);
    }
#endif

    MainWindow *win = nullptr;
    if (window) {
        win = new MainWindow(arb);
        win->show();
    }
    arb.start();
    int rc = app.exec();
    arb.stop();
    delete win;
    appPtr.reset();
    return rc;
}
