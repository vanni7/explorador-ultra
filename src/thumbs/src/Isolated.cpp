// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/thumbs/Isolated.h"

#include "eu/content/DocumentPreview.h"
#include "eu/content/TextExtractor.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QImageReader>
#include <QJsonArray>
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

    bool available() { return ensureRunning(); }

    /// Envía una petición y espera la respuesta, vigilando que el auxiliar
    /// no se cierre ni se cuelgue.
    core::Result<QJsonObject> ask(const QJsonObject &message, const QString &path, const tasks::CancelToken &cancel)
    {
        m_process->write(QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n');
        m_process->waitForBytesWritten(5000);
        QElapsedTimer timer;
        timer.start();
        while (!m_process->canReadLine()) {
            if (cancel.isCancelled()) {
                // No se le puede pedir que deje lo que está haciendo: se cierra
                // y el siguiente arranca otro.
                stop();
                return core::Error{core::ErrorCode::Cancelled, tr("Cancelado"), path, 0};
            }
            if (m_process->state() != QProcess::Running) {
                stop();
                return core::Error{core::ErrorCode::Crashed,
                                   tr("El archivo hizo cerrarse al lector de miniaturas"), path, 0};
            }
            if (timer.elapsed() > s_timeoutMs.load()) {
                stop();
                return core::Error{core::ErrorCode::Timeout, tr("El archivo tardó demasiado en leerse"), path, 0};
            }
            m_process->waitForReadyRead(50);
        }
        return QJsonDocument::fromJson(m_process->readLine()).object();
    }

    QList<meta::Section> metadata(const QString &path, const tasks::CancelToken &cancel)
    {
        if (!ensureRunning()) {
            return meta::readMetadata(path, cancel);
        }
        const core::Result<QJsonObject> answer =
            ask({{QStringLiteral("op"), QStringLiteral("meta")}, {QStringLiteral("path"), path}}, path, cancel);
        QList<meta::Section> sections;
        if (!answer.ok()) {
            return sections;
        }
        const QJsonArray list = answer.value().value(QStringLiteral("sections")).toArray();
        for (const QJsonValue &item : list) {
            const QJsonObject object = item.toObject();
            meta::Section section{object.value(QStringLiteral("title")).toString(), {}};
            const QJsonArray fields = object.value(QStringLiteral("fields")).toArray();
            for (const QJsonValue &field : fields) {
                const QJsonArray pair = field.toArray();
                section.fields.append({pair.at(0).toString(), pair.at(1).toString()});
            }
            sections.append(section);
        }
        return sections;
    }

    core::Result<QString> text(const QString &path, const tasks::CancelToken &cancel)
    {
        if (!ensureRunning()) {
            return content::extractText(path, cancel);
        }
        const QString out = m_scratch->filePath(QStringLiteral("texto.txt"));
        QFile::remove(out);
        const core::Result<QJsonObject> reply =
            ask({{QStringLiteral("op"), QStringLiteral("text")}, {QStringLiteral("path"), path},
                 {QStringLiteral("out"), out}},
                path, cancel);
        if (!reply.ok()) {
            return reply.error();
        }
        if (!reply.value().value(QStringLiteral("ok")).toBool()) {
            return core::Error{core::ErrorCode(reply.value().value(QStringLiteral("code")).toInt(int(core::ErrorCode::Unknown))),
                               reply.value().value(QStringLiteral("message")).toString(), path, 0};
        }
        QFile file(out);
        if (!file.open(QIODevice::ReadOnly)) {
            return core::Error{core::ErrorCode::IoError, tr("No se pudo leer el texto extraído"), path, 0};
        }
        const QString text = QString::fromUtf8(file.readAll());
        file.close();
        QFile::remove(out);
        return text;
    }

    core::Result<QImage> pdfPage(const QString &path, int page, const QSize &size, int *pageCount,
                                 const tasks::CancelToken &cancel)
    {
        if (!ensureRunning()) {
            return renderPdfPage(path, page, size, pageCount);
        }
        const QString out = m_scratch->filePath(QStringLiteral("pagina.png"));
        QFile::remove(out);
        const core::Result<QJsonObject> reply =
            ask({{QStringLiteral("op"), QStringLiteral("pdfpage")}, {QStringLiteral("path"), path},
                 {QStringLiteral("page"), page}, {QStringLiteral("width"), size.width()},
                 {QStringLiteral("height"), size.height()}, {QStringLiteral("out"), out}},
                path, cancel);
        if (!reply.ok()) {
            return reply.error();
        }
        if (pageCount != nullptr) {
            *pageCount = reply.value().value(QStringLiteral("pages")).toInt();
        }
        if (!reply.value().value(QStringLiteral("ok")).toBool()) {
            return core::Error{core::ErrorCode(reply.value().value(QStringLiteral("code")).toInt(int(core::ErrorCode::Unknown))),
                               reply.value().value(QStringLiteral("message")).toString(), path, 0};
        }
        if (page < 0) {
            return QImage();
        }
        QImageReader reader(out, "png");
        const QImage image = reader.read();
        QFile::remove(out);
        if (image.isNull()) {
            return core::Error{core::ErrorCode::IoError, tr("No se pudo leer la página pintada"), path, 0};
        }
        return image;
    }

    core::Result<content::DocumentPreview> document(const QString &path, const tasks::CancelToken &cancel)
    {
        if (!ensureRunning()) {
            return content::documentPreview(path, cancel);
        }
        const QString out = m_scratch->filePath(QStringLiteral("documento.json"));
        QFile::remove(out);
        const core::Result<QJsonObject> reply =
            ask({{QStringLiteral("op"), QStringLiteral("preview")}, {QStringLiteral("path"), path},
                 {QStringLiteral("out"), out}},
                path, cancel);
        if (!reply.ok()) {
            return reply.error();
        }
        if (!reply.value().value(QStringLiteral("ok")).toBool()) {
            return core::Error{core::ErrorCode(reply.value().value(QStringLiteral("code")).toInt(int(core::ErrorCode::Unknown))),
                               reply.value().value(QStringLiteral("message")).toString(), path, 0};
        }
        QFile file(out);
        if (!file.open(QIODevice::ReadOnly)) {
            return core::Error{core::ErrorCode::IoError, tr("No se pudo leer la vista previa del documento"), path, 0};
        }
        const QJsonObject object = QJsonDocument::fromJson(file.readAll()).object();
        file.close();
        QFile::remove(out);
        return content::documentPreviewFromJson(object);
    }

    core::Result<QImage> render(const ThumbnailRequest &request, const tasks::CancelToken &cancel)
    {
        if (!ensureRunning()) {
            return renderThumbnail(request, cancel);   // sin auxiliar: aquí mismo
        }
        const QString out = m_scratch->filePath(QStringLiteral("miniatura.png"));
        QFile::remove(out);
        const core::Result<QJsonObject> reply = ask({{QStringLiteral("path"), request.path},
                                                     {QStringLiteral("size"), double(request.size)},
                                                     {QStringLiteral("mtime"), double(request.modifiedMs)},
                                                     {QStringLiteral("maxSide"), request.maxSide},
                                                     {QStringLiteral("dir"), request.isDirectory},
                                                     {QStringLiteral("out"), out}},
                                                    request.path, cancel);
        if (!reply.ok()) {
            return reply.error();
        }
        const QJsonObject answer = reply.value();
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

core::Result<QString> extractTextIsolated(const QString &path, const tasks::CancelToken &cancel)
{
    if (cancel.isCancelled()) {
        return core::Error{core::ErrorCode::Cancelled, tr("Cancelado"), path, 0};
    }
    // El texto plano se lee aquí: no hay decodificador que pueda romperse y
    // es lo más abundante en una búsqueda.
    if (content::textSourceFor(path) == content::TextSource::Plain) {
        return content::extractText(path, cancel);
    }
    return helperForThisThread().text(path, cancel);
}

core::Result<content::DocumentPreview> documentPreviewIsolated(const QString &path, const tasks::CancelToken &cancel)
{
    if (cancel.isCancelled()) {
        return core::Error{core::ErrorCode::Cancelled, tr("Cancelado"), path, 0};
    }
    return helperForThisThread().document(path, cancel);
}

core::Result<QImage> renderPdfPageIsolated(const QString &path, int page, const QSize &size, int *pageCount,
                                           const tasks::CancelToken &cancel)
{
    if (cancel.isCancelled()) {
        return core::Error{core::ErrorCode::Cancelled, tr("Cancelado"), path, 0};
    }
    return helperForThisThread().pdfPage(path, page, size, pageCount, cancel);
}

QList<meta::Section> readMetadataIsolated(const QString &path, const tasks::CancelToken &cancel)
{
    if (cancel.isCancelled()) {
        return {};
    }
    return helperForThisThread().metadata(path, cancel);
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
