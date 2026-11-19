// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "OfficeThumbnail.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QPainter>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTransform>
#include <QUrl>

#ifdef EU_HAVE_LIBARCHIVE
#include <archive.h>
#include <archive_entry.h>
#endif
#ifdef EU_HAVE_LIBRAW
#include <libraw/libraw.h>
#endif

#include <algorithm>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>

namespace eu::thumbs::detail {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("eu::thumbs", text);
}

core::Error notSupported(const QString &path, const QString &why)
{
    return core::Error{core::ErrorCode::NotSupported, why, path, 0};
}

QImage fitted(QImage image, int maxSide)
{
    if (image.width() > maxSide || image.height() > maxSide) {
        image = image.scaled(maxSide, maxSide, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    // Un documento sin fondo propio es papel blanco, no transparente.
    QImage page(image.size(), QImage::Format_RGB32);
    page.fill(Qt::white);
    QPainter painter(&page);
    painter.drawImage(0, 0, image);
    painter.end();
    return page;
}

#ifdef EU_HAVE_LIBARCHIVE
/// Lee una entrada de un ZIP (los documentos de oficina lo son), como mucho
/// `maxBytes`. Vacío si no está.
QByteArray readZipEntry(const QString &path, const QStringList &names, qint64 maxBytes)
{
    struct ArchiveDeleter
    {
        void operator()(archive *handle) const { archive_read_free(handle); }
    };
    std::unique_ptr<archive, ArchiveDeleter> handle(archive_read_new());
    archive_read_support_format_zip(handle.get());
#ifdef Q_OS_WIN
    // En Windows, con la ruta en UTF-16: con la de 8 bits fallarían los nombres con acentos.
    const int opened = archive_read_open_filename_w(
        handle.get(), reinterpret_cast<const wchar_t *>(QDir::toNativeSeparators(path).utf16()), 64 * 1024);
#else
    const int opened = archive_read_open_filename(handle.get(), QFile::encodeName(path).constData(), 64 * 1024);
#endif
    if (opened != ARCHIVE_OK) {
        return {};
    }
    archive_entry *entry = nullptr;
    while (archive_read_next_header(handle.get(), &entry) == ARCHIVE_OK) {
        const QString name = QString::fromUtf8(archive_entry_pathname(entry));
        if (!names.contains(name, Qt::CaseInsensitive)) {
            archive_read_data_skip(handle.get());
            continue;
        }
        const la_int64_t size = archive_entry_size(entry);
        if (size <= 0 || size > maxBytes) {
            return {};
        }
        QByteArray data(qsizetype(size), Qt::Uninitialized);
        const la_ssize_t read = archive_read_data(handle.get(), data.data(), size_t(size));
        return read == size ? data : QByteArray();
    }
    return {};
}
#endif

/// Un solo LibreOffice a la vez: cada uno ocupa cientos de MB y, con el
/// mismo perfil, dos a la vez se estorban.
std::mutex &libreOfficeMutex()
{
    static std::mutex mutex;
    return mutex;
}

core::Result<QImage> renderWithLibreOffice(const ThumbnailRequest &request, const tasks::CancelToken &cancel)
{
    const QString program = libreOfficeProgram();
    if (program.isEmpty()) {
        return notSupported(request.path, tr("LibreOffice no está instalado"));
    }
    std::unique_lock<std::mutex> lock(libreOfficeMutex(), std::defer_lock);
    while (!lock.try_lock()) {
        if (cancel.isCancelled()) {
            return core::Error{core::ErrorCode::Cancelled, tr("Cancelado"), request.path, 0};
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    QTemporaryDir output;
    if (!output.isValid()) {
        return core::Error{core::ErrorCode::IoError, tr("No se pudo crear una carpeta temporal"), request.path, 0};
    }
    // Perfil propio: no choca con un LibreOffice que el usuario tenga abierto
    // ni toca su configuración. Se reutiliza entre conversiones (crearlo tarda).
    const QString profile = QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
        + QStringLiteral("/libreoffice-miniaturas");
    QDir().mkpath(profile);
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(program, {QStringLiteral("-env:UserInstallation=") + QUrl::fromLocalFile(profile).toString(),
                            QStringLiteral("--headless"), QStringLiteral("--norestore"),
                            QStringLiteral("--nolockcheck"), QStringLiteral("--convert-to"), QStringLiteral("png"),
                            QStringLiteral("--outdir"), output.path(), request.path});
    if (!process.waitForStarted(10000)) {
        return notSupported(request.path, tr("No se pudo arrancar LibreOffice"));
    }
    // Tiempo máximo: un documento enorme o dañado no deja la miniatura colgada.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!process.waitForFinished(100)) {
        if (cancel.isCancelled() || std::chrono::steady_clock::now() > deadline) {
            process.kill();
            process.waitForFinished(5000);
            return cancel.isCancelled()
                ? core::Error{core::ErrorCode::Cancelled, tr("Cancelado"), request.path, 0}
                : core::Error{core::ErrorCode::Timeout, tr("LibreOffice tardó demasiado"), request.path, 0};
        }
    }
    const QString png = output.filePath(QFileInfo(request.path).completeBaseName() + QStringLiteral(".png"));
    QImageReader reader(png, "png");
    const QImage image = reader.read();
    if (image.isNull()) {
        return core::Error{core::ErrorCode::Corrupted, tr("LibreOffice no pudo convertir el documento"),
                           request.path, 0};
    }
    return fitted(image, std::max(16, request.maxSide));
}

} // namespace

bool hasOfficeSupport()
{
#ifdef EU_HAVE_LIBARCHIVE
    return true;
#else
    return !libreOfficeProgram().isEmpty();
#endif
}

QString libreOfficeProgram()
{
    static const QString program = [] {
        for (const QString &name : {QStringLiteral("soffice"), QStringLiteral("libreoffice")}) {
            const QString found = QStandardPaths::findExecutable(name);
            if (!found.isEmpty()) {
                return found;
            }
        }
#ifdef Q_OS_WIN
        const QString windows = QStringLiteral("C:/Program Files/LibreOffice/program/soffice.exe");
        if (QFile::exists(windows)) {
            return windows;
        }
#elif defined(Q_OS_MACOS)
        const QString mac = QStringLiteral("/Applications/LibreOffice.app/Contents/MacOS/soffice");
        if (QFile::exists(mac)) {
            return mac;
        }
#endif
        return QString();
    }();
    return program;
}

core::Result<QImage> renderOffice(const ThumbnailRequest &request, const tasks::CancelToken &cancel)
{
    const int maxSide = std::max(16, request.maxSide);
#ifdef EU_HAVE_LIBARCHIVE
    // 1. La miniatura que lleva el propio archivo.
    const QByteArray embedded = readZipEntry(
        request.path,
        {QStringLiteral("Thumbnails/thumbnail.png"), QStringLiteral("docProps/thumbnail.jpeg"),
         QStringLiteral("docProps/thumbnail.jpg"), QStringLiteral("docProps/thumbnail.png")},
        8 * 1024 * 1024);
    if (!embedded.isEmpty()) {
        const QImage image = QImage::fromData(embedded);
        if (!image.isNull()) {
            return fitted(image, maxSide);
        }
    }
#endif
    if (cancel.isCancelled()) {
        return core::Error{core::ErrorCode::Cancelled, tr("Cancelado"), request.path, 0};
    }
    // 2. LibreOffice, si está.
    return renderWithLibreOffice(request, cancel);
}

bool hasRawSupport()
{
#ifdef EU_HAVE_LIBRAW
    return true;
#else
    return false;
#endif
}

core::Result<QImage> renderRaw(const ThumbnailRequest &request, const tasks::CancelToken &cancel)
{
#ifndef EU_HAVE_LIBRAW
    Q_UNUSED(cancel);
    return notSupported(request.path, tr("Esta compilación no lee fotos RAW"));
#else
    auto raw = std::make_unique<LibRaw>();
#ifdef Q_OS_WIN
    const int opened = raw->open_file(reinterpret_cast<const wchar_t *>(request.path.utf16()));
#else
    const int opened = raw->open_file(QFile::encodeName(request.path).constData());
#endif
    if (opened != LIBRAW_SUCCESS) {
        return core::Error{core::ErrorCode::Corrupted, tr("No se pudo leer la foto RAW"), request.path, 0};
    }
    if (cancel.isCancelled()) {
        return core::Error{core::ErrorCode::Cancelled, tr("Cancelado"), request.path, 0};
    }
    // La vista previa que guarda la cámara: casi siempre un JPEG grande.
    if (raw->unpack_thumb() != LIBRAW_SUCCESS) {
        return core::Error{core::ErrorCode::NotSupported, tr("La foto RAW no trae vista previa"), request.path, 0};
    }
    int error = 0;
    libraw_processed_image_t *thumbnail = raw->dcraw_make_mem_thumb(&error);
    if (thumbnail == nullptr) {
        return core::Error{core::ErrorCode::Corrupted, tr("No se pudo leer la vista previa"), request.path, 0};
    }
    QImage image;
    if (thumbnail->type == LIBRAW_IMAGE_JPEG) {
        image = QImage::fromData(thumbnail->data, int(thumbnail->data_size), "jpeg");
    } else if (thumbnail->type == LIBRAW_IMAGE_BITMAP && thumbnail->colors == 3 && thumbnail->bits == 8) {
        image = QImage(thumbnail->data, thumbnail->width, thumbnail->height, thumbnail->width * 3,
                       QImage::Format_RGB888)
                    .copy();
    }
    LibRaw::dcraw_clear_mem(thumbnail);
    if (image.isNull()) {
        return core::Error{core::ErrorCode::Corrupted, tr("Vista previa en un formato desconocido"), request.path, 0};
    }
    // Giro anotado por la cámara (LibRaw: 3 = 180°, 5 = 90° a la izquierda, 6 = 90° a la derecha).
    switch (raw->imgdata.sizes.flip) {
    case 3: image = image.transformed(QTransform().rotate(180)); break;
    case 5: image = image.transformed(QTransform().rotate(-90)); break;
    case 6: image = image.transformed(QTransform().rotate(90)); break;
    default: break;
    }
    const int maxSide = std::max(16, request.maxSide);
    if (image.width() > maxSide || image.height() > maxSide) {
        image = image.scaled(maxSide, maxSide, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    return image.convertToFormat(QImage::Format_RGB32);
#endif
}

} // namespace eu::thumbs::detail
