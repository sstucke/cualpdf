#include "logger.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTextStream>

#include <csignal>
#include <cstring>

#ifndef Q_OS_WIN
#include <execinfo.h>
#endif

static QFile *g_logFile = nullptr;

static void flushLog()
{
    if (g_logFile)
        g_logFile->flush();
}

// Writes raw return addresses to the log, no debug symbols required at
// crash time. Resolve them afterwards with:
//   addr2line -e <path-to-cualpdf-binary> -f -C <address>
// (run against the *same* binary that crashed, or the addresses won't
// mean anything).
static void logBacktrace()
{
#ifndef Q_OS_WIN
    if (!g_logFile || !g_logFile->isOpen())
        return;

    void *addresses[64];
    const int count = backtrace(addresses, 64);
    char **symbols = backtrace_symbols(addresses, count);

    QTextStream out(g_logFile);
    out << "Backtrace (" << count << " frames; resolve with addr2line -e <binary> -f -C <address>):\n";
    for (int i = 0; i < count; ++i)
        out << "  #" << i << " " << (symbols ? symbols[i] : "???") << "\n";
    out.flush();

    if (symbols)
        free(symbols);
#endif
}

static void crashHandler(int sig)
{
    if (g_logFile && g_logFile->isOpen()) {
        QTextStream out(g_logFile);
        out << QDateTime::currentDateTime().toString(Qt::ISODateWithMs) << " [FATAL] Crashed with signal "
            << sig;
#ifndef Q_OS_WIN
        out << " (" << strsignal(sig) << ")";
#endif
        out << "\n";
        out.flush();
    }
    logBacktrace();
    flushLog();
    signal(sig, SIG_DFL);
    raise(sig);
}

static void messageHandler(QtMsgType type, const QMessageLogContext &context, const QString &msg)
{
    if (!g_logFile || !g_logFile->isOpen())
        return;

    const char *level = [type]() -> const char * {
        switch (type) {
        case QtDebugMsg:    return "DEBUG";
        case QtInfoMsg:     return "INFO ";
        case QtWarningMsg:  return "WARN ";
        case QtCriticalMsg: return "ERROR";
        case QtFatalMsg:    return "FATAL";
        }
        return "?????";
    }();

    QTextStream out(g_logFile);
    out << QDateTime::currentDateTime().toString(Qt::ISODateWithMs)
        << " [" << level << "] " << msg;
    if (context.file)
        out << "  (" << context.file << ":" << context.line << ")";
    out << "\n";

    if (type == QtFatalMsg) {
        out.flush();
        flushLog();
        abort();
    }
}

void Logger::install()
{
    const QString dataDir =
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QDir().mkpath(dataDir);

    const QString logPath = dataDir + QStringLiteral("/cualpdf.log");
    const QString oldPath = dataDir + QStringLiteral("/cualpdf.log.old");

    QFile::remove(oldPath);
    QFile::rename(logPath, oldPath);

    g_logFile = new QFile(logPath);
    if (!g_logFile->open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        delete g_logFile;
        g_logFile = nullptr;
        return;
    }

    qInstallMessageHandler(messageHandler);

    signal(SIGSEGV, crashHandler);
    signal(SIGABRT, crashHandler);
#ifndef Q_OS_WIN
    signal(SIGBUS, crashHandler);
#endif

    qInfo() << "cualpdf" << QCoreApplication::applicationVersion() << "started";
    qInfo() << "Log file:" << logPath;
}

QString Logger::logFilePath()
{
    return g_logFile ? g_logFile->fileName() : QString();
}
