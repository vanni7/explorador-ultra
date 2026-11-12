// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/core/Log.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QMutex>
#include <QMutexLocker>

#include <cstdio>

namespace eu::core {

namespace {

/// Estado del registro. Un único mutex protege todo: se escribe poco y así
/// las líneas de hilos distintos nunca se mezclan.
struct LogState
{
    QMutex mutex;
    LogOptions options;
    QFile file;
    QtMessageHandler previousHandler = nullptr;
    bool active = false;
};

LogState &state()
{
    static LogState instance;
    return instance;
}

QString filePath(const LogOptions &options, int index)
{
    const QString suffix = (index == 0) ? QStringLiteral(".log") : QStringLiteral(".%1.log").arg(index);
    return QDir(options.directory).filePath(options.baseName + suffix);
}

/// Desplaza los archivos: .log -> .1.log -> .2.log … y borra el más antiguo.
/// Se llama con el mutex tomado.
void rotate(LogState &log)
{
    log.file.close();
    const int oldest = log.options.maxFiles - 1;
    QFile::remove(filePath(log.options, oldest));
    for (int index = oldest - 1; index >= 0; --index) {
        QFile::rename(filePath(log.options, index), filePath(log.options, index + 1));
    }
    log.file.setFileName(filePath(log.options, 0));
    if (!log.file.open(QIODevice::WriteOnly | QIODevice::Append)) {
        log.active = false;   // sin archivo, se sigue registrando solo en stderr
    }
}

const char *levelName(QtMsgType type)
{
    switch (type) {
    case QtDebugMsg:    return "DEBUG";
    case QtInfoMsg:     return "INFO ";
    case QtWarningMsg:  return "AVISO";
    case QtCriticalMsg: return "ERROR";
    case QtFatalMsg:    return "FATAL";
    }
    return "?";
}

void messageHandler(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    LogState &log = state();
    QMutexLocker locker(&log.mutex);

    const QByteArray line = QStringLiteral("%1 %2 [%3] %4\n")
        .arg(QDateTime::currentDateTime().toString(Qt::ISODateWithMs),
             QLatin1String(levelName(type)),
             QLatin1String(context.category ? context.category : "default"),
             message)
        .toUtf8();

    if (log.active && log.file.isOpen()) {
        if (log.file.size() + line.size() > log.options.maxFileBytes) {
            rotate(log);
        }
        log.file.write(line);
        log.file.flush();   // si la aplicación se cierra de golpe, la última línea ya está en disco
    }
    if (log.options.alsoToStderr || !log.active) {
        std::fwrite(line.constData(), 1, size_t(line.size()), stderr);
    }
}

} // namespace

bool startFileLogging(const LogOptions &options)
{
    LogState &log = state();
    QMutexLocker locker(&log.mutex);

    log.file.close();
    log.options = options;
    if (log.options.maxFiles < 1) {
        log.options.maxFiles = 1;
    }

    const bool directoryReady = QDir().mkpath(options.directory);
    log.file.setFileName(filePath(log.options, 0));
    const bool opened = directoryReady && log.file.open(QIODevice::WriteOnly | QIODevice::Append);

    log.active = opened;
    locker.unlock();   // qInstallMessageHandler no debe llamarse con el mutex tomado

    const QtMessageHandler previous = qInstallMessageHandler(messageHandler);
    if (previous != messageHandler) {
        log.previousHandler = previous;
    }
    return opened;
}

void stopFileLogging()
{
    LogState &log = state();
    qInstallMessageHandler(log.previousHandler);
    QMutexLocker locker(&log.mutex);
    log.file.close();
    log.active = false;
}

QString currentLogFile()
{
    LogState &log = state();
    QMutexLocker locker(&log.mutex);
    return log.active ? log.file.fileName() : QString();
}

} // namespace eu::core
