// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/thumbs/Isolated.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QImageReader>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <atomic>
#include <memory>

namespace eu::thumbs {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("eu::thumbs", text);
}

std::atomic<qint64> s_timeoutMs{30000};

QString helperProgram()
{
    static const QString program = [] {
#ifdef Q_OS_WIN
        const QString name = QStringLiteral("eu-thumbnailer.exe");
#else
        const QString name = QStringLiteral("eu-thumbnailer");
#endif
        const QString besideUs = QCoreApplication::applicationDirPath() + QLatin1Char('/') + name;
        return QFile::exists(besideUs) ? besideUs : QString();
    }();
    return program;
}

/// El auxiliar de un hilo. Se arranca cuando hace falta y se vuelve a
/// arrancar si se cerró o hubo que matarlo.
class Helper
{
public:
    ~Helper() { stop(); }

    core::Result<QImage> render(const ThumbnailRequest &request, const tasks::CancelToken &cancel)
    {
        if (!ensureRunning()) {
            return renderThumbnail(request, cancel);   // sin auxiliar: aquí mismo
        }
        const QString out = m_scratch->filePath(QStringLiteral("miniatura.png"));
        QFile::remove(out);
        QJsonObject message{{QStringLiteral("path"), request.path},
                            {QStringLiteral("size"), double(request.size)},
                            {QStringLiteral("mtime"), double(request.modifiedMs)},
                            {QStringLiteral("maxSide"), request.maxSide},
                            {QStringLiteral("dir"), request.isDirectory},
                            {QStringLiteral("out"), out}};
        m_process->write(QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n');
        m_process->waitForBytesWritten(5000);

        QElapsedTimer timer;
        timer.start();
        while (!m_process->canReadLine()) {
            if (cancel.isCancelled()) {
                // No se le puede pedir que deje lo que está haciendo: se cierra
                // y el siguiente arranca otro.
                stop();
                return core::Error{core::ErrorCode::Cancelled, tr("Cancelado"), request.path, 0};
            }
            if (m_process->state() != QProcess::Running) {
                stop();
                return core::Error{core::ErrorCode::Corrupted,
                                   tr("El archivo hizo cerrarse al lector de miniaturas"), request.path, 0};
            }
            if (timer.elapsed() > s_timeoutMs.load()) {
                stop();
                return core::Error{core::ErrorCode::Timeout, tr("El archivo tardó demasiado en leerse"),
                                   request.path, 0};
            }
            m_process->waitForReadyRead(50);
        }
        const QJsonObject answer = QJsonDocument::fromJson(m_process->readLine()).object();
        if (!answer.value(QStringLiteral("ok")).toBool()) {
            return core::Error{core::ErrorCode(answer.value(QStringLiteral("code")).toInt(int(core::ErrorCode::Unknown))),
                               answer.value(QStringLiteral("message")).toString(), request.path, 0};
        }
        QImageReader reader(out, "png");
        QImage image = reader.read();
        QFile::remove(out);
        if (image.isNull()) {
            return core::Error{core::ErrorCode::IoError, tr("No se pudo leer la miniatura generada"), request.path, 0};
        }
        return image.convertToFormat(image.hasAlphaChannel() ? QImage::Format_ARGB32_Premultiplied
                                                             : QImage::Format_RGB32);
    }

private:
    bool ensureRunning()
    {
        if (m_process && m_process->state() == QProcess::Running) {
            return true;
        }
        const QString program = helperProgram();
        if (program.isEmpty()) {
            return false;
        }
        if (!m_scratch) {
            m_scratch = std::make_unique<QTemporaryDir>();
            if (!m_scratch->isValid()) {
                m_scratch.reset();
                return false;
            }
        }
        m_process = std::make_unique<QProcess>();
        // Lo que el auxiliar escriba por su salida de errores (avisos de las
        // bibliotecas) no interesa y no debe llenar ninguna tubería.
        m_process->setProcessChannelMode(QProcess::SeparateChannels);
        m_process->setStandardErrorFile(QProcess::nullDevice());
        m_process->start(program, {});
        if (!m_process->waitForStarted(10000)) {
            m_process.reset();
            return false;
        }
        return true;
    }

    void stop()
    {
        if (m_process) {
            m_process->kill();
            m_process->waitForFinished(5000);
            m_process.reset();
        }
    }

    std::unique_ptr<QProcess> m_process;
    std::unique_ptr<QTemporaryDir> m_scratch;   ///< Donde el auxiliar deja cada imagen.
};

Helper &helperForThisThread()
{
    thread_local Helper helper;
    return helper;
}

} // namespace

bool hasThumbnailHelper()
{
    return !helperProgram().isEmpty();
}

void setThumbnailHelperTimeout(std::chrono::milliseconds timeout)
{
    s_timeoutMs = timeout.count();
}

core::Result<QImage> renderThumbnailIsolated(const ThumbnailRequest &request, const tasks::CancelToken &cancel)
{
    if (cancel.isCancelled()) {
        return core::Error{core::ErrorCode::Cancelled, tr("Cancelado"), request.path, 0};
    }
    if (thumbnailSourceFor(request) == ThumbnailSource::None) {
        return core::Error{core::ErrorCode::NotSupported, tr("No hay miniatura para este tipo de archivo"),
                           request.path, 0};
    }
    return helperForThisThread().render(request, cancel);
}

} // namespace eu::thumbs
