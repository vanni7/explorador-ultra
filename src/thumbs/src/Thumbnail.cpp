// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/thumbs/Thumbnail.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QImageReader>
#include <QSet>

namespace eu::thumbs {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("eu::thumbs", text);
}

/// Formatos de imagen que Qt sabe leer en esta instalación (complementos
/// incluidos: SVG, WebP, TIFF…), en minúsculas.
const QSet<QString> &readableImageSuffixes()
{
    static const QSet<QString> suffixes = [] {
        QSet<QString> set;
        const QList<QByteArray> formats = QImageReader::supportedImageFormats();
        for (const QByteArray &format : formats) {
            set.insert(QString::fromLatin1(format).toLower());
        }
        // Extensiones que no coinciden con el nombre del formato.
        if (set.contains(QStringLiteral("jpeg"))) {
            set.insert(QStringLiteral("jpg"));
            set.insert(QStringLiteral("jpe"));
            set.insert(QStringLiteral("jfif"));
        }
        if (set.contains(QStringLiteral("tiff"))) {
            set.insert(QStringLiteral("tif"));
        }
        if (set.contains(QStringLiteral("svg"))) {
            set.insert(QStringLiteral("svgz"));
        }
        // Qt PDF trae un complemento de imagen "pdf": las páginas se tratarán
        // aparte (Fase 3, paso del PDF), no como una imagen más.
        set.remove(QStringLiteral("pdf"));
        return set;
    }();
    return suffixes;
}

/// Lado mayor a partir del cual una imagen se rechaza sin decodificarla: una
/// imagen de 100 000 × 100 000 píxeles ocuparía 40 GB en memoria.
constexpr int maxSourceSide = 30000;

} // namespace

ThumbnailSource thumbnailSourceFor(const QString &fileName)
{
    const QString suffix = QFileInfo(fileName).suffix().toLower();
    if (!suffix.isEmpty() && readableImageSuffixes().contains(suffix)) {
        return ThumbnailSource::Image;
    }
    return ThumbnailSource::None;
}

core::Result<QImage> renderThumbnail(const ThumbnailRequest &request, const tasks::CancelToken &cancel)
{
    if (cancel.isCancelled()) {
        return core::Error{core::ErrorCode::Cancelled, tr("Cancelado"), request.path, 0};
    }
    if (thumbnailSourceFor(request.path) != ThumbnailSource::Image) {
        return core::Error{core::ErrorCode::NotSupported, tr("No hay miniatura para este tipo de archivo"),
                           request.path, 0};
    }
    QImageReader reader(request.path);
    // Gira según la etiqueta EXIF de orientación (fotos de móvil en vertical).
    reader.setAutoTransform(true);
    reader.setDecideFormatFromContent(true);   // una extensión equivocada no impide leerla

    const QSize original = reader.size();
    if (original.isValid()
        && (original.width() > maxSourceSide || original.height() > maxSourceSide)) {
        return core::Error{core::ErrorCode::NotSupported, tr("La imagen es demasiado grande para la miniatura"),
                           request.path, 0};
    }
    const int maxSide = std::max(16, request.maxSide);
    // Si el formato sabe decodificar ya reducido (JPEG), se le pide así: es
    // mucho más rápido y gasta menos memoria que decodificar entera y reducir.
    if (original.isValid() && (original.width() > maxSide || original.height() > maxSide)
        && reader.supportsOption(QImageIOHandler::ScaledSize)) {
        reader.setScaledSize(original.scaled(maxSide, maxSide, Qt::KeepAspectRatio));
    }

    QImage image = reader.read();
    if (image.isNull()) {
        return core::Error{core::ErrorCode::Corrupted,
                           tr("No se pudo leer la imagen: %1").arg(reader.errorString()), request.path, 0};
    }
    if (cancel.isCancelled()) {
        return core::Error{core::ErrorCode::Cancelled, tr("Cancelado"), request.path, 0};
    }
    if (image.width() > maxSide || image.height() > maxSide) {
        image = image.scaled(maxSide, maxSide, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    // Un formato único para la caché y para pintar rápido.
    if (image.format() != QImage::Format_ARGB32_Premultiplied && image.format() != QImage::Format_RGB32) {
        image = image.convertToFormat(image.hasAlphaChannel() ? QImage::Format_ARGB32_Premultiplied
                                                              : QImage::Format_RGB32);
    }
    return image;
}

} // namespace eu::thumbs
